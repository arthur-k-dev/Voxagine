#include "Framework/Check.h"

#include "Harness/VoxModelFile.h"
#include "Harness/VoxelWorldHarness.h"

#include "Core/ECS/Systems/Rendering/VoxelStamp.h"

#include <cstring>
#include <string>
#include <vector>

/* The resumable stamp, checked where it is cheap to check.
 *
 * CHUNK_STREAMING_PLAN.md phase 9. Splitting one renderer's stamp across frames
 * was built once, took the worst transition frame from 27.9 ms to 9.5 ms, and
 * was reverted because the level it drew was 580,000 voxels short - a deficit
 * that grew as the slices got finer, which is the signature of state that is not
 * carried across a slice boundary. The cause was never found in the game, where
 * one run costs a minute and the symptom is geometry that reads as content.
 *
 * ForEachStampedVoxelRange needs a model and nothing else, so the question the
 * game could not answer - does resuming produce the same voxels? - is a check
 * that runs in milliseconds and names the sample it diverged on.
 */

namespace
{
	struct Emitted
	{
		Vector3 v3Position = Vector3(0.f);
		uint32_t uiColor = 0;

		/* Compared by value, not by float equality: the two walks must emit the
		   *same bits*, and a NaN component is never equal to itself. If a pose
		   ever produces a non-finite position again, this reports "identical"
		   for identical output instead of failing with two lines that read the
		   same. */
		bool operator==(const Emitted& other) const
		{
			return uiColor == other.uiColor &&
				std::memcmp(&v3Position, &other.v3Position, sizeof(Vector3)) == 0;
		}
	};

	std::vector<Emitted> WalkWhole(const VoxelStampVoxels& voxels, uint32_t uiTag,
	                               const uint32_t* pOverride, const VoxelStampTransform& stamp)
	{
		std::vector<Emitted> out;

		ForEachStampedVoxel(voxels, uiTag, pOverride, stamp,
			[&](const Vector3& v3Position, uint32_t uiColor)
			{
				out.push_back({ v3Position, uiColor });
			});

		return out;
	}

	/* The same walk driven uiSamples at a time, restarting from the cursor each
	   round exactly as a frame boundary does. */
	std::vector<Emitted> WalkSliced(const VoxelStampVoxels& voxels, uint32_t uiTag,
	                                const uint32_t* pOverride, const VoxelStampTransform& stamp,
	                                uint32_t uiSamples, uint32_t& o_uiSlices)
	{
		std::vector<Emitted> out;

		VoxelStampCursor cursor;

		o_uiSlices = 0;

		while (true)
		{
			++o_uiSlices;

			const bool bComplete = ForEachStampedVoxelRange(voxels, uiTag, pOverride, stamp, cursor, uiSamples,
				[&](const Vector3& v3Position, uint32_t uiColor)
				{
					out.push_back({ v3Position, uiColor });
				});

			if (bComplete)
				break;

			/* A budget that makes no progress would spin here forever, which is
			   worth failing on rather than hanging on. */
			if (o_uiSlices > 4000000u)
				break;
		}

		return out;
	}

	/* A model with structure the stamp's own quirks can trip over: solid runs
	   (so the duplicate suppression fires under a scale fill), a one-voxel
	   feature, and a voxel at the model's origin. Positions are packed exactly
	   the way VoxModel packs them, sorted the way it sorts them. */
	class SyntheticModel
	{
	public:
		SyntheticModel()
		{
			for (uint32_t uiZ = 0; uiZ < 5; ++uiZ)
			for (uint32_t uiY = 0; uiY < 7; ++uiY)
			for (uint32_t uiX = 0; uiX < 4; ++uiX)
			{
				if (uiY > 3 && (uiX != 1 || uiZ != 1))
					continue;

				Add(uiX, uiY, uiZ, 0xFF000000u | (uiX << 16) | (uiY << 8) | uiZ);
			}
		}

		VoxelStampModel Describe() const
		{
			VoxelStampModel model;

			model.v3FittedSize = Vector3(4.f, 7.f, 5.f);
			model.v3FirstFittedSize = model.v3FittedSize;

			return model;
		}

		VoxelStampVoxels Voxels() const
		{
			VoxelStampVoxels voxels;

			voxels.pPositions = m_Positions.data();
			voxels.pColors = m_Colors.data();
			voxels.uiCount = static_cast<uint32_t>(m_Positions.size());

			return voxels;
		}

	private:
		void Add(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor)
		{
			m_Positions.push_back(VColor(
				static_cast<unsigned char>(uiX),
				static_cast<unsigned char>(uiY),
				static_cast<unsigned char>(uiZ),
				static_cast<unsigned char>(0)).inst.Color);

			m_Colors.push_back(uiColor);
		}

		std::vector<uint32_t> m_Positions;
		std::vector<uint32_t> m_Colors;
	};

	/* Poses chosen for what they do to the walk rather than for realism: an
	   axis-aligned one, a quantized rotation, a scale above one (which is the
	   only thing that makes the inner three loops run more than once, and so the
	   only thing that puts a slice boundary *inside* a model voxel), a negative
	   scale, and a fractional position that lands the origin off the lattice. */
	std::vector<VoxelStampPose> Poses()
	{
		std::vector<VoxelStampPose> poses;

		VoxelStampPose pose;
		pose.v3Position = Vector3(40.f, 20.f, 40.f);
		poses.push_back(pose);

		pose.Rotation = Quaternion(Vector3(0.f, 37.f, 0.f) * DEG2RAD);
		pose.fRotationLimitDegrees = 45.f;
		poses.push_back(pose);

		pose.Rotation = Quaternion(Vector3(90.f, 180.f, 90.f) * DEG2RAD);
		poses.push_back(pose);

		pose.fRotationLimitDegrees = 0.f;
		pose.Rotation = Quaternion(Vector3(11.f, 23.f, 47.f) * DEG2RAD);
		pose.v3Scale = Vector3(2.f, 3.f, 2.f);
		poses.push_back(pose);

		pose.v3Scale = Vector3(-2.f, 1.f, 1.5f);
		poses.push_back(pose);

		pose.v3Scale = Vector3(1.f);
		pose.v3Position = Vector3(40.3f, 19.6f, 40.9f);
		pose.Rotation = Quaternion();
		poses.push_back(pose);

		return poses;
	}

	const uint32_t k_uiTag = VoxelStateTag(RS_DEFAULT, false);
}

/* The acceptance criterion, at the one place it can be stated exactly: the
   sliced walk and the unbounded walk emit the same (position, colour) sequence,
   at every budget, for every pose. Budget 1 is the interesting one - every
   sample is a resumption - and it is also the cheapest to reason about when it
   fails. */
VOXAGINE_CHECK(VoxelStamp, SlicingAStampEmitsTheIdenticalVoxelSequence)
{
	const SyntheticModel model;
	const VoxelStampVoxels voxels = model.Voxels();

	const uint32_t uiOverride = 0xFF204080u;

	for (const VoxelStampPose& pose : Poses())
	for (uint32_t uiOverrideCase = 0; uiOverrideCase < 2; ++uiOverrideCase)
	{
		const uint32_t* pOverride = uiOverrideCase == 1 ? &uiOverride : nullptr;

		VoxelStampTransform stamp;
		ComputeVoxelStampTransform(model.Describe(), pose, Vector3(0.f), 1.f, stamp);

		const std::vector<Emitted> whole = WalkWhole(voxels, k_uiTag, pOverride, stamp);

		REQUIRE_TRUE(!whole.empty())
			<< "the pose emits nothing, so it proves nothing";

		for (uint32_t uiBudget : { 1u, 2u, 3u, 7u, 64u, 1000u })
		{
			uint32_t uiSlices = 0;
			const std::vector<Emitted> sliced = WalkSliced(voxels, k_uiTag, pOverride, stamp, uiBudget, uiSlices);

			CHECK_EQ(sliced.size(), whole.size())
				<< "budget " << uiBudget << " emitted a different number of voxels";

			const size_t uiCommon = sliced.size() < whole.size() ? sliced.size() : whole.size();

			for (size_t i = 0; i < uiCommon; ++i)
			{
				if (sliced[i] == whole[i])
					continue;

				CHECK_TRUE(false)
					<< "budget " << uiBudget << " diverged at emitted voxel " << i
					<< ": (" << sliced[i].v3Position.x << ", " << sliced[i].v3Position.y << ", "
					<< sliced[i].v3Position.z << ") colour " << sliced[i].uiColor
					<< " against (" << whole[i].v3Position.x << ", " << whole[i].v3Position.y << ", "
					<< whole[i].v3Position.z << ") colour " << whole[i].uiColor;

				break;
			}
		}
	}
}

/* A budget of one is only a single-step sweep if it really does stop after one
   sample. If a slice could run long, every equivalence above would be measuring
   a coarser split than it claims to. */
VOXAGINE_CHECK(VoxelStamp, ASliceRunsExactlyItsBudgetOfSamples)
{
	const SyntheticModel model;
	const VoxelStampVoxels voxels = model.Voxels();

	VoxelStampPose pose;
	pose.v3Position = Vector3(40.f, 20.f, 40.f);
	pose.v3Scale = Vector3(2.f, 2.f, 2.f);

	VoxelStampTransform stamp;
	ComputeVoxelStampTransform(model.Describe(), pose, Vector3(0.f), 1.f, stamp);

	/* Samples, not emitted voxels: the scale fill above makes most samples
	   duplicates, and the cost is per sample. */
	const uint32_t uiSamples = voxels.uiCount * 8;

	uint32_t uiSlices = 0;
	WalkSliced(voxels, k_uiTag, nullptr, stamp, 1u, uiSlices);

	/* One slice per sample and no more: the call that runs the last sample is
	   the one that reports the model finished, so a resumable stamp costs no
	   extra frame at the end of a renderer. */
	CHECK_EQ(uiSlices, uiSamples)
		<< "a budget of one sample did not advance one sample at a time";
}

/* The check the game version of this needs and cannot afford: a real model, at a
   real level transform, resumed at every cursor value. Sorted voxels and a
   1-voxel-thick feature are what make the duplicate suppression matter, and both
   are properties of shipped art rather than of anything a synthetic model would
   have by accident. */
VOXAGINE_CHECK(VoxelStamp, ARealModelSurvivesResumptionAtEverySample)
{
	/* The thin one and the big one. The bamboo is three stalks a voxel thick
	   (CLAUDE.md, "The floating bamboo"), so almost nothing about it is a solid
	   run; the riverbed is the model phase 5 measured at 140,640 voxels and
	   22 ms, which is the entire reason this phase exists. */
	const char* paths[] = {
		"Content/Enviourment_Models/Obstacle_Pillar_Small_Tall_Bamboo_Parts/Obstacle_Pillar_Small_Tall_Bamboo_4.vox",
		"Content/Enviourment_Models/Riverbed.vox",
	};

	for (const char* pPath : paths)
	{
		VoxModelFile model;

		if (!model.LoadFromContent(pPath))
		{
			CHECK_TRUE(false) << "could not load " << pPath << ": " << model.Error();
			continue;
		}

		REQUIRE_TRUE(model.Count() > 0) << pPath << " has no voxels";

		VoxelStampPose pose;
		pose.v3Position = Vector3(120.f, 15.f, 88.f);
		pose.Rotation = Quaternion(Vector3(0.f, 90.f, 0.f) * DEG2RAD);
		pose.fRotationLimitDegrees = 45.f;

		VoxelStampTransform stamp;
		ComputeVoxelStampTransform(model.Describe(), pose, Vector3(0.f), 1.f, stamp);

		const std::vector<Emitted> whole = WalkWhole(model.Voxels(), k_uiTag, nullptr, stamp);

		REQUIRE_TRUE(!whole.empty()) << pPath << " stamps nothing at this pose";

		for (uint32_t uiBudget : { 1u, 5u, 97u, 8192u })
		{
			uint32_t uiSlices = 0;
			const std::vector<Emitted> sliced = WalkSliced(model.Voxels(), k_uiTag, nullptr, stamp, uiBudget, uiSlices);

			CHECK_EQ(sliced.size(), whole.size())
				<< pPath << " at budget " << uiBudget << " lost or gained voxels";

			const size_t uiCommon = sliced.size() < whole.size() ? sliced.size() : whole.size();

			for (size_t i = 0; i < uiCommon; ++i)
			{
				if (sliced[i] == whole[i])
					continue;

				CHECK_TRUE(false) << pPath << " at budget " << uiBudget
					<< " diverged at emitted voxel " << i;
				break;
			}
		}
	}
}

/* Every voxel the walk emits is inside the box the AABB proxy is built from -
   which is what stops a sliced stamp from writing voxels no proxy covers, and is
   the half of "four ways a voxel goes missing" that a partial stamp could
   plausibly reopen. */
VOXAGINE_CHECK(VoxelStamp, TheStampedBoxContainsEveryStampedVoxel)
{
	const SyntheticModel model;

	for (const VoxelStampPose& pose : Poses())
	{
		VoxelStampTransform stamp;
		ComputeVoxelStampTransform(model.Describe(), pose, Vector3(0.f), 1.f, stamp);

		Vector3 v3Min(0.f);
		Vector3 v3Max(0.f);

		ComputeStampedGridBounds(model.Describe().v3FittedSize, stamp, v3Min, v3Max);

		ForEachStampedVoxel(model.Voxels(), k_uiTag, nullptr, stamp,
			[&](const Vector3& v3Position, uint32_t)
			{
				CHECK_TRUE(
					v3Position.x >= v3Min.x && v3Position.x <= v3Max.x &&
					v3Position.y >= v3Min.y && v3Position.y <= v3Max.y &&
					v3Position.z >= v3Min.z && v3Position.z <= v3Max.z)
					<< "a stamped voxel is outside the box the proxy is built from";
			});
	}
}

/* The same statement one level up: a model stamped in slices leaves the *world*
   in the state a one-shot stamp leaves it in - every representation of it, not
   just the sequence of positions. The hash folds in the CPU colours, the mapped
   words, the occupancy bitmap, the brick counts and the owner slots, which is
   the six-representations rule the whole voxel write path owes.

   This is the harness half of phase 9's acceptance. The other half is the
   in-game occupancy count, and it needs a GPU and a minute; this needs neither
   and fails with the slice size that broke it. */
VOXAGINE_CHECK(VoxelStamp, StampingAModelInSlicesLeavesTheSameWorld)
{
	VoxModelFile model;

	const char* pPath = "Content/Enviourment_Models/Riverbed.vox";

	if (!model.LoadFromContent(pPath))
	{
		CHECK_TRUE(false) << "could not load " << pPath << ": " << model.Error();
		return;
	}

	const UVector3 k_v3Size(128, 64, 128);
	const UVector3 k_v3ChunkSize(64, 64, 64);

	VoxelStampPose pose;
	pose.v3Position = Vector3(64.f, 32.f, 64.f);
	pose.Rotation = Quaternion(Vector3(0.f, 45.f, 0.f) * DEG2RAD);
	pose.fRotationLimitDegrees = 45.f;

	VoxelWorldHarness whole(k_v3Size, k_v3ChunkSize);
	const uint32_t uiDroppedWhole = whole.StampModel(model, pose, 3u);

	REQUIRE_TRUE(whole.CountOccupied() > 0) << "the model stamps nothing into the harness";
	CHECK_EQ(whole.Validate(), 0u) << "the one-shot stamp left the representations disagreeing";

	for (uint32_t uiSlice : { 1u, 37u, 8192u })
	{
		VoxelWorldHarness sliced(k_v3Size, k_v3ChunkSize);
		const uint32_t uiDroppedSliced = sliced.StampModel(model, pose, 3u, true, uiSlice);

		CHECK_EQ(sliced.Hash(), whole.Hash())
			<< "a stamp sliced at " << uiSlice << " samples left a different world";

		CHECK_EQ(sliced.CountOccupied(), whole.CountOccupied())
			<< "a stamp sliced at " << uiSlice << " samples wrote a different number of voxels";

		CHECK_EQ(uiDroppedSliced, uiDroppedWhole)
			<< "a stamp sliced at " << uiSlice << " samples dropped a different number of voxels";

		CHECK_EQ(sliced.Validate(), 0u)
			<< "a stamp sliced at " << uiSlice << " samples left the representations disagreeing";
	}
}
