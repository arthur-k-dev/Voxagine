#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/Math.h"
#include "Core/Particles/ParticleCore.h"
#include "Framework/Benchmark.h"
#include "Harness/VoxelWorldHarness.h"

/* One scripted explosion. */
struct Burst
{
	uint32_t uiTick = 0;
	Vector3 v3Center = Vector3(0.f);
	float fRadius = 0.f;
};

/* Debris dropped in from above at a chosen tick, bypassing destruction.
   The only way to measure the particle simulation at a *chosen* count rather
   than at whatever an explosion happens to produce - rule 11's "sweep the axis
   the optimization claims to attack". */
struct Injection
{
	uint32_t uiTick = 0;
	uint32_t uiCount = 0;
};

struct DestructionConfig
{
	UVector3 v3Size = UVector3(96, 64, 96);
	UVector3 v3ChunkSize = UVector3(32, 64, 32);

	uint32_t uiTicks = 320;
	uint64_t uiSeed = 1;

	uint32_t uiDebrisCapacity = 32768;
	uint32_t uiConvertBudget = 16384;
	uint32_t uiVisitBudget = IntegrityChecker::VISIT_BUDGET_PER_TICK;

	/* One debris particle per this many destroyed voxels.
	 *
	 * The engine uses 4: an explosion scatters, so its density is cosmetic
	 * there. Scenarios default to 1 deliberately - debris is what the
	 * floating-debris invariant is looking for, so four times as much of it is
	 * four times the chance of catching a case where a particle settles on
	 * something that is about to stop existing. Benchmarks set 4, because a
	 * cost measurement has to match what actually ships. */
	uint32_t uiSpawnEveryNth = 1;

	/* Debris dropped in directly, for the particle-cost sweep. */
	std::vector<Injection> injections;

	/* The oracle walks every voxel of the window with no memo and no budget, so
	   it costs seconds. Scenarios need it; benchmarks are measuring something
	   else and turn it off. */
	bool bClassifyStanding = true;

	/* Per-phase stopwatches. Off for scenarios so their run is not paying for
	   timing calls it never reads. */
	bool bMeasure = false;
};

/* Everything a run produced, which is what invariants and benchmarks are
   written against. Deliberately plain data: an invariant should not be able to
   reach into the world and ask a different question than the one it declares.
 */
struct DestructionResult
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

	uint64_t uiBuilt = 0;
	uint64_t uiDestroyed = 0;
	uint64_t uiConverted = 0;
	uint64_t uiBaked = 0;
	uint64_t uiRemaining = 0;

	/* What the integrity checker did, exactly, and reproducibly. These are the
	   metrics the perf suite gates on - see Framework/Benchmark.h. */
	IntegrityChecker::Stats checker;
	uint64_t uiIslandsFound = 0;

	/* Only filled when DestructionConfig::bMeasure is set. */
	PhaseTimer burstMs;
	PhaseTimer integrityMs;
	PhaseTimer convertMs;
	PhaseTimer particleMs;

	/* Integrity cost per quarter of the run - the #27 curve, which is the shape
	   that says whether the checker gets slower the more has been destroyed.
	   Flat is the goal. */
	std::vector<double> integrityQuarterMs;

	/* Particle cost against the number alive, for the sim sweep. One entry per
	   tick that simulated anything. */
	std::vector<std::pair<uint32_t, double>> simSamples;

	uint32_t uiPeakAlive = 0;
};

/* The destruction pipeline, driven once, in one place.
 *
 * This is the part that must not be duplicated. A scenario describes a world
 * and a script; a benchmark describes a world, a script and what to time.
 * Neither gets to reimplement bursts, debris, integrity or conversion, because
 * a test whose driver differs from the engine's tests the driver instead of the
 * engine. Same argument as extracting SphericalDestruction::Apply out of
 * PhysicsSystem in phase 2 - the scenarios, the benchmarks and the game now run
 * the same code.
 *
 * The gauntlet used to have its own copy of this loop, which is exactly the
 * duplication being removed: the two had already drifted apart on the
 * destructibility predicate and the debris density.
 */
class DestructionRun
{
public:
	/* The owner slot that refuses destruction. One is enough: what is under
	   test is that the predicate is consulted on every path. */
	static constexpr uint16_t k_uiProtectedSlot = 900;

	explicit DestructionRun(const DestructionConfig& config);

	VoxelWorldHarness& Harness() { return m_World; }
	const DestructionConfig& Config() const { return m_Config; }

	/* Runs the script to completion and fills in everything but the repeat
	   hash, which the runner supplies from a second identical run. */
	DestructionResult Run(const std::vector<Burst>& bursts);

	/* Voxels the exhaustive walk calls ungrounded, right now. The oracle. */
	void ClassifyStanding(uint64_t& o_uiVoxels, uint64_t& o_uiComponents,
	                      std::vector<Vector3>* pSamples = nullptr,
	                      const std::unordered_set<uint64_t>* pIgnore = nullptr) const;

	/* Every ungrounded voxel, for the "was it already like this" comparison. */
	void CollectStanding(std::unordered_set<uint64_t>& o_standing) const;

private:
	DestructionConfig m_Config;
	VoxelWorldHarness m_World;
	IntegrityChecker m_Checker;
	ParticleCore m_Debris;
};
