#pragma once

#include "Core/Math.h"

#include <cstdint>

/* Where a partial stamp stopped, and everything it has to remember to carry on
 * as though it had never stopped. CHUNK_STREAMING_PLAN.md phase 9.
 *
 * Its own header only because VoxRenderer::BakeData holds one and VoxelStamp.h -
 * where the walk that reads it lives - includes VoxRenderer.h.
 *
 * Two of the four fields are not loop counters: LastPosition is the walk's
 * duplicate suppression, and getting it wrong changes which voxels are
 * *emitted* rather than just where the walk resumes. That is state of the walk,
 * owned by the walk, which is why it is here rather than in the caller.
 *
 * LastPosition starts at the origin rather than at "nothing seen yet", which is
 * deliberate and is what the unbounded walk has always done: a model whose first
 * voxel lands on exactly grid (0,0,0) does not emit it. Preserving the quirk is
 * the point - the acceptance for this phase is that a sliced stamp produces the
 * *identical* voxel set, not a better one.
 */
struct VoxelStampCursor
{
	uint32_t uiVoxel = 0;

	uint32_t uiScaleX = 0;
	uint32_t uiScaleY = 0;
	uint32_t uiScaleZ = 0;

	Vector3 LastPosition = Vector3(0.f);

	bool IsAtStart() const
	{
		return uiVoxel == 0 && uiScaleX == 0 && uiScaleY == 0 && uiScaleZ == 0;
	}
};
