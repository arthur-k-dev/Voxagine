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
 *   explode <tick> <x> <y> <z> <radius>
 */

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/Utils/DeterministicRandom.h"
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

		std::vector<Explosion> explosions;
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

	/* The reference explosion: clear every occupied voxel inside the sphere and
	   seed the integrity checker above each one, which is what
	   PhysicsSystem::ApplySphericalDestruction does today (nine seeds per
	   destroyed voxel, deduped only inside one batch - D6).
	 *
	 * Kept close to today's shape on purpose. Phase 2 replaces this with a call
	 * into the extracted engine function and the gauntlet then measures the real
	 * thing; until then it is the baseline's stand-in and its cost is the
	 * write path's, not the sphere loop's.
	 */
	uint32_t Explode(VoxelWorldHarness& world, VoxelEditBatch& batch, const Explosion& explosion,
	                 DeterministicRandom& random, std::vector<uint64_t>& o_seeds)
	{
		const int32_t iRadius = static_cast<int32_t>(explosion.fRadius);
		const float fRadiusSquared = explosion.fRadius * explosion.fRadius;

		const int32_t iCenterX = static_cast<int32_t>(explosion.v3Center.x);
		const int32_t iCenterY = static_cast<int32_t>(explosion.v3Center.y);
		const int32_t iCenterZ = static_cast<int32_t>(explosion.v3Center.z);

		uint32_t uiDestroyed = 0;
		uint32_t uiSpawned = 0;

		for (int32_t iZ = iCenterZ - iRadius; iZ <= iCenterZ + iRadius; ++iZ)
		for (int32_t iY = iCenterY - iRadius; iY <= iCenterY + iRadius; ++iY)
		for (int32_t iX = iCenterX - iRadius; iX <= iCenterX + iRadius; ++iX)
		{
			if (iX < 0 || iY < 1 || iZ < 0)
				continue;

			const float fDX = static_cast<float>(iX - iCenterX);
			const float fDY = static_cast<float>(iY - iCenterY);
			const float fDZ = static_cast<float>(iZ - iCenterZ);

			if (fDX * fDX + fDY * fDY + fDZ * fDZ > fRadiusSquared)
				continue;

			const VoxelCell cell = world.Grid().GetCell(
				static_cast<uint32_t>(iX), static_cast<uint32_t>(iY), static_cast<uint32_t>(iZ));

			if (!cell || !cell.IsActive())
				continue;

			/* One particle per four destroyed voxels, matching the engine. The
			   draw is made whether or not a pool accepts it, so the RNG stream
			   depends only on the geometry. */
			if ((uiSpawned % 4) == 0)
				(void)random.Range(20.f, 60.f);

			++uiSpawned;

			batch.Clear(Vector3(
				static_cast<float>(iX), static_cast<float>(iY), static_cast<float>(iZ)));

			++uiDestroyed;

			for (int32_t iSeedZ = -1; iSeedZ <= 1; ++iSeedZ)
			{
				for (int32_t iSeedX = -1; iSeedX <= 1; ++iSeedX)
				{
					o_seeds.push_back(IntegrityChecker::PositionToHash(Vector3(
						static_cast<float>(iX + iSeedX),
						static_cast<float>(iY + 1),
						static_cast<float>(iZ + iSeedZ))));
				}
			}
		}

		return uiDestroyed;
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

	VoxelWorldHarness world(script.v3Size, script.v3ChunkSize);

	{
		const Stopwatch watch;
		BuildLevel(world, script);
		buildPhase.Add(watch.Milliseconds());
	}

	const uint64_t uiBuiltVoxels = world.CountOccupied();

	DeterministicRandom random(script.uiSeed);

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
			uiDestroyed += Explode(world, batch, script.explosions[uiNextExplosion], random, seeds);

			explodePhase.Add(watch.Milliseconds());

			++uiNextExplosion;
		}

		if (!seeds.empty())
			checker.EnqueueBulk(seeds);

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

	std::printf("\n[gauntlet] integrity cost by quarter of the run (the #27 curve; flat is the goal)\n  ");

	for (uint32_t uiSegment = 0; uiSegment < uiSegments; ++uiSegment)
		std::printf("%9.2f ms", segmentIntegrityMs[uiSegment]);

	std::printf("\n\n[gauntlet] %llu voxels built, %llu destroyed by explosions, %llu converted from %llu islands, %llu left\n",
	            static_cast<unsigned long long>(uiBuiltVoxels),
	            static_cast<unsigned long long>(uiDestroyed),
	            static_cast<unsigned long long>(uiConverted),
	            static_cast<unsigned long long>(uiIslandsFound),
	            static_cast<unsigned long long>(world.CountOccupied()));

	const uint32_t uiDisagreements = world.Validate();

	std::printf("[gauntlet] representation disagreements: %u\n", uiDisagreements);
	std::printf("[gauntlet] state hash: %016llx\n", static_cast<unsigned long long>(world.Hash()));

	return uiDisagreements == 0 ? 0 : 1;
}
