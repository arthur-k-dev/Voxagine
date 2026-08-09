#pragma once

#include <cstdint>
#include <vector>

#include "Core/Math.h"

/* One 16-byte record per drawn particle, written straight into the persistently
   mapped particle buffer. Lived in ParticleLinkedList.h until phase 3 deleted
   that; it describes the GPU contract, not the pool. */
struct GPUParticle
{
	Vector3 Position = Vector3(0.f);
	VColor VoxelColor = 0;
};

static_assert((sizeof(GPUParticle) % 16) == 0, "Particle not padded correctly");

/* A stable reference to a particle across frames.
 *
 * Swap-compaction moves the last live particle into a retired one's slot, so a
 * bare index is wrong the moment anything dies. The handle names a *slot* in a
 * sparse table that survives compaction, plus a generation that makes a stale
 * handle detectable rather than silently naming whoever reused the slot.
 *
 * Nothing in the engine holds one across frames today - deleting the claim
 * system is what removed the last consumer - but a spawner that wanted to track
 * its own debris would need exactly this, and building the pool without it
 * would mean building it twice. */
struct ParticleHandle
{
	uint32_t uiSlot = UINT32_MAX;
	uint32_t uiGeneration = 0;

	bool IsValid() const { return uiSlot != UINT32_MAX; }
};

struct ParticleSpawn
{
	/* Level space, not window space. A window slide changes what a grid
	   coordinate means, and the old pool cached one per particle - so a slide
	   under 100 units (the teleport clamp) left every particle in flight
	   claiming and releasing the wrong cells (ledger P9). Grid position is
	   derived from this each tick instead of stored. */
	Vector3 v3Position = Vector3(0.f);
	Vector3 v3Velocity = Vector3(0.f);

	uint32_t uiColor = 0;

	/* Debris bakes itself into the world where it lands; effect particles just
	   disappear. */
	bool bBakeOnImpact = true;

	/* Seconds until it retires on its own, or a negative for "never". The old
	   pool had this field and never set it to anything but the sentinel, so the
	   whole timer path was dead code (ledger M3); it is live here because the
	   component emitters need it. */
	float fTimer = -1.f;
};

/* The particle pool: structure of arrays, swap-compacted, generation-tagged.
 *
 * DESTRUCTION_PLAN.md phase 3. What it replaces is `ParticleLinkedList`, a
 * fixed vector of `Particle` with an intrusive free list and an intrusive alive
 * doubly-linked list, where `Particle` was a *union* of the live state and the
 * free-list link. Five ledger entries came from that shape and all five are
 * structural rather than incidental:
 *
 * - The GPU record was written after DestroyParticle, so a retired particle -
 *   whose free-list link now aliased its own position - was rendered one extra
 *   frame from whatever the link happened to look like as a float3 (P1).
 * - DestroyParticle had no double-free guard and no aliveness flag, so a second
 *   call cycled the free list back onto itself (P2).
 * - Its head/tail repair was four asymmetric branches (P3).
 * - Constructing with zero capacity indexed an empty vector and underflowed the
 *   loop bound (P11).
 * - Simulation walked it oldest-first by pointer chase, so hitting the cap
 *   starved the *newest* particles - the ones just spawned by the explosion the
 *   player is looking at (P13).
 *
 * Here, alive is "index < GetCount()", retiring is a swap and a decrement, the
 * arrays are contiguous, and a zero capacity is simply an empty pool.
 */
class ParticleCore
{
public:
	void Create(uint32_t uiCapacity);

	/* Retires everything. Used on world pause and teardown, where the old pool
	   was left holding particles whose grid positions referred to a world that
	   no longer existed (ledger P10). */
	void Clear();

	uint32_t GetCapacity() const { return m_uiCapacity; }
	uint32_t GetCount() const { return m_uiCount; }
	bool IsFull() const { return m_uiCount >= m_uiCapacity; }

	/* Returns an invalid handle when full. Rejection happens here, once, at
	   spawn - never mid-simulation, which is what made the old cap starve
	   whichever source ran last (P13, P14). */
	ParticleHandle Spawn(const ParticleSpawn& spawn);

	bool IsAlive(const ParticleHandle& handle) const;

	/* Dense index of a live handle, or UINT32_MAX. */
	uint32_t Resolve(const ParticleHandle& handle) const;

	/* Retires the particle at a dense index. The last live particle moves into
	   that index, so a forward walk must **not** advance after calling this. */
	void Retire(uint32_t uiIndex);

	/* Parallel arrays; entries [0, GetCount()) are alive. Public because the
	   simulation is a loop over them and an accessor per field per particle is
	   the one cost this layout exists to avoid. */
	std::vector<Vector3> Position;
	std::vector<Vector3> Velocity;
	std::vector<uint32_t> Color;
	std::vector<float> Timer;
	std::vector<uint8_t> BakeOnImpact;

	/* What the pool says about itself, for VOXAGINE_SYNC_AUDIT. The invariant
	   is that the sparse table and the dense arrays agree in both directions
	   for every live particle and that no slot is claimed twice. */
	struct AuditResult
	{
		uint32_t uiCount = 0;
		uint32_t uiCapacity = 0;
		uint32_t uiBrokenMappings = 0;
		uint32_t uiDuplicateSlots = 0;
		uint32_t uiFreeListSize = 0;

		bool IsSound() const
		{
			return uiBrokenMappings == 0 && uiDuplicateSlots == 0 &&
				uiCount + uiFreeListSize == uiCapacity;
		}
	};

	AuditResult Audit() const;

private:
	uint32_t m_uiCapacity = 0;
	uint32_t m_uiCount = 0;

	/* slot -> dense index, dense index -> slot, and the generation of whatever
	   currently occupies each slot. */
	std::vector<uint32_t> m_SlotToDense;
	std::vector<uint32_t> m_DenseToSlot;
	std::vector<uint32_t> m_SlotGeneration;

	/* Slots not currently held by a live particle. A plain stack: order does
	   not matter and nothing is intrusive, so there is no aliasing to get
	   wrong. */
	std::vector<uint32_t> m_FreeSlots;
};
