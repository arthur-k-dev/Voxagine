#include "pch.h"
#include "Core/ECS/Systems/Chunk/StreamingBudgets.h"

#include <cstdio>
#include <cstdlib>

namespace
{
	/* One budget is settable from the environment, and only because its
	 * acceptance criterion is a sweep. CHUNK_STREAMING_PLAN.md phase 9 requires
	 * the resident window's occupied-voxel count to come out identical at
	 * several slice sizes - a deficit that scales with slice size is the whole
	 * signature of state lost across a slice boundary - and a sweep that needs a
	 * rebuild between points is a sweep nobody re-runs. Same argument as
	 * `--map` replacing an edit to ProjectSettings.vgps.
	 *
	 * Deliberately not a launch option: it is a diagnostic for one measurement,
	 * not something a player or a level has any business setting.
	 */
	StreamingBudgets MakeActiveBudgets()
	{
		StreamingBudgets budgets;

		if (const char* pSamples = std::getenv("VOXAGINE_STAMP_SAMPLES"))
		{
			const long lSamples = std::strtol(pSamples, nullptr, 10);

			if (lSamples > 0)
			{
				budgets.VoxelBakingSamples = StreamingBudget::Units(static_cast<uint32_t>(lSamples));

				fprintf(stderr, "[bake] stamp slice budget overridden to %ld samples\n", lSamples);
			}
			else
			{
				budgets.VoxelBakingSamples = StreamingBudget::Unbounded();

				fprintf(stderr, "[bake] stamp slice budget overridden to unbounded\n");
			}
		}

		return budgets;
	}
}

StreamingBudgets StreamingBudgets::s_Active = MakeActiveBudgets();
