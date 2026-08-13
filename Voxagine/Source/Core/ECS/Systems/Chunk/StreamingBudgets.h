#pragma once

#include <chrono>
#include <cstdint>

/* Every bound chunk streaming works under, in one place. CHUNK_STREAMING_PLAN.md
 * rule R3.
 *
 * The experiment this plan re-derives scattered `2.0` five times plus `0.5`,
 * `4.0`, `12.0`, `8192` and `32768` as literals across three files, which is
 * how "bounded work per tick" stayed a comment rather than becoming a number
 * anybody could change, measure or assert against.
 *
 * **Wall clock is the right runtime behaviour and the wrong test behaviour.**
 * A millisecond budget is machine-dependent, so a scenario driven by one runs
 * a different number of slices on a fast machine than on a slow one - and the
 * resumption points a slow machine happens to hit are exactly the ones a fast
 * machine never tests. So a budget here is *either* a wall-clock allowance or a
 * unit count, and the harness replaces the whole set with unit budgets
 * (`StreamingBudgets::Units(1)` for the single-step sweep) before it drives
 * anything. That is T2, and it is built in from the first budget rather than
 * retrofitted: making an existing wall-clock loop injectable later means
 * touching every loop again.
 *
 * A budget of neither kind is `Unbounded()`, which is not a placeholder - it is
 * the honest description of a step this plan has not made resumable yet, and
 * naming it here is what makes the remaining unbounded work greppable instead
 * of invisible.
 *
 * **Every budgeted state is inside the update group, and that is a choice with
 * a cost.** Spreading the unload across frames removes it from the transition
 * frame and adds it to the transition's end-to-end latency: three chunks'
 * encode is 27-36 ms of work, which at 2 ms a frame is around 300 ms before the
 * group drains and the next one may start. That is affordable only because a
 * chunk is 256 units across, so two boundary crossings are seconds apart, not
 * milliseconds - the one case it can be felt is a player standing on a boundary
 * and re-crossing it immediately, which is the cancellation path the tests
 * cover. ChunkUpdateGroup::MillisecondsSinceCreated is the number to watch if
 * that trade ever needs revisiting.
 */
class StreamingBudget
{
public:
	static StreamingBudget Milliseconds(double fMilliseconds)
	{
		StreamingBudget budget;
		budget.m_fMilliseconds = fMilliseconds;
		return budget;
	}

	static StreamingBudget Units(uint32_t uiUnits)
	{
		StreamingBudget budget;
		budget.m_uiUnits = uiUnits;
		return budget;
	}

	static StreamingBudget Unbounded() { return StreamingBudget(); }

	bool IsUnbounded() const { return m_uiUnits == 0 && m_fMilliseconds <= 0.0; }

	/* The unit count, or UINT32_MAX when this budget is not counted in units -
	   which, for a loop whose only possible bound is a count, means all of it.
	   The one caller is VoxelBaker::Occupy, whose slice budget is passed *into*
	   the walk rather than polled around it: the walk is the only thing that
	   knows where a sample begins. */
	uint32_t UnitsOrUnbounded() const { return m_uiUnits == 0 ? UINT32_MAX : m_uiUnits; }

	/* One pass over a resumable loop. Constructed at the top of the state,
	   asked after each item, and never consulted before the first one: every
	   budgeted loop must make progress on at least one item per entry or a
	   budget of zero wedges the state machine rather than slowing it. */
	class Scope
	{
	public:
		explicit Scope(const StreamingBudget& budget) :
			m_Budget(budget),
			m_Start(budget.m_fMilliseconds > 0.0
				? std::chrono::steady_clock::now()
				: std::chrono::steady_clock::time_point())
		{
		}

		/* Charge for work done. Units are whatever the loop counts - roots,
		   nodes, encode runs - and each loop documents its own. */
		void Consume(uint32_t uiUnits = 1) { m_uiConsumed += uiUnits; }

		bool Exhausted() const
		{
			if (m_Budget.m_uiUnits != 0)
				return m_uiConsumed >= m_Budget.m_uiUnits;

			if (m_Budget.m_fMilliseconds <= 0.0)
				return false;

			return std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - m_Start).count() >= m_Budget.m_fMilliseconds;
		}

		uint32_t Consumed() const { return m_uiConsumed; }

		/* Whether this scope bounds anything at all. The unbounded scopes are
		   the non-streaming callers - ChunkSystem::Start's synchronous initial
		   window, the editor, an audit - and a slice counter that recorded them
		   would report their whole pass as a budget violation. */
		bool IsUnbounded() const { return m_Budget.IsUnbounded(); }

	private:
		const StreamingBudget& m_Budget;
		std::chrono::steady_clock::time_point m_Start;
		uint32_t m_uiConsumed = 0;
	};

private:
	double m_fMilliseconds = 0.0;
	uint32_t m_uiUnits = 0;
};

/* The named set. One instance, replaced wholesale by a test rather than mutated
   field by field, so a scenario's budgets are visible in one line. */
struct StreamingBudgets
{
	/* Constructing an incoming chunk's roots, detached from the world, in units
	   of *roots*. Phase 3.

	   This is where `Chunk::LoadEntities`'s 44 ms per chunk went. It runs
	   *before* the commit as well as after it - the chunk render job owns the
	   back buffer for its whole length and this touches no voxels - so on a
	   settled machine most of a slide's deserialization is already paid for by
	   the time the window publishes.

	   A root is the unit for the same reason UnloadSerialization uses one: the
	   largest root hierarchy in any shipped level is 68 nodes. */
	StreamingBudget EntityStaging = StreamingBudget::Milliseconds(2.0);

	/* Putting staged *static* roots into the world, in units of roots. Phase 3.

	   Admission itself is a pointer push per node; what this bounds is what
	   admission sets off next PreTick - component registration, Awake, and the
	   VoxelBaker stamp of every renderer that just arrived (17.6 ms a slide,
	   RENDERING_PLAN 4c). Phase 5 bounds the stamp itself; until it does, this
	   is the only thing keeping a chunk's worth of art out of one frame.

	   **A count rather than a clock, and it is the one budget here that has to
	   be.** Every other budgeted loop pays for its work as it does it, so a
	   millisecond allowance measures exactly the thing being bounded. Admission
	   pays nothing now and everything next PreTick, so a wall-clock budget would
	   cheerfully admit a thousand roots in 20 microseconds and hand the
	   following frame the stall this phase exists to remove. The unit is the
	   cost driver.

	   Non-static roots are deliberately *not* bounded by this - see
	   Chunk::AdmitStagedGameplay and StreamingCounters::
	   MaxGameplayRootsPerAdmission for why that is the contract rather than an
	   oversight. */
	StreamingBudget EntityAdmission = StreamingBudget::Units(16);

	/* Refreshing the renderers of chunks that only moved, in units of *chunks*.
	   One FindEntitiesInChunk pass each, 0.06 ms. */
	StreamingBudget EntityRefresh = StreamingBudget::Units(1);

	/* Stamping newly admitted static renderers into the resident window, in
	   milliseconds. Phase 5.

	   This is the last unbounded main-thread cost a window transition had, and
	   it was two of them: `RenderSystem::OnComponentAdded` stamped every
	   renderer inline as its component registered (34.3 ms of a `PreTick`), and
	   `VoxelBaker::Bake` re-stamped every renderer that asked for it in one
	   pass (54.7 ms of a `Render`). They are one budgeted loop now.

	   **Wall clock, unlike EntityAdmission, and for the opposite reason.** A
	   stamp pays for itself as it writes - a voxel written is a voxel of cost -
	   so a millisecond allowance measures exactly the thing being bounded.
	   Admission pays nothing until the next PreTick, which is why that one has
	   to count roots instead.

	   **Resumption needs no cursor and holds no pointer**, which is the whole
	   reason this shape was chosen: a renderer that did not get baked still has
	   its Updated/UpdateRequested flags set, so the next frame's scan finds it
	   again. The scan itself is a handful of comparisons per renderer and was
	   already happening every frame. Nothing survives a frame boundary, so
	   there is no ledger-E1 shape here to defend. */
	StreamingBudget VoxelBaking = StreamingBudget::Milliseconds(2.0);

	/* How much of *one* renderer's model may be stamped in one pass, in units of
	   voxel samples - one (model voxel, scale offset) pair, which is what the
	   walk costs whether or not it writes. Phase 9.
	 *
	 * The budget above bounds the pass; this bounds its atom. Phase 5 left them
	 * the same thing and measured what that costs: `RiverBedStraight10` stamps
	 * 140,640 voxels in 22 ms, so a frame that started one was a 22 ms frame
	 * however little else it did, and those single renderers were every
	 * remaining violation of the hitch gate.
	 *
	 * A count rather than a clock, and deliberately so even though the work pays
	 * as it goes: this is the loop whose resumption a test has to single-step,
	 * and the only honest way to sweep resumption points is for the machine not
	 * to decide where they are (see the file comment). 8,192 is what phase 5
	 * measured the split at - about 1.3 ms of the riverbed - and it is a tuning
	 * knob, not a contract.
	 *
	 * **Do not lower it to "make streaming smoother" without re-running the
	 * occupancy oracle.** A sliced stamp that loses geometry loses more of it the
	 * finer it slices, and the symptom is a level that reads as content. */
	StreamingBudget VoxelBakingSamples = StreamingBudget::Units(8192);


	/* Stamping the level's static geometry into the far-field volume, in units
	   of *static roots*. Phase 4.

	   447 ms for Fishing_Village_Beat2, and it used to run inside
	   World::Initialize - off the frame loop, so it was 447 ms of a loading
	   screen not animating. 4 ms rather than 2: the volume is not sampled at all
	   until the build finishes (it reports itself unbuilt), so the cost of
	   taking longer is a horizon that arrives later, and a level with no
	   horizon reads worse than one frame at 20 ms. */
	StreamingBudget FarFieldBuild = StreamingBudget::Milliseconds(4.0);

	/* Serializing an outgoing chunk's roots out to JSON, in units of *roots*.
	   Phase 2.

	   A root is the smallest unit deliberately: the largest root hierarchy in
	   any shipped level is 68 nodes (measured across all 17 `.wld` files, 3430
	   roots; the next largest is 34 and the median is 1), against a whole
	   chunk's roots serializing in 1.10 ms. Bounding below a root would need a
	   resumable post-order walk holding half-built JSON and a raw `Entity*`
	   across frames - which is exactly where the experiment's ledger E1 lives -
	   to save a few tens of microseconds. See Chunk::PrepareUnloadBatch. */
	StreamingBudget UnloadSerialization = StreamingBudget::Milliseconds(2.0);

	/* RLE-encoding an outgoing chunk's voxels, in units of *runs*. Phase 2.

	   A run covers at most 256 source voxels, so charging per run gives the
	   budget a fine upper bound without putting a clock read in the per-voxel
	   comparison. */
	StreamingBudget VoxelEncoding = StreamingBudget::Milliseconds(2.0);

	static const StreamingBudgets& Get() { return s_Active; }

	/* Test-only, and the reason this is a struct rather than a set of
	   constants. Not thread-safe and not meant to be: the harness sets it
	   before it drives anything and the game never calls it. */
	static void Set(const StreamingBudgets& budgets) { s_Active = budgets; }
	static void Reset() { s_Active = StreamingBudgets(); }

private:
	static StreamingBudgets s_Active;
};
