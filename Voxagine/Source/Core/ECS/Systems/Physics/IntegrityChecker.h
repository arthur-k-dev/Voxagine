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

private:
	/* Ends the island in progress, reporting it only if it never touched the
	   ground and actually collected something. */
	void EndCheck(bool bReport, std::vector<std::vector<uint64_t>>& o_results);

	const VoxelGrid* m_pVoxelGrid = nullptr;

	std::deque<uint64_t> m_Pending;

	bool m_bCheckActive = false;
	std::vector<Vector3> m_CheckStack;
	std::unordered_set<uint64_t> m_CheckedVoxels;
};
