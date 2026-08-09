#include <cstdio>
#include <vector>

#include "Core/Voxels/SphericalDestruction.h"
#include "Core/Voxels/VoxelEditBatch.h"
#include "Framework/Benchmark.h"
#include "Harness/WorldShapes.h"

/* The destruction pipeline, measured end to end and in isolation.
 *
 * This is what the standalone `voxagine_gauntlet` executable used to be. It had
 * its own copy of the tick loop - bursts, debris, integrity, conversion - which
 * had already drifted away from the scenarios' copy on two points: the
 * destructibility predicate and the debris density. Both now drive
 * DestructionRun, so a benchmark measures the same code path a scenario proves
 * correct, and neither measures a reimplementation.
 *
 * What each metric is for is in Framework/Benchmark.h. The short version: the
 * work counters are exact and gate CI, the timings only mean something against
 * a recording from the same machine.
 */
namespace
{
	/* Towers wide and tall enough that severing one leaves an island of a few
	   thousand voxels. That size is the point: the pathology this exists to
	   watch for is integrity cost growing with how much has ever been
	   destroyed, and an island of a few hundred voxels does not show it. */
	const uint32_t k_uiTowerWidth = 12;
	const uint32_t k_uiTowerHeight = 56;

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

	/* Every phase timer and work counter a destruction run produces, reported
	   under one benchmark. Shared because the sweep benchmarks below want the
	   same table over a different script. */
	void ReportRun(BenchmarkResult& result, const DestructionResult& run)
	{
		result.AddWork("voxels-destroyed", static_cast<double>(run.uiDestroyed));
		result.AddWork("voxels-converted", static_cast<double>(run.uiConverted));
		result.AddWork("voxels-baked", static_cast<double>(run.uiBaked));
		result.AddWork("voxels-remaining", static_cast<double>(run.uiRemaining));
		result.AddWork("islands-found", static_cast<double>(run.uiIslandsFound));

		result.AddWork("checker-seeds-offered", static_cast<double>(run.checker.uiSeedsOffered));
		result.AddWork("checker-seeds-deduplicated", static_cast<double>(run.checker.uiSeedsDeduplicated));
		result.AddWork("checker-seeds-memoised", static_cast<double>(run.checker.uiSeedsSkippedByMemo));
		result.AddWork("checker-visits", static_cast<double>(run.checker.uiVisits));
		result.AddWork("checker-memo-hits", static_cast<double>(run.checker.uiMemoHits));

		/* Zero, on every machine, forever. It is a work metric rather than a
		   note precisely so that CI fails when it stops being zero - which is
		   what the gauntlet's exit code used to do. */
		result.AddWork("representation-disagreements", static_cast<double>(run.uiRepresentation));

		result.AddTime("burst-total", run.burstMs.TotalMs());
		result.AddTime("burst-peak", run.burstMs.PeakMs());
		result.AddTime("integrity-total", run.integrityMs.TotalMs());
		result.AddTime("integrity-peak", run.integrityMs.PeakMs());
		result.AddTime("convert-total", run.convertMs.TotalMs());
		result.AddTime("convert-peak", run.convertMs.PeakMs());
		result.AddTime("particles-total", run.particleMs.TotalMs());
		result.AddTime("particles-peak", run.particleMs.PeakMs());

		char note[256];

		std::snprintf(note, sizeof(note), "state hash %016llx, %llu voxels built, %u peak debris alive",
		              static_cast<unsigned long long>(run.uiHash),
		              static_cast<unsigned long long>(run.uiBuilt),
		              run.uiPeakAlive);

		result.AddNote(note);

		/* Integrity cost per quarter of the run. Flat is the goal: a rising
		   curve is the checker getting slower the more has been destroyed,
		   which is the shape phase 4's memo exists to remove. Reported as a
		   note rather than as four metrics because the *shape* is the finding
		   and each quarter on its own is just noise. */
		std::string quarters = "integrity by quarter:";

		for (double fMilliseconds : run.integrityQuarterMs)
		{
			char cell[32];
			std::snprintf(cell, sizeof(cell), " %8.2f ms", fMilliseconds);
			quarters += cell;
		}

		result.AddNote(quarters);
	}
}

/* Three waves of explosions down a lattice of towers, which is the closest
   thing here to a play session: destruction overlapping destruction, islands
   falling into rubble, debris baking back into the world. Waves rather than one
   pass because the cost curve only appears once bursts land on what earlier
   bursts already broke. */
VOXAGINE_BENCHMARK(Destruction, SeveredTowerWaves)
{
	DestructionConfig config;
	config.v3Size = UVector3(192, 96, 192);
	config.v3ChunkSize = UVector3(64, 96, 64);
	config.uiSeed = 20260809;
	config.uiDebrisCapacity = 112500;
	config.uiConvertBudget = 16384;

	/* The engine's own density. Scenarios use one particle per voxel to make
	   the floating-debris invariant as likely as possible to fire; a cost
	   measurement has to match what ships instead. */
	config.uiSpawnEveryNth = 4;

	config.bClassifyStanding = false;
	config.bMeasure = true;

	std::vector<Burst> bursts;
	uint32_t uiTick = 8;

	for (float fHeight : { 40.f, 20.f, 8.f })
	{
		for (const UVector3& v3Origin : TowerOrigins(config.v3Size))
		{
			Burst burst;
			burst.uiTick = uiTick;
			burst.v3Center = Vector3(
				v3Origin.x + k_uiTowerWidth * 0.5f, fHeight, v3Origin.z + k_uiTowerWidth * 0.5f);
			burst.fRadius = 10.f;

			bursts.push_back(burst);

			uiTick += 4;
		}

		/* A gap between waves so the checker and the conversion budget can
		   drain - otherwise the run measures the backlog rather than the work. */
		uiTick += 40;
	}

	config.uiTicks = uiTick + 160;

	DestructionRun run(config);

	{
		const Stopwatch watch;

		run.Harness().FillGround(TestColours::k_uiGround);

		uint16_t uiSlot = 1;

		for (const UVector3& v3Origin : TowerOrigins(config.v3Size))
		{
			run.Harness().FillBox(v3Origin, UVector3(k_uiTowerWidth, k_uiTowerHeight, k_uiTowerWidth),
			                      TestColours::k_uiStone, uiSlot);
			++uiSlot;
		}

		result.AddTime("level-build", watch.Milliseconds());
	}

	ReportRun(result, run.Run(bursts));
}

/* One burst at a time into a solid slab, with nothing else running.
 *
 * The end-to-end benchmark above cannot separate the explosion from the
 * integrity work it causes; this can. It is also the one that would catch the
 * defects phase 2 fixed - a running position read one cell behind the sphere
 * test, a colour fetched back over PCIe per destroyed voxel - because both
 * showed up as cost per destroyed voxel and neither changed the end state. */
VOXAGINE_BENCHMARK(Destruction, BurstIntoSolidRock)
{
	const UVector3 v3Size(128, 64, 128);

	VoxelWorldHarness world(v3Size, UVector3(64, 64, 64));

	world.FillGround(TestColours::k_uiGround);
	world.FillBox(UVector3(4, 1, 4), UVector3(120, 56, 120), TestColours::k_uiStone, 1);

	uint64_t uiDestroyed = 0;
	uint64_t uiSeeds = 0;
	uint32_t uiBursts = 0;

	const Stopwatch watch;

	/* Spread over the slab so no burst lands in a hole an earlier one made -
	   destroying nothing is fast and would flatter the average. */
	for (uint32_t uiZ = 12; uiZ < 120; uiZ += 16)
	for (uint32_t uiX = 12; uiX < 120; uiX += 16)
	{
		VoxelEditBatch batch(world.MakeEditTarget());
		std::vector<uint64_t> seeds;

		const SphericalDestruction::Result burst = SphericalDestruction::Apply(
			batch, world.Grid(),
			Vector3(static_cast<float>(uiX), 28.f, static_cast<float>(uiZ)), 6.f,
			[](uint16_t) { return true; },
			[](const Vector3&, uint32_t) {},
			&seeds);

		SphericalDestruction::FilterSeeds(world.Grid(), seeds);

		uiDestroyed += burst.uiDestroyed;
		uiSeeds += seeds.size();
		++uiBursts;
	}

	const double fMilliseconds = watch.Milliseconds();

	result.AddWork("bursts", uiBursts);
	result.AddWork("voxels-destroyed", static_cast<double>(uiDestroyed));
	result.AddWork("integrity-seeds", static_cast<double>(uiSeeds));

	result.AddTime("total", fMilliseconds);
	result.AddRate("per-burst", fMilliseconds / uiBursts, "ms");
	result.AddRate("per-destroyed-voxel",
	               uiDestroyed > 0 ? fMilliseconds * 1000000.0 / static_cast<double>(uiDestroyed) : 0.0, "ns");

	result.AddWork("representation-disagreements", world.Validate());
}
