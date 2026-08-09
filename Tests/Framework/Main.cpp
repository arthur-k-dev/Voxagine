#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Framework/Baseline.h"
#include "Framework/Benchmark.h"
#include "Framework/Check.h"
#include "Framework/Invariant.h"
#include "Framework/Scenario.h"

/* The one test runner.
 *
 *   voxagine_tests [mode] [filter] [options]
 *
 *   checks       assertions about one unit at a time (Tests/<System>/...Checks.cpp)
 *   scenarios    every registered scenario against every registered invariant
 *   perf         every registered benchmark, compared against a baseline
 *   all          checks then scenarios (the default; perf is not a pass/fail
 *                gate on a machine whose speed nobody recorded)
 *
 *   --baseline <file>     compare perf against this recording
 *   --record <file>       write perf results as a new recording
 *   --record-work <file>  the same, without the timings - which is what the
 *                         checked-in Tests/Baselines/perf.txt holds, because a
 *                         millisecond figure from one machine means nothing on
 *                         another
 *   --strict            let a timing regression fail the run, not just work
 *
 * A filter is a substring: `voxagine_tests scenarios diagonal` or
 * `voxagine_tests checks VoxelGrid` while chasing one failure.
 *
 * It knows about none of the cases it runs: checks, scenarios, invariants and
 * benchmarks all arrive through their registries, so adding any of them is
 * adding a file. That is deliberate - every defect the first play session found
 * was a case that did not exist, and the cost of adding one is what decides
 * whether the next one gets written.
 */
namespace
{
	bool Matches(const char* pFilter, const std::string& name)
	{
		return pFilter == nullptr || name.find(pFilter) != std::string::npos;
	}

	uint32_t RunChecks(const char* pFilter)
	{
		const std::vector<std::unique_ptr<Check>>& checks = CheckRegistry::Get().Checks();

		uint32_t uiRun = 0;
		uint32_t uiFailed = 0;

		for (const std::unique_ptr<Check>& pCheck : checks)
		{
			const std::string name = std::string(pCheck->System()) + "/" + pCheck->Name();

			if (!Matches(pFilter, name))
				continue;

			++uiRun;

			CheckContext ctx;
			pCheck->Run(ctx);

			if (!ctx.Failed())
				continue;

			++uiFailed;

			std::printf("  FAIL  %s%s\n", name.c_str(), ctx.Abandoned() ? "  (abandoned)" : "");

			for (const std::string& failure : ctx.Failures())
				std::printf("          %s\n", failure.c_str());
		}

		std::printf("[checks] %u run, %u failed\n", uiRun, uiFailed);

		return uiFailed;
	}

	uint32_t RunScenarios(const char* pFilter)
	{
		const std::vector<std::unique_ptr<Scenario>>& scenarios = ScenarioRegistry::Get().Scenarios();
		const std::vector<std::unique_ptr<Invariant>>& invariants = InvariantRegistry::Get().Invariants();

		std::printf("[scenarios] %zu scenarios x %zu invariants\n",
		            scenarios.size(), invariants.size());

		uint32_t uiRun = 0;
		uint32_t uiFailed = 0;

		std::vector<std::string> failures;

		for (const std::unique_ptr<Scenario>& pScenario : scenarios)
		{
			if (!Matches(pFilter, pScenario->Name()))
				continue;

			++uiRun;

			DestructionConfig config;
			pScenario->Configure(config);

			std::vector<Burst> bursts;
			pScenario->Script(config, bursts);

			DestructionResult result;

			{
				DestructionRun run(config);
				pScenario->Build(run.Harness(), config);

				result = run.Run(bursts);
			}

			result.uiMinStanding = pScenario->MinStandingAtEnd(result.uiStandingAtStart);

			/* The same scenario again, from scratch. Determinism is checked
			   like any other invariant rather than being special-cased, which
			   is why the second run happens here and lands in the same result. */
			{
				DestructionRun run(config);
				pScenario->Build(run.Harness(), config);

				result.uiRepeatHash = run.Run(bursts).uiHash;
			}

			bool bScenarioFailed = false;

			for (const std::unique_ptr<Invariant>& pInvariant : invariants)
			{
				const std::string detail = pInvariant->Check(result);

				if (detail.empty())
					continue;

				bScenarioFailed = true;

				failures.push_back(std::string(pScenario->Name()) + " / " + pInvariant->Name() + ": " + detail);

				std::printf("  FAIL  %-24s %-24s %s\n",
				            pScenario->Name(), pInvariant->Name(), detail.c_str());

				for (const Vector3& v3Sample : result.standingSamples)
					std::printf("        at %.0f %.0f %.0f\n", v3Sample.x, v3Sample.y, v3Sample.z);
			}

			if (bScenarioFailed)
				++uiFailed;
			else
				std::printf("  ok    %-24s %6llu destroyed, %6llu converted, %6llu baked, %6llu standing\n",
				            pScenario->Name(),
				            static_cast<unsigned long long>(result.uiDestroyed),
				            static_cast<unsigned long long>(result.uiConverted),
				            static_cast<unsigned long long>(result.uiBaked),
				            static_cast<unsigned long long>(result.uiStanding));
		}

		std::printf("[scenarios] %u run, %u failed\n", uiRun, uiFailed);

		for (const std::string& failure : failures)
			std::printf("  %s\n", failure.c_str());

		return uiFailed;
	}

	/* Prints one metric row, and says whether it moved.
	 *
	 * Work metrics are exact, so an arrow either way is real. Timings are not,
	 * so they get the same arrow but never the verdict - see Benchmark.h for
	 * why the two kinds cannot be treated alike. */
	void ReportMetric(const Baseline::Comparison& comparison)
	{
		char value[64];
		std::snprintf(value, sizeof(value), "%12.4f %s", comparison.fValue, comparison.unit.c_str());

		if (!comparison.bHasBaseline)
		{
			std::printf("      %-34s %-20s %s\n", comparison.key.c_str(), value, "(no baseline)");
			return;
		}

		const double fDelta = comparison.fBaseline != 0.0
			? (comparison.fValue - comparison.fBaseline) / comparison.fBaseline * 100.0
			: 0.0;

		const char* pVerdict = "  ok";

		if (comparison.bRegressed)
			pVerdict = comparison.kind == MetricKind::Work ? "REGRESSED" : "slower";
		else if (comparison.bImproved)
			pVerdict = comparison.kind == MetricKind::Work ? "improved" : "faster";

		std::printf("      %-34s %-20s was %12.4f  %+7.1f%%  %s\n",
		            comparison.key.c_str(), value, comparison.fBaseline, fDelta, pVerdict);
	}

	uint32_t RunBenchmarks(const char* pFilter, const std::string& baselinePath,
	                       const std::string& recordPath, bool bRecordWorkOnly, bool bStrict)
	{
		const std::vector<std::unique_ptr<Benchmark>>& benchmarks = BenchmarkRegistry::Get().Benchmarks();

		Baseline baseline;
		bool bHaveBaseline = false;

		if (!baselinePath.empty())
		{
			std::string error;

			if (baseline.Load(baselinePath, error))
			{
				bHaveBaseline = true;
				std::printf("[perf] comparing against %s\n", baselinePath.c_str());
			}
			else
			{
				std::printf("[perf] %s - running without a comparison\n", error.c_str());
			}
		}

		Baseline recorded;

		uint32_t uiRegressed = 0;

		for (const std::unique_ptr<Benchmark>& pBenchmark : benchmarks)
		{
			const std::string name = std::string(pBenchmark->System()) + "/" + pBenchmark->Name();

			if (!Matches(pFilter, name))
				continue;

			std::printf("\n  %s\n", name.c_str());
			std::fflush(stdout);

			BenchmarkResult result;
			pBenchmark->Run(result);

			recorded.Record(pBenchmark->System(), pBenchmark->Name(), result);

			const std::vector<Baseline::Comparison> comparisons = bHaveBaseline
				? baseline.Compare(pBenchmark->System(), pBenchmark->Name(), result)
				: Baseline().Compare(pBenchmark->System(), pBenchmark->Name(), result);

			for (const Baseline::Comparison& comparison : comparisons)
			{
				ReportMetric(comparison);

				if (!comparison.bRegressed)
					continue;

				if (comparison.kind == MetricKind::Work || bStrict)
					++uiRegressed;
			}

			for (const std::string& note : result.Notes())
				std::printf("      %s\n", note.c_str());
		}

		if (!recordPath.empty())
		{
			std::string error;

			if (recorded.Save(recordPath, error, bRecordWorkOnly))
				std::printf("\n[perf] recorded to %s\n", recordPath.c_str());
			else
				std::printf("\n[perf] %s\n", error.c_str());
		}

		std::printf("\n[perf] %u regressions\n", uiRegressed);

		return uiRegressed;
	}
}

int main(int argc, char* argv[])
{
	std::string mode = "all";
	const char* pFilter = nullptr;

	std::string baselinePath;
	std::string recordPath;
	bool bRecordWorkOnly = false;
	bool bStrict = false;

	for (int i = 1; i < argc; ++i)
	{
		const std::string argument = argv[i];

		if (argument == "--strict")
		{
			bStrict = true;
		}
		else if (argument == "--baseline" && i + 1 < argc)
		{
			baselinePath = argv[++i];
		}
		else if (argument == "--record" && i + 1 < argc)
		{
			recordPath = argv[++i];
		}
		else if (argument == "--record-work" && i + 1 < argc)
		{
			recordPath = argv[++i];
			bRecordWorkOnly = true;
		}
		else if (argument == "checks" || argument == "scenarios" || argument == "perf" || argument == "all")
		{
			mode = argument;
		}
		else if (argument.rfind("--", 0) == 0)
		{
			std::fprintf(stderr, "unknown option '%s'\n", argument.c_str());
			return 2;
		}
		else
		{
			pFilter = argv[i];
		}
	}

	uint32_t uiFailed = 0;

	if (mode == "checks" || mode == "all")
		uiFailed += RunChecks(pFilter);

	if (mode == "scenarios" || mode == "all")
		uiFailed += RunScenarios(pFilter);

	if (mode == "perf")
		uiFailed += RunBenchmarks(pFilter, baselinePath, recordPath, bRecordWorkOnly, bStrict);

	return uiFailed == 0 ? 0 : 1;
}
