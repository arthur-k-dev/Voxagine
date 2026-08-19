#include "Framework/Check.h"
#include "Harness/StreamingHarness.h"

#include "Core/ECS/Entity.h"
#include "Core/ECS/Entities/Camera.h"
#include "Core/ECS/Systems/Chunk/Chunk.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/Utils/DeterministicRandom.h"

/* Phase 2 of CHUNK_STREAMING_PLAN.md: unloading a chunk is bounded work.
 *
 * Master did the whole of it - find this chunk's entities, serialize every root
 * to JSON through RTTR, destroy them, then RLE-encode eight million voxels - in
 * one uninterrupted pass, the first two thirds inside the chunk render job's
 * completion callback and the last third on a worker that freed 48 MiB while
 * the main thread was trying to draw. It is two budgeted states now
 * (`US_START_UNLOADING`, `US_ENCODING`), and what these check is that the
 * budgets are real, that interrupting either of them is safe, and that the
 * result does not depend on where the interruption landed.
 *
 * Every scenario runs at `Units(1)` somewhere, which is T2's point: a slice
 * boundary between every single root and every single RLE run exercises *all*
 * the resumption points rather than the ones this machine happens to be fast
 * enough to reach.
 */

namespace
{
	Vector3 CameraInColumn(uint32_t uiColumn)
	{
		return Vector3(uiColumn * 32.f + 16.f, 0.f, 16.f);
	}

	const Vector3 k_v3AtOffsetZero = CameraInColumn(1);
	const Vector3 k_v3AtOffset32 = CameraInColumn(2);
	const Vector3 k_v3AtOffset64 = CameraInColumn(3);

	StreamingBudgets SingleStepBudgets()
	{
		StreamingBudgets budgets;
		budgets.UnloadSerialization = StreamingBudget::Units(1);
		budgets.VoxelEncoding = StreamingBudget::Units(1);

		return budgets;
	}

	/* The three roots of a chunk in world column 0 - the column that leaves when
	   the window slides one step right. */
	uint32_t ColumnZeroRoots(const StreamingHarness& harness)
	{
		return harness.CountEntitiesNamed("Marker_0_") +
			harness.CountEntitiesNamed("Deep_0_") +
			harness.CountEntitiesNamed("Prop_0_");
	}

	/* R5's terminal condition, and the only one worth asserting after a
	   cancellation: nothing is part-way through anything. A chunk that kept an
	   encode cursor or an unload flag is a chunk the next group will inherit. */
	void CheckNoChunkIsMidStream(CheckContext& ctx, StreamingHarness& harness)
	{
		uint32_t uiOutstanding = 0;

		for (const auto& iter : harness.Chunks().GetChunks())
		{
			if (iter.second->IsUnloading() || iter.second->IsEncoding() || iter.second->IsLoading())
				++uiOutstanding;
		}

		CHECK_EQ(uiOutstanding, 0u)
			<< "a chunk was left part-way through an unload after everything settled";
	}

	void CheckNoInvariantViolations(CheckContext& ctx)
	{
		CHECK_EQ(StreamingCounters::Get().BackBufferFlushRaces.load(), 0u)
			<< "a main-thread flush walked the back buffer while the render job owned it";
		CHECK_EQ(StreamingCounters::Get().PublishesOutsideCommit.load(), 0u)
			<< "the window was published from outside the commit transaction";
		CHECK_EQ(StreamingCounters::Get().ChunkInstanceRestamps.load(), 0u)
			<< "a chunk-restored static renderer was re-stamped over its decoded voxels";
	}
}

VOXAGINE_CHECK(Streaming, EveryUnloadSliceHonoursItsBudget)
{
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	const uint32_t uiRootsAtRest = ColumnZeroRoots(harness);
	REQUIRE_TRUE(uiRootsAtRest == 9u) << "the fixture should hold three roots in each of column 0's three chunks";

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle()) << "a single-stepped unload never drained";

	/* One root and one RLE run per display frame, all the way through. The
	   maxima are what say the budget was consulted rather than merely
	   configured: a loop that read the budget once and then ran to completion
	   reports the whole chunk here. */
	CHECK_EQ(StreamingCounters::Get().MaxRootsPerUnloadSlice.load(), 1u)
		<< "the unload serialization slice exceeded its one-root budget";
	CHECK_EQ(StreamingCounters::Get().MaxEncodeRunsPerSlice.load(), 1u)
		<< "the encode slice exceeded its one-run budget";

	/* Nine roots left, and every one of them was written out - not eight with a
	   silently dropped tail, which is how an off-by-one in a resumable loop
	   reads. */
	CHECK_EQ(StreamingCounters::Get().UnloadRootsSerialized.load(), 9u);
	CHECK_EQ(ColumnZeroRoots(harness), 0u) << "column 0's roots were not taken away with it";

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	CHECK_EQ(ColumnZeroRoots(harness), uiRootsAtRest)
		<< "single-stepping the unload lost or duplicated a root across the round trip";
	CHECK_EQ(harness.Window().CountOccupiedFront(), 9ull * 32ull * 32ull);
	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);

	CheckNoChunkIsMidStream(ctx, harness);
	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, AnEntityDestroyedMidUnloadIsNotSerialized)
{
	/* Ledger E1. The experiment snapshotted raw `Entity*` here and serialized
	   them over many ticks while gameplay ran; an entity destroyed in between
	   was walked as freed memory by the serializer itself, and `IsDestroyed()`
	   was only consulted once a root had already finished. Under ASan this
	   scenario is the repro; without it, it is the semantic check that a
	   destroyed entity does not come back from the dead when its chunk does. */
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffset32);

	/* Step to the exact point where one root has been written and the rest have
	   not. Expressible only because the budget is a count: with a millisecond
	   budget "after the first root" is whatever this machine happened to do. */
	bool bInterrupted = false;

	for (uint32_t uiFrame = 0; uiFrame < 20000 && !bInterrupted; ++uiFrame)
	{
		harness.Frame();

		if (StreamingCounters::Get().UnloadRootsSerialized.load() != 1u)
			continue;

		bInterrupted = true;

		/* Gameplay destroying entities of a chunk that is on its way out. Every
		   root of column 0 that has not been serialized yet - which is all of
		   them but one. */
		for (const char* pPrefix : { "Marker_0_", "Deep_0_", "Prop_0_" })
		{
			for (Entity* pEntity : harness.GetWorld().GetEntities())
			{
				if (pEntity->GetParent() != nullptr || pEntity->IsDestroyed())
					continue;

				if (pEntity->GetName().compare(0, strlen(pPrefix), pPrefix) == 0)
					pEntity->Destroy();
			}
		}
	}

	REQUIRE_TRUE(bInterrupted) << "the unload never reached its first root, so nothing was interrupted";
	REQUIRE_TRUE(harness.Settle()) << "the state machine wedged after a mid-unload destroy";

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	/* Exactly the one root that had already been written comes back. The rest
	   were destroyed before the serializer reached them and are gone, which is
	   what "destroyed" has to mean - a chunk is not a way of undoing it. */
	CHECK_EQ(ColumnZeroRoots(harness), 1u)
		<< "the survivor set of a mid-unload destroy did not round-trip";

	CheckNoChunkIsMidStream(ctx, harness);
	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, ChunkStorageIsReusedRatherThanReallocated)
{
	/* Ledger E10. A resident chunk is 48 MiB of voxels and owner slots; a slide
	   turns over three of them. Without the pool that is three frees and three
	   allocations of 48 MiB inside a window transition. */
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	const uint64_t uiAllocatedAfterFirstWindow =
		StreamingCounters::Get().ChunkStorageAllocated.load();

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	/* The first slide cannot be served: nothing has left yet, so the pool is
	   empty and its three arrivals allocate. */
	CHECK_EQ(StreamingCounters::Get().ChunkStorageAllocated.load(),
		uiAllocatedAfterFirstWindow + 3u);
	CHECK_EQ(StreamingCounters::Get().ChunkStorageReused.load(), 0u);

	harness.PlaceCamera(k_v3AtOffset64);
	REQUIRE_TRUE(harness.Settle());

	/* The second slide is the steady state, and it allocates nothing. */
	CHECK_EQ(StreamingCounters::Get().ChunkStorageAllocated.load(),
		uiAllocatedAfterFirstWindow + 3u)
		<< "a settled slide still went to the allocator for chunk storage";
	CHECK_EQ(StreamingCounters::Get().ChunkStorageReused.load(), 3u);

	/* Recycled storage carries the previous occupant's voxels in its capacity.
	   A chunk that came back holding them would be showing another chunk's
	   geometry, so this is the assertion the pool has to earn. */
	CHECK_EQ(harness.Window().CountOccupiedFront(), 9ull * 32ull * 32ull);
	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);

	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, ARandomWalkAcrossBoundariesAlwaysSettles)
{
	/* T6. Cancellation is fuzzed rather than spot-checked: a camera that
	   crosses a boundary and reverses after an arbitrary number of budget units,
	   a few hundred times, with destruction edits interleaved. Seeded, so a
	   failure replays exactly - the same reason the particle sweep uses
	   DeterministicRandom rather than glm::linearRand. */
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	DeterministicRandom random(0x51EA31C6u);

	const Vector3 v3Columns[3] = { k_v3AtOffsetZero, k_v3AtOffset32, k_v3AtOffset64 };

	for (uint32_t uiStep = 0; uiStep < 200; ++uiStep)
	{
		harness.PlaceCamera(v3Columns[static_cast<uint32_t>(random.Next() % 3u)]);

		/* Reverse part-way through, at a point the budget makes reproducible. */
		const uint32_t uiFrames = static_cast<uint32_t>(random.Next() % 7u);

		for (uint32_t uiFrame = 0; uiFrame < uiFrames; ++uiFrame)
			harness.Frame();

		/* An edit into whatever is resident right now, so the codec sees a
		   volume that is not the one it was built from. Column and row are
		   whatever the window holds; y is above the ground row so a cleared
		   voxel is a change rather than a no-op. */
		const uint32_t uiX = static_cast<uint32_t>(random.Next() % 96u);
		const uint32_t uiZ = static_cast<uint32_t>(random.Next() % 96u);
		const VoxelCell cell = harness.Grid().GetCell(uiX, 1, uiZ);

		if (cell)
			cell.SetColor(uiStep % 2 == 0 ? 0u : 0xCC112233u);
	}

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle(200000)) << "the state machine never settled after a random walk";

	CHECK_EQ(harness.Grid().GetWorldOffset(), Vector3(0.f, 0.f, 0.f));
	CHECK_EQ(harness.ResidentChunkCount(), 9u);
	CHECK_TRUE(StreamingCounters::Get().CancelledGroups.load() >= 1u)
		<< "the walk never overtook a queued group, so it proves nothing about cancellation";

	/* Every resident chunk's roots are present exactly once - nine chunks, one
	   of each root per chunk. A cancelled or repeated unload that left a chunk
	   half-serialized shows up here as a missing or duplicated set, and that is
	   how the second-unload defect in Chunk::PrepareUnloadBatch was found. */
	if (harness.CountEntitiesNamed("Marker_") != 9u)
	{
		for (const auto& it : harness.Chunks().GetChunks())
			if (it.second->IsLoaded())
				fprintf(stderr, "[dbg] resident (%u,%u) roots=%zu marker=%d unloading=%d encoding=%d loading=%d targetLoaded=%d\n",
					it.second->GetChunkIndex().x, it.second->GetChunkIndex().y,
					it.second->GetRootEntities().size(),
					harness.FindEntityNamed("Marker_" + std::to_string(it.second->GetChunkIndex().x) + "_" + std::to_string(it.second->GetChunkIndex().y)) != nullptr,
					it.second->IsUnloading(), it.second->IsEncoding(), it.second->IsLoading(), it.second->IsTargetLoaded());
		fprintf(stderr, "[dbg] unloads-of-unloaded=%llu cancelled-groups=%llu\n",
			(unsigned long long)StreamingCounters::Get().UnloadsOfUnloadedChunks.load(),
			(unsigned long long)StreamingCounters::Get().CancelledGroups.load());
	}
	CHECK_EQ(harness.CountEntitiesNamed("Marker_"), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Deep_"), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Prop_"), 9u);

	/* And every resident chunk still round-trips through the codec, edits
	   included - the walk wrote into volumes that were encoded, cancelled,
	   re-encoded and decoded again. */
	uint64_t uiDiverged = 0;

	for (const auto& iter : harness.Chunks().GetChunks())
	{
		if (iter.second->IsLoaded())
			uiDiverged += iter.second->VerifyVoxelCodecRoundTrip();
	}

	CHECK_EQ(uiDiverged, 0u) << "a chunk stopped round-tripping through the RLE after a fuzzed walk";

	CheckNoChunkIsMidStream(ctx, harness);
	CheckNoInvariantViolations(ctx);
}

VOXAGINE_CHECK(Streaming, HostileChunkDataDoesNotWedgeTheStateMachine)
{
	/* T7. Streamed content is data, and data is hostile - a level saved by an
	   older build, a hand-edited `.wld`, a type that has since been renamed. The
	   contract is the same in every case: skip or defer the item, keep
	   streaming, never crash and never wedge. "Never wedge" is the half that
	   needs a test, because a state machine that stops advancing looks exactly
	   like a slow load.

	   The fixture puts three of them at the front of every chunk's root list, so
	   they are hit before anything valid: a root whose `EntityType` this build
	   has never heard of, a root with no `Children` member, and a root with no
	   `Components` member at all - not even a Transform, which the whole chunk
	   assignment is computed from. */
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingHostileRoots");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle()) << "a world with hostile roots never settled";

	/* The valid roots of the same chunks are unaffected: nine resident chunks,
	   three good roots each. A hostile root that took its siblings with it would
	   show up here rather than as a crash. */
	CHECK_EQ(harness.CountEntitiesNamed("Marker_"), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Prop_"), 9u);

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle()) << "the state machine wedged unloading a chunk with hostile roots";

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle()) << "the state machine wedged reloading a chunk with hostile roots";

	CHECK_EQ(harness.CountEntitiesNamed("Marker_"), 9u)
		<< "a hostile root cost its chunk's valid roots a round trip";
	CHECK_EQ(harness.CountEntitiesNamed("Prop_"), 9u);

	/* Whatever the deserializer did with them, it must not have multiplied
	   them: a root that fails to build and is then written back out on unload is
	   the shape that grows a chunk's root list every time the player walks past
	   it. */
	CHECK_TRUE(harness.CountEntitiesNamed("Hostile_") <= 27u)
		<< "hostile roots multiplied across an unload/reload cycle";

	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);

	CheckNoChunkIsMidStream(ctx, harness);
	CheckNoInvariantViolations(ctx);
}
