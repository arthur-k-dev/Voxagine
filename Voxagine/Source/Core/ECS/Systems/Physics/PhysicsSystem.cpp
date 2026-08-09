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
#include "Core/Voxels/SphericalDestruction.h"
#include "Core/Platform/Rendering/Passes/ParticlePass.h"
#include "External/optick/optick.h"

#include <External/glm/gtx/rotate_vector.hpp>

const float PhysicsSystem::PARTICLE_DESTROY_THRESHOLD = 1.f;
const float PhysicsSystem::PARTICLE_BOUNCE_MULIPLIER = 1.5f;
const Vector3 PhysicsSystem::PARTICLE_GRAVITY = Vector3(0.f, -39.81, 0.f);

#define PHYSICS_MAGICAL_EVERYTHING_SOLVING_VALUE 0.1f

PhysicsSystem::PhysicsSystem(World* pWorld, Vector3 gridSize, uint32_t voxelSize, uint32_t uiMaxParticles, UVector3 chunkSize) :
	ComponentSystem(pWorld),
	m_ParticlePool(uiMaxParticles)

{
	m_uiMaxParticleCount = uiMaxParticles;
	m_pStaticEntityBody = new Entity(m_pWorld);
	m_pStaticBody = m_pStaticEntityBody->AddComponent<PhysicsBody>();
	m_pStaticBody->SetInvMass(0);

	m_VoxelGrid.Create((uint32_t)gridSize.x, (uint32_t)gridSize.y, (uint32_t)gridSize.z, voxelSize, chunkSize);

	if (m_pWorld)
	{
		m_pGPUParticles = m_pWorld->GetApplication()->GetPlatform().GetRenderContext()->m_pParticlePass->GetMapper();
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

	/* Update particles */
	m_uiActiveParticleCount = 0;
	TickParticleSystems(fixedTimer);
	SimulateParticles(static_cast<float>(fixedTimer.GetElapsedSeconds()));

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
	   neither is a half-converted island. */
	m_IntegrityChecker.Reset();
	m_PendingIslands.clear();
	m_uiIslandCursor = 0;
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

	GPUParticle* pGPUParticles = reinterpret_cast<GPUParticle*>(m_pGPUParticles->GetData());
	GPUParticle gpuParticle;

	for (ParticleSystem* pSystem : m_ParticleSystems)
	{
		if (!pSystem->IsEnabled()) continue;

		pSystem->Tick((float)fixedTimer.GetElapsedSeconds());

		ParticlePool& pool = pSystem->GetParticles();
		for (uint32_t i = 0; i < pool.GetNumAliveParticles(); ++i)
		{
			pool.Position[i] += pool.Velocity[i] * (float)fixedTimer.GetElapsedSeconds();

			if (m_uiActiveParticleCount < m_uiMaxParticleCount)
			{
				m_uiActiveParticleCount++;

				// Create GPU particle
				gpuParticle.Position = pool.Position[i];
				gpuParticle.VoxelColor = pool.Color[i];

				std::memcpy(
					&pGPUParticles[m_uiActiveParticleCount - 1],
					&gpuParticle,
					sizeof(GPUParticle)
				);
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
		m_PendingIslands.push_back(std::move(checkedVoxels));

	if (m_PendingIslands.empty())
		return;

	/* One batch for the whole tick's conversion. It maintains the CPU voxel,
	   the mapped word, the occupancy bitmap, the brick count and - new in phase
	   1 - the owner slot, which this loop used to leave behind on every voxel
	   it cleared. A cleared-but-owned voxel lies to VoxelBaker::Clear's "is
	   somebody else's voxel here" test for the rest of the world's life. */
	VoxelEditBatch batch(m_pRenderSystem->MakeEditTarget());

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

			Particle* pParticle = m_ParticlePool.SpawnParticle();
			if (pParticle)
			{
				pParticle->Live.BakeOnImpact = true;
				pParticle->Live.GridPosition = vec;
				pParticle->Live.Position = m_VoxelGrid.GridToWorld(vec);
				pParticle->Live.VoxelColor = cell.GetColor();
			}

			batch.Clear(vec);

			/* After the clear, which now clears the owner - see the same
			   ordering note in ApplySphericalDestruction. */
			if (pParticle)
				cell.SetParticleOwner((uintptr_t)pParticle);
			}
		}

		if (m_uiIslandCursor >= checkedVoxels.size())
		{
			m_PendingIslands.pop_front();
			m_uiIslandCursor = 0;
		}
	}
}



void PhysicsSystem::AuditParticlePool() const
{
	const ParticleLinkedList::AuditResult result = m_ParticlePool.Audit();

	fprintf(stderr, "[pool-audit] %llu alive + %llu free of %llu%s%s%s%s\n",
	        (unsigned long long)result.uiAlive,
	        (unsigned long long)result.uiFree,
	        (unsigned long long)result.uiPool,
	        result.uiUnaccounted ? " - some particles are on neither list" : "",
	        result.uiDuplicated ? " - some particles are on both lists or listed twice" : "",
	        result.bAliveCycle ? " - the alive list does not terminate" : "",
	        result.bFreeCycle ? " - the free list does not terminate" : "");
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

			Particle* pParticle = m_ParticlePool.SpawnParticle();

			if (pParticle == nullptr)
				return;

			pParticle->Live.GridPosition = v3Position;
			pParticle->Live.Position = m_VoxelGrid.GridToWorld(v3Position);
			pParticle->Live.BakeOnImpact = bBakeParticle;
			pParticle->Live.VoxelColor = uiColor;

			Vector3 v3Away = v3Position - v3GridCenter;

			/* A voxel exactly at the centre normalises a zero vector, which is
			   NaN - and a NaN velocity becomes a NaN position, a non-finite
			   proxy AABB and a frame the marcher never finishes. */
			const float fLengthSquared = glm::length2(v3Away);

			v3Away = fLengthSquared > 0.f
				? v3Away / std::sqrt(fLengthSquared)
				: Vector3(0.f, 1.f, 0.f);

			pParticle->Live.Velocity = v3Away * m_ParticleRandom.Range(fForceMin, fForceMax);

			/* After the clear, which clears the owner - see the same note in
			   ProcessIntegrityChecks. */
			m_VoxelGrid.SetParticleOwner(
				static_cast<uint32_t>(v3Position.x),
				static_cast<uint32_t>(v3Position.y),
				static_cast<uint32_t>(v3Position.z),
				(uintptr_t)pParticle);
		});

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
	std::vector<uint64_t> integrityChecks;
	SphericalDestruction::CollectSeeds(m_VoxelGrid, v3GridCenter, fRadius, integrityChecks);

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

	for (float z = -halfSizeA.z; z <= halfSizeA.z; z += halfSizeA.z * 2.f)
	{
		for (float y = -halfSizeA.y; y <= halfSizeA.y; y += halfSizeA.y * 2.f)
		{
			for (float x = -halfSizeA.x; x <= halfSizeA.x; x += halfSizeA.x * 2.f)
			{
				origin = { x,y,z };

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

				while( rayLenght > 0.f && RayCastGroup(pIgnoreCollider, potentialColliders, rayStart + origin, manifold.Normal, hitRes, rayLenght))
				{
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
								if (dist == 0.f)
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

	/* One batch for the tick's bakes. Built even when nothing lands, which
	   costs a handful of pointer copies - the alternative is a conditional
	   construction inside three levels of loop. */
	VoxelEditBatch batch(m_pRenderSystem->MakeEditTarget());

	Particle* aliveParticle = m_ParticlePool.GetLastAlive();

	GPUParticle* pGPUParticles = reinterpret_cast<GPUParticle*>(m_pGPUParticles->GetData());
	GPUParticle gpuParticle;

	while (aliveParticle != nullptr)
	{
		if (m_uiActiveParticleCount >= m_uiMaxParticleCount)
			break;

		++m_uiActiveParticleCount;

		// Simulate CPU particle
		Particle* nextParticle = aliveParticle->Prev;

		// Handle particle timer
		if (aliveParticle->Live.Timer > 0.f)
		{
			if (UpdateParticleTimer(aliveParticle, fDeltaTime))
			{
				aliveParticle = nextParticle;
				continue;
			}
		}

		Vector3& prevGridPos = aliveParticle->Live.GridPosition;
		Vector3 newGridPos = m_VoxelGrid.WorldToGrid(aliveParticle->Live.Position, true);

		//Clamp previous grid position when its distance exceeds 100 units
		if (glm::distance2(newGridPos, prevGridPos) > 10000)
		{
			prevGridPos = newGridPos;
			aliveParticle->Live.GridPosition = prevGridPos;
		}

		aliveParticle->Live.Velocity += PARTICLE_GRAVITY * fDeltaTime;
		aliveParticle->Live.Position += aliveParticle->Live.Velocity * fDeltaTime;

		newGridPos = m_VoxelGrid.WorldToGrid(aliveParticle->Live.Position, true);

		// Clamp y to zero to avoid particles being destroyed by to high velocities
		if (newGridPos.y < 0.f)
			newGridPos.y = 0.f;

		// Update the particles position if its positions has changed
		if (prevGridPos != newGridPos)
		{
			const VoxelCell cell = m_VoxelGrid.GetCell(
				static_cast<int>(newGridPos.x),
				static_cast<int>(newGridPos.y),
				static_cast<int>(newGridPos.z)
			);

			/* "The claim on my old voxel is mine, or there is none" - the same
			   test the raw UserPointer comparison made, asked of the sparse
			   particle map instead. Resolved where it is needed rather than up
			   front, because the bounce path below never asks and the map
			   lookup is the one part of this that is not a plain array index.
			   See RENDERING_PLAN.md phase 4d. */
			const VoxelCell oldCell = m_VoxelGrid.GetCell(
				static_cast<int>(prevGridPos.x),
				static_cast<int>(prevGridPos.y),
				static_cast<int>(prevGridPos.z)
			);

			auto ownsOldCell = [&oldCell, aliveParticle]()
			{
				return oldCell &&
					(oldCell.GetParticleOwner() == (uintptr_t)aliveParticle || !oldCell.HasOwner());
			};

			if (cell && cell.IsActive())
			{
				float speed = glm::length(aliveParticle->Live.Velocity);

				Vector3 normal = glm::normalize(prevGridPos - newGridPos);

				// Bake particle and destroy if velocity is to low or the particle is going straight down
				if (speed < PARTICLE_DESTROY_THRESHOLD || normal == Vector3(0, 1, 0) || prevGridPos.y < 0) //particleVelocity.y + 1.f <= 0.001f
				{
					// Only handle particle if its old voxel is still valid
					if (ownsOldCell())
					{
						/* Set previous voxel to default state if the particle shouldn't bake */
						if (!aliveParticle->Live.BakeOnImpact)
						{
							oldCell.ClearOwner();
						}
						/* Bake the particle into the grid */
						else if (aliveParticle->Live.BakeOnImpact)
						{
							oldCell.ClearOwner();

							Vector3 bakeVoxelPos = newGridPos;
							bakeVoxelPos.y += 1;

							Vector3 bakeCellPos = bakeVoxelPos;
							VoxelCell bakeCell = m_VoxelGrid.GetCell(
								static_cast<int>(bakeVoxelPos.x),
								static_cast<int>(bakeVoxelPos.y),
								static_cast<int>(bakeVoxelPos.z)
							);

							if (bakeCell && bakeCell.IsActive())
							{
								if (bakeVoxelPos.y > 1) 
									bakeVoxelPos.y -= 1;
								bakeCell = FindEmtpyNeighbor(bakeVoxelPos, bakeCellPos);
							}

							if (bakeCell && !bakeCell.IsActive())
							{
								/* bakeCellPos is where bakeCell actually is, which
								   is not bakeVoxelPos once FindEmtpyNeighbor has
								   answered - a pre-existing mismatch (ledger P5),
								   left alone here so this change stays about the
								   write. Phase 3 resolves the landing cell once
								   and this stops having two positions.

								   The batch writes the colour, the mapped word,
								   the occupancy bit, the brick count and the
								   owner - none, which is what the old
								   SetParticleOwner call amounted to, since
								   Live.UserPointer was never assigned - and
								   registers the loose voxel itself. Baked debris
								   belongs to no renderer, so without a registry
								   entry nothing submits an AABB proxy for it and
								   the voxel pass, which rasterizes proxies and
								   nothing else, only draws it when some
								   unrelated model's box happens to cover the
								   pixel. That is the flicker settled debris used
								   to have. */
								batch.Set(
									bakeVoxelPos,
									aliveParticle->Live.VoxelColor.inst.Color,
									VoxelOwnerVolume::k_uiNoOwnerSlot);
							}
						}
					}
					m_ParticlePool.DestroyParticle(aliveParticle);
				}
				else
				{
					/* Apply simple bounce physics to particle */
					float contactVel = glm::dot(aliveParticle->Live.Velocity, normal);
					if (contactVel < 0)
					{
						aliveParticle->Live.Velocity += -normal * contactVel * PARTICLE_BOUNCE_MULIPLIER;
					}
				}
			}
			else if (cell && !cell.IsActive())
			{
				if (ownsOldCell())
					oldCell.ClearOwner();

				/* Fill and Clear voxels based on the new grid position */
				aliveParticle->Live.GridPosition = newGridPos;
				cell.SetParticleOwner((uintptr_t)aliveParticle);
			}
			else
			{
				/* Clear voxels that reach out of bounds */
				if (ownsOldCell())
					oldCell.ClearOwner();

				m_ParticlePool.DestroyParticle(aliveParticle);
			}
		}

		// Create GPU particle
		gpuParticle.Position = aliveParticle->Live.Position;
		gpuParticle.VoxelColor = aliveParticle->Live.VoxelColor;

		std::memcpy(
			&pGPUParticles[m_uiActiveParticleCount - 1],
			&gpuParticle,
			sizeof(GPUParticle)
		);

		// Iterate to next particle
		aliveParticle = nextParticle;
	}
}

bool PhysicsSystem::UpdateParticleTimer(Particle* pParticle, float fDeltaTime)
{
	pParticle->Live.Timer -= fDeltaTime;
	if (pParticle->Live.Timer <= 0.f)
	{
		Vector3 prevGridPos = pParticle->Live.GridPosition;
		const VoxelCell oldCell = m_VoxelGrid.GetCell(
			static_cast<int>(prevGridPos.x),
			static_cast<int>(prevGridPos.y),
			static_cast<int>(prevGridPos.z)
		);

		if (oldCell && (oldCell.GetParticleOwner() == (uintptr_t)pParticle || !oldCell.HasOwner()))
		{
			oldCell.ClearOwner();
		}

		m_ParticlePool.DestroyParticle(pParticle);
		return true;
	}
	return false;
}

/* Reports where it found the cell as well as the cell itself: ownership no
   longer lives inside the voxel, so a caller that means to write an owner has
   to know the coordinates rather than just hold a pointer. */
VoxelCell PhysicsSystem::FindEmtpyNeighbor(Vector3 gridPos, Vector3& foundGridPos, uint32_t ySearchCount)
{
	for (int y = 0; y < static_cast<int>(ySearchCount); ++y)
	{
		for (int x = -1; x <= 1; ++x)
		{
			for (int z = -1; z <= 1; ++z)
			{
				if (x == 0 && z == 0) continue;

				Vector3 bakeVoxelPos = gridPos + Vector3(
					static_cast<float>(x),
					static_cast<float>(y),
					static_cast<float>(z));

				const VoxelCell bakeCell = m_VoxelGrid.GetCell(
					static_cast<int>(bakeVoxelPos.x),
					static_cast<int>(bakeVoxelPos.y),
					static_cast<int>(bakeVoxelPos.z)
				);

				if (bakeCell && !bakeCell.IsActive())
				{
					foundGridPos = bakeVoxelPos;
					return bakeCell;
				}
			}
		}
	}

	return VoxelCell();
}


