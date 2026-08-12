#include "Framework/Check.h"
#include "Harness/StreamingHarness.h"

#include "Core/ECS/Entities/Camera.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"

/* What phase 1 of CHUNK_STREAMING_PLAN.md claims, asserted.
 *
 * The claim is R2: a window slide publishes physics volumes, world offset,
 * buffer swap and camera together, in one main-thread transaction, exactly
 * once - and everything before it is preparation nobody can observe. None of
 * that was expressible before this harness existed, which is why it was a
 * comment on master rather than a test.
 *
 * The fixture is a 4x4 chunk level of 32-voxel chunks, so the resident window
 * is 96x32x96 and a one-column slide replaces three chunks. Small enough that a
 * failure names a two-entity chunk; real enough that it is the shipping
 * ChunkSystem, the shipping VoxelGrid and the shipping RLE codec doing it.
 */

namespace
{
	/* The camera is the chunk system's only input. The fixture is 5x5 chunks of
	   32 voxels, so a camera in chunk column N puts the 3x3 window at world
	   offset (N - 1) * 32, clamped to 0..64 - three distinct window positions
	   along x, which is the fewest that can express a walk-back. */
	Vector3 CameraInColumn(uint32_t uiColumn)
	{
		return Vector3(uiColumn * 32.f + 16.f, 0.f, 16.f);
	}

	const Vector3 k_v3AtOffsetZero = CameraInColumn(1);
	const Vector3 k_v3AtOffset32 = CameraInColumn(2);
	const Vector3 k_v3AtOffset64 = CameraInColumn(3);

	uint64_t Commits() { return StreamingCounters::Get().Commits.load(); }

	/* Every check owes these: they are the T5 invariants, counted rather than
	   only asserted so that a Release run says the same thing a Debug one does. */
	void CheckNoInvariantViolations(CheckContext& ctx)
	{
		CHECK_EQ(StreamingCounters::Get().BackBufferFlushRaces.load(), 0u)
			<< "a main-thread flush walked the back buffer while the render job owned it";
		CHECK_EQ(StreamingCounters::Get().PublishesOutsideCommit.load(), 0u)
			<< "the window was published from outside the commit transaction";
	}
}

VOXAGINE_CHECK(Streaming, TheInitialWindowIsResidentBeforeAnythingTicks)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");

	/* ChunkSystem::Start loads the initial 3x3 synchronously, so this is true
	   before a single frame has run. It is also what R1 will lean on in phase
	   3: holding the first gameplay tick is only cheap because the window is
	   already there. */
	CHECK_EQ(harness.ResidentChunkCount(), 9u);
	CHECK_EQ(harness.Grid().GetWorldOffset(), Vector3(0.f, 0.f, 0.f));

	/* One chunk's ground layer, not nine - and that is master's behaviour, not
	   the harness's. ChunkSystem::Start pushes a chunk into the window only
	   where it is a *move*; the eight it loads outright reach the window when
	   something next republishes them. In the game the gap is covered by
	   RenderSystem::Start, which writes the whole window's y = 0 row itself
	   (RenderSystem::SetGroundPlane), and there is no RenderSystem here. Worth
	   stating as an expectation rather than a puzzle: a slide below turns this
	   into all nine. */
	CHECK_EQ(harness.Window().CountOccupiedFront(), 32ull * 32ull);

	/* The initial load is not a commit - no group published anything. */
	CHECK_EQ(Commits(), 0u);
	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, ASlidePublishesTheWindowExactlyOnce)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle()) << "the update group never drained";

	CHECK_EQ(Commits(), 1u);
	CHECK_EQ(StreamingCounters::Get().CommittedGroups.load(), 1u);
	CHECK_EQ(harness.Window().SwapCount(), 1u)
		<< "the window was swapped a different number of times than it was committed";

	CHECK_EQ(harness.Grid().GetWorldOffset(), Vector3(32.f, 0.f, 0.f));
	CHECK_EQ(harness.ResidentChunkCount(), 9u);

	/* Every resident chunk's ground row is in the published window now: the
	   render job rewrote all nine slices of the back buffer, which is the
	   difference between a slide and the initial load above. */
	CHECK_EQ(harness.Window().CountOccupiedFront(), 9ull * 32ull * 32ull);

	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, TheWindowMovesInOneFrameOrNotAtAll)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	const Vector3 v3OldOffset = harness.Grid().GetWorldOffset();

	harness.PlaceCamera(k_v3AtOffset32);

	/* The probe R2 is worth having: step frame by frame and check that on every
	   frame before the commit *nothing* has moved, and that on the one frame
	   the commit counter increments, all of it has. A transition that published
	   the offset a frame before the swap - which is what master's callback did
	   relative to entity loading - fails the first half of this. */
	bool bSawCommit = false;

	for (uint32_t uiFrame = 0; uiFrame < 20000 && !bSawCommit; ++uiFrame)
	{
		harness.Frame();

		if (Commits() == 0)
		{
			CHECK_EQ(harness.Grid().GetWorldOffset(), v3OldOffset)
				<< "the world offset moved before the commit";
			CHECK_EQ(harness.Window().SwapCount(), 0u)
				<< "the window was swapped before the commit";
			CHECK_EQ(harness.MainCamera().GetCameraOffset(), v3OldOffset)
				<< "the camera offset moved before the commit";
			continue;
		}

		bSawCommit = true;

		CHECK_EQ(harness.Grid().GetWorldOffset(), Vector3(32.f, 0.f, 0.f));
		CHECK_EQ(harness.MainCamera().GetCameraOffset(), Vector3(32.f, 0.f, 0.f));
		CHECK_EQ(harness.Window().SwapCount(), 1u);
	}

	CHECK_TRUE(bSawCommit) << "the slide never committed";

	REQUIRE_TRUE(harness.Settle());
	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, TheOccupancyHierarchyAgreesWithTheWindowAfterASlide)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	/* The whole point of the ground-only fast path and the row memcpy is that
	   they are supposed to leave the counts saying exactly what the words say.
	   Validate recomputes every level from the words and reports the
	   disagreements, which is the only check here that does not depend on
	   knowing what the answer should be. */
	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u)
		<< "the occupancy pyramid disagrees with the published window";

	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, AWalkBackCancelsTheGroupItOvertakes)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	const uint32_t uiMarkersAtRest = harness.CountEntitiesNamed("Marker_");

	/* R5. Two boundaries crossed in quick succession queue two groups; turning
	   round to the first one's window erases the second before it publishes
	   anything. That is the only shape in which master's group list actually
	   cancels, and getting there needs three window positions - which is why the
	   fixture is 5x5 rather than the smallest thing that streams.

	   The intermediate frames are deliberately single: a group that has already
	   drained cannot be the one overtaken. */
	harness.PlaceCamera(k_v3AtOffset32);
	harness.Frame();
	harness.PlaceCamera(k_v3AtOffset64);
	harness.Frame();
	harness.PlaceCamera(k_v3AtOffset32);

	REQUIRE_TRUE(harness.Settle()) << "the state machine wedged after a walk-back";

	CHECK_TRUE(StreamingCounters::Get().CancelledGroups.load() >= 1u)
		<< "nothing was actually cancelled, so this check proves nothing";

	/* Whatever was cancelled, the terminal state has to be the one the camera
	   asks for: the window at offset 32, nine resident chunks, every chunk's
	   roots present exactly once, and the occupancy agreeing with the words. */
	CHECK_EQ(harness.Grid().GetWorldOffset(), Vector3(32.f, 0.f, 0.f));
	CHECK_EQ(harness.ResidentChunkCount(), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Marker_"), uiMarkersAtRest);
	CHECK_EQ(harness.Window().CountOccupiedFront(), 9ull * 32ull * 32ull);
	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);

	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, ASlideAndBackLeavesTheWindowWhereItStarted)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	const uint32_t uiMarkersAtRest = harness.CountEntitiesNamed("Marker_");

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	/* Two commits, two swaps: out and back. An extra of either is a window
	   published twice for one transition, which is exactly what R2 forbids. */
	CHECK_EQ(Commits(), 2u);
	CHECK_EQ(harness.Window().SwapCount(), 2u);

	CHECK_EQ(harness.Grid().GetWorldOffset(), Vector3(0.f, 0.f, 0.f));
	CHECK_EQ(harness.ResidentChunkCount(), 9u);

	/* The three chunks that left were serialized out and deserialized back, so
	   an entity duplicated or dropped by that round trip shows up here. */
	CHECK_EQ(harness.CountEntitiesNamed("Marker_"), uiMarkersAtRest);
	CHECK_EQ(harness.Window().CountOccupiedFront(), 9ull * 32ull * 32ull);
	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);

	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, AChunkThatLeavesAndComesBackKeepsItsVoxels)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	/* Window slot (0, *, 0) is chunk column 0, which is exactly what leaves
	   when the window slides one column right. Write a colour the ground never
	   has, so the assertion cannot pass by accident. */
	const uint32_t uiMark = 0xAB123456u;

	{
		const VoxelCell cell = harness.Grid().GetCell(5, 4, 5);
		REQUIRE_TRUE(cell) << "the marked cell is not resident to begin with";
		cell.SetColor(uiMark);
	}

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	/* It has been through EncodeVoxels on the way out and DecodeVoxels on the
	   way back, and RenderChunk has republished it into the window. Both halves
	   matter: the RLE preserving it, and the slide putting it back on screen. */
	const VoxelCell cell = harness.Grid().GetCell(5, 4, 5);
	REQUIRE_TRUE(cell) << "the chunk did not come back";
	CHECK_EQ(cell.GetColor(), uiMark) << "the voxel RLE lost a voxel across an unload";

	const UVector3 v3Size = harness.WindowSize();
	const uint32_t uiWordID = 5 + 4 * v3Size.x + 5 * v3Size.x * v3Size.y;

	CHECK_EQ(harness.Window().FrontWords()[uiWordID], uiMark)
		<< "the returning chunk's voxels never reached the published window";

	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);
	CheckNoInvariantViolations(ctx);
}
