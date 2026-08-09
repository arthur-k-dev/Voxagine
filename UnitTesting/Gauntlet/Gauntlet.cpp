/* The deterministic destruction gauntlet. DESTRUCTION_PLAN.md phase 0.
 *
 * Fires a fixed sequence of explosions at a procedurally-built level, runs the
 * integrity checker and island conversion for a fixed number of ticks, and
 * prints per-phase timings and a state hash over every representation.
 *
 * It runs against VoxelWorldHarness rather than against the running game, and
 * that is the whole reason it can be deterministic. The game cannot: the level
 * is full of entities with their own randomness, the camera slides the window,
 * chunk streaming runs on job threads, and the frame rate decides how many
 * fixed ticks a given second contains. Here the tick count is the tick count.
 *
 * What it therefore does *not* cover is exactly that list, which is why the
 * in-game audits (VOXAGINE_SYNC_AUDIT, VOXAGINE_COVERAGE_AUDIT,
 * VOXAGINE_VOXEL_AUDIT) stay: they are the integration half and this is the
 * algorithmic half.
 *
 *   voxagine_gauntlet [script]
 *
 * With no script it runs the built-in one, which is what the baseline table in
 * DESTRUCTION_PLAN.md was measured with. Script commands are one per line:
 *
 *   size    <x> <y> <z>          window size in voxels
 *   chunk   <x> <y> <z>          chunk size (x/z divide the window)
 *   seed    <n>                  particle RNG seed
 *   level   <towers|slab>        which generator to build
 *   ticks   <n>                  fixed ticks to run
 *   budget  <visits> <convert>   integrity visits and island voxels per tick
 *   particles <n>                debris pool capacity
 *   spawn   <tick> <n>           spawn n debris at once, for the sim sweep
 *   explode <tick> <x> <y> <z> <radius>
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <unordered_set>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/Particles/ParticleCore.h"
#include "Core/Particles/ParticleSimulation.h"
#include "Core/Utils/DeterministicRandom.h"
#include "Core/Voxels/SphericalDestruction.h"
#include "Core/Voxels/VoxelEditBatch.h"
#include "Harness/VoxelWorldHarness.h"

namespace
{
	const uint32_t k_uiGround = 0xFF404040u;
	const uint32_t k_uiTower = 0xFF906030u;

	/* Towers wide and tall enough that severing one leaves an island of a few
	   thousand voxels. That size is the point: the #27 pathology is that the
	   checker has no memoisation between seeds, so a large island is walked
	   once per seed that lands on it, and an island of a few hundred voxels
	   does not show it. */
	const uint32_t k_uiTowerWidth = 12;
	const uint32_t k_uiTowerHeight = 56;

	struct Explosion
	{
		uint32_t uiTick = 0;
		Vector3 v3Center = Vector3(0.f);
		float fRadius = 0.f;
	};

	struct Script
	{
		UVector3 v3Size = UVector3(192, 96, 192);
		UVector3 v3ChunkSize = UVector3(64, 96, 64);

		uint64_t uiSeed = 20260809;
		std::string level = "towers";

		uint32_t uiTicks = 240;
		uint32_t uiVisitBudget = IntegrityChecker::VISIT_BUDGET_PER_TICK;
		uint32_t uiConvertBudget = 16384;

		/* Debris capacity. Also the phase 6 gate's independent variable: sweep
		   it with `particles <n>` and watch the sim row. */
		uint32_t uiParticleBudget = 112500;

		std::vector<Explosion> explosions;

		/* (tick, count) pairs. A direct injection of debris, for measuring the
		   simulation at a chosen particle count rather than at whatever an
		   explosion happens to produce - which is what phase 6's gate needs
		   (rule 11: sweep the axis the optimization claims to attack). */
		std::vector<std::pair<uint32_t, uint32_t>> spawns;
	};

	/* Total and peak, the same two numbers FrameProfiler keeps and for the same
	   reason: a one-off cost buried in an average of 240 ticks says nothing. */
	class Phase
	{
	public:
		explicit Phase(const char* pName) : m_pName(pName) {}

		void Add(double fMs)
		{
			m_fTotalMs += fMs;
			m_fPeakMs = std::max(m_fPeakMs, fMs);
			++m_uiSamples;
		}

		void Report() const
		{
			if (m_uiSamples == 0)
			{
				std::printf("  %-34s   (never ran)\n", m_pName);
				return;
			}

			std::printf("  %-34s total %9.2f ms / %6u  avg %8.4f  peak %9.3f\n",
			            m_pName, m_fTotalMs, m_uiSamples, m_fTotalMs / m_uiSamples, m_fPeakMs);
		}

	private:
		const char* m_pName;
		double m_fTotalMs = 0.0;
		double m_fPeakMs = 0.0;
		uint32_t m_uiSamples = 0;
	};

	class Stopwatch
	{
	public:
		Stopwatch() : m_Start(std::chrono::high_resolution_clock::now()) {}

		double Milliseconds() const
		{
			const std::chrono::duration<double, std::milli> span =
				std::chrono::high_resolution_clock::now() - m_Start;

			return span.count();
		}

	private:
		std::chrono::high_resolution_clock::time_point m_Start;
	};

	/* Where BuildLevel puts towers, so the built-in script can aim at them
	   without either half hard-coding the other's arithmetic. */
	std::vector<UVector3> TowerOrigins(const UVector3& v3Size)
	{
		std::vector<UVector3> origins;

		for (uint32_t uiZ = 24; uiZ + k_uiTowerWidth < v3Size.z - 8; uiZ += 40)
		{
			for (uint32_t uiX = 24; uiX + k_uiTowerWidth < v3Size.x - 8; uiX += 32)
				origins.push_back(UVector3(uiX, 1, uiZ));
		}

		return origins;
	}

	/* The built-in script. Three waves down the same towers, each aimed at a
	   tower's centre so the sphere severs its whole cross-section - an
	   explosion offset to a corner leaves the far corners standing and the
	   tower never becomes an island, which is realistic and measures nothing.
	 *
	 * Waves rather than one pass because the cost curve the plan cares about
	 * (#27: integrity getting slower the more has been destroyed) only appears
	 * once destruction overlaps destruction. The first wave cuts each tower and
	 * drops its top; the later ones work over the stump and the debris.
	 */
	Script BuiltInScript()
	{
		Script script;

		const std::vector<UVector3> origins = TowerOrigins(script.v3Size);
		const float fWaveHeights[] = { 40.f, 20.f, 8.f };

		uint32_t uiTick = 8;

		for (float fHeight : fWaveHeights)
		{
			for (const UVector3& v3Origin : origins)
			{
				Explosion explosion;
				explosion.uiTick = uiTick;
				explosion.v3Center = Vector3(
					v3Origin.x + k_uiTowerWidth * 0.5f,
					fHeight,
					v3Origin.z + k_uiTowerWidth * 0.5f);
				explosion.fRadius = 10.f;

				script.explosions.push_back(explosion);

				uiTick += 4;
			}

			/* A gap between waves so the checker and the conversion budget have
			   time to drain - otherwise the run measures the backlog rather
			   than the work. */
			uiTick += 40;
		}

		script.uiTicks = uiTick + 160;

		return script;
	}

	bool LoadScript(const char* pPath, Script& script)
	{
		std::ifstream file(pPath);

		if (!file.is_open())
		{
			std::fprintf(stderr, "[gauntlet] cannot open script '%s'\n", pPath);
			return false;
		}

		script.explosions.clear();

		std::string line;
		uint32_t uiLine = 0;

		while (std::getline(file, line))
		{
			++uiLine;

			const size_t uiComment = line.find('#');

			if (uiComment != std::string::npos)
				line.erase(uiComment);

			std::istringstream stream(line);
			std::string command;

			if (!(stream >> command))
				continue;

			if (command == "size")
				stream >> script.v3Size.x >> script.v3Size.y >> script.v3Size.z;
			else if (command == "chunk")
				stream >> script.v3ChunkSize.x >> script.v3ChunkSize.y >> script.v3ChunkSize.z;
			else if (command == "seed")
				stream >> script.uiSeed;
			else if (command == "level")
				stream >> script.level;
			else if (command == "ticks")
				stream >> script.uiTicks;
			else if (command == "budget")
				stream >> script.uiVisitBudget >> script.uiConvertBudget;
			else if (command == "particles")
				stream >> script.uiParticleBudget;
			else if (command == "spawn")
			{
				uint32_t uiTick = 0;
				uint32_t uiCount = 0;
				stream >> uiTick >> uiCount;
				script.spawns.emplace_back(uiTick, uiCount);
			}
			else if (command == "explode")
			{
				Explosion explosion;
				stream >> explosion.uiTick
				       >> explosion.v3Center.x >> explosion.v3Center.y >> explosion.v3Center.z
				       >> explosion.fRadius;
				script.explosions.push_back(explosion);
			}
			else
			{
				std::fprintf(stderr, "[gauntlet] %s:%u: unknown command '%s'\n", pPath, uiLine, command.c_str());
				return false;
			}

			if (stream.fail() && !stream.eof())
			{
				std::fprintf(stderr, "[gauntlet] %s:%u: malformed '%s'\n", pPath, uiLine, command.c_str());
				return false;
			}
		}

		/* Explosions must fire in tick order, and a script author should not
		   have to care. Stable so two entries on the same tick keep their
		   written order - which the hash depends on. */
		std::stable_sort(script.explosions.begin(), script.explosions.end(),
			[](const Explosion& a, const Explosion& b) { return a.uiTick < b.uiTick; });

		return true;
	}

	void BuildLevel(VoxelWorldHarness& world, const Script& script)
	{
		world.FillGround(k_uiGround);

		if (script.level == "slab")
		{
			world.FillBox(UVector3(8, 1, 8), UVector3(script.v3Size.x - 16, k_uiTowerHeight, script.v3Size.z - 16),
			              k_uiTower, 1);
			return;
		}

		/* Towers on a lattice, each its own owner slot so the owner half of the
		   hash carries information rather than being uniformly zero. Deliberately
		   *not* touching the ground row: a tower resting directly on y = 1 is
		   grounded, and blowing its base out is what produces an island. */
		uint16_t uiSlot = 1;

		for (const UVector3& v3Origin : TowerOrigins(script.v3Size))
		{
			world.FillBox(v3Origin, UVector3(k_uiTowerWidth, k_uiTowerHeight, k_uiTowerWidth), k_uiTower, uiSlot);
			++uiSlot;
		}
	}

	/* Drives the engine's own explosion, not a copy of it.
	 *
	 * The phase 0 gauntlet had a reference implementation here because the real
	 * one was a method on PhysicsSystem and needed a World. Phase 2 pulled the
	 * loop out into SphericalDestruction::Apply with the gameplay-shaped parts
	 * as callbacks, so this now measures and hashes the shipping algorithm.
	 */
	uint32_t Explode(VoxelWorldHarness& world, VoxelEditBatch& batch, ParticleCore& debris,
	                 const Explosion& explosion, DeterministicRandom& random)
	{
		uint32_t uiSpawnCounter = 0;

		const SphericalDestruction::Result result = SphericalDestruction::Apply(
			batch, world.Grid(), explosion.v3Center, explosion.fRadius,
			[](uint16_t) { return true; },
			[&](const Vector3& v3Position, uint32_t uiColor)
			{
				/* One particle per four destroyed voxels, matching the engine.
				   The counter advances per destroyed voxel whether or not the
				   pool had room, so density stays even. */
				const bool bSpawn = (uiSpawnCounter % 4) == 0;
				++uiSpawnCounter;

				if (!bSpawn)
					return;

				Vector3 v3Away = v3Position - explosion.v3Center;
				const float fLengthSquared = glm::length2(v3Away);

				v3Away = fLengthSquared > 0.f
					? v3Away / std::sqrt(fLengthSquared)
					: Vector3(0.f, 1.f, 0.f);

				ParticleSpawn spawn;
				spawn.v3Position = v3Position;
				spawn.v3Velocity = v3Away * random.Range(20.f, 60.f);
				spawn.uiColor = uiColor;
				spawn.bBakeOnImpact = true;

				debris.Spawn(spawn);
			});

		return result.uiDestroyed;
	}

	/* The engine's own per-particle step, driven here. Returns how many
	   particles were still alive afterwards. */
	uint32_t SimulateDebris(VoxelWorldHarness& world, ParticleCore& debris, float fDeltaTime,
	                        uint64_t& io_uiBaked)
	{
		VoxelEditBatch batch(world.MakeEditTarget());

		const ParticleSimulation::Settings settings;
		const UVector3 v3WindowSize = world.Size();
		const Vector3 v3WorldOffset = world.Grid().GetWorldOffset();

		uint32_t uiIndex = 0;

		while (uiIndex < debris.GetCount())
		{
			const ParticleSimulation::Outcome outcome = ParticleSimulation::Step(
				debris, uiIndex, fDeltaTime, world.Bricks(), v3WindowSize, v3WorldOffset, settings);

			if (outcome.bBake)
			{
				batch.Set(
					Vector3(static_cast<float>(outcome.iBakeX),
					        static_cast<float>(outcome.iBakeY),
					        static_cast<float>(outcome.iBakeZ)),
					debris.Color[uiIndex],
					VoxelOwnerVolume::k_uiNoOwnerSlot);

				++io_uiBaked;
			}

			if (outcome.bRetire)
			{
				debris.Retire(uiIndex);
				continue;
			}

			++uiIndex;
		}

		return debris.GetCount();
	}
}

int main(int argc, char* argv[])
{
	Script script = BuiltInScript();

	if (argc > 1 && !LoadScript(argv[1], script))
		return 1;

	if (script.v3Size.x % script.v3ChunkSize.x != 0 || script.v3Size.z % script.v3ChunkSize.z != 0)
	{
		std::fprintf(stderr, "[gauntlet] the chunk size must divide the window in x and z\n");
		return 1;
	}

	std::printf("[gauntlet] %ux%ux%u window, %ux%ux%u chunks, level '%s', %u ticks, seed %llu, %zu explosions\n",
	            script.v3Size.x, script.v3Size.y, script.v3Size.z,
	            script.v3ChunkSize.x, script.v3ChunkSize.y, script.v3ChunkSize.z,
	            script.level.c_str(), script.uiTicks,
	            static_cast<unsigned long long>(script.uiSeed), script.explosions.size());

	Phase buildPhase("level build");
	Phase explodePhase("explosion burst");
	Phase integrityPhase("integrity flood fill");
	Phase convertPhase("island conversion");
	Phase particlePhase("particle simulation");

	VoxelWorldHarness world(script.v3Size, script.v3ChunkSize);

	{
		const Stopwatch watch;
		BuildLevel(world, script);
		buildPhase.Add(watch.Milliseconds());
	}

	const uint64_t uiBuiltVoxels = world.CountOccupied();

	DeterministicRandom random(script.uiSeed);

	ParticleCore debris;
	debris.Create(script.uiParticleBudget);

	uint64_t uiBaked = 0;
	uint32_t uiPeakAlive = 0;

	/* Sim cost against the number alive, for the phase 6 gate. One row per
	   tick that simulated anything, bucketed by count. */
	struct SimSample
	{
		uint32_t uiAlive = 0;
		double fMilliseconds = 0.0;
	};

	std::vector<SimSample> simSamples;

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	std::deque<std::vector<uint64_t>> pendingIslands;
	size_t uiIslandCursor = 0;

	size_t uiNextExplosion = 0;
	uint64_t uiDestroyed = 0;
	uint64_t uiConverted = 0;
	uint64_t uiIslandsFound = 0;

	/* The #27 curve: how integrity cost moves as cumulative destruction grows.
	   Recorded per quarter of the run so the trend is visible without a
	   per-tick dump. */
	const uint32_t uiSegments = 4;
	std::vector<double> segmentIntegrityMs(uiSegments, 0.0);

	for (uint32_t uiTick = 0; uiTick < script.uiTicks; ++uiTick)
	{
		const uint32_t uiSegment = std::min(uiSegments - 1, uiTick * uiSegments / std::max(1u, script.uiTicks));

		std::vector<uint64_t> seeds;

		while (uiNextExplosion < script.explosions.size() &&
		       script.explosions[uiNextExplosion].uiTick == uiTick)
		{
			const Stopwatch watch;

			VoxelEditBatch batch(world.MakeEditTarget());
			const Explosion& explosion = script.explosions[uiNextExplosion];

			uiDestroyed += Explode(world, batch, debris, explosion, random);
			SphericalDestruction::CollectSeeds(world.Grid(), explosion.v3Center, explosion.fRadius, seeds);

			explodePhase.Add(watch.Milliseconds());

			++uiNextExplosion;
		}

		if (!seeds.empty())
			checker.EnqueueBulk(seeds);

		/* Direct injections, high above the level so they fall a long way and
		   the count stays where it was put for many ticks. */
		for (const std::pair<uint32_t, uint32_t>& spawn : script.spawns)
		{
			if (spawn.first != uiTick)
				continue;

			for (uint32_t i = 0; i < spawn.second; ++i)
			{
				ParticleSpawn particle;
				particle.v3Position = Vector3(
					random.Range(1.f, static_cast<float>(script.v3Size.x) - 2.f),
					random.Range(static_cast<float>(script.v3Size.y) * 0.6f,
					             static_cast<float>(script.v3Size.y) - 2.f),
					random.Range(1.f, static_cast<float>(script.v3Size.z) - 2.f));
				particle.v3Velocity = Vector3(
					random.Range(-4.f, 4.f), random.Range(-2.f, 2.f), random.Range(-4.f, 4.f));
				particle.uiColor = 0xFFC0C0C0u;
				particle.bBakeOnImpact = false;

				debris.Spawn(particle);
			}
		}

		/* The fixed step the engine runs at. */
		if (debris.GetCount() > 0)
		{
			const uint32_t uiBefore = debris.GetCount();

			const Stopwatch watch;
			const uint32_t uiAlive = SimulateDebris(world, debris, 1.f / 60.f, uiBaked);
			const double fMilliseconds = watch.Milliseconds();

			particlePhase.Add(fMilliseconds);

			SimSample sample;
			sample.uiAlive = uiBefore;
			sample.fMilliseconds = fMilliseconds;
			simSamples.push_back(sample);

			uiPeakAlive = std::max(uiPeakAlive, uiAlive);
		}

		{
			const Stopwatch watch;

			std::vector<std::vector<uint64_t>> islands;
			checker.Process(script.uiVisitBudget, islands);

			const double fMs = watch.Milliseconds();

			integrityPhase.Add(fMs);
			segmentIntegrityMs[uiSegment] += fMs;

			uiIslandsFound += islands.size();

			for (std::vector<uint64_t>& island : islands)
				pendingIslands.push_back(std::move(island));
		}

		if (pendingIslands.empty())
			continue;

		const Stopwatch watch;

		VoxelEditBatch convertBatch(world.MakeEditTarget());
		uint32_t uiBudget = script.uiConvertBudget;

		while (uiBudget > 0 && !pendingIslands.empty())
		{
			std::vector<uint64_t>& island = pendingIslands.front();

			for (; uiIslandCursor < island.size() && uiBudget > 0; ++uiIslandCursor)
			{
				--uiBudget;

				const Vector3 v3Position = IntegrityChecker::HashToPosition(island[uiIslandCursor]);

				const VoxelCell cell = world.Grid().GetCell(
					static_cast<uint32_t>(v3Position.x),
					static_cast<uint32_t>(v3Position.y),
					static_cast<uint32_t>(v3Position.z));

				if (!cell || !cell.IsActive())
					continue;

				(void)random.Range(0.f, 1.f);

				convertBatch.Clear(v3Position);

				++uiConverted;
			}

			if (uiIslandCursor >= island.size())
			{
				pendingIslands.pop_front();
				uiIslandCursor = 0;
			}
		}

		convertPhase.Add(watch.Milliseconds());
	}

	std::printf("\n[gauntlet] timings\n");
	buildPhase.Report();
	explodePhase.Report();
	integrityPhase.Report();
	convertPhase.Report();
	particlePhase.Report();

	std::printf("\n[gauntlet] integrity cost by quarter of the run (the #27 curve; flat is the goal)\n  ");

	for (uint32_t uiSegment = 0; uiSegment < uiSegments; ++uiSegment)
		std::printf("%9.2f ms", segmentIntegrityMs[uiSegment]);

	std::printf("\n[gauntlet] debris: %u pool, %u peak alive, %llu baked back into the world\n",
	            script.uiParticleBudget, uiPeakAlive, static_cast<unsigned long long>(uiBaked));

	if (!simSamples.empty())
	{
		/* Cost per particle per tick, which is the number phase 6's gate is
		   about: if it stays flat as the count grows, the CPU sim scales and
		   the gate stays shut. Reported over the busiest ticks, because a tick
		   with eleven particles in it measures loop overhead rather than the
		   per-particle cost. */
		std::sort(simSamples.begin(), simSamples.end(),
			[](const SimSample& a, const SimSample& b) { return a.uiAlive > b.uiAlive; });

		const size_t uiTop = std::min<size_t>(simSamples.size(), 32);

		double fTotalMs = 0.0;
		uint64_t uiTotalParticles = 0;

		for (size_t i = 0; i < uiTop; ++i)
		{
			fTotalMs += simSamples[i].fMilliseconds;
			uiTotalParticles += simSamples[i].uiAlive;
		}

		std::printf("[gauntlet] sim: busiest tick %u alive in %.3f ms; over the busiest %zu ticks "
		            "%.1f ns per particle per tick\n",
		            simSamples[0].uiAlive, simSamples[0].fMilliseconds, uiTop,
		            uiTotalParticles > 0 ? (fTotalMs * 1000000.0) / static_cast<double>(uiTotalParticles) : 0.0);
	}

	std::printf("\n[gauntlet] %llu voxels built, %llu destroyed by explosions, %llu converted from %llu islands, %llu left\n",
	            static_cast<unsigned long long>(uiBuiltVoxels),
	            static_cast<unsigned long long>(uiDestroyed),
	            static_cast<unsigned long long>(uiConverted),
	            static_cast<unsigned long long>(uiIslandsFound),
	            static_cast<unsigned long long>(world.CountOccupied()));

	/* The connectivity oracle, over the whole window. Anything the exhaustive
	   walk calls ungrounded that the run did not convert is an island the
	   memoised checker failed to find - which is phase 4's one real risk, and
	   the failure reads as "a thing that should have fallen did not" rather
	   than as a crash. */
	{
		const Stopwatch oracleWatch;

		std::unordered_set<uint64_t> classified;
		std::vector<uint64_t> island;

		uint64_t uiMissed = 0;
		uint64_t uiMissedComponents = 0;

		for (uint32_t uiZ = 0; uiZ < script.v3Size.z; ++uiZ)
		for (uint32_t uiY = 1; uiY < script.v3Size.y; ++uiY)
		for (uint32_t uiX = 0; uiX < script.v3Size.x; ++uiX)
		{
			if (!world.Grid().GetCell(uiX, uiY, uiZ).IsActive())
				continue;

			const uint64_t uiHash = IntegrityChecker::PositionToHash(uiX, uiY, uiZ);

			if (classified.count(uiHash) != 0)
				continue;

			if (!checker.ClassifyExhaustive(
				Vector3(static_cast<float>(uiX), static_cast<float>(uiY), static_cast<float>(uiZ)), island))
				continue;

			classified.insert(island.begin(), island.end());

			uiMissed += island.size();
			++uiMissedComponents;
		}

		const IntegrityChecker::Stats& stats = checker.GetStats();

		std::printf("\n[gauntlet] checker: %llu seeds offered, %llu deduplicated, %llu answered from the memo, "
		            "%llu visits, %llu memo hits mid-walk, %llu islands emitted\n",
		            static_cast<unsigned long long>(stats.uiSeedsOffered),
		            static_cast<unsigned long long>(stats.uiSeedsDeduplicated),
		            static_cast<unsigned long long>(stats.uiSeedsSkippedByMemo),
		            static_cast<unsigned long long>(stats.uiVisits),
		            static_cast<unsigned long long>(stats.uiMemoHits),
		            static_cast<unsigned long long>(stats.uiIslandsEmitted));

		std::printf("[gauntlet] oracle: %llu voxels in %llu ungrounded components still standing (%.0f ms)\n",
		            static_cast<unsigned long long>(uiMissed),
		            static_cast<unsigned long long>(uiMissedComponents),
		            oracleWatch.Milliseconds());
	}

	const uint32_t uiDisagreements = world.Validate();

	std::printf("[gauntlet] representation disagreements: %u\n", uiDisagreements);
	std::printf("[gauntlet] state hash: %016llx\n", static_cast<unsigned long long>(world.Hash()));

	return uiDisagreements == 0 ? 0 : 1;
}
