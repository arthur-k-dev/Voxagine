#include <gtest/gtest.h>

#include <algorithm>
#include <limits>
#include <vector>

#include "Core/ECS/Systems/Physics/IntegrityChecker.h"
#include "Core/Voxels/SphericalDestruction.h"
#include "Harness/VoxelWorldHarness.h"

namespace
{
	const UVector3 k_v3Size(64, 32, 64);
	const UVector3 k_v3ChunkSize(32, 32, 32);

	const uint32_t k_uiStone = 0xFF808080u;

	/* Destroys everything and counts what came back. */
	struct Recorder
	{
		std::vector<Vector3> positions;
		std::vector<uint32_t> colours;

		void operator()(const Vector3& v3Position, uint32_t uiColor)
		{
			positions.push_back(v3Position);
			colours.push_back(uiColor);
		}
	};

	SphericalDestruction::Result Blow(VoxelWorldHarness& world, const Vector3& v3Center, float fRadius,
	                                  Recorder& recorder)
	{
		VoxelEditBatch batch(world.MakeEditTarget());

		return SphericalDestruction::Apply(
			batch, world.Grid(), v3Center, fRadius,
			[](uint16_t) { return true; },
			[&recorder](const Vector3& v3Position, uint32_t uiColor) { recorder(v3Position, uiColor); });
	}
}

/* Ledger D3. The shape is the whole contract: every occupied voxel whose centre
   is within the radius goes, and nothing else does. The old loop carried a
   running position with row-wrap corrections that disagreed with the flat index
   into the fetched box, so on a wrap the cleared voxel, the sphere test and the
   voxel being read were three different cells. */
TEST(SphericalDestruction, ClearsExactlyTheSphere)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillBox(UVector3(0, 1, 0), UVector3(k_v3Size.x, k_v3Size.y - 1, k_v3Size.z), k_uiStone, 1);

	Recorder recorder;
	const Vector3 v3Center(32.f, 16.f, 32.f);
	const float fRadius = 7.f;

	const SphericalDestruction::Result result = Blow(world, v3Center, fRadius, recorder);

	uint32_t uiExpected = 0;

	for (uint32_t uiZ = 0; uiZ < k_v3Size.z; ++uiZ)
	for (uint32_t uiY = 1; uiY < k_v3Size.y; ++uiY)
	for (uint32_t uiX = 0; uiX < k_v3Size.x; ++uiX)
	{
		const float fDX = static_cast<float>(uiX) - v3Center.x;
		const float fDY = static_cast<float>(uiY) - v3Center.y;
		const float fDZ = static_cast<float>(uiZ) - v3Center.z;

		const bool bInside = fDX * fDX + fDY * fDY + fDZ * fDZ <= fRadius * fRadius;

		if (bInside)
			++uiExpected;

		EXPECT_EQ(world.Grid().GetCell(uiX, uiY, uiZ).IsActive(), !bInside)
			<< "at " << uiX << "," << uiY << "," << uiZ;
	}

	EXPECT_EQ(result.uiDestroyed, uiExpected);
	EXPECT_EQ(recorder.positions.size(), uiExpected);
	EXPECT_EQ(world.Validate(), 0u);
}

/* Ledger D4: the colour handed to the debris spawner comes from the CPU voxel.
   Reading it back out of the mapping is an uncached PCIe read of VRAM per
   destroyed voxel. */
TEST(SphericalDestruction, ReportsTheColourThatWasThere)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	for (uint32_t uiZ = 10; uiZ < 14; ++uiZ)
	for (uint32_t uiY = 10; uiY < 14; ++uiY)
	for (uint32_t uiX = 10; uiX < 14; ++uiX)
		world.Set(uiX, uiY, uiZ, 0xFF000000u | (uiX * 100 + uiY * 10 + uiZ), 1);

	Recorder recorder;
	Blow(world, Vector3(11.5f, 11.5f, 11.5f), 8.f, recorder);

	ASSERT_EQ(recorder.positions.size(), 4u * 4u * 4u);

	for (size_t i = 0; i < recorder.positions.size(); ++i)
	{
		const Vector3& v3 = recorder.positions[i];
		const uint32_t uiExpected = 0xFF000000u |
			(static_cast<uint32_t>(v3.x) * 100 + static_cast<uint32_t>(v3.y) * 10 + static_cast<uint32_t>(v3.z));

		EXPECT_EQ(recorder.colours[i], uiExpected);
	}
}

TEST(SphericalDestruction, NeverTouchesTheGroundLayer)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillGround(k_uiStone);
	world.FillBox(UVector3(28, 1, 28), UVector3(8, 8, 8), k_uiStone, 1);

	Recorder recorder;
	Blow(world, Vector3(32.f, 2.f, 32.f), 12.f, recorder);

	for (uint32_t uiZ = 0; uiZ < k_v3Size.z; ++uiZ)
	for (uint32_t uiX = 0; uiX < k_v3Size.x; ++uiX)
		EXPECT_TRUE(world.Grid().GetCell(uiX, 0, uiZ).IsActive()) << "ground at " << uiX << "," << uiZ;
}

/* Ledger D2. A negative or non-finite radius used to reach `(uint32_t)fRadius *
   2` and `diameter^3`, which is undefined for the first and overflows uint32_t
   above 1625 for the second - after two heap allocations had already been made
   (D1). */
TEST(SphericalDestruction, RejectsNonsenseRadii)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillBox(UVector3(0, 1, 0), UVector3(k_v3Size.x, k_v3Size.y - 1, k_v3Size.z), k_uiStone, 1);

	const uint64_t uiBefore = world.Hash();

	Recorder recorder;

	for (float fRadius : { -1.f, 0.f, 0.5f,
	                       std::numeric_limits<float>::quiet_NaN(),
	                       std::numeric_limits<float>::infinity() })
	{
		const SphericalDestruction::Result result = Blow(world, Vector3(32.f, 16.f, 32.f), fRadius, recorder);

		EXPECT_TRUE(result.bRejected) << "radius " << fRadius;
		EXPECT_EQ(result.uiDestroyed, 0u);
	}

	/* And a non-finite centre, which is the NaN transform this tree still
	   produces from somewhere. */
	const SphericalDestruction::Result nan = Blow(
		world, Vector3(std::numeric_limits<float>::quiet_NaN(), 16.f, 32.f), 4.f, recorder);

	EXPECT_TRUE(nan.bRejected);
	EXPECT_EQ(world.Hash(), uiBefore);
	EXPECT_TRUE(recorder.positions.empty());
}

TEST(SphericalDestruction, ClampsAnAbsurdRadius)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillBox(UVector3(0, 1, 0), UVector3(k_v3Size.x, k_v3Size.y - 1, k_v3Size.z), k_uiStone, 1);

	Recorder recorder;
	const SphericalDestruction::Result result = Blow(world, Vector3(32.f, 16.f, 32.f), 100000.f, recorder);

	EXPECT_TRUE(result.bRadiusClamped);
	EXPECT_FALSE(result.bRejected);

	/* Clamped, but still larger than this world, so everything above the ground
	   goes - the point is that it terminated rather than overflowing. */
	EXPECT_GT(result.uiDestroyed, 0u);
	EXPECT_EQ(world.Validate(), 0u);
}

TEST(SphericalDestruction, LeavesVoxelsTheOwnerProtects)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);

	world.FillBox(UVector3(24, 4, 24), UVector3(16, 16, 16), k_uiStone, 1);
	world.FillBox(UVector3(30, 8, 30), UVector3(4, 4, 4), k_uiStone, 7);

	VoxelEditBatch batch(world.MakeEditTarget());

	const SphericalDestruction::Result result = SphericalDestruction::Apply(
		batch, world.Grid(), Vector3(32.f, 10.f, 32.f), 20.f,
		[](uint16_t uiSlot) { return uiSlot != 7; },
		[](const Vector3&, uint32_t) {});

	EXPECT_EQ(result.uiProtected, 4u * 4u * 4u);

	for (uint32_t uiZ = 30; uiZ < 34; ++uiZ)
	for (uint32_t uiY = 8; uiY < 12; ++uiY)
	for (uint32_t uiX = 30; uiX < 34; ++uiX)
		EXPECT_TRUE(world.Grid().GetCell(uiX, uiY, uiZ).IsActive());

	EXPECT_FALSE(world.Grid().GetCell(25, 5, 25).IsActive());
	EXPECT_EQ(world.Validate(), 0u);
}

TEST(SphericalDestruction, SurvivesASphereOverTheWindowEdge)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillBox(UVector3(0, 1, 0), UVector3(k_v3Size.x, k_v3Size.y - 1, k_v3Size.z), k_uiStone, 1);

	Recorder recorder;

	/* Every corner and face, including one entirely outside. */
	Blow(world, Vector3(0.f, 1.f, 0.f), 6.f, recorder);
	Blow(world, Vector3(63.f, 31.f, 63.f), 6.f, recorder);
	Blow(world, Vector3(-20.f, 16.f, 32.f), 6.f, recorder);
	Blow(world, Vector3(32.f, 200.f, 32.f), 6.f, recorder);

	EXPECT_FALSE(world.Grid().GetCell(1, 1, 1).IsActive());
	EXPECT_FALSE(world.Grid().GetCell(62, 30, 62).IsActive());
	EXPECT_EQ(world.Validate(), 0u);
}

/* Ledger D6's producer half, and the scope is the whole point.
 *
 * The original rule pushed nine hashes for every destroyed voxel. The first
 * replacement swept the sphere's bounding box afterwards and seeded every
 * occupied voxel with an empty face neighbour - cheaper, more thorough, and
 * wrong, because it re-asks the integrity question about geometry the burst
 * never touched. A bullet clipping one voxel of rubble beside a building seeded
 * the building's whole surface, and a building that was never ground-connected
 * then converted to debris in one go.
 *
 * Support can only have changed for a voxel that lost a neighbour. */
TEST(SphericalDestruction, SeedsOnlyWhatLostANeighbour)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillGround(k_uiStone);

	/* Two structures. One gets clipped; the other is well outside the sphere
	   but comfortably inside the box the old rule would have swept. */
	world.FillBox(UVector3(30, 1, 30), UVector3(4, 8, 4), k_uiStone, 1);
	world.FillBox(UVector3(44, 1, 30), UVector3(4, 8, 4), k_uiStone, 2);

	Recorder recorder;
	std::vector<uint64_t> seeds;

	{
		VoxelEditBatch batch(world.MakeEditTarget());

		SphericalDestruction::Apply(
			batch, world.Grid(), Vector3(31.f, 4.f, 31.f), 3.f,
			[](uint16_t) { return true; },
			[&recorder](const Vector3& v3Position, uint32_t uiColor) { recorder(v3Position, uiColor); },
			&seeds);
	}

	ASSERT_GT(recorder.positions.size(), 0u);

	SphericalDestruction::FilterSeeds(world.Grid(), seeds);
	ASSERT_FALSE(seeds.empty());

	/* Every seed is occupied and touches the hole. */
	for (uint64_t uiSeed : seeds)
	{
		ASSERT_NE(uiSeed, IntegrityChecker::k_uiInvalidHash);

		const Vector3 v3 = IntegrityChecker::HashToPosition(uiSeed);

		EXPECT_TRUE(world.Grid().GetCell(
			static_cast<uint32_t>(v3.x), static_cast<uint32_t>(v3.y), static_cast<uint32_t>(v3.z)).IsActive());

		/* Nothing from the untouched structure, whose nearest voxel is 13 away
		   from the centre - well inside the radius+2 box the old sweep used. */
		EXPECT_LT(v3.x, 40.f) << "seeded a structure this burst never touched";
	}

	/* And the voxel directly above the hole - what the original rule seeded -
	   is still in the set, so nothing it found is lost. */
	bool bFoundAbove = false;

	for (uint64_t uiSeed : seeds)
	{
		const Vector3 v3 = IntegrityChecker::HashToPosition(uiSeed);

		if (v3.x == 31.f && v3.z == 31.f && v3.y > 4.f)
			bFoundAbove = true;
	}

	EXPECT_TRUE(bFoundAbove);
}

/* The candidates are gathered during the clear, so most of them name voxels the
   same burst goes on to destroy. Filtering happens once at the end. */
TEST(SphericalDestruction, SeedsInsideTheHoleAreFilteredOut)
{
	VoxelWorldHarness world(k_v3Size, k_v3ChunkSize);
	world.FillBox(UVector3(0, 1, 0), UVector3(k_v3Size.x, k_v3Size.y - 1, k_v3Size.z), k_uiStone, 1);

	Recorder recorder;
	std::vector<uint64_t> seeds;

	{
		VoxelEditBatch batch(world.MakeEditTarget());

		SphericalDestruction::Apply(
			batch, world.Grid(), Vector3(32.f, 16.f, 32.f), 6.f,
			[](uint16_t) { return true; },
			[&recorder](const Vector3& v3Position, uint32_t uiColor) { recorder(v3Position, uiColor); },
			&seeds);
	}

	const size_t uiCandidates = seeds.size();

	SphericalDestruction::FilterSeeds(world.Grid(), seeds);

	/* Twenty-six candidates per destroyed voxel - the checker walks
	   26-connectivity, so the seed set has to as well - of which only the shell
	   survives the filter. */
	EXPECT_EQ(uiCandidates, recorder.positions.size() * 26u);
	EXPECT_LT(seeds.size(), uiCandidates / 4u);

	for (uint64_t uiSeed : seeds)
	{
		const Vector3 v3 = IntegrityChecker::HashToPosition(uiSeed);

		EXPECT_TRUE(world.Grid().GetCell(
			static_cast<uint32_t>(v3.x), static_cast<uint32_t>(v3.y), static_cast<uint32_t>(v3.z)).IsActive());
	}
}

/* Ledger D11. Two open-coded copies of this hash both truncated float to
   uint16_t, so a negative coordinate wrapped to the far side of the world
   instead of being rejected. */
TEST(SphericalDestruction, TheSharedHashRejectsPositionsItCannotRepresent)
{
	EXPECT_EQ(IntegrityChecker::PositionToHash(Vector3(-1.f, 0.f, 0.f)), IntegrityChecker::k_uiInvalidHash);
	EXPECT_EQ(IntegrityChecker::PositionToHash(Vector3(0.f, -1.f, 0.f)), IntegrityChecker::k_uiInvalidHash);
	EXPECT_EQ(IntegrityChecker::PositionToHash(Vector3(0.f, 0.f, -0.5f)), IntegrityChecker::k_uiInvalidHash);
	EXPECT_EQ(IntegrityChecker::PositionToHash(Vector3(70000.f, 0.f, 0.f)), IntegrityChecker::k_uiInvalidHash);
	EXPECT_EQ(IntegrityChecker::PositionToHash(Vector3(std::numeric_limits<float>::quiet_NaN(), 0.f, 0.f)),
	          IntegrityChecker::k_uiInvalidHash);

	/* The two overloads have to agree, or the consolidation moved the answer. */
	EXPECT_EQ(IntegrityChecker::PositionToHash(Vector3(12.f, 34.f, 56.f)),
	          IntegrityChecker::PositionToHash(12u, 34u, 56u));

	EXPECT_EQ(IntegrityChecker::HashToPosition(IntegrityChecker::PositionToHash(12u, 34u, 56u)),
	          Vector3(12.f, 34.f, 56.f));
}
