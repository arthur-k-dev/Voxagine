#include "Framework/Benchmark.h"
#include "Harness/VoxelWorldHarness.h"

/* Reading the world, which is what every other system does between writes.
 *
 * VoxelGrid::GetCell resolves one voxel through the chunk index arithmetic and
 * hands back the colour, the owner slot and the index together. The integrity
 * checker calls it once per cell it visits and the particle simulation calls it
 * on every impact, so its cost is a multiplier on both - which makes it worth a
 * number of its own rather than only appearing inside theirs.
 *
 * Phase 4d moved the owner out of the voxel and into a parallel uint16 volume,
 * taking a resident chunk from 128 to 48 MiB. The reason that was safe is that
 * GetCell resolves the index once for both; a change that resolves it twice
 * would show up here and nowhere else.
 */
VOXAGINE_BENCHMARK(VoxelStorage, GridTraversal)
{
	const UVector3 v3Size(128, 64, 128);

	VoxelWorldHarness world(v3Size, UVector3(64, 64, 64));

	world.FillGround(0xFF404040u);
	world.FillBox(UVector3(8, 1, 8), UVector3(112, 48, 112), 0xFF906030u, 1);

	uint64_t uiActive = 0;
	uint64_t uiOwned = 0;
	uint64_t uiCells = 0;

	const Stopwatch watch;

	for (uint32_t uiZ = 0; uiZ < v3Size.z; ++uiZ)
	for (uint32_t uiY = 0; uiY < v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < v3Size.x; ++uiX)
	{
		const VoxelCell cell = world.Grid().GetCell(uiX, uiY, uiZ);

		++uiCells;

		if (cell.IsActive())
			++uiActive;

		if (cell.GetSlot() != VoxelOwnerVolume::k_uiNoOwnerSlot)
			++uiOwned;
	}

	const double fMilliseconds = watch.Milliseconds();

	result.AddWork("cells-read", static_cast<double>(uiCells));
	result.AddWork("cells-active", static_cast<double>(uiActive));
	result.AddWork("cells-owned", static_cast<double>(uiOwned));

	result.AddTime("total", fMilliseconds);
	result.AddRate("ns-per-cell", fMilliseconds * 1000000.0 / static_cast<double>(uiCells), "ns");
}
