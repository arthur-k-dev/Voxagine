#include "pch.h"
#include "VoxelBaker.h"

#include "RenderSystem.h"
#include "Core/ECS/Systems/Physics/PhysicsSystem.h"

#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Systems/Rendering/VoxelStamp.h"
#include "Core/ECS/Systems/Chunk/StreamingBudgets.h"
#include "Core/Resources/Formats/VoxModel.h"
#include "Core/Application.h"
#include "Core/Platform/Rendering/FrameProfiler.h"

#include <chrono>
#include <cmath>

#define PI 3.14159265359

double degreesToRadians(double angle_in_degrees) {
	return angle_in_degrees * (PI / 180.0);
}

void VoxelBaker::Init(RenderSystem* pRenderSystem, PhysicsSystem* pPhysicsSystem)
{
	m_pRenderSystem = pRenderSystem;
	m_pPhysicsSystem = pPhysicsSystem;

	m_pRenderContext = m_pRenderSystem->m_pRenderContext;
}

bool VoxelBaker::Bake()
{
	/* RENDERING_PLAN.md Phase 0: the known main-thread cost this pass
	   measures. Guarded so a disabled profiler pays nothing but this one
	   branch - no chrono call, no Report(). */
	const bool bProfiling = FrameProfiler::Get().IsEnabled();
	const std::chrono::steady_clock::time_point start =
		bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;

	m_fRepairMilliseconds = 0.0;

	/* DYNAMIC_MODELS_PLAN.md phase 0. The whole plan turns on how much of this
	   pass is spent re-voxelizing things that move, and the aggregate counter
	   above cannot say - a world load's static stamping and a walking
	   character's per-frame re-stamp land in the same number.

	   Counted as well as timed, and the counts are the ones to quote: they are
	   exact and machine-independent, so they are valid on a busy machine and
	   comparable between two builds. See FrameProfiler::ReportCount. */
	double fStaticMs = 0.0;
	double fDynamicMs = 0.0;

	uint32_t uiStaticRenderers = 0;
	uint32_t uiDynamicRenderers = 0;

	uint32_t uiStaticVoxels = 0;
	uint32_t uiDynamicVoxels = 0;

	/* Phase 5. Whatever this pass does not reach keeps its flags and is picked
	   up by the next one - see StreamingBudgets::VoxelBaking for why that is
	   the resumption mechanism rather than a cursor. */
	StreamingBudget::Scope budget(StreamingBudgets::Get().VoxelBaking);

	bool bPending = false;

	for (VoxRenderer* pRenderer : m_pRenderSystem->m_VoxRenderers)
	{
		/* DYNAMIC_MODELS_PLAN.md phase 3. A dynamic renderer is never stamped -
		   RenderSystem::OnComponentAdded skips its initial Occupy the same
		   way - so there is nothing here for it: no clear, no repair scan,
		   none of the swap/generation bookkeeping below, which exists only to
		   answer "does the voxel buffer still hold what was stamped" for a
		   renderer that was in fact stamped. It renders through
		   VoxelModelPass now (phase 2). Left with everything false/reset so a
		   renderer that is *later* made static starts clean rather than
		   inheriting a stale Updated flag. */
		if (!pRenderer->GetOwner()->IsStatic())
		{
			pRenderer->m_BakeData.Updated = false;
			pRenderer->m_bUpdateRequested = false;
			continue;
		}

		bool bEnabled = pRenderer->IsEnabled();

		/* Phase 9. A stamp that ran out of budget is half in the buffer, and the
		   only thing that may happen to it is being finished. Every skip test
		   below asks whether re-stamping this renderer would change anything,
		   and for a half-stamped one the answer is "yes, the rest of the model" -
		   which none of them can see, because the stamp key and the generation
		   describe the stamp that is *running*. Clearing it again is worse still:
		   the clear erases what has already been written and the resumed walk
		   never goes back for it, which is exactly how the first attempt at this
		   phase drew a level 580,000 voxels short.

		   The partial is only meaningful while the buffer still holds it and the
		   grid still means the same thing. A swap or a window slide voids it, and
		   then the whole stamp has to start again - through the ordinary path,
		   with an update requested so that nothing below skips a renderer whose
		   voxels are half written. */
		bool bResuming = false;

		if (pRenderer->m_BakeData.OccupyInProgress)
		{
			/* bEnabled is part of it: a renderer disabled mid-stamp must reach
			   the clear below rather than be finished, or the half of it that is
			   already in the buffer stays there with nothing left to erase it. */
			bResuming = bEnabled &&
				pRenderer->m_BakeData.Generation == m_pRenderContext->GetVoxelGeneration() &&
				pRenderer->m_BakeData.WorldOffset == grid.GetWorldOffset() &&
				ComputeStampKey(pRenderer) == pRenderer->m_BakeData.Stamp;

			if (!bResuming)
			{
				pRenderer->m_BakeData.OccupyInProgress = false;
				pRenderer->RequestUpdate();

				StreamingCounters::Get().VoxelStampRestarts.fetch_add(1, std::memory_order_relaxed);
			}
		}

		bool bIsStaticChunkLoaded = pRenderer->IsChunkInstanceLoaded() && pRenderer->GetOwner()->IsStatic();

		/* A forced update used to mean "re-stamp every renderer". It has to
		   mean "re-examine every renderer": at a world load
		   RenderSystem::OnComponentAdded has already stamped all of them, so
		   the forced first bake was a clear and an occupy that exactly
		   cancelled - two thirds of the work, and every voxel of it paid for
		   twice in the mapping, the occupancy bitmap and the brick counts.

		   What a force actually guards against is the voxel buffer no longer
		   holding what was stamped into it, and there are exactly two ways for
		   that to happen: the buffer was cleared or resized (Generation), or
		   the chunk window slid underneath it (WorldOffset). Both are recorded
		   at bake time, so each renderer can answer for itself. Transform,
		   rotation, scale and enabled changes come through Updated as before. */
		const bool bBakeCurrent =
			pRenderer->m_BakeData.Positions != nullptr &&
			pRenderer->m_BakeData.Generation == m_pRenderContext->GetVoxelGeneration() &&
			pRenderer->m_BakeData.WorldOffset == grid.GetWorldOffset();

		const bool bForced = m_pRenderSystem->m_bForcedUpdate && !bBakeCurrent;

		/* bEnabled is deliberately not part of this test any more. It used to
		   be, which meant a disabled renderer could never be skipped: it was
		   cleared on every single frame for the rest of its life, and once the
		   first clear had dropped its Positions every later one took Clear's
		   fallback branch, which allocates and scans the renderer's whole
		   bounding box out of the physics grid. The disabled path resets the
		   same flags the enabled one does now, so it clears exactly once. */
		/* The swap that comes with a window slide rewrites the whole resident
		   window from the chunks' own voxels, which never held a dynamic
		   renderer's stamp - so one that has not moved is not "already correct
		   in the buffer", it is missing from it. It has to be re-examined even
		   though nothing about it changed. See Clear. */
		const bool bSwapDropped =
			pRenderer->m_BakeData.WorldOffset != grid.GetWorldOffset() &&
			!pRenderer->GetOwner()->IsStatic();

		if (!bResuming && !bForced && !bSwapDropped && !pRenderer->UpdateRequested() && (!pRenderer->m_BakeData.Updated || bIsStaticChunkLoaded))
			continue;

		/* Everything above says a re-bake was *asked for*. This asks whether it
		   would change anything, and at a world load the answer is no for every
		   renderer in the level: RenderSystem::OnComponentAdded stamps each
		   one, Chunk::UpdateRenderer then requests an update for each one on
		   first load, and the resulting clear-and-re-occupy reproduces the same
		   voxels exactly - measured, 218 of 218 identical, 780 ms of writing
		   back what was already there.

		   Note this is *not* the Updated flag. CheckRendererChange sets that
		   from the transform, and a transform can move without moving a single
		   voxel - it reaches the stamp through a quantized rotation and a
		   floor. All 218 had it set. The stamp key is the thing the voxels
		   actually depend on.

		   Only meaningful while the buffer still holds the previous stamp,
		   which is what Generation and WorldOffset establish above. */
		if (!bResuming && bEnabled && bBakeCurrent && ComputeStampKey(pRenderer) == pRenderer->m_BakeData.Stamp)
		{
			pRenderer->m_BakeData.Updated = false;
			pRenderer->m_bUpdateRequested = false;

			continue;
		}


		/* Past every skip above, so this renderer genuinely wants re-stamping -
		   which is exactly the point at which the budget is worth consulting.
		   Everything before here is comparisons; everything after is voxel
		   writes, and conflating the two is what made this pass unbounded.

		   Nothing is recorded: the flags that got us here are still set, so the
		   next frame's scan finds this renderer and every one behind it. That is
		   the whole resumption mechanism. */
		if (budget.Exhausted())
		{
			bPending = true;
			break;
		}

		/* **Why is a renderer that was already stamped being stamped again?**
		 *
		 * VOXAGINE_CHUNK_IO_TIMINGS=1. Reported as: destroy a pillar, walk
		 * closer, watch it come back. A re-stamp writes the *pristine* model, so
		 * any re-stamp of damaged geometry restores it - that is M7, and M7's
		 * guard (IsChunkInstanceLoaded) only covers the reload path. This names
		 * the entity and the flag that let it through, which is the one thing
		 * that separates "the chunk came back" from "something asked for an
		 * update" from "the window slid and the stamp key check could no longer
		 * apply". */
		/* Resuming is not a re-stamp - it is the rest of one, and printing it
		   buries the lines that matter under one per slice. */
		if (bEnabled && !bResuming && pRenderer->m_BakeData.Positions != nullptr)
		{
			static const bool s_bWhy = std::getenv("VOXAGINE_CHUNK_IO_TIMINGS") != nullptr;

			if (s_bWhy)
			{
				fprintf(stderr, "[bake] re-stamping '%s': forced %d, swapDropped %d, updateRequested %d, updated %d, resuming %d, chunkLoaded %d, bakeCurrent %d (offset %s)\n",
					pRenderer->GetOwner() ? pRenderer->GetOwner()->GetName().c_str() : "?",
					bForced ? 1 : 0, bSwapDropped ? 1 : 0,
					pRenderer->UpdateRequested() ? 1 : 0,
					pRenderer->m_BakeData.Updated ? 1 : 0,
					bResuming ? 1 : 0,
					pRenderer->IsChunkInstanceLoaded() ? 1 : 0,
					bBakeCurrent ? 1 : 0,
					pRenderer->m_BakeData.WorldOffset == grid.GetWorldOffset() ? "same" : "moved");
			}
		}

		/* Its cost belongs to one side of the split. Recorded at both exits
		   below - a disabled renderer clears without stamping, which is real
		   work and would otherwise vanish from the count. */
		const bool bRendererStatic = pRenderer->GetOwner()->IsStatic();

		/* M7 (CHUNK_STREAMING_PLAN.md). A chunk that comes back is restored from
		   its encoded voxels, which are what it looked like when it left - damage
		   included. Re-stamping the pristine model over them is precisely
		   "destroyed terrain comes back", and nothing said so until this counter
		   existed. Counted here rather than asserted because it has to be
		   visible to a Release headless run and to the perf gate. */
		if (bRendererStatic && pRenderer->IsChunkInstanceLoaded())
		{
			StreamingCounters::Get().ChunkInstanceRestamps.fetch_add(1, std::memory_order_relaxed);

			static const bool s_bAudit = std::getenv("VOXAGINE_CHUNK_IO_TIMINGS") != nullptr;

			if (s_bAudit)
			{
				fprintf(stderr, "[chunk] re-stamping reloaded '%s' (updateRequested %d, updated %d, forced %d)\n",
					pRenderer->GetOwner()->GetName().c_str(),
					pRenderer->UpdateRequested() ? 1 : 0,
					pRenderer->m_BakeData.Updated ? 1 : 0,
					bForced ? 1 : 0);
			}
		}

		const std::chrono::steady_clock::time_point rendererStart =
			bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

		const auto fAccount = [&](uint32_t uiVoxels)
		{
			(bRendererStatic ? uiStaticRenderers : uiDynamicRenderers) += 1;
			(bRendererStatic ? uiStaticVoxels : uiDynamicVoxels) += uiVoxels;

			if (!bProfiling)
				return;

			const double fMs = std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - rendererStart).count();

			(bRendererStatic ? fStaticMs : fDynamicMs) += fMs;

			/* One renderer's stamp is the *atom* of the bake budget, so its
			   worst case is the floor on any frame the bake touches. Naming the
			   renderer is what says whether the remaining hitch is "too many
			   renderers in a frame" (a budget problem, solved) or "one model is
			   too big" (which needs a resumable Occupy - phase 5's notes). */
			static const bool s_bReportSlow = std::getenv("VOXAGINE_CHUNK_IO_TIMINGS") != nullptr;

			if (s_bReportSlow && fMs >= 5.0)
			{
				fprintf(stderr, "[bake] '%s' stamped %u voxels in %.2f ms\n",
					pRenderer->GetOwner()->GetName().c_str(), uiVoxels, fMs);
			}
		};

		/* Remove old voxels. A bake that is itself a repair does not start
		   another one - see NotifyClearedRegion.

		   Not for a resumed stamp: what is in the buffer is the first half of
		   the stamp about to be finished, and erasing it is the one mistake
		   this phase exists to not repeat. */
		const bool bRepairOnly = pRenderer->m_BakeData.RepairOnly;

		if (!bResuming)
		{
			pRenderer->m_BakeData.RepairOnly = false;

			Clear(pRenderer, nullptr, !bRepairOnly);
		}

		if (!bEnabled)
		{
			/* Its voxels are gone; nothing more to do until something about it
			   changes. Leaving these set is what made the clear repeat. */
			pRenderer->m_BakeData.Updated = false;
			pRenderer->m_bUpdateRequested = false;

			fAccount(0);

			continue;
		}

		/* Reset */
		if (!bResuming)
		{
			Vector3 pos = pRenderer->GetTransform()->GetPosition();

			pos.x = floor(pos.x);
			pos.y = floor(pos.y);
			pos.z = floor(pos.z);

			pRenderer->m_BakeData.LastLocation = pos;
			pRenderer->m_BakeData.LastRotation = pRenderer->GetTransform()->GetRotation();
			pRenderer->m_BakeData.LastScale = pRenderer->GetTransform()->GetScale();
			pRenderer->m_BakeData.WorldOffset = grid.GetWorldOffset();
		}

		pRenderer->m_BakeData.Updated = false;
		pRenderer->m_bUpdateRequested = false;

		/* What this pass is accountable for is what it writes, which for a
		   resumed stamp is the difference. Zero rather than the recorded Size
		   for a fresh one: Occupy restarts the count, and the stale value is
		   whatever the *previous* stamp wrote. */
		const uint32_t uiSizeBefore = bResuming ? pRenderer->m_BakeData.Size : 0;

		/* Occupy new voxels if position is in bounds. Phase 9's inner bound: one
		   renderer is no longer the atom of this pass, because one of them is a
		   140,640-voxel riverbed that costs 22 ms whatever the outer budget
		   says. */
		Occupy(pRenderer, &pRenderer->m_BakeData, StreamingBudgets::Get().VoxelBakingSamples.UnitsOrUnbounded());

		/* A renderer left mid-stamp is the reason this pass reports itself
		   unfinished: nothing else about it is set, and IsInitialWindowReady
		   waits on exactly this (phase 5's R1). */
		if (pRenderer->m_BakeData.OccupyInProgress)
		{
			bPending = true;

			StreamingCounters::Get().VoxelStampSlices.fetch_add(1, std::memory_order_relaxed);
		}

		fAccount(pRenderer->m_BakeData.Size - uiSizeBefore);

		budget.Consume();
	}

	if (bProfiling)
	{
		/* Counts and timings both go behind the same guard - the profiler's
		   contract is that a disabled build reads no clock and calls no
		   Report, and four string-keyed lookups a frame is exactly the cost it
		   exists to avoid. What makes the counts more trustworthy than the
		   timings is that they do not vary with what else the machine is
		   doing, not that they are free. */
		FrameProfiler::Get().ReportCount("Bake renderers (static)", uiStaticRenderers);
		FrameProfiler::Get().ReportCount("Bake renderers (dynamic)", uiDynamicRenderers);
		FrameProfiler::Get().ReportCount("Bake voxels (static)", uiStaticVoxels);
		FrameProfiler::Get().ReportCount("Bake voxels (dynamic)", uiDynamicVoxels);

		const double fMilliseconds =
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();

		FrameProfiler::Get().Report("CPU VoxelBaker::Bake", fMilliseconds);

		/* The two halves of that number. They do not sum to it - the skip tests
		   above run for every renderer whether or not it is re-baked, and that
		   remainder is the part DYNAMIC_MODELS_PLAN.md does *not* remove. */
		FrameProfiler::Get().Report("CPU Bake (static)", fStaticMs);
		FrameProfiler::Get().Report("CPU Bake (dynamic)", fDynamicMs);

		/* Reported beside it rather than inside it: the repair scan is the one
		   part of the bake whose cost is a function of how many renderers exist
		   rather than how many voxels moved, so it is the part that would grow
		   quietly with the level. */
		FrameProfiler::Get().Report("CPU VoxelBaker::Bake (repair scan)", m_fRepairMilliseconds);
	}

	return !bPending;
}

namespace
{
	/* The renderer-side half of a stamp key, shared by the two places that
	   build one so they cannot drift. */
	void FillStampKey(VoxRenderer* pRenderer, const VoxelStampTransform& stamp,
	                  VoxRenderer::BakeData::StampKey& out)
	{
		out.Origin = stamp.Origin;
		out.Rotation = stamp.Rotation;
		out.Scale = stamp.Scale;
		out.RoundedScale = stamp.RoundedScale;

		out.Frame = pRenderer->GetFrame();
		out.OverrideColor = pRenderer->GetOverrideColor().inst.Color;
		/* The whole tag byte rather than the state alone, so that toggling
		   Emissive counts as a changed stamp - the key's contract is "everything
		   the stamped voxels are a function of", and the tag is one of them
		   (RENDERING_PLAN.md 7.4). */
		out.State = static_cast<int32_t>(VoxelStateTag(pRenderer->GetState(), pRenderer->IsEmissive()));
	}
}

VoxRenderer::BakeData::StampKey VoxelBaker::ComputeStampKey(VoxRenderer* pRenderer)
{
	VoxRenderer::BakeData::StampKey key;

	VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;
	VoxelStampTransform stamp;

	if (!pRenderer->GetFrame() ||
		!ComputeVoxelStampTransform(pRenderer, grid.GetWorldOffset(), 1.f / static_cast<float>(grid.GetVoxelSize()), stamp))
		return key;

	FillStampKey(pRenderer, stamp, key);

	return key;
}

uint32_t* VoxelBaker::Occupy(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData, uint32_t uiMaxSamples)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return nullptr;

	if (pRenderer->GetWorld()->GetApplication()->IsShuttingDown())
		return nullptr;

	/* Voxel world grid */
	VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;

	bool bIsStatic = pRenderer->GetOwner()->IsStatic();

	/* Placement is shared with the far-field build (RENDERING_PLAN.md phase 4)
	   so that the two grids cannot disagree about where a model is - see
	   VoxelStamp.h. The window's grid origin is the chunk window's world
	   offset, which is exactly what VoxelGrid::WorldToGrid subtracts. */
	VoxelStampTransform stamp;

	if (!ComputeVoxelStampTransform(pRenderer, grid.GetWorldOffset(), 1.f / static_cast<float>(grid.GetVoxelSize()), stamp))
		return nullptr;

	uint32_t uiSolidVoxelCount = pFrame->GetSolidVoxelCount();

	/* Phase 9. Everything below is written so that a stamp interrupted by the
	   budget is an ordinary state rather than a special case: the bookkeeping
	   is set up before the first voxel is written and describes what is in the
	   buffer *so far*, so a partial stamp can be cleared, resumed or abandoned
	   by the same code that handles a whole one. The reverted first attempt
	   recorded it at the end instead, which is what made a half-stamped
	   renderer indistinguishable from a finished one. */
	pBakeData = pBakeData ? pBakeData : &pRenderer->m_BakeData;

	if (!pBakeData->OccupyInProgress)
	{
		delete[] pBakeData->Positions;

		pBakeData->Positions = new uint32_t[static_cast<size_t>(
			uiSolidVoxelCount * stamp.RoundedScale.x * stamp.RoundedScale.y * stamp.RoundedScale.z)];

		pBakeData->Size = 0;
		pBakeData->StampCursor = VoxelStampCursor();

		pBakeData->StampDropped = 0;
		pBakeData->StampPlaced = 0;

		pBakeData->IsStatic = bIsStatic;

		/* Recorded here rather than at the end because this is what says the
		   buffer holds this stamp - and while a stamp is in flight it holds
		   part of it, which is a truth Bake needs in order to decide between
		   resuming and starting over. See RenderContext::GetVoxelGeneration. */
		pBakeData->Generation = m_pRenderSystem->m_pRenderContext->GetVoxelGeneration();

		FillStampKey(pRenderer, stamp, pBakeData->Stamp);

		/* And the box it goes into, for the repair pass - see
		   BakeData::StampMin. Derived from the stamp transform rather than from
		   the voxels written, so it is already correct for a partial stamp. */
		if (!ComputeStampedGridBounds(pRenderer, stamp, pBakeData->StampMin, pBakeData->StampMax))
		{
			pBakeData->StampMin = Vector3(1.f);
			pBakeData->StampMax = Vector3(0.f);
		}
	}

	uint32_t* pBaked = pBakeData->Positions;
	uint32_t uiBakedID = pBakeData->Size;

	uint64_t uiEntityID = pRenderer->GetOwner()->GetId();

	/* Resolved once rather than per voxel: the arbitration below only has to
	   know whether an existing owner is *this* renderer, and a slot compares
	   as well as an id does. See RENDERING_PLAN.md phase 4d. */
	const uint16_t uiOwnerSlot = bIsStatic ? grid.AcquireOwnerSlot(uiEntityID) : VoxelOwnerVolume::k_uiNoOwnerSlot;

	/* Counted so the drop below stops being silent, and accumulated on the
	   bake data rather than here because one stamp may span several frames.
	   See the report after the loop. */
	uint32_t& uiDropped = pBakeData->StampDropped;
	uint32_t& uiPlaced = pBakeData->StampPlaced;

	/* VOXAGINE_CHUNK_IO_TIMINGS=1, and it answers a specific report: "I can see
	   walls being regenerated but still walk through them".
	 *
	 * There are two writes in the loop below and only one of them is solid. The
	 * bForceVoxel path writes the CPU voxel, the owner slot *and* the mapping,
	 * which is geometry you collide with; the fallback writes the mapping alone,
	 * which is geometry you can see and walk through. A re-stamp that lands on
	 * cells it is not allowed to own takes the second path for every voxel, and
	 * the result looks exactly like that report. */
	uint32_t uiSolidWrites = 0;
	uint32_t uiRenderOnlyWrites = 0;

	VoxelStampVoxels voxels;
	uint32_t uiStateTag = 0;
	uint32_t uiOverrideColor = 0;
	bool bHasOverrideColor = false;

	DescribeVoxelStampVoxels(pRenderer, voxels, uiStateTag, uiOverrideColor, bHasOverrideColor);

	const bool bComplete = ForEachStampedVoxelRange(
		voxels, uiStateTag, bHasOverrideColor ? &uiOverrideColor : nullptr, stamp,
		pBakeData->StampCursor, uiMaxSamples,
		[&](const Vector3& worldPosition, uint32_t uiColor)
	{
		/* Written as an in-range test rather than a rejection test so a
		   NaN is discarded too: every comparison against NaN is false,
		   so the old form let it through, and static_cast<int32_t> of a
		   NaN is INT32_MIN - which as a uint32 world ID indexes two
		   billion elements past the voxel array. A renderer whose
		   transform or rotation has gone non-finite is enough to
		   produce one. */
		if (!(worldPosition.x >= 0.f && worldPosition.x < m_pRenderSystem->m_v3WorldSize.x &&
		      worldPosition.y >= 0.f && worldPosition.y < m_pRenderSystem->m_v3WorldSize.y &&
		      worldPosition.z >= 0.f && worldPosition.z < m_pRenderSystem->m_v3WorldSize.z))
		{
			/* Out of bounds is ordinary - a model straddling the world
			   edge. Non-finite is not, and naming it once is the
			   difference between a silent skip and knowing which
			   entity's transform went bad. */
			static bool s_bWarned = false;

			if (!s_bWarned && !std::isfinite(worldPosition.x + worldPosition.y + worldPosition.z))
			{
				s_bWarned = true;
				fprintf(stderr, "[bake] non-finite voxel position from '%s': origin(%.2f %.2f %.2f) voxel(%.2f %.2f %.2f)\n",
				        pRenderer->GetOwner()->GetName().c_str(),
				        stamp.Origin.x, stamp.Origin.y, stamp.Origin.z,
				        worldPosition.x, worldPosition.y, worldPosition.z);
			}

			++uiDropped;
			return;
		}

		++uiPlaced;

		// World space ID
		const uint32_t uiWorldID = static_cast<uint32_t>(
			static_cast<int32_t>(worldPosition.x) +
			static_cast<int32_t>(worldPosition.y) * m_pRenderSystem->m_v3WorldSize.x +
			static_cast<int32_t>(worldPosition.z) * m_pRenderSystem->m_v3WorldSize.x * m_pRenderSystem->m_v3WorldSize.y
			);

		bool bForceVoxel = false;

		// Bake as static
		if (bIsStatic)
		{
			// Get grid voxel and the ownership beside it
			const VoxelCell cell = grid.GetCell(
				static_cast<uint32_t>(worldPosition.x),
				static_cast<uint32_t>(worldPosition.y),
				static_cast<uint32_t>(worldPosition.z)
			);

			// Check for out-of-bounds
			if (!cell) return;

			/* Was "(!owner && !active) || owner == me". The slot says which of
			   the two branches applies without resolving anything: zero is
			   unowned, and any other value only matters if it is mine.

			   A particle claim used to land here too and blocked the stamp.
			   Phase 3 deleted claims, so what a stamp can now overwrite is a
			   cell a debris particle happens to be flying through - one
			   overlapping cube for a frame or two, which is the cosmetic cost
			   that whole argument came down to. */
			const uint16_t uiExisting = cell.GetSlot();

			bForceVoxel = uiExisting == VoxelOwnerVolume::k_uiNoOwnerSlot
				? !cell.IsActive()
				: uiExisting == uiOwnerSlot;

			//TODO: check for layer
			if (bForceVoxel)
			{
				/* VOXAGINE_BOUNDS_AUDIT=1 counts the moment the damage is set
				   up: this cell has no owner, so nothing static put a colour
				   there, yet the voxel buffer says something is drawn - which
				   makes it a dynamic renderer's, about to be taken over and
				   recorded on two Positions lists at once. The occupancy comes
				   from the brick grid's CPU bitmap, so asking costs a cached
				   read rather than a PCIe read of VRAM. */
				static const bool s_bAudit = std::getenv("VOXAGINE_BOUNDS_AUDIT") != nullptr;

				if (s_bAudit &&
					uiExisting == VoxelOwnerVolume::k_uiNoOwnerSlot &&
					m_pRenderSystem->m_pRenderContext->GetBrickGrid().IsOccupied(uiWorldID))
				{
					static uint64_t s_uiTaken = 0;

					if (++s_uiTaken == 1 || (s_uiTaken % 1000) == 0)
						fprintf(stderr, "[stamp] %llu voxels taken over from a dynamic renderer by static '%s'\n",
							static_cast<unsigned long long>(s_uiTaken),
							pRenderer->GetOwner()->GetName().c_str());
				}

				cell.SetColor(uiColor);
				cell.SetSlot(uiOwnerSlot);

				m_pRenderSystem->m_pRenderContext->ModifyVoxelFast(uiWorldID, uiColor);

				++uiSolidWrites;
			}
		}

		// Bake color into world
		if (bForceVoxel)
		{
			pBaked[uiBakedID] = uiWorldID;
			uiBakedID++;
		}
		else if (m_pRenderSystem->ModifyVoxel(uiWorldID, uiColor, false))
		{
			++uiRenderOnlyWrites;

			pBaked[uiBakedID] = uiWorldID;
			uiBakedID++;
		}
	});

	/* Voxels dropped for landing outside the world, reported once per renderer.
	 *
	 * This used to be entirely silent, and it hides a real authoring defect:
	 * ComputeVoxelStampTransform centres a model on its transform, so a
	 * renderer whose y is less than half its model's height has its base
	 * clipped away with nothing said. A survey found 853 of 8039 renderers
	 * across 21 levels clipped, 141 of them destructible - and in the arena a
	 * walking monster was reduced to a 1-voxel sliver, which reads on screen as
	 * a character that has partly disappeared.
	 *
	 * Most of the non-destructible ones are deliberate: burying a foundation is
	 * how you hide a seam. So this warns rather than corrects - the fix for any
	 * given entity is a level edit, and which ones are wrong is a judgement
	 * nobody can make from in here. Once per renderer, because it happens every
	 * time the thing is re-stamped. */
	if (uiDropped > 0 && !pRenderer->m_BakeData.bWarnedClipped && bComplete)
	{
		pRenderer->m_BakeData.bWarnedClipped = true;

		fprintf(stderr, "[bake] '%s' (%s): %u of %u voxels are outside the world and were dropped%s\n",
		        pRenderer->GetOwner() ? pRenderer->GetOwner()->GetName().c_str() : "?",
		        pRenderer->GetFrame() ? "model" : "?",
		        uiDropped, uiDropped + uiPlaced,
		        uiPlaced == 0 ? " - nothing of it is drawn" : "");
	}

	{
		static const bool s_bReport = std::getenv("VOXAGINE_CHUNK_IO_TIMINGS") != nullptr;

		if (s_bReport && uiRenderOnlyWrites > 0)
		{
			fprintf(stderr, "[bake] '%s' wrote %u voxels you can walk through (%u solid) - static %d\n",
				pRenderer->GetOwner() ? pRenderer->GetOwner()->GetName().c_str() : "?",
				uiRenderOnlyWrites, uiSolidWrites, bIsStatic ? 1 : 0);
		}
	}

	/* Positions, Size, Generation, the stamp key and the stamped box were all
	   written before the first voxel of this stamp - see the top of the
	   function. What is left is how far it got. */
	pBakeData->Size = uiBakedID;
	pBakeData->OccupyInProgress = !bComplete;

	return pBaked;
}

/* A static renderer that moves takes other renderers' voxels with it, and this
 * is what puts them back.
 *
 * Occupy only consults the physics grid for a *static* renderer
 * (`if (bIsStatic)`), and only a static renderer writes to it. A dynamic
 * renderer's voxels therefore exist in the render buffer and nowhere else, so
 * when a static one is stamped over them its `bForceVoxel` test - "is this cell
 * free of an owner and inactive?" - answers yes. It overwrites them *and
 * records them in its own Positions*, and the next Clear erases them. The
 * dynamic renderer has not changed, so nothing re-stamps it, and the hole stays
 * until something else moves it: drag a wall across a character in the editor
 * and the character loses its middle.
 *
 * Rather than stop the overwrite, which is load-bearing - the wall needs those
 * voxels once the character walks away, or it grows a character-shaped hole -
 * tell whoever might have been erased to stamp again. `Generation = 0` is what
 * marks them: it means exactly "the buffer no longer holds what you stamped",
 * which is the truth here, and it is what stops Bake's stamp-key check from
 * skipping a renderer whose stamp is unchanged but whose voxels are gone.
 *
 * Static renderers are deliberately not notified. They cannot be damaged this
 * way - the physics grid makes them visible to each other's bForceVoxel test,
 * and Clear now declines to erase a voxel another owner holds - and notifying
 * them would let two overlapping renderers mark each other every frame,
 * forever.
 *
 * A *dynamic* renderer's clear reaches here too, which the first version of
 * this did not do. Two dynamic renderers are invisible to each other in exactly
 * the same way: whichever stamps second is refused the overlapping voxels and
 * never records them, so when the first walks away it erases voxels the second
 * should be showing, and the second has no idea. That is the ping-pong the
 * comment above warns about, so the repair itself does not notify: one round is
 * all a repair needs, since it re-stamps everything it holds.
 */
void VoxelBaker::NotifyClearedRegion(VoxRenderer* pCleared, const Vector3& v3GridMin, const Vector3& v3GridMax)
{
	const bool bProfiling = FrameProfiler::Get().IsEnabled();
	const std::chrono::steady_clock::time_point start =
		bProfiling ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point();

	VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;

	/* World space, so that a renderer baked before the window slid is compared
	   against the same origin this clear was measured in. VoxelGrid::GridToWorld
	   against the grid's *current* offset; the candidates below carry the offset
	   their own bake used. */
	const float fVoxelSize = static_cast<float>(grid.GetVoxelSize());

	Box cleared;
	cleared.Min = grid.GridToWorld(v3GridMin);
	cleared.Max = grid.GridToWorld(v3GridMax + Vector3(1.f));

	for (VoxRenderer* pRenderer : m_pRenderSystem->m_VoxRenderers)
	{
		if (pRenderer == pCleared || !pRenderer->IsEnabled() || pRenderer->GetOwner()->IsStatic())
			continue;

		const VoxRenderer::BakeData& bake = pRenderer->m_BakeData;

		if (!bake.Positions || bake.StampMin.x > bake.StampMax.x)
			continue;

		Box stamped;
		stamped.Min = bake.StampMin * fVoxelSize + bake.WorldOffset;
		stamped.Max = (bake.StampMax + Vector3(1.f)) * fVoxelSize + bake.WorldOffset;

		if (!stamped.Intersects(cleared))
			continue;

		pRenderer->m_BakeData.Generation = 0;
		pRenderer->m_BakeData.RepairOnly = true;
		pRenderer->RequestUpdate();
	}

	if (bProfiling)
		m_fRepairMilliseconds +=
			std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void VoxelBaker::ForgetChunkStamp(VoxRenderer* pRenderer)
{
	if (pRenderer == nullptr)
		return;

	VoxRenderer::BakeData& data = pRenderer->m_BakeData;

	delete[] data.Positions;

	data.Positions = nullptr;
	data.Size = 0;
	data.IsStatic = false;
	data.Generation = 0;
	data.Stamp = VoxRenderer::BakeData::StampKey();
	data.StampMin = Vector3(1.f);
	data.StampMax = Vector3(0.f);

	data.OccupyInProgress = false;
	data.StampCursor = VoxelStampCursor();
}

void VoxelBaker::Clear(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData, bool bNotify)
{
	if (pRenderer->GetWorld()->GetApplication()->IsShuttingDown())
		return;

	/* DYNAMIC_MODELS_PLAN.md phase 6. A dynamic renderer that has never been
	   baked (which, since phase 3, is every dynamic renderer - VoxelBaker::Bake
	   and RenderSystem::OnComponentAdded both skip Occupy for one) has nothing
	   in the voxel buffer or the physics grid to remove. Without this,
	   OnComponentDestroyed's call into here for a despawning dynamic renderer
	   (a dead monster, a bullet) falls through to the "else" branch below and
	   pays for a grid.GetChunk() scan of its whole bounding box to discover
	   that, which it does on every dynamic despawn in the game. Guarded on
	   Positions being null rather than IsStatic() alone so a renderer that was
	   static and toggled dynamic still gets its real cleanup - OnComponentAdded's
	   StaticPropertyChanged handler already runs this Clear while the renderer
	   is still static, before the toggle, so Positions is null here precisely
	   when there is truly nothing left to do. */
	const VoxRenderer::BakeData& existingBakeData = pBakeData ? *pBakeData : pRenderer->m_BakeData;

	if (!pRenderer->GetOwner()->IsStatic() && existingBakeData.Positions == nullptr)
		return;

	VoxelGrid& grid = m_pPhysicsSystem->m_VoxelGrid;
	UVector3 gridDims = grid.GetDimensions();
	Vector3 worldOffsetDiff = pRenderer->m_BakeData.WorldOffset - grid.GetWorldOffset();

	pBakeData = pBakeData ? pBakeData : &pRenderer->m_BakeData;

	/* Whatever this clear goes on to do, a stamp that was in flight cannot be
	   resumed across it: the cursor names a walk whose first half this is about
	   to erase, and continuing from it would leave the model with a hole in
	   exactly the shape of what was already written. Phase 9. Every caller
	   reaches this - a despawn, a disable, a static/dynamic toggle, the editor -
	   so the flag is cleared here rather than at each of them. */
	pBakeData->OccupyInProgress = false;
	pBakeData->StampCursor = VoxelStampCursor();

	/* A window slide voids a dynamic renderer's recorded positions outright,
	 * and replaying them is destructive.
	 *
	 * The voxel buffer is double buffered. When the chunk window moves,
	 * ChunkSystem::RenderChunk rewrites every resident chunk's slice of the
	 * *back* buffer in full - from the chunk's own CPU voxels, which hold static
	 * geometry and nothing else - and then swaps. A dynamic renderer never
	 * writes a chunk's voxels, only the mapping, so the swap drops its stamp
	 * entirely. Its Positions then name addresses whose contents are gone, and
	 * the shift below maps them onto addresses that now hold the freshly
	 * rendered static world: erasing those punches a renderer-shaped hole in
	 * whatever the window slid over.
	 *
	 * There is nothing to erase, so do not. Dropping the list also means Bake
	 * re-stamps this renderer into the new front buffer, which it has to - the
	 * swap took its voxels with it, and a renderer standing still would
	 * otherwise stay invisible until something moved it.
	 *
	 * A static renderer is the opposite case and keeps the shift: its colour is
	 * in the chunk's voxels, so RenderChunk puts it back at the shifted address
	 * and the address is exactly where its Positions say it is. */
	if (pBakeData->Positions && worldOffsetDiff != Vector3(0.f) && !pRenderer->GetOwner()->IsStatic())
	{
		/* VOXAGINE_BOUNDS_AUDIT=1 prices what the replay would have cost: how
		   many of the shifted addresses hold geometry right now. Every one of
		   them is a voxel of the freshly slid-in world that the old path was
		   about to zero. Occupancy comes from the brick grid's CPU bitmap. */
		static const bool s_bAudit = std::getenv("VOXAGINE_BOUNDS_AUDIT") != nullptr;

		if (s_bAudit)
		{
			uint32_t uiWouldErase = 0;

			for (uint32_t i = 0; i < pBakeData->Size; ++i)
			{
				const uint32_t uiShifted = (uint32_t)((int)pBakeData->Positions[i] +
					(int)(worldOffsetDiff.x + worldOffsetDiff.z * gridDims.y * gridDims.x));

				if (uiShifted < m_pRenderSystem->m_pRenderContext->GetVoxelDataSize() &&
					m_pRenderSystem->m_pRenderContext->GetBrickGrid().IsOccupied(uiShifted))
					++uiWouldErase;
			}

			fprintf(stderr, "[slide] '%s': %u of %u recorded voxels dropped rather than replayed; %u of the shifted addresses hold world geometry\n",
				pRenderer->GetOwner()->GetName().c_str(),
				pBakeData->Size, pBakeData->Size, uiWouldErase);
		}

		delete[] pBakeData->Positions;

		pBakeData->Positions = nullptr;
		pBakeData->Size = 0;
		pBakeData->IsStatic = false;
		pBakeData->Generation = 0;
		pBakeData->Stamp = VoxRenderer::BakeData::StampKey();

		return;
	}

	/* Remove old voxels if array is valid */
	if (pBakeData->Positions)
	{
		uint32_t arrSize = pBakeData->Size;
		bool bStatic = pBakeData->IsStatic;
		Vector3 voxelPos(0);

		/* Grid-space bounds of what this actually erased - see the notify pass
		   after the loop. */
		Vector3 clearedMin(FLT_MAX);
		Vector3 clearedMax(-FLT_MAX);

		/* Which slot, if any, means "still mine". A dynamic renderer never owns
		   a voxel, so for one of those any owner at all is somebody else. */
		const uint16_t uiOwnerSlot = grid.FindOwnerSlot(pRenderer->GetOwner()->GetId());

		for (uint32_t i = 0; i < arrSize; ++i)
		{
			//Continue on invalid position
			if (pBakeData->Positions[i] == UINT_MAX)
				continue;

			voxelPos = (Vector3)grid.GetVoxelPosition(pBakeData->Positions[i]);
			if (grid.IsOutOfBounds(voxelPos + worldOffsetDiff))
				continue;

			uint32_t chunkOffsetPosition = (uint32_t)((int)pBakeData->Positions[i] + (int)(worldOffsetDiff.x + worldOffsetDiff.z * gridDims.y * gridDims.x));
			if (chunkOffsetPosition >= grid.GetNumVoxels())
				continue;

			voxelPos = grid.GetVoxelPosition(chunkOffsetPosition);
			const VoxelCell cell = grid.GetCell((uint32_t)voxelPos.x, (uint32_t)voxelPos.y, (uint32_t)voxelPos.z);

			/* A recorded position is not a claim that the voxel is still this
			   renderer's. A static bake stamps straight over a dynamic
			   renderer's voxels - it has to, or a wall grows a character-shaped
			   hole the moment the character walks away - and it records them as
			   its own, so the same voxel is now on two Positions lists with the
			   static one holding the grid's ownership. Erasing it here on the
			   strength of a stale list is what punches a hole in the wall when
			   the character moves off, and nothing re-stamps a static renderer
			   whose own stamp has not changed, so the hole stays.

			   The owner slot answers "is it still mine" out of ordinary cached
			   memory, which is the only reason this can afford to ask: reading
			   the voxel back out of the mapping would be a PCIe read of VRAM
			   per cleared voxel.

			   The owner has to be *drawing* something there, which is why the
			   cell has to be active as well as owned. Two states look like an
			   owner and are not: destruction zeroes a voxel's colour and leaves
			   the slot behind, so a blasted area is full of owned but empty
			   cells, and a particle claim reserves a cell without ever putting a
			   colour in the voxel buffer, since particles are drawn by their own
			   pass. Treating either as an owner strands this renderer's colour
			   at that address for good - which is the exact shape of walking a
			   character through a destroyed area and leaving its colours in it.
			   An active owned cell, on the other hand, was written into the grid
			   and the voxel buffer together, so its colour is genuinely there. */
			const uint16_t uiExisting = cell ? cell.GetSlot() : VoxelOwnerVolume::k_uiNoOwnerSlot;

			if (cell && cell.IsActive() &&
				uiExisting != VoxelOwnerVolume::k_uiNoOwnerSlot &&
				uiExisting != VoxelOwnerVolume::k_uiReservedSlot &&
				uiExisting != uiOwnerSlot)
			{
				/* VOXAGINE_BOUNDS_AUDIT=1 counts what this rule saves, which is
				   the only way to see the defect from outside: every one of
				   these was a voxel erased out of somebody else's model. */
				static const bool s_bAudit = std::getenv("VOXAGINE_BOUNDS_AUDIT") != nullptr;

				if (s_bAudit)
				{
					static uint64_t s_uiKept = 0;

					if (++s_uiKept == 1 || (s_uiKept % 10000) == 0)
						fprintf(stderr, "[clear] %llu voxels not erased because another renderer owns them now (latest: '%s' over slot %u)\n",
							static_cast<unsigned long long>(s_uiKept),
							pRenderer->GetOwner()->GetName().c_str(),
							uiExisting);
				}

				continue;
			}

			/* Phase 12's invariant, counted at the write rather than at the
			   rule above it: a mapping word zeroed while the CPU cell keeps a
			   colour is exactly the "occupied only on the CPU" the sync audit
			   reports, whatever reasoning led here. Must stay zero. */
			if (!bStatic && cell && cell.IsActive())
			{
				StreamingCounters::Get().VoxelStampDivergingErases.fetch_add(
					1, std::memory_order_relaxed);
			}

			m_pRenderSystem->m_pRenderContext->ModifyVoxelFast(chunkOffsetPosition, 0);

			clearedMin = glm::min(clearedMin, voxelPos);
			clearedMax = glm::max(clearedMax, voxelPos);

			if (bStatic && cell)
			{
				cell.SetColor(0);
				cell.ClearOwner();
			}
		}

		if (bNotify && clearedMin.x <= clearedMax.x)
			NotifyClearedRegion(pRenderer, clearedMin, clearedMax);

		delete[] pBakeData->Positions;
		pBakeData->Positions = nullptr;
		pBakeData->IsStatic = false;
		pBakeData->Generation = 0;
		pBakeData->Stamp = VoxRenderer::BakeData::StampKey();
	}
	else /* Try to remove voxels which could have been placed by the chunk system rendering */
	{
		Box bounds = pRenderer->GetBounds();
		UVector3 size = static_cast<UVector3>(bounds.GetSize());
		uint32_t numVoxels = size.x * size.y * size.z;
		Voxel** voxels = new Voxel*[numVoxels];
		uint16_t* ownerSlots = new uint16_t[numVoxels];
		UVector3 chunkStart = grid.WorldToGrid(pBakeData->LastLocation - static_cast<Vector3>(size) * 0.5f, true);
		if (!grid.GetChunk(voxels, chunkStart, size, true, ownerSlots))
		{
			delete[] voxels;
			delete[] ownerSlots;
			return;
		}

		bool bIsStatic = pRenderer->GetOwner()->IsStatic();
		const uint16_t uiOwnerSlot = bIsStatic ? grid.AcquireOwnerSlot(pRenderer->GetOwner()->GetId()) : VoxelOwnerVolume::k_uiNoOwnerSlot;

		for (uint32_t i = 0; i < numVoxels; ++i)
		{
			/* Skip if the voxel is invalid */
			if (!voxels[i]) continue;

			if (bIsStatic && uiOwnerSlot != VoxelOwnerVolume::k_uiNoOwnerSlot && ownerSlots[i] == uiOwnerSlot)
			{
				UVector3 chunkRelVec = VoxelGrid::IndexToVector(i, size);
				uint32_t voxelPos = (chunkRelVec.x + chunkStart.x) + (chunkRelVec.y + chunkStart.y) * gridDims.x + (chunkRelVec.z + chunkStart.z) * gridDims.x * gridDims.y;

				m_pRenderSystem->m_pRenderContext->ModifyVoxelFast(voxelPos, 0);

				voxels[i]->Color = 0;
				grid.SetOwnerSlot(
					chunkRelVec.x + chunkStart.x,
					chunkRelVec.y + chunkStart.y,
					chunkRelVec.z + chunkStart.z,
					VoxelOwnerVolume::k_uiNoOwnerSlot);
			}
		}

		delete[] voxels;
		delete[] ownerSlots;
	}
}