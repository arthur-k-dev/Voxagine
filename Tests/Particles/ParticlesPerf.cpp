#include <algorithm>
#include <cstdio>
#include <vector>

#include "Framework/Benchmark.h"
#include "Harness/WorldShapes.h"

/* The CPU particle simulation, swept against the number of particles alive.
 *
 * Rule 11: sweep the axis the optimization claims to attack. The question the
 * sweep answers is not "how fast is it" but "does the cost per particle stay
 * flat as the count grows" - a per-particle cost that climbs means something in
 * the step is not O(1) in the population, and that is what decides whether the
 * sim needs to move off the CPU at all. It stayed flat over a 30x range, which
 * is what closed DESTRUCTION_PLAN.md's phase 6 gate.
 *
 * Debris is injected directly rather than produced by explosions, because an
 * explosion gives you whatever count it happens to give you and the independent
 * variable has to be the count.
 *
 * This replaces the three sim-<n>.gauntlet script files. A sweep whose points
 * live in a data file is a sweep nobody re-runs; as code it is one benchmark
 * with a list of counts in it.
 */
namespace
{
	void SweepAt(BenchmarkResult& result, uint32_t uiCount)
	{
		DestructionConfig config;
		config.v3Size = UVector3(192, 96, 192);
		config.v3ChunkSize = UVector3(64, 96, 64);
		config.uiSeed = 20260809;

		config.uiTicks = 200;
		config.uiDebrisCapacity = uiCount + 1024;

		config.bClassifyStanding = false;
		config.bMeasure = true;

		/* All of it at once, high up, so the population stays where it was put
		   for many ticks rather than draining as it lands. */
		Injection injection;
		injection.uiTick = 4;
		injection.uiCount = uiCount;

		config.injections.push_back(injection);

		DestructionRun run(config);

		run.Harness().FillGround(TestColours::k_uiGround);
		WorldShapes::BuildTowers(run.Harness(), config, false);

		const DestructionResult measured = run.Run(std::vector<Burst>());

		/* Over the busiest ticks only. A tick with eleven particles left in it
		   measures loop overhead rather than the per-particle cost, and there
		   are a lot of those at the tail of every run. */
		std::vector<std::pair<uint32_t, double>> samples = measured.simSamples;

		std::sort(samples.begin(), samples.end(),
			[](const std::pair<uint32_t, double>& a, const std::pair<uint32_t, double>& b)
			{ return a.first > b.first; });

		const size_t uiTop = std::min<size_t>(samples.size(), 32);

		double fTotalMs = 0.0;
		uint64_t uiTotalParticles = 0;

		for (size_t i = 0; i < uiTop; ++i)
		{
			fTotalMs += samples[i].second;
			uiTotalParticles += samples[i].first;
		}

		const std::string suffix = "-" + std::to_string(uiCount);

		result.AddWork("peak-alive" + suffix, measured.uiPeakAlive);
		result.AddWork("baked" + suffix, static_cast<double>(measured.uiBaked));

		result.AddRate("ns-per-particle-per-tick" + suffix,
		               uiTotalParticles > 0
		                   ? fTotalMs * 1000000.0 / static_cast<double>(uiTotalParticles)
		                   : 0.0,
		               "ns");

		result.AddTime("busiest-tick" + suffix, samples.empty() ? 0.0 : samples[0].second);
	}
}

/* Three points over a 30x range. The number that matters is whether
   ns-per-particle-per-tick is the same at all three. */
VOXAGINE_BENCHMARK(Particles, SimulationSweep)
{
	SweepAt(result, 5000);
	SweepAt(result, 50000);
	SweepAt(result, 150000);
}
