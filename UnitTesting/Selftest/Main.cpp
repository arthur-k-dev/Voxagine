#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "Selftest/SelftestInvariant.h"
#include "Selftest/SelftestScenario.h"

/* Runs every registered scenario against every registered invariant.
 *
 * It knows about neither: both arrive through their registries, so adding a
 * case or a check is adding a file. That is deliberate - every defect the first
 * play session found was a scenario that did not exist, and the cost of adding
 * one is the thing that decides whether the next one gets written.
 *
 *   voxagine_selftest [name-filter]
 *
 * With a filter, only scenarios whose name contains it run - `voxagine_selftest
 * random` or `voxagine_selftest protected` while chasing one failure.
 */
int main(int argc, char* argv[])
{
	const char* pFilter = argc > 1 ? argv[1] : nullptr;

	const std::vector<std::unique_ptr<SelftestScenario>>& scenarios =
		SelftestRegistry::Get().Scenarios();

	const std::vector<std::unique_ptr<SelftestInvariant>>& invariants =
		SelftestInvariantRegistry::Get().Invariants();

	std::printf("[selftest] %zu scenarios x %zu invariants\n\n", scenarios.size(), invariants.size());

	uint32_t uiRun = 0;
	uint32_t uiFailed = 0;

	std::vector<std::string> failures;

	for (const std::unique_ptr<SelftestScenario>& pScenario : scenarios)
	{
		if (pFilter != nullptr && std::strstr(pScenario->Name(), pFilter) == nullptr)
			continue;

		++uiRun;

		SelftestConfig config;
		pScenario->Configure(config);

		std::vector<Burst> bursts;
		pScenario->Script(config, bursts);

		SelftestResult result;

		{
			SelftestWorld world(config);
			pScenario->Build(world.Harness(), config);

			result = world.Run(bursts);
		}

		result.uiMinStanding = pScenario->MinStandingAtEnd(result.uiStandingAtStart);

		/* The same scenario again, from scratch. Determinism is checked like
		   any other invariant rather than being special-cased, which is why the
		   second run happens here and lands in the same result. */
		{
			SelftestWorld world(config);
			pScenario->Build(world.Harness(), config);

			result.uiRepeatHash = world.Run(bursts).uiHash;
		}

		bool bScenarioFailed = false;

		for (const std::unique_ptr<SelftestInvariant>& pInvariant : invariants)
		{
			const std::string detail = pInvariant->Check(result);

			if (detail.empty())
				continue;

			bScenarioFailed = true;

			failures.push_back(std::string(pScenario->Name()) + " / " + pInvariant->Name() + ": " + detail);

			std::printf("  FAIL  %-20s %-14s %s\n", pScenario->Name(), pInvariant->Name(), detail.c_str());

			for (const Vector3& v3Sample : result.standingSamples)
			{
				std::printf("        at %.0f %.0f %.0f\n", v3Sample.x, v3Sample.y, v3Sample.z);
			}
		}

		if (bScenarioFailed)
			++uiFailed;
		else
			std::printf("  ok    %-20s %6llu destroyed, %6llu converted, %6llu baked, %6llu standing\n",
			            pScenario->Name(),
			            static_cast<unsigned long long>(result.uiDestroyed),
			            static_cast<unsigned long long>(result.uiConverted),
			            static_cast<unsigned long long>(result.uiBaked),
			            static_cast<unsigned long long>(result.uiStanding));
	}

	std::printf("\n[selftest] %u scenarios, %u failed\n", uiRun, uiFailed);

	for (const std::string& failure : failures)
		std::printf("  %s\n", failure.c_str());

	return uiFailed == 0 ? 0 : 1;
}
