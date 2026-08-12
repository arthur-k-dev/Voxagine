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
 * and it is *unbounded* after phase 1: this phase moved that work to after the
 * commit and changed nothing about how much of it there is. Phases 2 and 3 make
 * it resumable against StreamingBudgets::EntityWork, at which point this
 * baseline entry ratchets down to the budget. Recording the unbounded value now
 * is what turns "bounded work per tick" from a comment into a number CI holds.
 */
VOXAGINE_BENCHMARK(Streaming, WindowSlideAndReturn)
{
	StreamingCounters::Reset();

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

	/* T5, as baseline entries: both must stay at zero forever. */
	result.AddWork("back-buffer-flush-races", static_cast<double>(counters.BackBufferFlushRaces.load()));
	result.AddWork("publishes-outside-commit", static_cast<double>(counters.PublishesOutsideCommit.load()));

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
