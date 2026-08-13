#include "pch.h"
#include "Core/ECS/Systems/Physics/PhysicsSystem.h"

#include "Core/ECS/Components/PhysicsBody.h"
#include "Core/Resources/Formats/VoxModel.h"
#include "Core/ECS/Components/Transform.h"
#include "Core/ECS/Components/BoxCollider.h"
#include "Core/ECS/Systems/Physics/Manifold.h"
#include "Core/ECS/Systems/Physics/Box.h"
#include "Core/ECS/Systems/Physics/Sphere.h"
#include "Core/GameTimer.h"
#include "Core/Application.h"
#include "Core/ECS/World.h"
#include "Core/ECS/Systems/Physics/HitResult.h"
#include "Core/Platform/Rendering/RenderContext.h"
#include "Core/ECS/Systems/Rendering/RenderSystem.h"
#include "Core/ECS/Entities/Camera.h"
#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/ECS/Components/Particles/ParticleSystem.h"
#include "Core/ECS/Components/Particles/ParticlePool.h"
#include <iostream>

#include "Core/Platform/Platform.h"
#include "Core/Platform/Rendering/FrameProfiler.h"
#include "Core/Particles/ParticleLanding.h"
#include "Core/Particles/ParticleSimulation.h"
#include "Core/Voxels/SphericalDestruction.h"
#include "Core/Platform/Rendering/Passes/ParticlePass.h"
#include "External/optick/optick.h"

#include <External/glm/gtx/rotate_vector.hpp>

const float PhysicsSystem::PARTICLE_DESTROY_THRESHOLD = 1.f;
const float PhysicsSystem::PARTICLE_BOUNCE_MULIPLIER = 1.5f;
const Vector3 PhysicsSystem::PARTICLE_GRAVITY = Vector3(0.f, -39.81, 0.f);

#define PHYSICS_MAGICAL_EVERYTHING_SOLVING_VALUE 0.1f

PhysicsSystem::PhysicsSystem(World* pWorld, Vector3 gridSize, uint32_t voxelSize, uint32_t uiMaxParticles, UVector3 chunkSize) :
	ComponentSystem(pWorld)
{
	m_uiMaxParticleCount = uiMaxParticles;

	/* Split once, here, rather than raced for per tick. Both budgets together
	   are the size of the GPU buffer, and the two sources write disjoint,
	   contiguous ranges of it - debris from 0, emitters from the debris count -
	   so the instanced draw sees one run and neither source can consume the
	   other's share (ledger P14). */
	const uint32_t uiDebrisBudget = static_cast<uint32_t>(uiMaxParticles * k_fDebrisBudgetShare);

	m_Debris.Create(uiDebrisBudget);
	m_uiEmitterBudget = uiMaxParticles - uiDebrisBudget;
	m_pStaticEntityBody = new Entity(m_pWorld);
	m_pStaticBody = m_pStaticEntityBody->AddComponent<PhysicsBody>();
	m_pStaticBody->SetInvMass(0);

	m_VoxelGrid.Create((uint32_t)gridSize.x, (uint32_t)gridSize.y, (uint32_t)gridSize.z, voxelSize, chunkSize);

	if (m_pWorld)
	{
		RenderContext* pRenderContext = m_pWorld->GetApplication()->GetPlatform().GetRenderContext();
		ParticlePass* pParticlePass = pRenderContext != nullptr ? pRenderContext->m_pParticlePass : nullptr;
		m_pGPUParticles = pParticlePass != nullptr ? pParticlePass->GetMapper() : nullptr;
		if (m_pGPUParticles != nullptr)
			m_pGPUParticles->Resize(m_uiMaxParticleCount, sizeof(GPUParticle));

		m_pWorld->Paused += Event<World*>::Subscriber(std::bind(&PhysicsSystem::OnWorldPaused, this, std::placeholders::_1), this);
		m_pWorld->Resumed += Event<World*>::Subscriber(std::bind(&PhysicsSystem::OnWorldResumed, this, std::placeholders::_1), this);
	}
}

PhysicsSystem::~PhysicsSystem()
{
	delete m_pStaticEntityBody;

	/* Nothing to stop or join: the checker is a value member driven from this
	   thread. The job this replaced was still reading m_VoxelGrid here. */
}

void PhysicsSystem::Start()
{
	m_IntegrityChecker.SetVoxelGrid(&m_VoxelGrid);
}

bool PhysicsSystem::CanProcessComponent(Component* pComponent)
{
	return dynamic_cast<PhysicsBody*>(pComponent) || dynamic_cast<BoxCollider*>(pComponent) || dynamic_cast<ParticleSystem*>(pComponent);
}

void PhysicsSystem::FixedTick(const GameTimer& fixedTimer)
{
	OPTICK_CATEGORY("Physics", Optick::Category::Physics);
	ScopedFrameTimer tick("CPU PhysicsSystem::FixedTick");

	ProcessIntegrityChecks();

	/* Update particles. Debris first, so it occupies the GPU buffer from index
	   zero and the emitters fill in above it - the two ranges are disjoint and
	   contiguous, which is what an instanced draw needs and what stops either
	   source from starving the other (ledger P14). */
	m_uiActiveParticleCount = 0;
	SimulateParticles(static_cast<float>(fixedTimer.GetElapsedSeconds()));
	TickParticleSystems(fixedTimer);

	ResolveContinousCollision(static_cast<float>(fixedTimer.GetElapsedSeconds()));
	TickBodies(fixedTimer);

	/* Handle collisions */
	AccumulateManifolds();
	ResolveManifolds();
	HandleCallbacks();
	
	

	for (Collider* pCollider : m_Colliders)
	{
		Vector3 gridPos = pCollider->GetTransform()->GetPosition();
		gridPos.x = round(gridPos.x);
		gridPos.y = round(gridPos.y);
		gridPos.z = round(gridPos.z);
		pCollider->SetGridPosition(gridPos);
	}
}

void PhysicsSystem::PostTick(float fDeltaTime)
{
#if defined(EDITOR) || defined(_DEBUG)
	OPTICK_CATEGORY("Physics", Optick::Category::Physics);
	for (Collider* pCollider : m_Colliders)
	{
		DebugBox box;
		box.m_Center = pCollider->GetTransform()->GetPosition();
		box.m_Color = VColors::Red;
		box.m_Extents = static_cast<BoxCollider*>(pCollider)->GetHalfBoxSize();
		m_pWorld->GetDebugRenderer()->AddBox(box);
	}
#endif
}

void PhysicsSystem::OnComponentAdded(Component* pComponent)
{
	if (PhysicsBody* pBody = dynamic_cast<PhysicsBody*>(pComponent))
	{
		if (pBody->GetOwner()->IsStatic())
			return;

		m_Bodies.push_back(pBody);
	}
	else if (BoxCollider* pCollider = dynamic_cast<BoxCollider*>(pComponent))
	{
		m_Colliders.push_back(pCollider);
	}
	else if (ParticleSystem* pParticleSystem = dynamic_cast<ParticleSystem*>(pComponent))
	{
		m_ParticleSystems.push_back(pParticleSystem);
	}
}

void PhysicsSystem::OnComponentDestroyed(Component* pComponent)
{
	if (PhysicsBody* pBody = dynamic_cast<PhysicsBody*>(pComponent))
	{
		std::vector<PhysicsBody*>::iterator iter = std::find(m_Bodies.begin(), m_Bodies.end(), pBody);
		if (iter != m_Bodies.end())
			m_Bodies.erase(iter);
	}
	else if (BoxCollider* pCollider = dynamic_cast<BoxCollider*>(pComponent))
	{
		std::vector<BoxCollider*>::iterator iter = std::find(m_Colliders.begin(), m_Colliders.end(), pCollider);
		if (iter != m_Colliders.end())
			m_Colliders.erase(iter);
	}
	else if (ParticleSystem* pParticleSystem = dynamic_cast<ParticleSystem*>(pComponent))
	{
		std::vector<ParticleSystem*>::iterator iter = std::find(m_ParticleSystems.begin(), m_ParticleSystems.end(), pParticleSystem);
		if (iter != m_ParticleSystems.end())
			m_ParticleSystems.erase(iter);
	}
}

void PhysicsSystem::OnWorldPaused(World* pWorld)
{
	/* Positions queued against the paused world are not worth resuming, and
	   neither is a half-converted island - nor, since phase 3, a pool full of
	   debris whose level-space positions refer to a world that is about to stop
	   existing (ledger P10). */
	m_IntegrityChecker.Reset();
	m_PendingIslands.clear();
	m_PendingIslandVoxels.clear();
	m_uiIslandCursor = 0;
	m_Debris.Clear();
	m_uiActiveParticleCount = 0;
}

void PhysicsSystem::OnWorldResumed(World* pWorld)
{
	m_IntegrityChecker.SetVoxelGrid(&m_VoxelGrid);
}

void PhysicsSystem::TickBodies(const GameTimer& fixedTimer)
{
	OPTICK_EVENT();
	for (PhysicsBody* pBody : m_Bodies)
	{
		if (!pBody->IsEnabled()) continue;

		/* Update body */
		pBody->Tick(static_cast<float>(fixedTimer.GetElapsedSeconds()));

		/* Clamp body to grid bounds */
		Vector3 pos = pBody->GetTransform()->GetPosition();
		if (ClampToBounds(pos, pBody->GetCollider()))
		{
			pBody->GetTransform()->SetPosition(pos);
		}

		/* Perform StepCheck on body */
		if (pBody->GetStepHeight() > 0)
		{
			const VoxFrame* pFrame = nullptr;
			VoxRenderer* pRenderer = pBody->GetOwner()->GetComponent<VoxRenderer>();

			if (pRenderer)
				pFrame = pRenderer->GetFrame();

			StepCheck(pBody, pFrame);
		}
		else pBody->SetResting(false);
	}
}

void PhysicsSystem::TickParticleSystems(const GameTimer& fixedTimer)
{
	OPTICK_EVENT();
	ScopedFrameTimer timer("CPU PhysicsSystem::TickParticleSystems");

	if (m_pGPUParticles == nullptr)
		return;

	/* P16: write the tick's records into the back buffer, not the one the GPU
	   may still be drawing from. RenderSystem::Render swaps it in once this
	   frame's fixed ticks are done. */
	GPUParticle* pGPUParticles = reinterpret_cast<GPUParticle*>(m_pGPUParticles->GetBackBufferData());

	if (pGPUParticles == nullptr)
		return;

	GPUParticle gpuParticle;

	/* Emitters write the range *above* the debris, and only as far as their own
	   budget. The old code shared one running counter with debris and consumed
	   it first, so a busy emitter could take the whole cap and leave the
	   destruction the player is looking at unsimulated and undrawn (P14). */
	const uint32_t uiFirst = m_uiActiveParticleCount;
	const uint32_t uiLimit = std::min(uiFirst + m_uiEmitterBudget, m_uiMaxParticleCount);

	for (ParticleSystem* pSystem : m_ParticleSystems)
	{
		if (!pSystem->IsEnabled()) continue;

		pSystem->Tick((float)fixedTimer.GetElapsedSeconds());

		ParticlePool& pool = pSystem->GetParticles();
		for (uint32_t i = 0; i < pool.GetNumAliveParticles(); ++i)
		{
			pool.Position[i] += pool.Velocity[i] * (float)fixedTimer.GetElapsedSeconds();

			if (m_uiActiveParticleCount < uiLimit)
			{
				// Create GPU particle
				gpuParticle.Position = pool.Position[i];
				gpuParticle.VoxelColor = pool.Color[i];

				std::memcpy(
					&pGPUParticles[m_uiActiveParticleCount],
					&gpuParticle,
					sizeof(GPUParticle)
				);

				m_uiActiveParticleCount++;
			}
		}
	}
}

void PhysicsSystem::ProcessIntegrityChecks()
{
	OPTICK_EVENT();

	/* Runs the flood fill inline for a bounded number of voxel visits. */
	std::vector<std::vector<uint64_t>> results;

	{
		ScopedFrameTimer timer("CPU IntegrityChecker::Process");
		m_IntegrityChecker.Process(IntegrityChecker::VISIT_BUDGET_PER_TICK, results);
	}

	for (std::vector<uint64_t>& checkedVoxels : results)
	{
		m_PendingIslandVoxels.insert(checkedVoxels.begin(), checkedVoxels.end());
		m_PendingIslands.push_back(std::move(checkedVoxels));
	}

	if (m_PendingIslands.empty())
		return;

	/* One batch for the whole tick's conversion. It maintains the CPU voxel,
	   the mapped word, the occupancy bitmap, the brick count and - new in phase
	   1 - the owner slot, which this loop used to leave behind on every voxel
	   it cleared. A cleared-but-owned voxel lies to VoxelBaker::Clear's "is
	   somebody else's voxel here" test for the rest of the world's life. */
	VoxelEditBatch batch(m_pRenderSystem->MakeEditTarget());

	/* Same one-element cache the destruction path uses, for the same reason:
	   an island is overwhelmingly one model's voxels in a row. */
	uint16_t uiCachedSlot = VoxelOwnerVolume::k_uiNoOwnerSlot;
	bool bCachedDestructible = true;
	bool bCacheValid = false;

	/* Conversion deliberately seeds nothing, and the argument is worth keeping
	   because the opposite looks obviously right.
	 *
	 * "A converted voxel disappeared, so whatever rested on it has lost
	 * support" - true of a *burst*, false of an island. An island is a maximal
	 * 26-connected component: the walk collects everything reachable, and
	 * EndCheck only reports when the stack is exhausted, so a component is
	 * always complete. Anything adjacent to an island is therefore *in* it and
	 * falls with it. Nothing can rest on an island without being part of it.
	 *
	 * Seeding it anyway cost 6.99 ms of the gauntlet's 10.83 ms conversion
	 * total - 65% of it - and the oracle never once attributed a find to it.
	 * That is rule 11 in miniature: it was insurance against a case the data
	 * structure makes impossible.
	 *
	 * The one narrow hole: a debris voxel that bakes onto an island in the
	 * frames between classification and conversion is not part of the
	 * component, and loses its support without being seeded. It is one floating
	 * voxel, it is what the pre-rewrite code did too, and it is not worth seven
	 * milliseconds. */

	/* Below the early return, so the accumulator holds only ticks that actually
	   converted something - an average over the ticks that did nothing says
	   nothing about what conversion costs. */
	ScopedFrameTimer convertTimer("CPU IntegrityChecker island conversion");

	/* Converting an island is budgeted too, and that is not symmetry for its
	   own sake. An island is however large the disconnected structure is, and
	   the more of a level has been destroyed the more it fragments, so islands
	   grow over a session. Every voxel in one costs a particle spawn and a
	   batched clear, and doing a whole island in one tick is a frame as long as
	   the island is big - measured as 127 frames over 30 ms in a single
	   session, several pegged at 100 ms, with 94 ms and 41 ms immediately
	   before a GPU timeout that a bullet penetration triggered.

	   Part of that clear reaches the GPU: it writes the voxel mapper, which is
	   DEVICE_LOCAL | HOST_VISIBLE, so an unbounded burst is an unbounded burst
	   of CPU writes into VRAM the voxel pass is reading at the same time.

	   The cost of spreading it is that a large island takes a few frames to
	   fall rather than one. At 16384 voxels a tick that is 60 ms for a 64 K
	   island, against a 100 ms stall for the same work done at once. */
	uint32_t uiConvertBudget = k_uiIntegrityConvertPerTick;

	while (uiConvertBudget > 0 && !m_PendingIslands.empty())
	{
		std::vector<uint64_t>& checkedVoxels = m_PendingIslands.front();

		for (; m_uiIslandCursor < checkedVoxels.size() && uiConvertBudget > 0; ++m_uiIslandCursor)
		{
			--uiConvertBudget;

			uint64_t& vecHash = checkedVoxels[m_uiIslandCursor];
			{
			const Vector3 vec = IntegrityChecker::HashToPosition(vecHash);

			const VoxelCell cell = m_VoxelGrid.GetCell(
				static_cast<uint32_t>(vec.x),
				static_cast<uint32_t>(vec.y),
				static_cast<uint32_t>(vec.z));

			//If the voxel is not active we don't make it into a particle
			if (!cell.IsActive()) continue;

			/* An island does not get to ignore destructibility.
			 *
			 * Conversion never checked it, which was latent for as long as
			 * seeding only ever reached geometry a burst had just cut. It
			 * stopped being latent the moment phase 2 widened the seed set:
			 * a structure that was never ground-connected in the first place -
			 * a pristine level has 14,532 such voxels - would be found and
			 * converted whole, destructible or not. Joey saw a non-destructible
			 * building collapse while a bullet bounced off it.
			 *
			 * Phase 2's seeding is scoped properly now, which stops the
			 * *discovery*. This stops the conversion, and it is the half that
			 * has to hold even when discovery is correct: an indestructible
			 * thing that genuinely loses its support is meant to stay put. */
			const uint16_t uiSlot = cell.GetSlot();

			if (!bCacheValid || uiSlot != uiCachedSlot)
			{
				const uint64_t uiOwnerID = m_VoxelGrid.ResolveOwnerSlot(uiSlot);
				const Entity* pOwner = m_pWorld->FindEntity(uiOwnerID);

				uiCachedSlot = uiSlot;
				bCachedDestructible = (pOwner == nullptr || pOwner->IsDestructible());
				bCacheValid = true;
			}

			if (!bCachedDestructible) continue;

			/* One particle per voxel, and that is not an inconsistency with the
			   explosion's one-per-four (ledger P17 read it as one).
			 *
			 * An explosion scatters debris, so its density is cosmetic. An
			 * island *falls as a shape* - a roof, a balcony, a chunk of wall -
			 * and the particles are that shape. Thinning them to one in four
			 * took three quarters of it away: the sampling runs in sorted hash
			 * order, which is x then y then z, so it thins along one axis and
			 * leaves stripes. Joey reported it as gaps between voxels and the
			 * shape not being preserved during the fall, which is exactly what
			 * it was. */
			ParticleSpawn spawn;
			spawn.v3Position = m_VoxelGrid.GridToWorld(vec);
			spawn.uiColor = cell.GetColor();
			spawn.bBakeOnImpact = true;

			m_Debris.Spawn(spawn);

			batch.Clear(vec);
			}
		}

		if (m_uiIslandCursor >= checkedVoxels.size())
		{
			m_PendingIslands.pop_front();
			m_uiIslandCursor = 0;
		}
	}

	/* Conversion cleared voxels, so anything the checker had classified about
	   the geometry around them is no longer true. */
	if (uiConvertBudget < k_uiIntegrityConvertPerTick)
		m_IntegrityChecker.Invalidate();
}



void PhysicsSystem::AuditParticlePool() const
{
	const ParticleCore::AuditResult result = m_Debris.Audit();

	fprintf(stderr, "[pool-audit] debris %u alive + %u free of %u%s%s%s\n",
	        result.uiCount, result.uiFreeListSize, result.uiCapacity,
	        result.uiBrokenMappings ? " - the slot table and the dense arrays disagree" : "",
	        result.uiDuplicateSlots ? " - a slot is claimed twice" : "",
	        result.IsSound() ? "" : " - UNSOUND");
}

/* The connectivity oracle. DESTRUCTION_PLAN.md phase 4.
 *
 * Phase 4 made the checker remember what it has already classified, which is
 * what stops an explosion's thousands of seeds each re-flooding the same
 * building. A memo is exactly the kind of optimization that can be subtly wrong
 * for a long time before anyone notices - the failure is "a thing that should
 * have fallen did not", which reads as level design rather than as a bug.
 *
 * So the pre-phase-4 walk is kept, verbatim and unbudgeted, as
 * IntegrityChecker::ClassifyExhaustive, and this runs it over the window and
 * compares.
 *
 * **The absolute number is not the finding; the delta is.** A pristine
 * Fishing_Village_Beat1 reports 14,532 voxels in 211 ungrounded components
 * before anything has been destroyed - decorative geometry, props resting on
 * other props, models whose only contact with the ground is through another
 * model the checker's 26-connectivity does not join. None of that is a defect:
 * nothing ever seeds those voxels, so they simply stand, which is what a level
 * designer intended. The first run is therefore taken as the baseline and every
 * run after it reports against it. A delta that grows with destruction and does
 * not come back down is the checker failing to find an island.
 *
 * It walks the whole resident window with an exhaustive flood fill per
 * component, so it costs seconds - 18.7 of them on a 2.1 M-voxel window. On
 * demand only.
 */
void PhysicsSystem::AuditIntegrity()
{
	const UVector3 v3Dimensions = m_VoxelGrid.GetDimensions();

	if (v3Dimensions.x == 0 || v3Dimensions.y == 0 || v3Dimensions.z == 0)
	{
		fprintf(stderr, "[integrity-audit] skipped: no voxel window yet\n");
		return;
	}

	/* Voxels already known to belong to an island the checker has found and is
	   waiting to convert. Those are correctly classified by construction. */
	std::unordered_set<uint64_t> pending;

	for (const std::vector<uint64_t>& island : m_PendingIslands)
		pending.insert(island.begin(), island.end());

	std::unordered_set<uint64_t> classified;
	std::vector<uint64_t> island;

	uint64_t uiOccupied = 0;
	uint64_t uiUngrounded = 0;
	uint64_t uiMissed = 0;
	uint64_t uiIslands = 0;

	const std::chrono::high_resolution_clock::time_point start =
		std::chrono::high_resolution_clock::now();

	for (uint32_t uiZ = 0; uiZ < v3Dimensions.z; ++uiZ)
	for (uint32_t uiY = 1; uiY < v3Dimensions.y; ++uiY)
	for (uint32_t uiX = 0; uiX < v3Dimensions.x; ++uiX)
	{
		const Voxel* pVoxel = m_VoxelGrid.GetVoxel(uiX, uiY, uiZ);

		if (pVoxel == nullptr || !pVoxel->IsActive())
			continue;

		++uiOccupied;

		const uint64_t uiHash = IntegrityChecker::PositionToHash(uiX, uiY, uiZ);

		if (classified.count(uiHash) != 0)
			continue;

		if (!m_IntegrityChecker.ClassifyExhaustive(
			Vector3(static_cast<float>(uiX), static_cast<float>(uiY), static_cast<float>(uiZ)), island))
		{
			/* Grounded. Nothing to compare - the checker never claims
			   something is an island without having walked it. */
			continue;
		}

		++uiIslands;
		uiUngrounded += island.size();

		classified.insert(island.begin(), island.end());

		/* An ungrounded component that the checker has not queued for
		   conversion is one it will never find, because nothing will seed it
		   again. That is the failure this audit exists for. */
		if (pending.count(uiHash) == 0)
			uiMissed += island.size();
	}

	const std::chrono::duration<double, std::milli> span =
		std::chrono::high_resolution_clock::now() - start;

	const IntegrityChecker::Stats& stats = m_IntegrityChecker.GetStats();

	/* Set on the first run and never again - see the function comment on why
	   the absolute count is not the interesting number. */
	static bool s_bHaveBaseline = false;
	static uint64_t s_uiBaselineMissed = 0;
	static uint64_t s_uiBaselineComponents = 0;

	if (!s_bHaveBaseline)
	{
		s_bHaveBaseline = true;
		s_uiBaselineMissed = uiMissed;
		s_uiBaselineComponents = uiIslands;

		fprintf(stderr, "[integrity-audit] baseline: %llu occupied, %llu voxels in %llu ungrounded components "
		                "standing before anything was destroyed; %.0f ms. Later runs report against this.\n",
		        (unsigned long long)uiOccupied, (unsigned long long)uiUngrounded,
		        (unsigned long long)uiIslands, span.count());
	}
	else
	{
		const int64_t iDeltaMissed = static_cast<int64_t>(uiMissed) - static_cast<int64_t>(s_uiBaselineMissed);
		const int64_t iDeltaComponents = static_cast<int64_t>(uiIslands) - static_cast<int64_t>(s_uiBaselineComponents);

		fprintf(stderr, "[integrity-audit] %llu occupied; %llu voxels in %llu ungrounded components not queued "
		                "for conversion (%+lld voxels, %+lld components against the baseline); %.0f ms\n",
		        (unsigned long long)uiOccupied, (unsigned long long)uiMissed,
		        (unsigned long long)uiIslands,
		        (long long)iDeltaMissed, (long long)iDeltaComponents, span.count());
	}

	fprintf(stderr, "[integrity-audit] checker since last reset: %llu seeds offered, %llu deduplicated, "
	                "%llu answered from the memo, %llu visits, %llu memo hits mid-walk, %llu islands emitted\n",
	        (unsigned long long)stats.uiSeedsOffered,
	        (unsigned long long)stats.uiSeedsDeduplicated,
	        (unsigned long long)stats.uiSeedsSkippedByMemo,
	        (unsigned long long)stats.uiVisits,
	        (unsigned long long)stats.uiMemoHits,
	        (unsigned long long)stats.uiIslandsEmitted);
}

bool PhysicsSystem::RayCast(Vector3 start, Vector3 dir, HitResult& hitResult, float fLength /*= FLT_MAX*/, uint32_t uiLayer /*= CollisionLayer::CL_ALL*/)
{
	return RayCastGroup(m_Colliders, start, dir, hitResult, fLength, uiLayer);
}

bool PhysicsSystem::RayCastSingle(const BoxCollider* pCollider, Vector3 start, Vector3 dir, HitResult& hitResult, float fLength /*= FLT_MAX*/, uint32_t uiLayer /*= -1*/)
{
	if (glm::length2(dir) == 0)
		return false;

	dir = glm::normalize(dir);
	dir *= fLength;
	float invDir[3]
	{
		dir.x != 0 ? 1.f / dir.x : 0,
		dir.y != 0 ? 1.f / dir.y : 0,
		dir.z != 0 ? 1.f / dir.z : 0
	};
	float rayStart[3]{ start.x, start.y, start.z };

	float tNear = FLT_MAX;

	if ((pCollider->GetLayer() & uiLayer) == 0) return false;

	Vector3 boxMin = pCollider->GetBoxMin();
	Vector3 boxMax = pCollider->GetBoxMax();

	if (IntersectRayAABB(invDir, rayStart, (float*)&boxMin, (float*)&boxMax, tNear))
	{
		Vector3 hitPoint = start + dir * tNear;

#if defined(EDITOR) || defined(_DEBUG)							
		{
			auto debugRenderer = m_pWorld->GetDebugRenderer();
			{
				DebugLine line;
				line.m_Start = start;
				line.m_End = hitPoint;
				line.m_Color = VColors::Yellow;
				debugRenderer->AddLine(line);
			}
		}
#endif

		/* Don't hit if the distance between start and hit point are greater than ray length */
		if (glm::distance(hitPoint, start) > fLength) return false;

		/* set new hit values */
		hitResult.HitEntity = pCollider->GetOwner();
		hitResult.HitPoint = hitPoint;
		return true;
	}

	return false;
}



bool PhysicsSystem::RayCastGroup(const BoxCollider* pIgnoreCollider, std::vector<BoxCollider*>& colliders, Vector3 start, Vector3 dir, HitResult& hitResult, float fLength /*= FLT_MAX*/, uint32_t uiLayer /*= -1*/)
{
	if (glm::length2(dir) == 0)
		return false;

	dir = glm::normalize(dir);
	dir *= fLength;
	float invDir[3]
	{
		dir.x != 0 ? 1.f / dir.x : 0,
		dir.y != 0 ? 1.f / dir.y : 0,
		dir.z != 0 ? 1.f / dir.z : 0
	};
	float rayStart[3]{ start.x, start.y, start.z };

	float tNear = FLT_MAX;
	bool hit = false;

	for (BoxCollider* pBox : colliders)
	{
		if(pBox == pIgnoreCollider)
			continue;

		if ((pBox->GetLayer() & uiLayer) == 0) continue;

		Vector3 boxMin = pBox->GetBoxMin();
		Vector3 boxMax = pBox->GetBoxMax();

		if (IntersectRayAABB(invDir, rayStart, (float*)&boxMin, (float*)&boxMax, tNear))
		{
			Vector3 hitPoint = start + dir * tNear;

#if defined(EDITOR) || defined(_DEBUG)							
			{
				auto debugRenderer = m_pWorld->GetDebugRenderer();
				{
					DebugLine line;
					line.m_Start = start;
					line.m_End = hitPoint;
					line.m_Color = VColors::Yellow;
					debugRenderer->AddLine(line);
				}

				{
					DebugSphere sphere;
					sphere.m_Center = hitPoint;
					sphere.m_fRadius = 0.5f;
					sphere.m_Color = VColors::Orange;
					debugRenderer->AddSphere(sphere);
				}
			}
#endif

			/* Don't hit if the distance between start and hit point are greater than ray length */
			if (glm::distance(hitPoint, start) > fLength) continue;

			/* set new hit values */
			hit = true;
			hitResult.HitEntity = pBox->GetOwner();
			hitResult.HitPoint = hitPoint;
		}
	}
	return hit;
}

bool PhysicsSystem::RayCastGroup(std::vector<BoxCollider*>& colliders, Vector3 start, Vector3 dir, HitResult& hitResult, float fLength /*= FLT_MAX*/, uint32_t uiLayer /*= -1*/)
{
	return RayCastGroup(nullptr, colliders, start, dir, hitResult, fLength, uiLayer);
}

void PhysicsSystem::ApplySphericalDestruction(const Vector3& position, float fRadius, float fForceMin, float fForceMax, bool bBakeParticle /* true */)
{
	ScopedFrameTimer timer("CPU PhysicsSystem::ApplySphericalDestruction");

	/* Ledger D9. Nothing in PhysicsSystem ever set this - RenderSystem's
	   constructor is the only wiring - so a world without a RenderSystem
	   dereferenced a garbage pointer here. It is null-initialised now and the
	   two paths that need it say so instead of crashing. */
	if (m_pRenderSystem == nullptr)
	{
		static bool s_bWarned = false;

		if (!s_bWarned)
		{
			s_bWarned = true;
			fprintf(stderr, "[physics] destruction requested with no RenderSystem attached; ignored\n");
		}

		return;
	}

	const Vector3 v3GridCenter = m_VoxelGrid.WorldToGrid(position, true);

	VoxelEditBatch batch(m_pRenderSystem->MakeEditTarget());

	/* One-element cache, as before: a burst walks a sphere and consecutive
	   voxels overwhelmingly belong to the same model. */
	uint16_t uiCachedSlot = VoxelOwnerVolume::k_uiNoOwnerSlot;
	bool bCachedDestructible = true;
	bool bCacheValid = false;

	uint32_t uiSpawnCounter = 0;
	std::vector<uint64_t> integrityChecks;

	const SphericalDestruction::Result result = SphericalDestruction::Apply(
		batch, m_VoxelGrid, v3GridCenter, fRadius,
		[&](uint16_t uiOwnerSlot)
		{
			if (bCacheValid && uiOwnerSlot == uiCachedSlot)
				return bCachedDestructible;

			/* A particle claim resolves to 0, so FindEntity gets nothing and
			   the voxel counts as destructible - which is what it did when the
			   field held a raw Particle* and FindEntity failed to match it. */
			const uint64_t uiOwnerID = m_VoxelGrid.ResolveOwnerSlot(uiOwnerSlot);
			const Entity* pOwner = m_pWorld->FindEntity(uiOwnerID);

			uiCachedSlot = uiOwnerSlot;
			bCachedDestructible = (pOwner == nullptr || pOwner->IsDestructible());
			bCacheValid = true;

			return bCachedDestructible;
		},
		[&](const Vector3& v3Position, uint32_t uiColor)
		{
			/* One particle per four destroyed voxels, as before. The counter
			   advances for every destroyed voxel whether or not the pool had
			   room, so the debris stays evenly spread through the sphere
			   rather than clumping wherever the pool happened to refill. */
			const bool bSpawn = (uiSpawnCounter % 4) == 0;
			++uiSpawnCounter;

			if (!bSpawn)
				return;

			Vector3 v3Away = v3Position - v3GridCenter;

			/* A voxel exactly at the centre normalises a zero vector, which is
			   NaN - and a NaN velocity becomes a NaN position, a non-finite
			   proxy AABB and a frame the marcher never finishes. */
			const float fLengthSquared = glm::length2(v3Away);

			v3Away = fLengthSquared > 0.f
				? v3Away / std::sqrt(fLengthSquared)
				: Vector3(0.f, 1.f, 0.f);

			ParticleSpawn spawn;
			spawn.v3Position = m_VoxelGrid.GridToWorld(v3Position);
			spawn.v3Velocity = v3Away * m_ParticleRandom.Range(fForceMin, fForceMax);
			spawn.uiColor = uiColor;
			spawn.bBakeOnImpact = bBakeParticle;

			/* No claim on the cell it came from. The claim system is deleted -
			   see DESTRUCTION_PLAN.md phase 3 on why every consumer of it had a
			   cheaper answer or was satisfied by "particles own nothing". */
			m_Debris.Spawn(spawn);
		},
		&integrityChecks);

	if (result.bRadiusClamped)
	{
		static bool s_bWarned = false;

		if (!s_bWarned)
		{
			s_bWarned = true;
			fprintf(stderr, "[physics] destruction radius %.1f clamped to %.1f\n",
			        fRadius, SphericalDestruction::k_fMaxRadius);
		}
	}

	if (result.uiDestroyed == 0)
		return;

	/* Seeds come from what is *left* standing around the hole, gathered after
	   the clear - see SphericalDestruction::CollectSeeds. The old code pushed
	   nine per destroyed voxel during the loop, most of them naming voxels the
	   same loop destroyed moments later. */
	/* The memo describes a world this burst has just changed, so it goes first.
	   Ordering matters: invalidating *after* enqueuing would leave the new
	   seeds answerable from stale classifications. */
	m_IntegrityChecker.Invalidate();

	/* The candidates were gathered as neighbours of voxels this burst removed;
	   most of them were then removed by the same burst. What is left standing
	   is what could have lost support. */
	SphericalDestruction::FilterSeeds(m_VoxelGrid, integrityChecks);

	m_IntegrityChecker.EnqueueBulk(integrityChecks);
}

bool PhysicsSystem::OverlapSphere(std::vector<BoxCollider*>& colliders, Vector3 center, float fRadius, uint32_t uiLayer /*= -1*/, bool queryTriggers /*= false*/) const
{
	Sphere overlapSphere(center, fRadius);

	bool overlapFound = false;
	for (BoxCollider* pBox : m_Colliders)
	{
		if ((pBox->GetLayer() & uiLayer) == 0) continue;
		if (!queryTriggers && pBox->IsTrigger()) continue;

		Box colliderBox(pBox);
		if (overlapSphere.Intersects(colliderBox))
		{
			colliders.push_back(pBox);
			overlapFound = true;
		}
	}

	return overlapFound;
}

bool PhysicsSystem::OverlapBox(std::vector<BoxCollider*>& colliders, Vector3 center, Vector3 extends, uint32_t uiLayer /*= -1*/, bool queryTriggers /*= false*/) const
{
	Box overlapBox;
	overlapBox.Min = center - extends;
	overlapBox.Max = center + extends;

	bool overlapFound = false;
	for (BoxCollider* pBox : m_Colliders)
	{
		if ((pBox->GetLayer() & uiLayer) == 0) continue;
		if (!queryTriggers && pBox->IsTrigger()) continue;

		Box colliderBox(pBox);
		if (overlapBox.Intersects(colliderBox))
		{
			colliders.push_back(pBox);
			overlapFound = true;
		}
	}

	return overlapFound;
}

bool sortbyfirst(const std::pair<float, BoxCollider*> &a,
	const std::pair<float, BoxCollider*> &b)
{
	return (a.first < b.first);
}


void PhysicsSystem::ResolveContinousCollision(const float deltaTime)
{
	for (BoxCollider* pColliderA : m_Colliders)
	{
		if (!pColliderA->IsEnabled()) continue;

		if (pColliderA->ContinuousCollision())
		{
			Manifold aManifold;
			if (CheckContinousCollision(pColliderA, aManifold, deltaTime))
			{
				if (aManifold.ShouldResolve)
				{
					auto transform = aManifold.Body1->GetTransform();
					if (transform)
					{
						if (!aManifold.Body2)
							aManifold.Body2 = m_pStaticBody;

						/* Impulse resolution */
						Vector3 relativeVel = aManifold.Body1->GetVelocity() - aManifold.Body2->GetVelocity();
						float contactVel = glm::dot(relativeVel, aManifold.Normal);

						if (contactVel <= -PHYSICS_EPSILON)
							continue;

						const float invMassSum = aManifold.Body1->GetInvMass() + aManifold.Body2->GetInvMass();
						if (invMassSum == 0) continue;

						contactVel /= invMassSum;

						Vector3 impulse = aManifold.Normal * contactVel;
						aManifold.Body1->ApplyImpulse(-impulse);
						aManifold.Body2->ApplyImpulse(impulse);

						// Adjust location
						Vector3 origin = transform->GetPosition();
						origin += aManifold.Normal * aManifold.Overlap;
						transform->SetPosition(origin);

					}

					aManifold.ShouldResolve = false;
				}
					

				m_Manifolds.push_back(aManifold);
			}
		}
	}
}


void PhysicsSystem::AccumulateManifolds()
{
	OPTICK_EVENT();
	m_Manifolds.clear();

	for (BoxCollider* pColliderA : m_Colliders)
	{
		if (!pColliderA->IsEnabled()) continue;

		for (BoxCollider* pColliderB : m_Colliders)
		{
			if (!pColliderB->IsEnabled()) continue;
			if (pColliderA == pColliderB) continue;
			if (pColliderA->GetOwner()->IsStatic()) continue;

			Manifold manifold;
			manifold.Collider1 = pColliderA;
			manifold.Body1 = pColliderA->GetOwner()->GetComponent<PhysicsBody>();
			manifold.Collider2 = pColliderB;
			manifold.ShouldResolve = !pColliderA->IsTrigger() && !pColliderB->IsTrigger();

			Box boxA(pColliderA);
			Box boxB(pColliderB);
			if (boxA.Intersects(boxB, manifold))
			{
				manifold.Body2 = pColliderB->GetOwner()->GetComponent<PhysicsBody>();
				m_Manifolds.push_back(manifold);
			}
		}
	}
}

void PhysicsSystem::ResolveManifolds()
{
	OPTICK_EVENT();
	for (Manifold& manifold : m_Manifolds)
	{
		PhysicsBody* pBody1 = manifold.Body1;
		PhysicsBody* pBody2 = manifold.Body2;
		if (!manifold.Collider1->IgnoreVoxels() && !manifold.Collider1->ContinuousVoxelCollision() && 
			!pBody2 && manifold.Collider2->GetOwner()->IsStatic() && manifold.Collider2->VoxelPreciseCollision())
		{
			SolveVoxelPreciseCollision(manifold.Collider1, manifold.Collider2);
			continue;
		}
		else if (manifold.ShouldResolve && pBody1)
		{
			if (!pBody2)
				pBody2 = m_pStaticBody;

			/* Impulse resolution */
			Vector3 relativeVel = pBody1->GetVelocity() - pBody2->GetVelocity();
			float contactVel = glm::dot(relativeVel, manifold.Normal);

			if (contactVel >= -PHYSICS_EPSILON)
				continue;

			const float invMassSum = pBody1->GetInvMass() + pBody2->GetInvMass();
			if (invMassSum == 0) continue;

			contactVel /= invMassSum;

			Vector3 impulse = manifold.Normal * contactVel;
			pBody1->ApplyImpulse(-impulse);
			pBody2->ApplyImpulse(impulse);

			/* Positional correction */
			Vector3 worldPos = manifold.Collider1->GetTransform()->GetPosition();
			worldPos += manifold.Normal * manifold.Overlap;

			/* Small check to keep inside grid bounds */
			if (pBody1->GetCollider())
				ClampToBounds(worldPos, manifold.Collider1);

			pBody1->GetTransform()->SetPosition(worldPos);

			Vector3 gridPos = m_VoxelGrid.WorldToGrid(worldPos);
			manifold.Collider1->SetGridPosition(gridPos);
		}
	}

	// Handle voxel precise collision for colliders with the Continuous Voxel Collision option on
	// Warning voxel precise collision can impact performance if many colliders have it active
	//for (BoxCollider* pCollider : m_Colliders)
	//{
	//	if (pCollider->ContinuousVoxelCollision() && !pCollider->IgnoreVoxels() && !pCollider->GetOwner()->IsStatic())
	//	{
	//		SolveVoxelPreciseCollision(pCollider);
	//	}
	//}
}

void PhysicsSystem::SolveVoxelPreciseCollision(Collider* pColliderA, const Collider* pColliderB)
{
	OPTICK_EVENT();
	BoxCollider* pBoxColliderA = static_cast<BoxCollider*>(pColliderA);
	const BoxCollider* pBoxColliderB = static_cast<const BoxCollider*>(pColliderB);

	Vector3 max = glm::min(pBoxColliderA->GetBoxMax(), pBoxColliderB->GetBoxMax());
	Vector3 min = glm::max(pBoxColliderA->GetBoxMin(), pBoxColliderB->GetBoxMin());

	min = glm::min(min, max);
	max = glm::max(min, max);

#if defined(EDITOR) || defined(_DEBUG)
	{
		auto debugRenderer = m_pWorld->GetDebugRenderer();
		DebugBox box;
		box.m_Color = VColors::BlueViolet;
		box.m_Extents = (max - min) * 0.5f;
		box.m_Center = min + box.m_Extents;
		debugRenderer->AddBox(box);
	}
#endif

	Vector3 colliderGridPos = m_VoxelGrid.WorldToGrid(min, true);
	UVector3 chunkSize = static_cast<UVector3>(max - min);
	uint32_t numVoxels = chunkSize.x * chunkSize.y * chunkSize.z;
	Voxel** voxels = new Voxel*[numVoxels];

	/* Owner slots come along because a callback receives no coordinates and so
	   cannot look ownership up for itself - Bullet's combo streak asks "is this
	   a different model than the last one I hit", and a slot is exactly that
	   identity. See RENDERING_PLAN.md phase 4d. */
	uint16_t* ownerSlots = new uint16_t[numVoxels];
	m_VoxelGrid.GetChunk(voxels, colliderGridPos, chunkSize, true, ownerSlots);

	bool isHandled = false;
	pColliderA->OnVoxelCollision(voxels, ownerSlots, numVoxels, isHandled);

	if (!isHandled)
	{
		bool isColliding = false;

		/* Collision check that makes a bounding box around the active voxels it encounters
		   Simple Box to Box intersection is used after that */
		Vector3 voxelBoxMin = chunkSize;
		Vector3 voxelBoxMax(0, 0, 0);

		for (uint32_t i = 0; i < numVoxels; ++i)
		{
			if (!voxels[i] || !voxels[i]->IsActive()) continue;

			Vector3 voxelPos(
				i % chunkSize.x,
				(i / chunkSize.x) % chunkSize.y,
				i / (chunkSize.x * chunkSize.y)
			);

			if (voxelBoxMin.x > voxelPos.x) voxelBoxMin.x = voxelPos.x;
			if (voxelBoxMin.y > voxelPos.y) voxelBoxMin.y = voxelPos.y;
			if (voxelBoxMin.z > voxelPos.z) voxelBoxMin.z = voxelPos.z;

			if (voxelBoxMax.x < voxelPos.x + 1) voxelBoxMax.x = voxelPos.x + 1;
			if (voxelBoxMax.y < voxelPos.y + 1) voxelBoxMax.y = voxelPos.y + 1;
			if (voxelBoxMax.z < voxelPos.z + 1) voxelBoxMax.z = voxelPos.z + 1;

			isColliding = true;
		}

		if (isColliding)
		{
			Box A, B;
			A.Min = m_VoxelGrid.WorldToGrid(pBoxColliderA->GetBoxMin(), true);
			A.Max = m_VoxelGrid.WorldToGrid(pBoxColliderA->GetBoxMax(), true);
			B.Max = colliderGridPos + voxelBoxMax;
			B.Min = colliderGridPos + voxelBoxMin;

			Manifold manifold;
			if (A.Intersects(B, manifold))
			{
				Vector3 newPos = glm::floor(pColliderA->GetTransform()->GetPosition() + manifold.Normal * manifold.Overlap);
				pColliderA->GetTransform()->SetPosition(newPos);
			}
		}
	}

	delete[] voxels;
	delete[] ownerSlots;
}

void PhysicsSystem::HandleCallbacks()
{
	OPTICK_EVENT();
	for (Collider* pCollider : m_Colliders)
		pCollider->ResetCollisions();

	for (Manifold& manifold : m_Manifolds)
	{
		manifold.Collider1->HandleCollision(manifold);
	}

	for (Collider* pCollider : m_Colliders)
		pCollider->CleanCollisions();
}

void PhysicsSystem::StepCheck(PhysicsBody* pBody, const VoxFrame* pFrame /*= nullptr*/)
{
	uint32_t dimensionX, dimensionY, dimensionZ;
	m_VoxelGrid.GetDimensions(dimensionX, dimensionY, dimensionZ);

	BoxCollider* pCollider = pBody->GetCollider();
	UVector3 boxSize = static_cast<UVector3>(pCollider->GetBoxSize());
	Vector3 chunkStart = m_VoxelGrid.WorldToGrid(pCollider->GetBoxMin());

	/* return if the chunk start is already out of bounds */
	if (chunkStart.y >= dimensionY) 
		return;

	Voxel** voxels = new Voxel*[static_cast<size_t>(boxSize.x * boxSize.z)];
	bool valid = m_VoxelGrid.GetChunk(voxels, chunkStart, Vector3(boxSize.x, 1, boxSize.z), true);
	if (!valid)
	{
		delete[] voxels;
		return;
	}

	for (uint32_t x = 0; x < boxSize.x; ++x)
	{
		for (uint32_t z = 0; z < boxSize.z; ++z)
		{
			uint32_t chunkId = static_cast<uint32_t>(x + boxSize.x * z);

			/* Skip if the voxel is not active or invalid */
			if (!voxels[chunkId] || !voxels[chunkId]->IsActive()) continue;

			/* Skip if a voxel above the maximum step height is active or invalid */
			const Voxel* pVoxel = m_VoxelGrid.GetVoxel(
				static_cast<int>(chunkStart.x + x),
				static_cast<int>(chunkStart.y + pBody->GetStepHeight()),
				static_cast<int>(chunkStart.z + z)
			);

			if (!pVoxel || pVoxel->IsActive()) break;

			uint32_t yHeight = 0;
			for (uint32_t i = 0; i <= pBody->GetStepHeight(); ++i)
			{
				const Voxel* pStepVoxel = m_VoxelGrid.GetVoxel(
					static_cast<int>(chunkStart.x + x),
					static_cast<int>(chunkStart.y + i),
					static_cast<int>(chunkStart.z + z)
				);

				if (pStepVoxel && pStepVoxel->IsActive())
					yHeight = i + 1;
				else break;
			}

			Vector3 pos = pBody->GetTransform()->GetPosition();
			pos.y += yHeight;
			pBody->GetTransform()->SetPosition(pos);

			/* Reset y velocity on step */
			Vector3 vel = pBody->GetVelocity();
			vel.y = 0;

			pBody->SetVelocity(vel);
			pBody->SetResting(true);

			delete[] voxels;
			return;
		}
	}

	// Check voxels below the collider to make sure it can still stay in resting state
	if (pBody->IsResting())
	{
		pBody->SetResting(false);

		uint32_t arrSize = static_cast<uint32_t>(boxSize.x * boxSize.z);
		Voxel** voxelBelow = new Voxel*[arrSize];
		chunkStart.y -= 1;
		if (m_VoxelGrid.GetChunk(voxelBelow, chunkStart, Vector3(boxSize.x, 1, boxSize.z), true))
		{
			for (uint32_t i = 0; i < arrSize; ++i)
			{
				if (voxelBelow[i] && voxelBelow[i]->IsActive())
				{
					pBody->SetResting(true);
					break;
				}
			}
		}

		delete[] voxelBelow;
	}
	
	delete[] voxels;
}

bool PhysicsSystem::ClampToBounds(Vector3& position, Collider* pCollider)
{
	BoxCollider* pBoxCollider = static_cast<BoxCollider*>(pCollider);
	Vector3 vel = Vector3(0.f);
	PhysicsBody* pBody = pCollider->GetOwner()->GetComponent<PhysicsBody>();
	if (pBody)
		vel = pBody->GetVelocity();

	Vector3 boxBounds = pBoxCollider->GetHalfBoxSize();

	uint32_t dimensionX, dimensionY, dimensionZ;
	m_VoxelGrid.GetDimensions(dimensionX, dimensionY, dimensionZ);
	dimensionX = m_pWorld->GetWorldSize().x;
	dimensionZ = m_pWorld->GetWorldSize().y;
	
	bool clamped = false;

	if (position.x - boxBounds.x < 1)
	{
		clamped = true;
		position.x = boxBounds.x + 1;
		vel.x = 0;
	}
	else if (position.x + boxBounds.x > dimensionX - 1)
	{
		clamped = true;
		position.x = dimensionX - boxBounds.x - 1;
		vel.x = 0;
	}

	if (position.y - boxBounds.y < 1)
	{
		clamped = true;
		position.y = boxBounds.y + 1;
		vel.y = 0;

		if (pBody)
			pBody->SetResting(true);
		
	}
	else if (position.y + boxBounds.y > dimensionY - 1)
	{
		clamped = true;
		position.y = dimensionY - boxBounds.y - 1;
		vel.y = 0;
	}

	if (position.z - boxBounds.z < 1)
	{
		clamped = true;
		position.z = boxBounds.z + 1;
		vel.z = 0;
	}
	else if (position.z + boxBounds.z > dimensionZ - 1)
	{
		clamped = true;
		position.z = dimensionZ - boxBounds.z - 1;
		vel.z = 0;
	}	

	if (pBody)
	{
		pBody->SetVelocity(vel);
	}		

	return clamped;
}

bool PhysicsSystem::IntersectRayAABB(const float* invRayDir, const float* rayStart, const float* boxMin, const float* boxMax, float& tNear)
{
	float tNearLocal = -FLT_MAX;
	float tFarLocal = FLT_MAX;

	for (uint32_t i = 0; i < 3; i++)
	{
 		if (invRayDir[i] == 0)
		{
			if (rayStart[i] < boxMin[i] || rayStart[i] > boxMax[i])
				return false;
		}
		else
		{
			float tMin = (boxMin[i] - rayStart[i]) * invRayDir[i];
			float tMax = (boxMax[i] - rayStart[i]) * invRayDir[i];

			// Make sure tMin is always minimum value
			if (tMin > tMax)
			{
				float temp = tMin;
				tMin = tMax;
				tMax = temp;
			}

			if (tMin > tNearLocal)
				tNearLocal = tMin;

			if (tMax < tFarLocal)
				tFarLocal = tMax;

			if (tNearLocal > tFarLocal || tFarLocal < 0)
				return false;
		}
	}

	if (tNearLocal < tNear && tNearLocal > 0)
		tNear = tNearLocal;
	else if (tFarLocal < tNear)
		tNear = tFarLocal;

	return true;
}

bool PhysicsSystem::CheckContinousCollision(BoxCollider* pColliderA, Manifold& manifold, float deltaTime)
{
	PhysicsBody* pPhysBodyA = pColliderA->GetOwner()->GetComponent<PhysicsBody>();
	if (!pPhysBodyA)
		return false;

	manifold.Body1 = pPhysBodyA;
	manifold.Collider1 = pColliderA;

	Transform* transform = pColliderA->GetTransform();
	Vector3 centerA = transform->GetPosition();
	Vector3 extends = pPhysBodyA->GetVelocity() * deltaTime;

	if( (std::abs(extends.x) + std::abs(extends.y) + std::abs(extends.z)) < 5.f)
		return false;

	float lenght = glm::length(extends);
	manifold.Normal = normalize(extends);

	Vector3 halfSizeA = pColliderA->GetHalfBoxSize();

	HitResult hitRes;

	std::vector<BoxCollider*> potentialColliders;

	Vector3 OverlapBoxMin = glm::min(pColliderA->GetBoxMin(), pColliderA->GetBoxMin() + extends);
	Vector3 OverlapBoxMax = glm::max(pColliderA->GetBoxMax(), pColliderA->GetBoxMax() + extends);

	Vector3 OverlapBoxExtends = OverlapBoxMax - OverlapBoxMin;

	// Check for potential colliders
	if (!OverlapBox(potentialColliders, OverlapBoxMin, glm::abs(OverlapBoxExtends), pColliderA->GetLayer(), true) || potentialColliders.size() < 2u)
		return false;

	float dot = FLT_MAX;
	Vector3 origin;

	/* The eight corners of the collider, counted rather than stepped to.
	 *
	 * This was three nested loops of the form
	 * `for (float z = -h.z; z <= h.z; z += h.z * 2.f)`, which visit -h and +h
	 * and are meant to be the corners - but **the step is h * 2, so a collider
	 * with a zero extent on any axis steps by zero and the loop never ends**.
	 * A flat collider, or one whose size was never set, is enough. It only
	 * matters for a body moving more than 5 units in a tick (the early-out
	 * above), which in this game is a thrown bullet, and in Debug or the editor
	 * every iteration submits a debug line - so the process spins on the main
	 * thread *and* grows m_DebugDrawLines without bound. Pausing it lands in
	 * DebugRenderer, which is where it was first reported from and is not where
	 * the defect is.
	 *
	 * Counting to 8 gives the identical set of origins for any collider with
	 * real extents, and for a degenerate one it simply repeats a corner, which
	 * costs a few redundant casts and terminates. */
	for (uint32_t uiCorner = 0; uiCorner < 8; ++uiCorner)
	{
		{
			{
				origin = {
					(uiCorner & 1) ? halfSizeA.x : -halfSizeA.x,
					(uiCorner & 2) ? halfSizeA.y : -halfSizeA.y,
					(uiCorner & 4) ? halfSizeA.z : -halfSizeA.z };

#if defined(EDITOR) || defined(_DEBUG)							
					{
						auto debugRenderer = m_pWorld->GetDebugRenderer();
						{
							DebugLine line;
							line.m_Start = centerA + origin;
							line.m_End = centerA + origin + manifold.Normal * (lenght * 2.f);
							line.m_Color = VColors::Blue;
							debugRenderer->AddLine(line);
						}
					}
#endif
				Vector3 rayStart = centerA;
				BoxCollider* pIgnoreCollider = pColliderA;
				float rayLenght = lenght;

				/* This loop can only repeat through the `continue` below, and it
				 * terminates only because that path shortens the ray. Both of its
				 * bounds were therefore one float comparison:
				 *
				 *   - `dist == 0.f` is exact equality. A hit that advances the ray
				 *     by 1e-30 is not zero, so it passed the guard, subtracted
				 *     nothing from rayLenght, moved rayStart nowhere, and the next
				 *     cast returned the identical hit - forever. A frame that never
				 *     ends looks exactly like the GPU hang in CLAUDE.md and is not
				 *     the same thing at all.
				 *   - Nothing bounded the number of hits. `pIgnoreCollider` excludes
				 *     only the collider hit last, so two overlapping voxel-precise
				 *     colliders can hand the ray back and forth.
				 *
				 * So: progress must be real (an epsilon, which also disposes of a
				 * NaN distance - `!(dist > eps)` is true for one, where the old
				 * test was false), and there is a hard step cap behind it. Same
				 * argument as the marcher's MARCH_STEP_BUDGET: a wrong answer in a
				 * pathological case is cheaper than a frame that never returns. */
				const float k_fMinRayAdvance = 1e-3f;
				const uint32_t k_uiMaxRaySteps = 32;

				uint32_t uiRaySteps = 0;

				while( rayLenght > 0.f && RayCastGroup(pIgnoreCollider, potentialColliders, rayStart + origin, manifold.Normal, hitRes, rayLenght))
				{
					if (++uiRaySteps > k_uiMaxRaySteps)
					{
						static bool s_bWarned = false;

						if (!s_bWarned)
						{
							s_bWarned = true;
							fprintf(stderr, "[physics] ray against '%s' passed %u voxel-precise colliders and was cut short\n",
								pColliderA->GetOwner() ? pColliderA->GetOwner()->GetName().c_str() : "?",
								k_uiMaxRaySteps);
						}

						break;
					}

					if (hitRes.HitEntity)
					{
						BoxCollider* pColliderB = hitRes.HitEntity->GetComponent<BoxCollider>();
						if (pColliderB)
						{
							if (!pColliderB->VoxelPreciseCollision() && pColliderB->IsEnabled(true))
							{
								float newDot = glm::dot(hitRes.HitPoint - (rayStart - origin), manifold.Normal);

								if (newDot < dot)
								{
									dot = newDot;
								}
							}
							else
							{
								float dist = glm::distance(hitRes.HitPoint, rayStart);

								/* Not `dist == 0.f`: see the loop's comment. This
								   is the only thing that makes the loop shorter. */
								if (!(dist > k_fMinRayAdvance))
									break;

								rayLenght -= dist;
								rayStart = hitRes.HitPoint;
								pIgnoreCollider = pColliderB;		
								continue;
							}
						}
					}
					break;
				}
			}
		}
	}

	if(!hitRes.HitEntity || dot > lenght)
		return false;

	BoxCollider* pColliderB = hitRes.HitEntity->GetComponent<BoxCollider>();

#if defined(EDITOR) || defined(_DEBUG)							
	{

		Vector3 origin = centerA + manifold.Normal * (dot - PHYSICS_MAGICAL_EVERYTHING_SOLVING_VALUE);

		auto debugRenderer = m_pWorld->GetDebugRenderer();
		{
			DebugLine line;
			line.m_Start = centerA;
			line.m_End = origin;
			line.m_Color = VColors::Yellow;
			debugRenderer->AddLine(line);
		}

		{
			DebugBox box;
			box.m_Center = centerA + extends;
			box.m_Extents = glm::abs(extends) + pColliderA->GetHalfBoxSize();
			box.m_Color = VColors::LimeGreen;
			debugRenderer->AddBox(box);
		}

		{
			DebugSphere sphere;
			sphere.m_Center = origin;
			sphere.m_fRadius = 5.f;
			sphere.m_Color = VColors::Aqua;
			debugRenderer->AddSphere(sphere);
		}

		{
			DebugBox box;
			box.m_Center = origin;
			box.m_Extents = pColliderA->GetHalfBoxSize();
			box.m_Color = VColors::Yellow;
			debugRenderer->AddBox(box);
		}


	}

#endif

	manifold.Overlap = dot - PHYSICS_MAGICAL_EVERYTHING_SOLVING_VALUE;
	manifold.Body2 = pColliderB->GetOwner()->GetComponent<PhysicsBody>();
	manifold.Collider2 = pColliderB;
	manifold.ShouldResolve = !pColliderA->IsTrigger() && !pColliderB->IsTrigger();

	return true;
}

void PhysicsSystem::SimulateParticles(float fDeltaTime)
{
	OPTICK_EVENT();
	ScopedFrameTimer timer("CPU PhysicsSystem::SimulateParticles");

	if (m_pRenderSystem == nullptr || m_pGPUParticles == nullptr)
		return;

	/* Ledger P12: this used to dereference the mapper unconditionally, and the
	   mapper stays null when PhysicsSystem is constructed without a World -
	   which is exactly the unit-test path. */
	/* P16: the back buffer, for the same reason as TickParticleSystems above -
	   this and that call write disjoint ranges of the same tick's records,
	   and both have to land before RenderSystem::Render's swap makes them
	   visible together. */
	GPUParticle* pGPUParticles = reinterpret_cast<GPUParticle*>(m_pGPUParticles->GetBackBufferData());

	if (pGPUParticles == nullptr)
		return;

	VoxelEditBatch batch(m_pRenderSystem->MakeEditTarget());

	const VoxelBrickGrid& bricks = m_pRenderSystem->m_pRenderContext->GetBrickGrid();
	const UVector3 v3WindowSize = m_VoxelGrid.GetDimensions();
	const Vector3 v3WorldOffset = m_VoxelGrid.GetWorldOffset();

	ParticleSimulation::Settings settings;
	settings.v3Gravity = PARTICLE_GRAVITY;
	settings.fDestroyThreshold = PARTICLE_DESTROY_THRESHOLD;
	settings.fBounceMultiplier = PARTICLE_BOUNCE_MULIPLIER;

	uint32_t uiIndex = 0;

	while (uiIndex < m_Debris.GetCount())
	{
		const ParticleSimulation::Outcome outcome = ParticleSimulation::Step(
			m_Debris, uiIndex, fDeltaTime, m_VoxelGrid, bricks, v3WindowSize, v3WorldOffset, settings);

		if (outcome.bRetire)
		{
			/* Not onto something that is already falling, or that is about to
			   be told it is falling.
			 *
			 * The support would be cleared within a few ticks and the debris
			 * left in mid-air, and nothing would ever seed it - a burst seeds
			 * its neighbours, but island conversion deliberately seeds nothing
			 * (see ProcessIntegrityChecks on why it cannot need to). Keeping
			 * the particle alive lets it settle once the island has gone, which
			 * is what it would do anyway.
			 *
			 * Two sets, because a walk is resumable: a voxel can be already
			 * enumerated into an island that has not been reported yet, and
			 * debris landing on it in that window is invisible to both the
			 * island and any future seed. */
			const uint64_t uiSupport = IntegrityChecker::PositionToHash(
				static_cast<uint32_t>(outcome.iImpactX),
				static_cast<uint32_t>(outcome.iImpactY),
				static_cast<uint32_t>(outcome.iImpactZ));

			if (m_PendingIslandVoxels.count(uiSupport) != 0 ||
				m_IntegrityChecker.IsClassifying(uiSupport))
			{
				++uiIndex;
				continue;
			}
		}

		if (outcome.bBake)
		{
			batch.Set(
				Vector3(static_cast<float>(outcome.iBakeX),
				        static_cast<float>(outcome.iBakeY),
				        static_cast<float>(outcome.iBakeZ)),
				m_Debris.Color[uiIndex],
				VoxelOwnerVolume::k_uiNoOwnerSlot);
		}

		if (outcome.bRetire)
		{
			/* Retire swaps the last particle into this index, so the loop must
			   not advance. */
			m_Debris.Retire(uiIndex);
			continue;
		}

		/* The GPU record is written *before* anything can retire this particle,
		   and only for particles that survive the tick. The old code wrote it
		   after DestroyParticle, so a retired particle - whose free-list link
		   aliased its own position through a union - was drawn one more frame
		   from whatever that pointer looked like as a float3 (P1). */
		GPUParticle gpuParticle;
		gpuParticle.Position = m_Debris.Position[uiIndex];
		gpuParticle.VoxelColor = m_Debris.Color[uiIndex];

		std::memcpy(&pGPUParticles[uiIndex], &gpuParticle, sizeof(GPUParticle));

		++uiIndex;
	}

	/* The live count, exactly. The old counter incremented for particles it
	   then destroyed, so the instance count could exceed the number of records
	   actually written and the pass drew stale ones (P15). */
	m_uiActiveParticleCount = m_Debris.GetCount();
}
