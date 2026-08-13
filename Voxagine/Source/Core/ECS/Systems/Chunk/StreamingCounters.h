#pragma once

#include <atomic>
#include <cstdint>

/* Exact, machine-independent counts of what streaming did. T4.
 *
 * The destruction plan's split, applied here: **work is gated, time is
 * reported**. A commit count, a roots-per-pass maximum or a
 * flush-on-the-wrong-thread count is the same number in Debug and Release, on
 * this machine and on a CI runner, so `Tests/Baselines/perf.txt` can fail a run
 * on any increase with no flakiness at all. Wall-clock numbers cannot do that
 * and are printed rather than enforced.
 *
 * Cheap enough to leave on in the shipping build: every counter here is
 * incremented once per chunk, per group or per state transition, never per
 * voxel or per entity. The one that looks per-voxel - VoxelWordsWritten - is
 * added to once per chunk region with the region's size.
 *
 * Atomic because the render job increments VoxelWordsWritten from a worker
 * while the main thread may be reading the set; relaxed, because nothing here
 * orders anything.
 */
struct StreamingCounters
{
	/* US_COMMIT entries. Exactly one per update group, ever: the atomic-publish
	   rule (R2) is precisely the statement that a window is published once. */
	std::atomic<uint64_t> Commits{ 0 };

	/* Update groups that reached US_COMMIT. Commits / CommittedGroups must be
	   1 - the two are counted separately so a scenario can say which side of
	   the ratio broke. */
	std::atomic<uint64_t> CommittedGroups{ 0 };

	/* Groups erased before they committed - the walk-back (R5). Not a defect;
	   recorded so a cancellation scenario can assert it actually cancelled
	   something rather than silently completing. */
	std::atomic<uint64_t> CancelledGroups{ 0 };

	/* Chunk regions written into the window, and the words they covered.
	   A first-load chunk's ground-only fast path writes the same words as a
	   full one, so this does not move when that path is taken - which is the
	   point: it measures the publish, not the scan. */
	std::atomic<uint64_t> ChunkRegionsWritten{ 0 };
	std::atomic<uint64_t> VoxelWordsWritten{ 0 };

	/* Root entities deserialized and admitted in one entity-work pass, largest
	   seen. Unbounded on master and after phase 1; phase 3 ratchets it down
	   against StreamingBudgets::EntityWork. */
	std::atomic<uint64_t> MaxRootsPerEntityPass{ 0 };

	/* Root entities serialized out in one unload slice, largest seen. Bounded
	   by StreamingBudgets::UnloadSerialization as of phase 2, so this is the
	   number that says the bound is real. */
	std::atomic<uint64_t> MaxRootsPerUnloadSlice{ 0 };

	/* Root entities serialized out since the last reset, running total. The
	   maximum above says the bound holds; this says how far the unload has got,
	   which is what a scenario needs to interrupt one at a chosen point. */
	std::atomic<uint64_t> UnloadRootsSerialized{ 0 };

	/* RLE runs written in one encode slice, largest seen. Same role, against
	   StreamingBudgets::VoxelEncoding. */
	std::atomic<uint64_t> MaxEncodeRunsPerSlice{ 0 };

	/* Unload items for a chunk that was never actually loaded. Skipped rather
	   than serialized, because serializing one writes an empty root list and an
	   empty voxel stream over the only copy of both. Not zero - a walk that
	   re-crosses a boundary faster than a group drains produces them, and the
	   number is here so that a phase which fixes the T_MOVE-for-an-unloaded-chunk
	   scheduling underneath it can watch it go to zero. */
	std::atomic<uint64_t> UnloadsOfUnloadedChunks{ 0 };

	/* Encodes abandoned part-way by a cancelled group (R5). Not a defect - the
	   chunk keeps every voxel it had and is simply still resident - but a
	   cancellation scenario has to be able to say it actually interrupted one. */
	std::atomic<uint64_t> CancelledEncodes{ 0 };

	/* Chunk voxel/owner allocations served from the reuse pool rather than from
	   the allocator, and the ones the pool could not serve. A slide turns over
	   three chunks, so a settled game should be serving every one of them from
	   the pool - 48 MiB allocated and freed per chunk is what E10 is about. */
	std::atomic<uint64_t> ChunkStorageReused{ 0 };
	std::atomic<uint64_t> ChunkStorageAllocated{ 0 };

	/* --- T5: invariants, counted rather than only asserted -------------------
	   An assert fires once and only in Debug. These make the same violations
	   visible to a Release scenario run and to the perf gate, where "must stay
	   zero" is a baseline entry like any other. */

	/* A main-thread flush of the *back* buffer's dirty bits while the chunk
	   render job owns it. This is M1: the data race and the 68.8 ms hitch. */
	std::atomic<uint64_t> BackBufferFlushRaces{ 0 };

	/* A window publish - offset, volumes, swap - reached from outside the
	   commit transaction. */
	std::atomic<uint64_t> PublishesOutsideCommit{ 0 };

	/* A static renderer re-stamped by VoxelBaker while its chunk instance was
	   restored from encoded chunk storage - M7. The decoded voxels are what that
	   chunk looked like when it left, damage included; stamping the pristine
	   model over them is exactly "destroyed terrain comes back". Must stay
	   zero, which is why it is a counter and not only a comment. */
	std::atomic<uint64_t> ChunkInstanceRestamps{ 0 };

	static StreamingCounters& Get() { return s_Counters; }

	static void Reset()
	{
		StreamingCounters& c = Get();
		c.Commits.store(0, std::memory_order_relaxed);
		c.CommittedGroups.store(0, std::memory_order_relaxed);
		c.CancelledGroups.store(0, std::memory_order_relaxed);
		c.ChunkRegionsWritten.store(0, std::memory_order_relaxed);
		c.VoxelWordsWritten.store(0, std::memory_order_relaxed);
		c.MaxRootsPerEntityPass.store(0, std::memory_order_relaxed);
		c.MaxRootsPerUnloadSlice.store(0, std::memory_order_relaxed);
		c.UnloadRootsSerialized.store(0, std::memory_order_relaxed);
		c.MaxEncodeRunsPerSlice.store(0, std::memory_order_relaxed);
		c.CancelledEncodes.store(0, std::memory_order_relaxed);
		c.UnloadsOfUnloadedChunks.store(0, std::memory_order_relaxed);
		c.ChunkStorageReused.store(0, std::memory_order_relaxed);
		c.ChunkStorageAllocated.store(0, std::memory_order_relaxed);
		c.BackBufferFlushRaces.store(0, std::memory_order_relaxed);
		c.PublishesOutsideCommit.store(0, std::memory_order_relaxed);
		c.ChunkInstanceRestamps.store(0, std::memory_order_relaxed);
	}

	static void RaiseMax(std::atomic<uint64_t>& target, uint64_t uiValue)
	{
		uint64_t uiSeen = target.load(std::memory_order_relaxed);

		while (uiValue > uiSeen &&
			!target.compare_exchange_weak(uiSeen, uiValue, std::memory_order_relaxed))
		{
		}
	}

private:
	static StreamingCounters s_Counters;
};
