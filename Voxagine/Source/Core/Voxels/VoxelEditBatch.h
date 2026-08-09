#pragma once

#include <cstdint>
#include <vector>

#include "Core/Math.h"

class VoxelGrid;
class VoxelBrickGrid;

/* A voxel written into the world by something that is not a VoxRenderer needs a
   rasterized proxy or it is only drawn when some unrelated model's box happens
   to cover the pixel. RenderSystem is the real implementation; this exists so
   the batch can be built and tested with no RenderSystem at all. */
class ILooseVoxelSink
{
public:
	virtual ~ILooseVoxelSink() = default;

	/* Grid space, i.e. the coordinates the batch takes. */
	virtual void AddLooseVoxel(const Vector3& v3GridPosition) = 0;
};

/* Everything one voxel lives in, gathered in one place.
 *
 * Plain data with no virtuals on purpose. A batch touches every field per voxel
 * and destruction writes tens of thousands of them in a burst, so an interface
 * here would be a virtual call per representation per voxel. The only thing
 * behind an interface is the loose-voxel sink, which is called for a small
 * minority of writes.
 *
 * RenderSystem::MakeEditTarget assembles this; the phase 0 harness assembles
 * the same thing out of a std::vector<uint32_t>, which is what lets the write
 * path be unit-tested with no GPU. */
struct VoxelEditTarget
{
	VoxelGrid* pGrid = nullptr;
	VoxelBrickGrid* pBricks = nullptr;

	/* The mapped voxel buffer. Write-only from the CPU (rule 1) - the batch
	   never reads it and does not need to, because old occupancy comes from
	   the brick grid's bitmap and old colour from the CPU voxel. */
	uint32_t* pWords = nullptr;
	uint32_t uiWordCount = 0;

	UVector3 v3WindowSize = UVector3(0, 0, 0);

	/* Optional. Null means ownerless writes go unregistered, which is correct
	   for a harness and wrong for the engine. */
	ILooseVoxelSink* pLooseVoxels = nullptr;

	bool IsValid() const;
};

/* The one place a voxel is written.
 *
 * DESTRUCTION_PLAN.md phase 1. Rule 3 says every representation moves together
 * or not at all - CPU voxel, mapped word, occupancy bitmap, brick count, owner
 * slot, and the loose registry for a voxel no renderer owns. Before this,
 * that was an obligation on each call site and the ledger is largely a list of
 * call sites that met some of it: three sites made the paired colour calls and
 * none of them cleared the owner slot, so three of every four destroyed voxels
 * kept their dead owner (D5).
 *
 * Two decisions worth knowing:
 *
 * **Edits apply immediately, they are not buffered and flushed.** The plan
 * sketched a brick-sorted deferred apply. Deferring changes semantics - the
 * integrity checker reads the grid immediately after a destruction burst and
 * would see the old world - and brick-sorting is an optimization with no
 * measurement behind it yet (rule 11). What the batch keeps instead is the set
 * of *dirty bricks*, which is the input phase 2's seeding and phase 4's
 * incremental connectivity actually need. Sorting the writes can be added
 * later behind a measurement; nothing about the API assumes it is not there.
 *
 * **A clear clears the owner.** That is the one deliberate behavioural change
 * in phase 1 and it is what D5 asks for. A cleared voxel with a live owner slot
 * lies to `VoxelBaker::Clear`'s "is somebody else's voxel here" test and to the
 * destruction path's destructibility check.
 */
class VoxelEditBatch
{
public:
	explicit VoxelEditBatch(const VoxelEditTarget& target);

	/* Writes a colour and an owner. An ownerless set registers a loose voxel,
	   because nothing else will submit a proxy for it. Returns false if the
	   position was rejected - out of bounds, non-finite, or in a chunk that is
	   not resident. */
	bool Set(const Vector3& v3GridPosition, uint32_t uiColor, uint16_t uiOwnerSlot);

	/* Colour to zero *and* owner to none. See the class comment. */
	bool Clear(const Vector3& v3GridPosition);

	/* Every voxel of a box. Returns how many were actually written. Clamped to
	   the window rather than rejected, so a region straddling the edge does the
	   part of the work it can. */
	uint32_t ClearRegion(const UVector3& v3Min, const UVector3& v3Size);

	uint32_t GetWrites() const { return m_uiWrites; }
	uint32_t GetRejected() const { return m_uiRejected; }

	/* Bricks whose contents changed, deduplicated, in ascending order. Empty
	   until something has been written. Phase 2 seeds integrity from this
	   instead of pushing nine voxel hashes per destroyed voxel (D6). */
	const std::vector<uint32_t>& GetDirtyBricks();

	/* Non-finite input is rejected centrally here rather than at each call
	   site, and it is worth being loud about: a NaN passes every comparison in
	   a rejection test, so the coordinate that reaches an index is INT32_MIN
	   and the write lands two billion elements past the buffer. That has
	   happened in this tree. */
	uint32_t GetNonFiniteRejections() const { return m_uiNonFinite; }

private:
	/* Resolves and validates one position, then writes every representation.
	   The single choke point; Set/Clear/ClearRegion are its callers. */
	bool Write(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor, uint16_t uiOwnerSlot);

	/* Shared front half of Set and Clear: finiteness, sign and bounds. */
	bool Resolve(const Vector3& v3GridPosition, uint32_t& o_uiX, uint32_t& o_uiY, uint32_t& o_uiZ);

	VoxelEditTarget m_Target;

	std::vector<uint32_t> m_DirtyBricks;
	bool m_bDirtyBricksSorted = true;

	uint32_t m_uiWrites = 0;
	uint32_t m_uiRejected = 0;
	uint32_t m_uiNonFinite = 0;
};
