#include "Core/ECS/Systems/Chunk/ChunkSystem.h"
#include "Core/ECS/Systems/Chunk/StreamingCounters.h"
#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Framework/Benchmark.h"
#include "Harness/StreamingHarness.h"

/* What a window slide costs, in exact counts. CHUNK_STREAMING_PLAN.md T4.
 *
 * The destruction plan's split applies here unchanged: **work is gated, time is
 * reported**. Everything below marked `work` is the same number in Debug and
 * Release, on this machine and on a CI runner, so any increase fails the run.
 * The timings are printed because a streaming slice's wall clock is the whole
 * question at 60 Hz - but only against a recording from the same machine.
 *
 * The counters that matter most are `roots-per-staging-slice` and
 * `roots-per-admission-slice`. Deserializing an incoming chunk's roots was one
 * uninterrupted main-thread pass through phase 2 - 44 ms per chunk, and by then
 * the whole of what was left of the transition hitch. Phase 3 split it into
 * budgeted staging and budgeted admission, and these two numbers are what say
 * the budgets are real rather than commented.
 *
 * `gameplay-roots-per-admission` is the opposite kind of number: it is
 * deliberately *not* bounded, and a drop in it is the regression. Every
 * non-static root of the incoming window enters together so that two gameplay
 * entities referencing each other are never half-present (ledger E3).
 *
 * **The budgets here are unit budgets, and they have to be.** A wall-clock
 * budget makes "roots serialized in one slice" a function of how fast the
 * machine is, which is exactly the kind of number this file refuses to gate on.
 * Stated in units, the two phase-2 bounds become exact: a chunk carries three
 * roots and the budget is two, so a slice must serialize exactly two; the
 * encode budget is sixty-four runs against a volume that needs a hundred and
 * twenty-eight, so a slice must write exactly sixty-four. Either number moving
 * means a budgeted loop stopped honouring its budget. The timings below are
 * therefore the cost of *slicing*, not the cost of the shipping defaults.
 */
VOXAGINE_BENCHMARK(Streaming, WindowSlideAndReturn)
{
	StreamingCounters::Reset();

	StreamingBudgets budgets;
	budgets.EntityStaging = StreamingBudget::Units(2);
	budgets.EntityAdmission = StreamingBudget::Units(2);
	budgets.UnloadSerialization = StreamingBudget::Units(2);
	budgets.VoxelEncoding = StreamingBudget::Units(64);

	const StreamingBudgetOverride override(budgets);

	StreamingHarness harness("StreamingGrid5x5");

	const Stopwatch total;

	harness.PlaceCamera(Vector3(48.f, 0.f, 16.f));
	harness.Settle();

	const Stopwatch outbound;
	harness.PlaceCamera(Vector3(80.f, 0.f, 16.f));
	const bool bOutboundSettled = harness.Settle();
	const double fOutboundMs = outbound.Milliseconds();

	const Stopwatch inbound;
	harness.PlaceCamera(Vector3(48.f, 0.f, 16.f));
	const bool bInboundSettled = harness.Settle();
	const double fInboundMs = inbound.Milliseconds();

	const StreamingCounters& counters = StreamingCounters::Get();

	/* Two transitions, and R2 says each publishes exactly once. Recorded as two
	   separate counters rather than as a ratio so that a failure says which
	   half moved: a second commit for one group and a group that committed
	   twice are different defects. */
	result.AddWork("commits", static_cast<double>(counters.Commits.load()));
	result.AddWork("committed-groups", static_cast<double>(counters.CommittedGroups.load()));
	result.AddWork("window-swaps", static_cast<double>(harness.Window().SwapCount()));

	/* Nine chunk slices rewritten per transition. A slide that stopped
	   republishing the six chunks that only *moved* would halve this and lose
	   their geometry a swap later, which is exactly the failure the number
	   exists to catch. */
	result.AddWork("chunk-regions-written", static_cast<double>(counters.ChunkRegionsWritten.load()));
	result.AddWork("voxel-words-written", static_cast<double>(counters.VoxelWordsWritten.load()));

	/* Phase 3's two halves of loading a chunk's entities. Staging is bounded by
	   a budget stated in units above, so a slice must construct exactly two
	   roots; static admission is bounded the same way, and the fixture's one
	   static root per chunk is what keeps it below its budget rather than at
	   it. Either exceeding its budget means a loop stopped honouring one. */
	result.AddWork("roots-per-staging-slice", static_cast<double>(counters.MaxRootsPerStagingSlice.load()));
	result.AddWork("roots-per-admission-slice", static_cast<double>(counters.MaxRootsPerAdmissionSlice.load()));

	/* The contract number (ledger E3): every non-static root of the incoming
	   window enters in one frame, deliberately unbudgeted. Two chunk columns of
	   three chunks each arrive over the two transitions, two gameplay roots
	   apiece, so a transition admits six together. If this ever reads less than
	   the incoming window's gameplay roots, the admission was split and a
	   cross-link between two of them can be transiently null. */
	result.AddWork("gameplay-roots-per-admission", static_cast<double>(counters.MaxGameplayRootsPerAdmission.load()));

	/* What staging is holding at its peak - roots constructed and not yet in the
	   world. The high-water mark of the one place this phase holds raw Entity*s
	   across a frame boundary. */
	result.AddWork("staged-roots-high-water", static_cast<double>(counters.MaxStagedRoots.load()));

	/* Serialized entity references still waiting for both ends to be in the
	   world (K6), and the ones given up on. The fixture has no links at all, so
	   both are zero here and the numbers are carried by the link checks - but a
	   phase that starts leaking retries should have to change a checked-in
	   number rather than a comment. */
	result.AddWork("pending-world-links", static_cast<double>(counters.MaxPendingWorldLinks.load()));
	result.AddWork("world-links-abandoned", static_cast<double>(counters.WorldLinksAbandoned.load()));
	result.AddWork("world-links-source-lost", static_cast<double>(counters.WorldLinksSourceLost.load()));

	/* T7: a root of a type this binary does not have. None in this fixture. */
	result.AddWork("staged-roots-rejected", static_cast<double>(counters.StagedRootsRejected.load()));

	/* Phase 2's two bounds, and the whole point of the unit budgets above: these
	   are the budget values, exactly, or a loop is not honouring its budget. */
	result.AddWork("roots-per-unload-slice", static_cast<double>(counters.MaxRootsPerUnloadSlice.load()));
	result.AddWork("encode-runs-per-slice", static_cast<double>(counters.MaxEncodeRunsPerSlice.load()));

	/* Six chunks leave over the two transitions and six arrive. The first three
	   arrivals cannot be served from an empty pool, so three allocations and
	   three reuses is the shape of a settled slide - and a rising allocation
	   count with a flat reuse count is the pool quietly not working. */
	result.AddWork("chunk-storage-reused", static_cast<double>(counters.ChunkStorageReused.load()));
	result.AddWork("chunk-storage-allocated", static_cast<double>(counters.ChunkStorageAllocated.load()));

	/* T5, as baseline entries: all of these must stay at zero forever. */
	result.AddWork("back-buffer-flush-races", static_cast<double>(counters.BackBufferFlushRaces.load()));
	result.AddWork("publishes-outside-commit", static_cast<double>(counters.PublishesOutsideCommit.load()));

	/* Phase 14. A staged root destroyed by ~Chunk rather than by the world that
	   owns the systems its components point at - M9's SIGSEGV. Zero here because
	   the harness settles its window before the benchmark runs, and checked in
	   because the number that must never move is the one worth checking in. */
	result.AddWork("staged-roots-outliving-systems",
		static_cast<double>(counters.StagedRootsOutlivingSystems.load()));

	/* M7. A static renderer re-stamped over voxels that were decoded with their
	   damage in them is exactly "destroyed terrain comes back". There is no
	   RenderSystem here so this is zero by construction; it is in the baseline
	   because the same counter is what the game reports, and a phase that
	   reintroduces the re-stamp should have to change a checked-in number. */
	result.AddWork("chunk-instance-restamps", static_cast<double>(counters.ChunkInstanceRestamps.load()));

	/* Phase 12. The benchmark writes no voxels of its own, so both are zero
	   here - and that is the statement worth checking in: a slide over a world
	   nobody wrote to must republish nothing, or the repair has turned into a
	   second full window write on every transition. The check that exercises
	   the non-zero case is
	   Streaming/AVoxelWrittenWhileTheWindowIsBuildingSurvivesTheSwap. */
	result.AddWork("window-commit-writes-replayed",
		static_cast<double>(counters.WindowCommitWritesReplayed.load()));
	result.AddWork("window-commit-writes-lost",
		static_cast<double>(counters.WindowCommitWritesLost.load()));

	/* A mapping voxel erased while the CPU kept its colour, which is the same
	   invisible-but-solid disagreement seen from the stamp side. Zero by
	   construction in a world with no RenderSystem; in the baseline for the
	   reason chunk-instance-restamps is - the game reports the same counter. */
	result.AddWork("voxel-stamp-diverging-erases",
		static_cast<double>(counters.VoxelStampDivergingErases.load()));

	/* The terminal state, so a benchmark that stopped streaming for the wrong
	   reason cannot report cheap numbers. */
	result.AddWork("resident-chunks", static_cast<double>(harness.ResidentChunkCount()));
	result.AddWork("occupancy-disagreements",
		static_cast<double>(harness.Bricks().Validate(false, harness.Window().FrontWords().data())));

	result.AddTime("outbound slide", fOutboundMs);
	result.AddTime("return slide", fInboundMs);
	result.AddTime("total", total.Milliseconds());

	if (!bOutboundSettled || !bInboundSettled)
		result.AddNote("a transition did not drain - the timings below are meaningless");
}
