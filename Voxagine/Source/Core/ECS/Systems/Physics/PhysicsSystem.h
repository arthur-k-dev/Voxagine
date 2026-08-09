#pragma once
#include <unordered_map>
#include <deque>
#include "Core/ECS/ComponentSystem.h"
#include "Core/Particles/ParticleCore.h"
#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/ECS/Systems/Chunk/Chunk.h"
#include "Core/Utils/DeterministicRandom.h"

#define PHYSICS_EPSILON 0.000797

class PhysicsBody;
class BoxCollider;
struct VoxFrame;
struct Manifold;
struct HitResult;
class Box;
class RenderSystem;
class ParticleSystem;
class Mapper;

class PhysicsSystem : public ComponentSystem
{
public:
	friend class RenderSystem;
	friend class VoxelBaker;

	PhysicsSystem(World* pWorld, Vector3 gridSize, uint32_t voxelSize, uint32_t uiMaxParticles = 150000, UVector3 chunkSize = DEFAULT_CHUNK_SIZE);
	virtual ~PhysicsSystem();

	virtual void Start() override;

	virtual bool CanProcessComponent(Component* pComponent) override;

	virtual void FixedTick(const GameTimer& fixedTimer) override;
	virtual void PostTick(float fDeltaTime) override;
	
	// Performs a ray intersection test against all collider's in the scene
	// Takes a start point and a direction with a specific length
	// Specific collision layers can be tested against, default is all layers
	bool RayCast(Vector3 start, Vector3 dir, HitResult& hitResult, float fLength = FLT_MAX, uint32_t uiLayer = -1);
	bool RayCastSingle(const BoxCollider* pCollider, Vector3 start, Vector3 dir, HitResult& hitResult, float fLength = FLT_MAX, uint32_t uiLayer = -1);

	bool RayCastGroup(const BoxCollider* pIgnoreCollider, std::vector<BoxCollider*>& colliders, Vector3 start, Vector3 dir, HitResult& hitResult, float fLength = FLT_MAX, uint32_t uiLayer = -1);
	inline bool RayCastGroup(std::vector<BoxCollider*>& colliders, Vector3 start, Vector3 dir, HitResult& hitResult, float fLength = FLT_MAX, uint32_t uiLayer = -1);
	
	// Creates particles of static voxels in the radius of the sphere with a force applied from the center of the sphere
	void ApplySphericalDestruction(const Vector3& position, float fRadius, float fForceMin, float fForceMax, bool bBakeParticles = true);

	// Queries all colliders in the world against a given sphere
	// Fills the colliders vector with intersected colliders
	// Returns true if any collider was intersected
	bool OverlapSphere(std::vector<BoxCollider*>& colliders, Vector3 center, float fRadius, uint32_t uiLayer = -1, bool queryTriggers = false) const;

	// Queries all colliders in the world against a given bounding box
	// Fills the colliders vector with intersected colliders
	// Returns true if any collider was intersected
	bool OverlapBox(std::vector<BoxCollider*>& colliders, Vector3 center, Vector3 extends, uint32_t uiLayer = -1, bool queryTriggers = false) const;

	VoxelGrid* GetVoxelGrid() { return &m_VoxelGrid; }
	uint32_t GetParticlePoolSize() { return m_uiMaxParticleCount; }

	Mapper* GetGPUParticles() const { return m_pGPUParticles; }

	void SetRenderSystem(RenderSystem* pRenderSystem) { m_pRenderSystem = pRenderSystem; }

	/* DESTRUCTION_PLAN.md phase 0. Checks that the particle pool's sparse table
	   and its dense arrays agree in both directions. Reported alongside the
	   representation-sync audit; see the definition. */
	void AuditParticlePool() const;

	/* DESTRUCTION_PLAN.md phase 4's acceptance instrument, triggered by
	   VOXAGINE_INTEGRITY_AUDIT=<seconds>. Re-derives every voxel's
	   grounded/island classification with the exhaustive pre-phase-4 flood
	   fill and reports anything the memoised checker would answer differently.
	   See the definition. */
	void AuditIntegrity();

	uint32_t m_uiActiveParticleCount = 0;

	static const Vector3 PARTICLE_GRAVITY;

protected:
	virtual void OnComponentAdded(Component* pComponent) override;
	virtual void OnComponentDestroyed(Component* pComponent) override;

	void OnWorldPaused(World* pWorld);
	void OnWorldResumed(World* pWorld);

	void TickBodies(const GameTimer& fixedTimer);
	void TickParticleSystems(const GameTimer& fixedTimer);
	void ProcessIntegrityChecks();

	void ResolveContinousCollision(const float deltaTime);
	void AccumulateManifolds();
	void ResolveManifolds();

	void SolveVoxelPreciseCollision(Collider* pColliderA, const Collider* pColliderB);

	void HandleCallbacks();

	//Performs StepHeight check for physics bodies. Default StepCheck will be performed using the attached collider,
	//the function can be supplied with VoxModel in which case a voxel perfect check will be performed.
	//Voxel perfect StepHeight is currently not supported
	void StepCheck(PhysicsBody* pBody, const VoxFrame* pFrame = nullptr);

	bool ClampToBounds(Vector3& position, Collider* pCollider);
	bool IntersectRayAABB(const float* invRayDir, const float* rayStart, const float* boxMin, const float* boxMax, float& tNear);


	bool CheckContinousCollision(BoxCollider* pColliderA, Manifold& manifold, float deltaTime);

protected:
	// Particle simulation functions
	void SimulateParticles(float fDeltaTime);

private:
	VoxelGrid m_VoxelGrid;

	/* Set by RenderSystem's constructor, which is the only wiring there is -
	   nothing here initialised it, so a PhysicsSystem built without a
	   RenderSystem (the unit-test path) held a garbage pointer that the
	   destruction and bake paths dereference. Initialised here so it is at
	   least null; ledger D9 makes the wiring explicit in phase 2. */
	RenderSystem* m_pRenderSystem = nullptr;

	/* Owned by value and driven from FixedTick. It used to be a Job on a worker
	   thread holding a raw pointer to m_VoxelGrid above, which nothing joined
	   before this object was destroyed - see IntegrityChecker.h. */
	IntegrityChecker m_IntegrityChecker;

	/* Islands the checker has finished but that have not been turned into
	   debris yet, and how far into the front one we got. Converting is budgeted
	   per tick because an island is as large as the disconnected structure and
	   those grow as a level fragments - see ProcessIntegrityChecks. */
	std::deque<std::vector<uint64_t>> m_PendingIslands;
	size_t m_uiIslandCursor = 0;

	/* Every voxel of every island that has been found but not yet converted.
	 *
	 * Debris that settles on a doomed voxel is left floating the moment that
	 * voxel is cleared, and nothing will ever seed it: a burst seeds its
	 * neighbours, but conversion deliberately seeds nothing (see
	 * ProcessIntegrityChecks on why it cannot need to). Rather than repair that
	 * afterwards, a particle simply does not settle on something that is
	 * already falling - which is also what it would do. One hash lookup per
	 * impact, against a set that only exists while islands are pending. */
	std::unordered_set<uint64_t> m_PendingIslandVoxels;

	static constexpr uint32_t k_uiIntegrityConvertPerTick = 16384;

	Entity* m_pStaticEntityBody;
	PhysicsBody* m_pStaticBody;

	uint32_t m_uiMaxParticleCount = 0;
	Mapper* m_pGPUParticles = nullptr;

	/* Debris - destruction and falling islands. Its capacity is its budget:
	   the old code shared one running counter with the component particle
	   systems and consumed it in tick order, so a busy emitter starved debris
	   simulation entirely (ledger P14). Splitting the capacity means neither
	   source can take the other's, and rejection happens at spawn rather than
	   part-way through a walk (P13). */
	ParticleCore m_Debris;

	/* How much of the total goes to debris. Three quarters because debris is
	   what a destruction-heavy moment produces thousands of at once, while the
	   component emitters are authored per effect and bounded by their own
	   MaxParticles. */
	static constexpr float k_fDebrisBudgetShare = 0.75f;

	uint32_t m_uiEmitterBudget = 0;

	/* Debris velocities used to come from glm::linearRand, which draws on a
	   process-global engine that every other caller perturbs. Replaying a
	   destruction script therefore diverged on the first unrelated call
	   anywhere. Seeded per system, so the same call sequence gives the same
	   debris - see DESTRUCTION_PLAN.md phase 0. */
	DeterministicRandom m_ParticleRandom;

	std::vector<ParticleSystem*> m_ParticleSystems;
	std::vector<PhysicsBody*> m_Bodies;
	std::vector<BoxCollider*> m_Colliders;
	std::vector<Manifold> m_Manifolds;

	static const float PARTICLE_DESTROY_THRESHOLD;
	static const float PARTICLE_BOUNCE_MULIPLIER;
};