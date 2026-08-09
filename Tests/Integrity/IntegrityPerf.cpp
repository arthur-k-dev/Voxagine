#include <vector>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/Voxels/SphericalDestruction.h"
#include "Core/Voxels/VoxelEditBatch.h"
#include "Framework/Benchmark.h"
#include "Harness/WorldShapes.h"

/* The connectivity checker on its own, with no particles and no conversion.
 *
 * The failure this watches for has a name and a shape: integrity cost that
 * grows with how much has *ever* been destroyed rather than with how much just
 * changed. The cause was the flood fill discarding everything it had collected
 * the moment it reached the ground, so the next seed standing on the same
 * building flooded the whole building again - and one explosion produces
 * thousands of seeds on the same few structures.
 *
 * checker-visits is the metric that catches it, and it catches it exactly:
 * visits are the count of cells the fill entered, they are deterministic, and
 * the regression multiplies them by the number of seeds. A stopwatch would show
 * the same thing on this machine and hide it on a faster one.
 */
VOXAGINE_BENCHMARK(Integrity, RepeatedCutsIntoOneStructure)
{
	const UVector3 v3Size(128, 96, 128);

	VoxelWorldHarness world(v3Size, UVector3(64, 96, 64));

	world.FillGround(TestColours::k_uiGround);

	/* One large block, so every cut seeds the same component and the memo is
	   the only thing standing between this and quadratic behaviour. */
	world.FillBox(UVector3(16, 1, 16), UVector3(96, 80, 96), TestColours::k_uiStone, 1);

	IntegrityChecker checker;
	checker.SetVoxelGrid(&world.Grid());

	PhaseTimer processMs;

	uint64_t uiIslands = 0;
	uint32_t uiCuts = 0;

	const Stopwatch total;

	/* Cuts climbing the block. Each one leaves the geometry above it still
	   attached at the edges, so nothing actually falls - which is the point: it
	   isolates the *classification* cost from the conversion that would
	   otherwise change the world underneath the next cut. */
	for (uint32_t uiY = 8; uiY < 72; uiY += 8)
	for (uint32_t uiX = 32; uiX < 96; uiX += 16)
	{
		std::vector<uint64_t> seeds;

		{
			VoxelEditBatch batch(world.MakeEditTarget());

			SphericalDestruction::Apply(
				batch, world.Grid(),
				Vector3(static_cast<float>(uiX), static_cast<float>(uiY), 64.f), 7.f,
				[](uint16_t) { return true; },
				[](const Vector3&, uint32_t) {},
				&seeds);
		}

		SphericalDestruction::FilterSeeds(world.Grid(), seeds);
		checker.EnqueueBulk(seeds);

		++uiCuts;

		/* Drained to completion rather than budgeted, so the measurement is the
		   total work rather than how the budget happened to slice it. */
		for (uint32_t uiPass = 0; uiPass < 64; ++uiPass)
		{
			const Stopwatch watch;

			std::vector<std::vector<uint64_t>> islands;
			checker.Process(IntegrityChecker::VISIT_BUDGET_PER_TICK, islands);

			processMs.Add(watch.Milliseconds());

			uiIslands += islands.size();
		}
	}

	const IntegrityChecker::Stats& stats = checker.GetStats();

	result.AddWork("cuts", uiCuts);
	result.AddWork("checker-seeds-offered", static_cast<double>(stats.uiSeedsOffered));
	result.AddWork("checker-seeds-deduplicated", static_cast<double>(stats.uiSeedsDeduplicated));
	result.AddWork("checker-seeds-memoised", static_cast<double>(stats.uiSeedsSkippedByMemo));
	result.AddWork("checker-visits", static_cast<double>(stats.uiVisits));
	result.AddWork("checker-memo-hits", static_cast<double>(stats.uiMemoHits));
	result.AddWork("islands-found", static_cast<double>(uiIslands));

	result.AddTime("total", total.Milliseconds());
	result.AddTime("process-total", processMs.TotalMs());
	result.AddTime("process-peak", processMs.PeakMs());

	result.AddRate("ns-per-visit",
	               stats.uiVisits > 0
	                   ? processMs.TotalMs() * 1000000.0 / static_cast<double>(stats.uiVisits)
	                   : 0.0,
	               "ns");
}
