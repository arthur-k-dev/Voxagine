#pragma once

#include "Core/ECS/Components/VoxRenderer.h"

#include <cstdint>

class RenderContext;
class RenderSystem;
class PhysicsSystem;

class VoxelBaker
{
public:
	enum BakeType
	{
		E_OCCUPY,
		E_CLEAR
	};

	struct BakeCommand
	{
		BakeType Type;
		VoxRenderer* Renderer;
		VoxRenderer::BakeData* Data;
	};

	void Init(RenderSystem* pRenderSystem, PhysicsSystem* pPhysicsSystem);

	/* One budgeted pass over the renderers that want re-stamping
	   (StreamingBudgets::VoxelBaking). Whatever it does not reach keeps its
	   Updated/UpdateRequested flags and is found again next frame, so the loop
	   is resumable without a cursor and without holding a pointer across a
	   frame - see the budget's comment.

	   Returns true when nothing is left wanting a stamp, which is what
	   RenderSystem::HasPendingVoxelBakes answers with. */
	virtual bool Bake();

	/* Stamps the renderer's model into the resident window, at most uiMaxSamples
	   voxel-samples of it. A sample is one (model voxel, scale offset) pair -
	   what the walk costs - not one voxel written.
	 *
	 * Whatever it does not reach is left on pBakeData->StampCursor with
	 * OccupyInProgress set, and calling again continues from there. Positions
	 * and Size describe what is in the buffer at all times, including
	 * mid-stamp, which is what makes an interrupted stamp safe to clear or
	 * abandon. CHUNK_STREAMING_PLAN.md phase 9; the acceptance for the split is
	 * an occupancy count rather than a frame time, and phase 5's notes say why.
	 *
	 * The default budget is the whole model in one call, which is what the
	 * editor's own bake data and any non-streaming caller want. */
	virtual uint32_t* Occupy(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData = nullptr,
	                         uint32_t uiMaxSamples = UINT32_MAX);

	/* bNotify false suppresses the repair pass below, and is how a repair is
	   stopped from provoking another one - see NotifyClearedRegion. */
	virtual void Clear(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData = nullptr, bool bNotify = true);

	/* Clear's counterpart for a renderer leaving with its chunk: forget what was
	   stamped without erasing a single voxel. Everything Clear resets is reset
	   here too, so a renderer that comes back with the chunk starts from "never
	   baked" rather than from a stale generation. */
	void ForgetChunkStamp(VoxRenderer* pRenderer);

	/* What Occupy would stamp, as a value: see VoxRenderer::BakeData::StampKey.
	   O(1) - it computes the stamp transform and reads three fields, it does
	   not walk the model. */
	VoxRenderer::BakeData::StampKey ComputeStampKey(VoxRenderer* pRenderer);

	/* Marks the dynamic renderers a Clear may have erased, so they stamp
	   themselves again. See the definition - the asymmetry between static and
	   dynamic is the whole reason this is needed. */
	void NotifyClearedRegion(VoxRenderer* pCleared, const Vector3& v3GridMin, const Vector3& v3GridMax);

protected:
	/* Milliseconds this bake spent in NotifyClearedRegion, accumulated across
	   every clear in the pass and reported once - the scan is per clear and
	   per renderer, so a per-call average would say nothing about the frame.
	   Only written while the profiler is on. */
	double m_fRepairMilliseconds = 0.0;

	RenderContext* m_pRenderContext = nullptr;
	RenderSystem* m_pRenderSystem = nullptr;
	PhysicsSystem* m_pPhysicsSystem = nullptr;
};