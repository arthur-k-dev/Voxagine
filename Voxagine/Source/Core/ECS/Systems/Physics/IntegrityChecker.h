#pragma once
#include <cstdint>
#include <deque>
#include <unordered_set>
#include <vector>

#include "Core/Math.h"

class VoxelGrid;

/* Finds voxel islands that are no longer connected to the ground, so
   PhysicsSystem can turn them into falling debris.

   This was an IntegrityJob running on a worker thread against the live
   VoxelGrid, and that was unsound in two separate ways. It read
   m_ChunkVolumes - an ordinary std::vector of pointers - while the main thread
   mutated it on every chunk residency change, so a reallocation under the
   reader handed it a freed buffer rather than a stale value. And nothing ever
   joined the worker: ~PhysicsSystem called Stop(), which only sets an atomic,
   then destroyed the very grid the worker held a pointer to.

   It runs on the main thread now. There was no throughput to lose - the worker
   slept 10 ms between items, so this was never latency- or bandwidth-critical
   - and on the main thread the grid cannot move underneath the reader at all,
   which removes the whole class rather than narrowing it.

   The one thing that buys is bounded: a single island can be very large, and
   running one to completion inside a tick would trade a data race for a frame
   spike. So the walk is resumable - Process() spends a fixed number of voxel
   visits and keeps its stack across ticks.

   The grid can still change between two slices of one walk. That is not a
   correctness problem: a voxel destroyed meanwhile simply reads as inactive
   and is not collected, which is the same answer a later check would give. */
class IntegrityChecker
{
public:
	/* Voxel visits one Process() call may spend. A visit is a bounds-checked
	   grid lookup plus a hash-set probe; 20k of them costs well under a
	   millisecond, and the queue drains over the following ticks rather than
	   in one. */
	static constexpr uint32_t VISIT_BUDGET_PER_TICK = 20000;

	void SetVoxelGrid(const VoxelGrid* pGrid) { m_pVoxelGrid = pGrid; }

	void EnqueueBulk(std::vector<uint64_t>& checks);

	/* Spends at most uiVisitBudget voxel visits across as many islands as fit,
	   appending each completed ungrounded island to o_results. An island that
	   reaches the ground contributes nothing, which is what the job did too. */
	void Process(uint32_t uiVisitBudget, std::vector<std::vector<uint64_t>>& o_results);

	/* Drops queued and part-finished work. Used when the world is paused or
	   swapped, where the grid the pending positions refer to is about to stop
	   meaning anything. */
	void Reset();

	/* The one position hash. There used to be two open-coded copies of this -
	   here and in ApplySphericalDestruction - and both truncated float to
	   uint16_t, so a negative coordinate wrapped to somewhere near 65535 and
	   named a voxel on the far side of the world instead of being rejected
	   (ledger D11).

	   The unsigned overload is the real one and cannot be given a negative.
	   The Vector3 overload checks, and returns k_uiInvalidHash for anything
	   negative, non-finite or past 16 bits; EnqueueBulk drops those. */
	static constexpr uint64_t k_uiInvalidHash = UINT64_MAX;

	static uint64_t PositionToHash(uint32_t uiX, uint32_t uiY, uint32_t uiZ)
	{
		return (static_cast<uint64_t>(uiX & 0xFFFFu) << 32) |
		       (static_cast<uint64_t>(uiY & 0xFFFFu) << 16) |
		        static_cast<uint64_t>(uiZ & 0xFFFFu);
	}

	static uint64_t PositionToHash(const Vector3& v3Position);
	static Vector3 HashToPosition(uint64_t uiHash);

	/* Is this voxel part of the walk currently in progress?
	 *
	 * A walk is resumable - it spends a visit budget and carries its collected
	 * set across ticks - so a voxel can be *already enumerated* into an island
	 * that has not been reported yet. Debris that bakes onto such a voxel in
	 * the meantime is not in the island, so conversion clears its support and
	 * leaves it floating, and nothing will ever seed it.
	 *
	 * Found by the selftest, not by reasoning: two to five voxels per scenario,
	 * stable across settle time, always inside a tower that had been cut. The
	 * bake path asks this as well as the pending-island set. */
	bool IsClassifying(uint64_t uiHash) const { return m_CheckedVoxels.count(uiHash) != 0; }

	/* Everything the memo below believes stops being true the moment a voxel
	   changes, and the checker notices that for itself: it compares the grid's
	   write generation on every entry point and drops the memo when it has
	   moved. Nothing has to remember to call this.

	   That is not defensive design, it is a bug that already happened. The
	   first version had callers invalidate, and the gauntlet - which writes
	   through the same batch but is not PhysicsSystem - kept a stale memo and
	   found 20 islands where it should have found 100.

	   Still public, for the cases that are not writes: a world pause, a grid
	   swap. Cheap either way - the sets only hold what a drain actually walked,
	   which is bounded by the visit budget times the ticks it spans. */
	void Invalidate();

	/* What the memo saved, for the phase 4 acceptance measurement. */
	struct Stats
	{
		uint64_t uiSeedsOffered = 0;
		uint64_t uiSeedsDeduplicated = 0;
		uint64_t uiSeedsSkippedByMemo = 0;
		uint64_t uiVisits = 0;
		uint64_t uiMemoHits = 0;
		uint64_t uiIslandsEmitted = 0;
	};

	const Stats& GetStats() const { return m_Stats; }
	void ResetStats() { m_Stats = Stats(); }

	/* The exhaustive answer for one seed, computed from scratch with no memo
	   and no budget. This is the oracle VOXAGINE_INTEGRITY_AUDIT compares
	   against: it is deliberately the algorithm as it was before phase 4, kept
	   so the memoised one can be checked against something independent rather
	   than against itself.

	   Returns true when the seed's component is ungrounded, filling o_island
	   with it, sorted. Unbudgeted by design - not for per-frame use. */
	bool ClassifyExhaustive(const Vector3& v3Seed, std::vector<uint64_t>& o_island) const;

private:
	/* Ends the island in progress, reporting it only if it never touched the
	   ground and actually collected something. Either way its voxels go into
	   the memo, which is the point of phase 4. */
	void EndCheck(bool bReport, std::vector<std::vector<uint64_t>>& o_results);

	const VoxelGrid* m_pVoxelGrid = nullptr;

	std::deque<uint64_t> m_Pending;

	/* Mirrors m_Pending, so a seed is deduplicated against everything queued
	   rather than only against the batch it arrived in (ledger D6's consumer
	   half). The old code sorted and uniqued each EnqueueBulk call in
	   isolation: nine seeds per destroyed voxel from one explosion collapsed
	   nicely, and the next explosion re-queued every one of them. */
	std::unordered_set<uint64_t> m_PendingSet;

	bool m_bCheckActive = false;
	std::vector<Vector3> m_CheckStack;
	std::unordered_set<uint64_t> m_CheckedVoxels;

	/* The memo, and the whole of phase 4.
	 *
	 * The walk used to *discard* everything it had collected the moment it
	 * touched the ground - so the next seed standing on the same building
	 * flooded the entire building again, and one explosion produces thousands
	 * of seeds on the same few structures. That is the shape of #27: cost
	 * proportional to seeds times island size, growing as a level fragments.
	 *
	 * Recording the answer instead makes each voxel's classification cost paid
	 * once per epoch rather than once per seed, and a walk that meets a voxel
	 * already known grounded is itself grounded and stops immediately.
	 *
	 * Sets rather than bitmaps deliberately. A bitmap over the window is 9.4
	 * MiB and would have to be cleared on every invalidation - a memset per
	 * explosion. These hold only what a drain actually walked, so clearing them
	 * costs what they are worth. */
	std::unordered_set<uint64_t> m_GroundedMemo;
	std::unordered_set<uint64_t> m_IslandMemo;

	/* The grid's write generation the memo was built against. */
	uint64_t m_uiMemoGeneration = 0;

	/* Drops the memo if the grid has been written since it was built. */
	void RefreshMemo();

	Stats m_Stats;
};
