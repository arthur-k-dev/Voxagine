#pragma once

#include <cstdint>

class VoxelBrickGrid;

/* The resident voxel window, reduced to the five questions chunk streaming asks
 * of it. CHUNK_STREAMING_PLAN.md T1.
 *
 * ChunkSystem used to reach through World -> Application -> Platform ->
 * RenderContext for its voxel buffers, and that is what made the streaming
 * state machine untestable: RenderContext's front/back pointers come out of a
 * Mapper, a Mapper needs a device, and a World cannot even be constructed
 * without a RenderSystem that resizes one. VoxelWorldHarness gets away with a
 * plain std::vector<uint32_t> for the mapping because the *write* path only
 * ever writes it (rule 1); the streaming path is the same, and this interface
 * is what lets a test say so.
 *
 * RenderContext implements it over its voxel Mapper and brick grid.
 * StreamingHarness implements it over two vectors and a VoxelBrickGrid it owns.
 * Nothing else should: this is a seam, not an abstraction layer, and the two
 * implementations are the whole point of it existing.
 *
 * The front buffer is what the GPU reads and what every point write
 * (ModifyVoxel, VoxelBaker) touches. The back buffer is what a window slide is
 * built into, wholesale, on a worker; Swap() publishes it. Both buffers are
 * host-visible and write-combined in the real implementation, so a caller may
 * write them freely and must never read them back - see the ReBAR note in
 * CLAUDE.md.
 */
class IVoxelWindow
{
public:
	virtual ~IVoxelWindow() = default;

	/* The live window: what the GPU is reading this frame. */
	virtual uint32_t* GetFrontData() = 0;

	/* The window under construction. Null where the implementation is
	   single-buffered; every caller checks, because ChunkSystem's first-load
	   path writes the front buffer directly. */
	virtual uint32_t* GetBackData() = 0;

	/* Words in either buffer. Streaming compares it against the voxel grid's
	   own voxel count and declines to write if they disagree - a window that
	   has been resized out from under a group is not one to publish into. */
	virtual uint32_t GetWordCount() const = 0;

	/* The occupancy hierarchy over the same two buffers. Front and back are
	   selected per call by the bBack flag every method there already takes. */
	virtual VoxelBrickGrid& GetBrickGrid() = 0;

	/* Retire everything still reading the front buffer. Swap() reverses the two
	   buffers' roles, so the buffer an in-flight submission is fetching from
	   becomes the CPU's next writable back buffer the moment it returns; this
	   is the only thing standing between that and a torn frame. A no-op with no
	   GPU, which is exactly why it belongs on the interface rather than being
	   called through the render context at the one site that needs it. */
	virtual void WaitForReaders() {}

	/* Publish. Front and back exchange roles, here and in the brick grid, which
	   must move together - a brick grid built against the pre-swap window
	   describes the wrong voxels. */
	virtual void Swap() = 0;
};
