#pragma once

#include <cstdint>
#include <vector>

#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/Platform/Rendering/VoxelBrickGrid.h"

/* A whole voxel world with no GPU, no window and no World.
 *
 * DESTRUCTION_PLAN.md phase 0. Everything destruction touches - the CPU voxel,
 * the mapped GPU word, the occupancy bitmap, the brick counts and the owner
 * slot - is ordinary memory except the mapping, and the mapping is a
 * `uint32_t*` that the engine only ever writes (rule 1). So the entire write
 * path can run in a unit test at full fidelity by handing it a plain array.
 *
 * That is what makes a deterministic gauntlet possible at all. The alternative
 * - scripting the running game - cannot be made reproducible: the level is full
 * of entities with their own randomness, the camera slides the window, and
 * chunk streaming runs on job threads. This harness has none of that and
 * therefore hashes the same twice, which is the property phases 1-2 refactor
 * against.
 *
 * The writer below (`Set`/`Clear`) is deliberately the dumbest possible
 * implementation of rule 3: touch every representation, in the open, with no
 * batching or ordering cleverness. It is the *reference*, not the fast path.
 * Phase 1's VoxelEditBatch is checked against it by running the same edit
 * script through both and comparing hashes - a much stronger statement than
 * "the hash did not change", because it says the new path agrees with an
 * independent implementation rather than with its own previous self.
 */
class VoxelWorldHarness
{
public:
	/* Chunked exactly like the real grid: the chunk-index arithmetic in
	   VoxelGrid is a real source of off-by-ones, so a harness with one big
	   chunk would test the wrong thing. */
	VoxelWorldHarness(const UVector3& v3Size, const UVector3& v3ChunkSize);

	VoxelGrid& Grid() { return m_Grid; }
	VoxelBrickGrid& Bricks() { return m_Bricks; }

	uint32_t* Words() { return m_Words.data(); }
	const uint32_t* Words() const { return m_Words.data(); }

	const UVector3& Size() const { return m_v3Size; }
	uint32_t VoxelID(uint32_t uiX, uint32_t uiY, uint32_t uiZ) const
	{
		return uiX + uiY * m_v3Size.x + uiZ * m_v3Size.x * m_v3Size.y;
	}

	/* The reference write. Maintains CPU colour, mapped word, occupancy bitmap,
	   brick count and owner slot, in that order, for one voxel. Out of bounds
	   is a no-op, as it must be for every writer. */
	void Set(uint32_t uiX, uint32_t uiY, uint32_t uiZ, uint32_t uiColor, uint16_t uiOwnerSlot);
	void Clear(uint32_t uiX, uint32_t uiY, uint32_t uiZ);

	/* A ground layer at y = 0 plus a solid box, which is the shape every
	   destruction test wants: something to blow a hole in and something for the
	   connectivity check to call grounded. */
	void FillGround(uint32_t uiColor);
	void FillBox(const UVector3& v3Min, const UVector3& v3Size, uint32_t uiColor, uint16_t uiOwnerSlot);

	/* Every representation folded into one number. Two runs of the same script
	   must agree; see DESTRUCTION_PLAN.md phase 0. */
	uint64_t Hash() const;

	/* Occupied voxels according to the mapped words, counted directly rather
	   than taken from the brick counts - so a test can catch the counts and the
	   words disagreeing. */
	uint64_t CountOccupied() const;

	/* Recomputes the brick counts and occupancy bits from the words and returns
	   the number of disagreements. Zero is the invariant every write path owes
	   (rule 3b). */
	uint32_t Validate() const;

private:
	UVector3 m_v3Size;
	UVector3 m_v3ChunkSize;

	VoxelGrid m_Grid;
	VoxelBrickGrid m_Bricks;

	/* The chunk storage the grid points at. Held by value here because there is
	   no ChunkSystem to own it; deque-like stability is why these are separate
	   vectors rather than one big one being resized. */
	std::vector<std::vector<Voxel>> m_ChunkVoxels;
	std::vector<VoxelOwnerVolume> m_ChunkOwners;

	/* Stands in for the mapped voxel buffer. */
	std::vector<uint32_t> m_Words;

	/* The brick mapper's mirror. Written, never read - same contract as the
	   real one. */
	std::vector<uint32_t> m_BrickMirrorFront;
	std::vector<uint32_t> m_BrickMirrorBack;
};
