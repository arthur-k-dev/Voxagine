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
 * The counter that matters most is `roots-per-entity-pass`. It is the number of
 * root entities the main thread deserializes in a single uninterrupted pass,
 * and it is *unbounded* still: phase 1 moved that work to after the commit and
 * phase 2 left it alone. Phase 3 makes it resumable against
 * StreamingBudgets::EntityWork, at which point this baseline entry ratchets down
 * to the budget. Recording the unbounded value now is what turns "bounded work
 * per tick" from a comment into a number CI holds.
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

	/* Unbounded today - see the file comment. */
	result.AddWork("roots-per-entity-pass", static_cast<double>(counters.MaxRootsPerEntityPass.load()));

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

	/* M7. A static renderer re-stamped over voxels that were decoded with their
	   damage in them is exactly "destroyed terrain comes back". There is no
	   RenderSystem here so this is zero by construction; it is in the baseline
	   because the same counter is what the game reports, and a phase that
	   reintroduces the re-stamp should have to change a checked-in number. */
	result.AddWork("chunk-instance-restamps", static_cast<double>(counters.ChunkInstanceRestamps.load()));

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
