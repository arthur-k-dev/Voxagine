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
	/* Entity work after the window commits: deserializing an incoming chunk's
	   roots and admitting them, and refreshing the renderers of chunks that only
	   moved.

	   **Unbounded, and that is the honest state of it.** Phase 1 moved this
	   work to after the atomic commit and changed nothing else about it; it is
	   still `Chunk::LoadEntities` in full, 41.6 ms of it per incoming chunk
	   (phase 0's baseline). Phases 2 and 3 make it resumable, at which point
	   this becomes a real number and `Tests/Baselines/perf.txt`'s
	   nodes-per-slice counter ratchets down against it. Until then the counter
	   records what master actually does. */
	StreamingBudget EntityWork = StreamingBudget::Unbounded();

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
