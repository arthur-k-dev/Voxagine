#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/Math.h"
#include "Core/Particles/ParticleCore.h"
#include "Harness/VoxelWorldHarness.h"

/* One scripted explosion. */
struct Burst
{
	uint32_t uiTick = 0;
	Vector3 v3Center = Vector3(0.f);
	float fRadius = 0.f;
};

struct SelftestConfig
{
	UVector3 v3Size = UVector3(96, 64, 96);
	UVector3 v3ChunkSize = UVector3(32, 64, 32);

	uint32_t uiTicks = 320;
	uint64_t uiSeed = 1;

	uint32_t uiDebrisCapacity = 32768;
	uint32_t uiConvertBudget = 16384;
};

/* Everything a run produced, which is what invariants are written against.
   Deliberately plain data: an invariant should not be able to reach into the
   world and ask a different question than the one it declares. */
struct SelftestResult
{
	uint64_t uiHash = 0;
	uint64_t uiRepeatHash = 0;

	uint32_t uiRepresentation = 0;
	bool bPoolSound = true;

	/* From the exhaustive oracle, before and after. */
	uint64_t uiStandingAtStart = 0;
	uint64_t uiStandingComponentsAtStart = 0;
	uint64_t uiStanding = 0;
	uint64_t uiStandingComponents = 0;

	/* A few of the offending positions, and whether each was already standing
	   before the run. A count alone tells you an invariant broke; this tells
	   you where to look, which is the difference between a failing test and a
	   useful one. */
	std::vector<Vector3> standingSamples;

	/* Whatever the scenario says must still be standing at the end. */
	uint64_t uiMinStanding = 0;

	uint64_t uiProtectedAtStart = 0;
	uint64_t uiProtectedCleared = 0;

	uint64_t uiDestroyed = 0;
	uint64_t uiConverted = 0;
	uint64_t uiBaked = 0;
};

/* The destruction pipeline, driven once, in one place.
 *
 * This is the part that must not be duplicated. A scenario describes a world
 * and a script; it does not get to reimplement bursts, debris, integrity or
 * conversion, because a selftest whose driver differs from the engine's tests
 * the driver instead of the engine. Same argument as extracting
 * SphericalDestruction::Apply out of PhysicsSystem in phase 2 - the scenarios
 * and the game now run the same code.
 */
class SelftestWorld
{
public:
	/* The owner slot that refuses destruction. One is enough: what is under
	   test is that the predicate is consulted on every path. */
	static constexpr uint16_t k_uiProtectedSlot = 900;

	explicit SelftestWorld(const SelftestConfig& config);

	VoxelWorldHarness& Harness() { return m_World; }
	const SelftestConfig& Config() const { return m_Config; }

	/* Runs the script to completion and fills in everything but the repeat
	   hash, which the runner supplies from a second identical run. */
	SelftestResult Run(const std::vector<Burst>& bursts);

	/* Voxels the exhaustive walk calls ungrounded, right now. The oracle. */
	void ClassifyStanding(uint64_t& o_uiVoxels, uint64_t& o_uiComponents,
	                      std::vector<Vector3>* pSamples = nullptr,
	                      const std::unordered_set<uint64_t>* pIgnore = nullptr) const;

	/* Every ungrounded voxel, for the "was it already like this" comparison. */
	void CollectStanding(std::unordered_set<uint64_t>& o_standing) const;

private:
	SelftestConfig m_Config;
	VoxelWorldHarness m_World;
	IntegrityChecker m_Checker;
	ParticleCore m_Debris;
};
