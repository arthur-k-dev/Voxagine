#include "Framework/Check.h"

#include <cmath>
#include <limits>

#include "Core/Voxels/VoxelEditBatch.h"
#include "Harness/VoxelWorldHarness.h"

namespace
{
	const UVector3 k_v3Size(48, 24, 48);
	const UVector3 k_v3ChunkSize(16, 24, 16);

	const uint32_t k_uiStone = 0xFF808080u;
	const uint32_t k_uiWood = 0xFF3060A0u;
}

VOXAGINE_CHECK(VoxelEditBatch, SetReachesEveryRepresentation)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelEditBatch batch(world.MakeEditTarget());

	REQUIRE_TRUE(batch.Set(Vector3(5.f, 7.f, 9.f), k_uiStone, 3));

	const VoxelCell cell = world.Grid().GetCell(5, 7, 9);

	REQUIRE_TRUE(static_cast<bool>(cell));
	CHECK_EQ(cell.GetColor(), k_uiStone);
	CHECK_EQ(cell.GetSlot(), 3);
	CHECK_EQ(world.Words()[world.VoxelID(5, 7, 9)], k_uiStone);
	CHECK_TRUE(world.Bricks().IsOccupied(world.VoxelID(5, 7, 9)));
	CHECK_EQ(world.Bricks().GetCount(false, world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9))), 1u);
	CHECK_EQ(world.Validate(), 0u);
	CHECK_EQ(batch.GetWrites(), 1u);
}

/* Ledger D5. The three dual-write sites cleared the colour and left the owner
   slot behind, so three of every four destroyed voxels kept naming a model that
   no longer had anything there. */
VOXAGINE_CHECK(VoxelEditBatch, ClearAlsoClearsTheOwner)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelEditBatch batch(world.MakeEditTarget());

	batch.Set(Vector3(5.f, 7.f, 9.f), k_uiStone, 42);
	REQUIRE_EQ(world.Grid().GetCell(5, 7, 9).GetSlot(), 42);

	REQUIRE_TRUE(batch.Clear(Vector3(5.f, 7.f, 9.f)));

	const VoxelCell cell = world.Grid().GetCell(5, 7, 9);

	CHECK_FALSE(cell.IsActive());
	CHECK_EQ(cell.GetSlot(), VoxelOwnerVolume::k_uiNoOwnerSlot);
	CHECK_EQ(world.Words()[world.VoxelID(5, 7, 9)], 0u);
	CHECK_FALSE(world.Bricks().IsOccupied(world.VoxelID(5, 7, 9)));
	CHECK_EQ(world.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelEditBatch, OverwritingDoesNotMoveTheBrickCount)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelEditBatch batch(world.MakeEditTarget());

	const uint32_t uiBrick = world.Bricks().VoxelToBrick(world.VoxelID(5, 7, 9));

	batch.Set(Vector3(5.f, 7.f, 9.f), k_uiStone, 1);
	batch.Set(Vector3(5.f, 7.f, 9.f), k_uiWood, 2);

	CHECK_EQ(world.Bricks().GetCount(false, uiBrick), 1u);
	CHECK_EQ(world.Grid().GetCell(5, 7, 9).GetSlot(), 2);
	CHECK_EQ(world.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelEditBatch, RejectsOutOfBoundsAndNegativePositions)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelEditBatch batch(world.MakeEditTarget());

	const uint64_t uiBefore = world.Hash();

	CHECK_FALSE(batch.Set(Vector3(static_cast<float>(k_v3Size.x), 0.f, 0.f), k_uiStone, 1));
	CHECK_FALSE(batch.Set(Vector3(0.f, static_cast<float>(k_v3Size.y), 0.f), k_uiStone, 1));
	CHECK_FALSE(batch.Set(Vector3(0.f, 0.f, static_cast<float>(k_v3Size.z)), k_uiStone, 1));

	/* Negative has to be caught *before* the cast, or -1 becomes 4294967295 and
	   the bounds test above passes on the way to a write far outside the
	   buffer. */
	CHECK_FALSE(batch.Set(Vector3(-1.f, 0.f, 0.f), k_uiStone, 1));
	CHECK_FALSE(batch.Set(Vector3(0.f, -1.f, 0.f), k_uiStone, 1));
	CHECK_FALSE(batch.Clear(Vector3(0.f, 0.f, -1.f)));

	CHECK_EQ(world.Hash(), uiBefore);
	CHECK_EQ(batch.GetWrites(), 0u);
	CHECK_EQ(batch.GetRejected(), 6u);
}

/* A NaN passes every comparison in a rejection test, so a check written as
   "reject if outside" lets it straight through - which is how this tree once
   wrote two billion elements past the voxel buffer. */
VOXAGINE_CHECK(VoxelEditBatch, RejectsNonFinitePositions)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelEditBatch batch(world.MakeEditTarget());

	const uint64_t uiBefore = world.Hash();

	const float fNaN = std::numeric_limits<float>::quiet_NaN();
	const float fInf = std::numeric_limits<float>::infinity();

	CHECK_FALSE(batch.Set(Vector3(fNaN, 1.f, 1.f), k_uiStone, 1));
	CHECK_FALSE(batch.Set(Vector3(1.f, fNaN, 1.f), k_uiStone, 1));
	CHECK_FALSE(batch.Set(Vector3(1.f, 1.f, fNaN), k_uiStone, 1));
	CHECK_FALSE(batch.Set(Vector3(fInf, 1.f, 1.f), k_uiStone, 1));
	CHECK_FALSE(batch.Clear(Vector3(-fInf, 1.f, 1.f)));

	CHECK_EQ(world.Hash(), uiBefore);
	CHECK_EQ(batch.GetNonFiniteRejections(), 5u);
}

VOXAGINE_CHECK(VoxelEditBatch, RegistersOwnerlessSetsAsLooseVoxels)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	RecordingLooseVoxelSink sink;
	VoxelEditBatch batch(world.MakeEditTarget(&sink));

	/* Baked debris: a colour with no owner. Nothing submits an AABB proxy for
	   it, so the registry is the only thing that gets it drawn. */
	batch.Set(Vector3(4.f, 5.f, 6.f), k_uiStone, VoxelOwnerVolume::k_uiNoOwnerSlot);
	REQUIRE_EQ(sink.Registered().size(), 1u);
	CHECK_EQ(sink.Registered()[0], Vector3(4.f, 5.f, 6.f));

	/* An owned voxel has a renderer submitting a proxy for it already. */
	batch.Set(Vector3(7.f, 5.f, 6.f), k_uiStone, 9);
	CHECK_EQ(sink.Registered().size(), 1u);

	/* And a clear writes nothing to draw. */
	batch.Clear(Vector3(4.f, 5.f, 6.f));
	CHECK_EQ(sink.Registered().size(), 1u);
}

VOXAGINE_CHECK(VoxelEditBatch, ClearRegionClampsToTheWindow)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillBox(UVector3(0, 0, 0), k_v3Size, k_uiStone, 1);

	VoxelEditBatch batch(world.MakeEditTarget());

	/* Straddles the far corner. The part inside the window must be cleared and
	   the rest must simply not happen. */
	const uint32_t uiCleared = batch.ClearRegion(UVector3(44, 20, 44), UVector3(16, 16, 16));

	CHECK_EQ(uiCleared, 4u * 4u * 4u);
	CHECK_FALSE(world.Grid().GetCell(47, 23, 47).IsActive());
	CHECK_TRUE(world.Grid().GetCell(43, 23, 47).IsActive());
	CHECK_EQ(world.Validate(), 0u);
}

VOXAGINE_CHECK(VoxelEditBatch, ReportsTheBricksItTouched)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelEditBatch batch(world.MakeEditTarget());

	CHECK_TRUE(batch.GetDirtyBricks().empty());

	/* Two writes in one brick and one in another - the set must be
	   deduplicated, because phase 2 seeds integrity from it and phase 4 feeds
	   the connectivity graph from it. */
	batch.Set(Vector3(1.f, 1.f, 1.f), k_uiStone, 1);
	batch.Set(Vector3(2.f, 2.f, 2.f), k_uiStone, 1);
	batch.Set(Vector3(20.f, 1.f, 1.f), k_uiStone, 1);

	const std::vector<uint32_t>& dirty = batch.GetDirtyBricks();

	REQUIRE_EQ(dirty.size(), 2u);
	CHECK_LT(dirty[0], dirty[1]);
	CHECK_EQ(dirty[0], world.Bricks().VoxelToBrick(world.VoxelID(1, 1, 1)));
	CHECK_EQ(dirty[1], world.Bricks().VoxelToBrick(world.VoxelID(20, 1, 1)));
}

/* The strongest statement available about the batch: run one edit script
   through it and the same script through the harness's deliberately naive
   reference writer, and the two worlds must be indistinguishable in every
   representation. That says the batch agrees with an independent
   implementation, rather than with its own previous self the way a recorded
   golden hash would. */
VOXAGINE_CHECK(VoxelEditBatch, AgreesWithTheReferenceWriter)
{
	VoxelWorldHarness reference(k_v3Size, k_v3ChunkSize);
	VoxelWorldHarness batched(k_v3Size, k_v3ChunkSize);

	auto script = [](auto&& set, auto&& clear)
	{
		for (uint32_t uiZ = 0; uiZ < k_v3Size.z; ++uiZ)
		for (uint32_t uiX = 0; uiX < k_v3Size.x; ++uiX)
			set(uiX, 0u, uiZ, 0xFF404040u, static_cast<uint16_t>(0));

		for (uint32_t uiZ = 8; uiZ < 32; ++uiZ)
		for (uint32_t uiY = 1; uiY < 18; ++uiY)
		for (uint32_t uiX = 8; uiX < 32; ++uiX)
			set(uiX, uiY, uiZ, 0xFF906030u, static_cast<uint16_t>(1 + ((uiX + uiZ) % 5)));

		/* A sphere out of the middle, which is the shape destruction makes. */
		for (int32_t iZ = 10; iZ < 30; ++iZ)
		for (int32_t iY = 2; iY < 16; ++iY)
		for (int32_t iX = 10; iX < 30; ++iX)
		{
			const int32_t iDX = iX - 20;
			const int32_t iDY = iY - 9;
			const int32_t iDZ = iZ - 20;

			if (iDX * iDX + iDY * iDY + iDZ * iDZ <= 36)
				clear(static_cast<uint32_t>(iX), static_cast<uint32_t>(iY), static_cast<uint32_t>(iZ));
		}

		/* Then debris landing back into the hole, unowned. */
		for (uint32_t i = 0; i < 32; ++i)
			set(14u + (i % 12), 2u + (i % 7), 14u + ((i * 5) % 12), 0xFFA0A0A0u, static_cast<uint16_t>(0));
	};

	script(
		[&reference](uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor, uint16_t uiSlot)
		{
			reference.Set(uiX, uiY, uiZ, uiColor, uiSlot);
		},
		[&reference](uint32_t uiX, uint32_t uiY, uint32_t uiZ) { reference.Clear(uiX, uiY, uiZ); });

	{
		VoxelEditBatch batch(batched.MakeEditTarget());

		script(
			[&batch](uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor, uint16_t uiSlot)
			{
				batch.Set(Vector3(uiX, uiY, uiZ), uiColor, uiSlot);
			},
			[&batch](uint32_t uiX, uint32_t uiY, uint32_t uiZ)
			{
				batch.Clear(Vector3(uiX, uiY, uiZ));
			});
	}

	CHECK_EQ(batched.CountOccupied(), reference.CountOccupied());
	CHECK_EQ(batched.Validate(), 0u);
	CHECK_EQ(batched.Hash(), reference.Hash());
}

/* Phase 1 detaches a chunk's storage on the main thread before the job that
   frees it runs (ledger P7), so "not resident" is an ordinary state that every
   reader and writer has to survive rather than an impossible one. */
VOXAGINE_CHECK(VoxelEditBatch, ADetachedChunkAcceptsNoWrites)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	VoxelEditBatch batch(world.MakeEditTarget());

	REQUIRE_TRUE(batch.Set(Vector3(20.f, 1.f, 4.f), k_uiStone, 1));

	/* Chunk (1, 0) covers x in [16, 32). */
	world.Grid().SetChunkStorage(UVector2(1, 0), nullptr, nullptr);

	CHECK_FALSE(static_cast<bool>(world.Grid().GetCell(20, 1, 4)));
	CHECK_FALSE(world.Grid().GetCell(20, 1, 4).IsActive());
	CHECK_EQ(world.Grid().GetVoxel(20, 1, 4), nullptr);

	CHECK_FALSE(batch.Set(Vector3(20.f, 1.f, 4.f), k_uiWood, 2));
	CHECK_FALSE(batch.Clear(Vector3(20.f, 1.f, 4.f)));

	/* The chunk beside it is untouched by the detach. */
	CHECK_TRUE(batch.Set(Vector3(4.f, 1.f, 4.f), k_uiWood, 2));
}
