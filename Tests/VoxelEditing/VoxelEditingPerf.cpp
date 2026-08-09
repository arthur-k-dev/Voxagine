#include <vector>

#include "Core/Utils/DeterministicRandom.h"
#include "Core/Voxels/VoxelEditBatch.h"
#include "Framework/Benchmark.h"
#include "Harness/VoxelWorldHarness.h"

/* The single voxel write path, on its own.
 *
 * Every destruction number above is built out of these, so a regression here
 * moves everything at once and is worth being able to see by itself. A write
 * maintains six representations (rule 3) and the interesting property is that
 * none of them is a read of the mapped GPU buffer: the mapping is write-only
 * from the CPU because a read is an uncached PCIe read of VRAM, and the one
 * question a write actually needs - was this cell occupied - is answered from
 * the brick grid's CPU-side occupancy bitmap. That change alone took five
 * seconds off a world load.
 *
 * So a rise in ns-per-write is worth chasing hard: the most likely cause is
 * something on the write path having started to read the mapping back.
 */
namespace
{
	const uint32_t k_uiWrites = 400000;
}

VOXAGINE_BENCHMARK(VoxelEditing, ScatteredWrites)
{
	const UVector3 v3Size(128, 64, 128);

	VoxelWorldHarness world(v3Size, UVector3(64, 64, 64));
	world.FillGround(0xFF404040u);

	/* Scattered rather than sequential, which is the honest case: a burst
	   walks a sphere and debris lands wherever it lands. Positions are
	   generated up front so the RNG is not inside the timed loop. */
	std::vector<Vector3> positions;
	positions.reserve(k_uiWrites);

	DeterministicRandom random(4242);

	for (uint32_t i = 0; i < k_uiWrites; ++i)
	{
		positions.push_back(Vector3(
			std::floor(random.Range(0.f, static_cast<float>(v3Size.x) - 1.f)),
			std::floor(random.Range(1.f, static_cast<float>(v3Size.y) - 1.f)),
			std::floor(random.Range(0.f, static_cast<float>(v3Size.z) - 1.f))));
	}

	uint32_t uiSetWrites = 0;
	uint32_t uiClearWrites = 0;
	size_t uiDirtyBricks = 0;

	double fSetMs = 0.0;
	double fClearMs = 0.0;

	{
		VoxelEditBatch batch(world.MakeEditTarget());

		const Stopwatch watch;

		for (const Vector3& v3Position : positions)
			batch.Set(v3Position, 0xFF806040u, 7);

		fSetMs = watch.Milliseconds();

		uiSetWrites = batch.GetWrites();
		uiDirtyBricks = batch.GetDirtyBricks().size();
	}

	{
		VoxelEditBatch batch(world.MakeEditTarget());

		const Stopwatch watch;

		for (const Vector3& v3Position : positions)
			batch.Clear(v3Position);

		fClearMs = watch.Milliseconds();

		uiClearWrites = batch.GetWrites();
	}

	result.AddWork("set-writes", uiSetWrites);
	result.AddWork("clear-writes", uiClearWrites);
	result.AddWork("dirty-bricks", static_cast<double>(uiDirtyBricks));
	result.AddWork("representation-disagreements", world.Validate());

	result.AddTime("set-total", fSetMs);
	result.AddTime("clear-total", fClearMs);

	result.AddRate("ns-per-set",
	               uiSetWrites > 0 ? fSetMs * 1000000.0 / uiSetWrites : 0.0, "ns");
	result.AddRate("ns-per-clear",
	               uiClearWrites > 0 ? fClearMs * 1000000.0 / uiClearWrites : 0.0, "ns");
}
