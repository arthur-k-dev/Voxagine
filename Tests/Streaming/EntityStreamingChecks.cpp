#include "Framework/Check.h"
#include "Harness/StreamingHarness.h"
#include "Harness/StreamingProbe.h"

#include "Core/ECS/Entity.h"
#include "Core/ECS/Systems/Chunk/Chunk.h"
#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/World.h"

/* Phase 3 of CHUNK_STREAMING_PLAN.md: loading a chunk's entities is bounded
 * work, and what gameplay is promised about it.
 *
 * Master deserialized every root of every incoming chunk in one uninterrupted
 * main-thread pass - 41.6 ms per chunk at phase 0, still 44.2 at phase 2, and
 * by then the *whole* of what was left of the transition hitch. It is two
 * halves now, and the split is the point rather than an implementation detail:
 *
 *   - **staging** constructs the roots detached from the world, so it can start
 *     while the chunk worker is still building the back buffer and can be
 *     interrupted anywhere without anything observing a partial world;
 *   - **admission** puts them in, and is where the contract lives. Every
 *     non-static root of the whole incoming window enters in one frame, so two
 *     gameplay entities that reference each other are never half-present; the
 *     static art follows in budgeted slices.
 *
 * That contract is ledger E3's replacement. The experiment admitted everything
 * in budgeted batches and then patched the transient nulls it produced in
 * GameManager, SpawnerManager (polled from four places) and Weapon::Fire. None
 * of that lands, and these checks are why it does not have to.
 */

namespace
{
	Vector3 CameraInColumn(uint32_t uiColumn)
	{
		return Vector3(uiColumn * 32.f + 16.f, 0.f, 16.f);
	}

	const Vector3 k_v3AtOffsetZero = CameraInColumn(1);
	const Vector3 k_v3AtOffset32 = CameraInColumn(2);

	/* Chunk column 3 is what arrives when the window slides one step right from
	   its starting position, so these names are the incoming roots. */
	uint32_t IncomingGameplayRoots(const StreamingHarness& harness)
	{
		return harness.CountEntitiesNamed("Marker_3_") +
			harness.CountEntitiesNamed("Deep_3_");
	}

	uint32_t IncomingStaticRoots(const StreamingHarness& harness)
	{
		return harness.CountEntitiesNamed("Prop_3_");
	}

	StreamingBudgets SingleStepBudgets()
	{
		StreamingBudgets budgets;
		budgets.EntityStaging = StreamingBudget::Units(1);
		budgets.EntityAdmission = StreamingBudget::Units(1);
		budgets.EntityRefresh = StreamingBudget::Units(1);
		budgets.UnloadSerialization = StreamingBudget::Units(1);
		budgets.VoxelEncoding = StreamingBudget::Units(1);

		return budgets;
	}
}

/* Staging happens before the commit and must not be observable. If it were -
   if a root reached the world while the window it belongs to was still being
   built in the back buffer - gameplay would be ticking entities standing on
   geometry that is not published yet, which is exactly what R1 and R2 exist to
   prevent. */
VOXAGINE_CHECK(Streaming, NoIncomingRootIsAdmittedBeforeTheWindowCommits)
{
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	REQUIRE_EQ(IncomingGameplayRoots(harness), 0u)
		<< "the incoming column is already resident, so this proves nothing";

	const uint64_t uiCommitsBefore = StreamingCounters::Get().Commits.load();

	harness.PlaceCamera(k_v3AtOffset32);

	bool bStagedSomething = false;
	uint32_t uiAdmittedBeforeCommit = 0;

	for (uint32_t uiFrame = 0; uiFrame < 20000; ++uiFrame)
	{
		harness.Frame();

		const bool bCommitted =
			StreamingCounters::Get().Commits.load() > uiCommitsBefore;

		if (!bCommitted)
		{
			uiAdmittedBeforeCommit =
				std::max(uiAdmittedBeforeCommit,
					IncomingGameplayRoots(harness) + IncomingStaticRoots(harness));

			bStagedSomething = bStagedSomething ||
				StreamingCounters::Get().MaxStagedRoots.load() > 0;
		}
		else if (!harness.Chunks().IsStreaming())
		{
			break;
		}
	}

	CHECK_TRUE(bStagedSomething)
		<< "nothing was ever staged before the commit, so the overlap this phase "
		   "exists for is not happening";

	CHECK_EQ(uiAdmittedBeforeCommit, 0u)
		<< "an incoming root reached the world before the window that contains its "
		   "geometry was published";

	CHECK_EQ(IncomingGameplayRoots(harness), 6u) << "the incoming column's gameplay roots";
	CHECK_EQ(IncomingStaticRoots(harness), 3u) << "the incoming column's static roots";
}

/* The contract. Admission is ordered gameplay-first and the gameplay half is
   not sliced, so however small the static budget is, every non-static root of
   the incoming window appears together. */
VOXAGINE_CHECK(Streaming, EveryGameplayRootOfTheIncomingWindowAdmitsInOneFrame)
{
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffset32);

	uint32_t uiGameplayBefore = 0;
	uint32_t uiGameplayAfter = 0;
	uint32_t uiStaticAtGameplayFrame = 0;
	bool bSeen = false;

	for (uint32_t uiFrame = 0; uiFrame < 20000 && !bSeen; ++uiFrame)
	{
		const uint32_t uiBefore = IncomingGameplayRoots(harness);

		harness.Frame();

		const uint32_t uiAfter = IncomingGameplayRoots(harness);

		if (uiAfter != uiBefore)
		{
			uiGameplayBefore = uiBefore;
			uiGameplayAfter = uiAfter;
			uiStaticAtGameplayFrame = IncomingStaticRoots(harness);
			bSeen = true;
		}
	}

	REQUIRE_TRUE(bSeen) << "the incoming column's gameplay roots never arrived";

	CHECK_EQ(uiGameplayBefore, 0u);
	CHECK_EQ(uiGameplayAfter, 6u)
		<< "the incoming window's gameplay roots arrived in more than one frame, so a "
		   "link between two of them can be transiently null - this is ledger E3";

	CHECK_EQ(uiStaticAtGameplayFrame, 0u)
		<< "static art was admitted before the gameplay roots, which is the ordering "
		   "K4 exists to prevent";

	REQUIRE_TRUE(harness.Settle());
	CHECK_EQ(IncomingStaticRoots(harness), 3u);
}

VOXAGINE_CHECK(Streaming, StaticAdmissionHonoursItsBudget)
{
	StreamingCounters::Reset();

	StreamingBudgets budgets;
	budgets.EntityAdmission = StreamingBudget::Units(2);

	const StreamingBudgetOverride override(budgets);

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	/* One static root per incoming chunk, so a budget of two is never reached
	   within a chunk - which is the honest statement of what this fixture can
	   prove. What it does prove is that the slice never exceeds the budget. */
	CHECK_LE(StreamingCounters::Get().MaxRootsPerAdmissionSlice.load(), 2u)
		<< "a static admission slice admitted more roots than its budget allows";

	CHECK_EQ(IncomingStaticRoots(harness), 3u);
	CHECK_EQ(IncomingGameplayRoots(harness), 6u);
}

VOXAGINE_CHECK(Streaming, StagingHonoursItsBudget)
{
	StreamingCounters::Reset();

	StreamingBudgets budgets;
	budgets.EntityStaging = StreamingBudget::Units(1);

	const StreamingBudgetOverride override(budgets);

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	/* ChunkSystem::Start loads the initial window through Chunk::LoadEntities,
	   which stages under an explicitly Unbounded budget - it is not a slice and
	   its three roots are not a budget violation. Only the slide below is. */
	StreamingCounters::Reset();

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	CHECK_EQ(StreamingCounters::Get().MaxRootsPerStagingSlice.load(), 1u)
		<< "a staging slice constructed more roots than its budget allows";
}

/* T2's single-step sweep, for the two states this phase adds: every resumption
   point between every root is exercised, and the world that comes out is the
   same one an unbounded pass would have produced. */
VOXAGINE_CHECK(Streaming, ASingleSteppedSlideAndBackLeavesEveryEntityOnce)
{
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingGrid5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	/* Nine resident chunks, three roots each, none duplicated and none lost. */
	CHECK_EQ(harness.CountEntitiesNamed("Marker_"), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Deep_"), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Prop_"), 9u);

	CHECK_EQ(harness.ResidentChunkCount(), 9u);
	CHECK_EQ(StreamingCounters::Get().MaxStagedRoots.load() > 0, true);

	CHECK_EQ(harness.Bricks().Validate(false, harness.Window().FrontWords().data()), 0u);
}

/* K6 / M3. Two entities in different chunks of the same incoming column, one
   holding a reflected Entity* to the other. Master resolved links once per
   PreTick and cleared the list unconditionally, so the half of the pair that
   was one frame early lost its reference permanently. */
VOXAGINE_CHECK(Streaming, ACrossChunkLinkSurvivesAWindowSlide)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingLinks5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	auto checkProbe = [&](const std::string& sProbe, const std::string& sTarget)
	{
		Entity* pProbe = harness.FindEntityNamed(sProbe);
		Entity* pTarget = harness.FindEntityNamed(sTarget);

		REQUIRE_TRUE(pProbe != nullptr) << sProbe << " was never admitted";
		REQUIRE_TRUE(pTarget != nullptr) << sTarget << " was never admitted";

		StreamingProbeEntity* pTyped = static_cast<StreamingProbeEntity*>(pProbe);

		CHECK_TRUE(pTyped->GetLinkTarget() == pTarget)
			<< sProbe << " did not reconnect to " << sTarget;
	};

	checkProbe("Probe_0_0", "Marker_0_1");
	checkProbe("Probe_1_1", "Marker_1_2");

	/* And again for a column that arrives through the streaming path rather
	   than through ChunkSystem::Start's synchronous initial load. */
	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	/* Two settles: the link is made in PreTick, which for the frame that admits
	   is the *next* frame's. Settle already runs one extra frame past the last
	   group; this makes the dependency explicit rather than incidental. */
	harness.Frame();

	checkProbe("Probe_3_0", "Marker_3_1");
	checkProbe("Probe_3_1", "Marker_3_2");

	CHECK_EQ(StreamingCounters::Get().WorldLinksSourceLost.load(), 0ull)
		<< "a link's source entity was gone by the time it was resolved - M3";
}

/* M8. The link is a raw Entity* once it is made, and chunk streaming destroys
   the target routinely: the two ends of a cross-chunk reference are in
   different chunks and one of them leaves first. Nothing knew those pointers
   existed, so they were left dangling - and the reader that bites is the
   *serializer*, which dereferences an Entity* property to write its id when the
   holder's own chunk unloads in turn. Found by ASan through this fixture; no
   engine type has a reflected Entity*, which is why nothing had caught it. */
VOXAGINE_CHECK(Streaming, DestroyingALinkTargetNullsThePointersToIt)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingLinks5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	Entity* pProbe = harness.FindEntityNamed("Probe_0_0");
	Entity* pTarget = harness.FindEntityNamed("Marker_0_1");

	REQUIRE_TRUE(pProbe != nullptr && pTarget != nullptr);
	REQUIRE_TRUE(static_cast<StreamingProbeEntity*>(pProbe)->GetLinkTarget() == pTarget)
		<< "the link was never made, so destroying the target proves nothing";

	pTarget->Destroy();
	harness.Frame();

	REQUIRE_TRUE(harness.FindEntityNamed("Marker_0_1") == nullptr)
		<< "the target survived Destroy, so this is not testing the repair";

	Entity* pProbeAfter = harness.FindEntityNamed("Probe_0_0");
	REQUIRE_TRUE(pProbeAfter != nullptr) << "the holder was taken with the target";

	CHECK_TRUE(static_cast<StreamingProbeEntity*>(pProbeAfter)->GetLinkTarget() == nullptr)
		<< "a link to a destroyed entity is still pointing at freed memory - M8";

	CHECK_TRUE(StreamingCounters::Get().EntityLinksCleared.load() > 0ull)
		<< "the repair did not run, so the null above is a coincidence";

	CHECK_EQ(StreamingCounters::Get().EntityLinksLeftDangling.load(), 0ull)
		<< "a link the repair could not reach";
}

/* And the whole of it through the streaming path: the linked chunks unload,
   which serializes the holder - the exact sequence that crashed. */
VOXAGINE_CHECK(Streaming, UnloadingBothEndsOfACrossChunkLinkIsSafe)
{
	StreamingCounters::Reset();

	const StreamingBudgetOverride override(SingleStepBudgets());

	StreamingHarness harness("StreamingLinks5x5");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	{
		Entity* pProbe = harness.FindEntityNamed("Probe_0_0");
		Entity* pTarget = harness.FindEntityNamed("Marker_0_1");

		REQUIRE_TRUE(pProbe != nullptr && pTarget != nullptr);
		REQUIRE_TRUE(static_cast<StreamingProbeEntity*>(pProbe)->GetLinkTarget() == pTarget)
			<< "the link was never made, so unloading it proves nothing";
	}

	/* Column 0 leaves, taking both ends of Probe_0_0 -> Marker_0_1 with it. */
	harness.PlaceCamera(k_v3AtOffset32);
	REQUIRE_TRUE(harness.Settle());

	CHECK_TRUE(harness.FindEntityNamed("Probe_0_0") == nullptr);

	/* Deliberately no assertion about EntityLinksCleared here. Which end of the
	   link is destroyed first is a scheduling question - the two chunks unload
	   in camera-distance order - and when the *source* goes first there is
	   nothing left to null, so the record is simply forgotten. The check above
	   is the one that pins the repair; this one exists because the sequence
	   below is what actually crashed under ASan: serializing a holder whose
	   target had already been freed. */

	/* And back, so the stored JSON is read again - a holder serialized with a
	   nulled link must come back with the link, because both ends return. */
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());
	harness.Frame();

	Entity* pProbe = harness.FindEntityNamed("Probe_0_0");
	Entity* pTarget = harness.FindEntityNamed("Marker_0_1");

	REQUIRE_TRUE(pProbe != nullptr && pTarget != nullptr);

	CHECK_TRUE(static_cast<StreamingProbeEntity*>(pProbe)->GetLinkTarget() == pTarget)
		<< "a cross-chunk link did not survive both ends unloading and coming back";
}

/* T7. A level may reference an entity it no longer contains. The answer is to
   stop retrying, count it and carry on - never to spin forever and never to
   keep the source alive waiting for it. */
VOXAGINE_CHECK(Streaming, ALinkWhoseTargetNeverArrivesIsGivenUpOnQuietly)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingLinks5x5");

	/* Chunk row 4 is only resident with the camera near the far edge, and it is
	   where the fixture's orphan probes live. */
	harness.PlaceCamera(Vector3(48.f, 0.f, 112.f));
	REQUIRE_TRUE(harness.Settle());

	Entity* pOrphan = harness.FindEntityNamed("Orphan_1_4");
	REQUIRE_TRUE(pOrphan != nullptr) << "the orphan probe was never admitted";

	CHECK_TRUE(static_cast<StreamingProbeEntity*>(pOrphan)->GetLinkTarget() == nullptr);

	/* Past the retry bound. The world is settled, so these frames cost nothing
	   but the link pass itself - which is the cost being bounded. */
	for (uint32_t uiFrame = 0; uiFrame < World::k_uiMaxWorldLinkRetries + 8; ++uiFrame)
		harness.Frame();

	CHECK_TRUE(StreamingCounters::Get().WorldLinksAbandoned.load() > 0ull)
		<< "a link with no possible target is still being retried";

	CHECK_TRUE(harness.FindEntityNamed("Orphan_1_4") != nullptr)
		<< "an unresolvable link took its source entity with it";

	/* And the retry list actually drains rather than growing. */
	const uint64_t uiAbandoned = StreamingCounters::Get().WorldLinksAbandoned.load();

	for (uint32_t uiFrame = 0; uiFrame < 64; ++uiFrame)
		harness.Frame();

	CHECK_EQ(StreamingCounters::Get().WorldLinksAbandoned.load(), uiAbandoned)
		<< "links are still being abandoned after the list should be empty";
}

/* T7. Streamed content is data, and data is hostile: a root naming a type this
   binary does not have is the ordinary case of a level saved by a build that
   had it. */
VOXAGINE_CHECK(Streaming, AnUnknownEntityTypeIsSkippedAndCounted)
{
	StreamingCounters::Reset();

	StreamingHarness harness("StreamingHostileRoots");
	harness.PlaceCamera(k_v3AtOffsetZero);
	REQUIRE_TRUE(harness.Settle());

	CHECK_TRUE(StreamingCounters::Get().StagedRootsRejected.load() > 0ull)
		<< "the fixture's unknown-type roots were not rejected, so this check is "
		   "not exercising the path it names";

	CHECK_EQ(harness.CountEntitiesNamed("Hostile_Unknown_"), 0u)
		<< "a root of an unregistered type reached the world";

	/* The good roots of the same chunks are unaffected - a hostile root must
	   cost its own line and nothing else. */
	CHECK_EQ(harness.CountEntitiesNamed("Marker_"), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Hostile_NoChildren_"), 9u);
	CHECK_EQ(harness.CountEntitiesNamed("Hostile_Bare_"), 9u);
}

/* R1. The one state in which the hold is live today: ChunkSystem::Start builds
   the initial window synchronously, so a world becomes ready during
   World::Initialize - and the frames before that are the frames phase 4 will
   put a loading screen over. */
VOXAGINE_CHECK(Streaming, GameplayIsHeldUntilTheInitialWindowIsResident)
{
	StreamingCounters::Reset();
	StreamingProbeEntity::ResetCounters();

	StreamingHarness harness("StreamingGrid5x5", false);
	harness.SetTickWorld(true);

	World& world = harness.GetWorld();

	REQUIRE_TRUE(world.IsGameplayHeld())
		<< "a world whose chunk system has not started is not held, so nothing "
		   "stops gameplay ticking against a window that does not exist";

	/* An entity that is unambiguously in the world - added and PreTicked - so a
	   tick count of zero can only be the hold. */
	world.AddEntity(new StreamingProbeEntity(&world));
	world.PreTick();

	for (uint32_t uiFrame = 0; uiFrame < 8; ++uiFrame)
		harness.Frame();

	CHECK_EQ(StreamingProbeEntity::TicksTaken(), 0ull)
		<< "an entity ticked while the initial window was still missing (R1)";
	CHECK_EQ(StreamingProbeEntity::FixedTicksTaken(), 0ull)
		<< "an entity fixed-ticked while the initial window was still missing (R1)";

	CHECK_TRUE(StreamingCounters::Get().GameplayTicksHeld.load() > 0ull)
		<< "the hold is not being counted, so nothing in the game can report it";

	harness.Initialize();

	CHECK_FALSE(world.IsGameplayHeld())
		<< "the initial window is resident and gameplay is still held";

	harness.Frame();

	CHECK_TRUE(StreamingProbeEntity::TicksTaken() > 0ull)
		<< "gameplay never resumed after the initial window arrived";
}
