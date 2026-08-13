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

	/* --- phase 3: staged construction and admission --------------------------
	   Loading a chunk is two questions - construct the roots, then put them in
	   the world - and these say that each half stayed inside its budget. */

	/* Roots constructed in one staging slice, largest seen. Bounded by
	   StreamingBudgets::EntityStaging. */
	std::atomic<uint64_t> MaxRootsPerStagingSlice{ 0 };

	/* Static roots admitted in one slice, largest seen. Bounded by
	   StreamingBudgets::EntityAdmission. */
	std::atomic<uint64_t> MaxRootsPerAdmissionSlice{ 0 };

	/* Non-static roots admitted in one frame, largest seen - the *contract*
	   number (E3). Deliberately unbounded: every gameplay root of the incoming
	   window enters together so no cross-link between two of them is ever
	   transiently null. The worst case in any shipped level is 430 roots for a
	   three-chunk slide (Walking_Through_The_Maru_Beat3), and admission is a
	   pointer push per node - the cost that scales with this is the per-renderer
	   stamp it induces, which is phase 5's budget and not this one's. */
	std::atomic<uint64_t> MaxGameplayRootsPerAdmission{ 0 };

	/* Roots constructed but not yet admitted, across every chunk, largest seen.
	   The high-water mark of what staging is holding. */
	std::atomic<uint64_t> MaxStagedRoots{ 0 };

	/* Roots the serializer refused - an unknown EntityType, most likely a
	   reflection-only TU that never made it into the binary. Skipped, so a
	   hostile or older chunk cannot wedge the load (T7). */
	std::atomic<uint64_t> StagedRootsRejected{ 0 };

	/* Chunks whose staged-but-unadmitted roots were deleted by a cancellation
	   (R5). Not a defect - the roots are re-staged from the same JSON - but a
	   scenario has to be able to say it interrupted one. */
	std::atomic<uint64_t> CancelledStagings{ 0 };

	/* Frames in which gameplay was held because the initial window was not
	   resident yet (R1). Zero once the world is running; non-zero exactly at a
	   world's first frames, and the number phase 4 puts a loading screen over. */
	std::atomic<uint64_t> GameplayTicksHeld{ 0 };

	/* --- phase 3: world links (M3 / K6) --------------------------------------
	   A serialized entity reference whose target has not been admitted yet is
	   retried rather than dropped; these say how much retrying there is. */

	/* Links still waiting for their target, largest seen. */
	std::atomic<uint64_t> MaxPendingWorldLinks{ 0 };

	/* Links given up on after k_uiMaxWorldLinkRetries attempts - the target
	   never arrived. Not necessarily a defect (a level can reference an entity
	   that no longer exists) but it is the number that says a cross-chunk link
	   is being lost, so a scenario gates on it. */
	std::atomic<uint64_t> WorldLinksAbandoned{ 0 };

	/* Links dropped because the *source* entity is gone before the link could
	   be made. This is M3: the source used to be a raw rttr::instance and this
	   case was a use-after-free rather than a count. */
	std::atomic<uint64_t> WorldLinksSourceLost{ 0 };

	/* Links nulled because their target entity was destroyed - M8. Not a
	   defect: a cross-chunk reference whose target's chunk unloaded *should*
	   become null, and before this it stayed a dangling pointer that the
	   serializer then dereferenced. */
	std::atomic<uint64_t> EntityLinksCleared{ 0 };

	/* Links the repair could not reach - a sequential-container element, or a
	   property type a null could not be converted to. Zero on every shipped
	   level; a number rather than a crash the day one appears. */
	std::atomic<uint64_t> EntityLinksLeftDangling{ 0 };

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
		c.MaxRootsPerStagingSlice.store(0, std::memory_order_relaxed);
		c.MaxRootsPerAdmissionSlice.store(0, std::memory_order_relaxed);
		c.MaxGameplayRootsPerAdmission.store(0, std::memory_order_relaxed);
		c.MaxStagedRoots.store(0, std::memory_order_relaxed);
		c.StagedRootsRejected.store(0, std::memory_order_relaxed);
		c.CancelledStagings.store(0, std::memory_order_relaxed);
		c.GameplayTicksHeld.store(0, std::memory_order_relaxed);
		c.MaxPendingWorldLinks.store(0, std::memory_order_relaxed);
		c.WorldLinksAbandoned.store(0, std::memory_order_relaxed);
		c.WorldLinksSourceLost.store(0, std::memory_order_relaxed);
		c.EntityLinksCleared.store(0, std::memory_order_relaxed);
		c.EntityLinksLeftDangling.store(0, std::memory_order_relaxed);
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
