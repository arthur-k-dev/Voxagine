#pragma once

class World;
class FarFieldVolume;

/* Fills a FarFieldVolume with the whole level's static geometry -
 * RENDERING_PLAN.md phase 4.
 *
 * **Where the level's voxels actually come from.** The plan expected to build
 * this from the RLE data in `Chunk::m_pEncodedVoxelData`. There is none to read:
 * a `.wld` stores per chunk only a `RootEntities` array, and a chunk's
 * `m_VoxelData` is a *product* of loading - the ground plane written by
 * `Chunk::UpdateGroundPlane` plus whatever `VoxelBaker` stamps once the chunk's
 * entities are in the world. `EncodeVoxels` runs on *unload*, so a chunk that
 * has never been resident has neither decoded nor encoded voxels. Building from
 * chunk voxel data would therefore have produced a far field showing only the
 * places the player had already been.
 *
 * So the source is the same thing the detail window's source is: the entities.
 * Each chunk's `RootEntities` values are deserialized into throwaway entities
 * that are never added to the world - the pattern `Chunk::LoadEntities` already
 * uses for an entity it decides not to keep - walked for `VoxRenderer`s, and
 * stamped through the shared `VoxelStamp.h` placement so the far field and the
 * window cannot disagree about where a model is.
 *
 * **The ground plane is deliberately not baked in**, and it was tried. The
 * endless ground `GetBackground` draws is not an approximation of the chunk
 * ground plane - it samples the resident window's own y=0 layer, which *is*
 * `Chunk::UpdateGroundPlane`'s output, tiled with `fmod`. So it already
 * reproduces the level's ground exactly, everywhere, and past the level's edge
 * as well, where a baked one would stop.
 *
 * Baking it in on top of that put the same surface in two places with two
 * different lightings - the far field's cells win on distance inside the level
 * and the analytic plane takes over outside it - and the result was a hard
 * brightness edge along the detail window's boundary. It also marked the whole
 * bottom brick layer occupied, so every downward ray descended to the fine
 * walk. Both costs, for a surface that was already being drawn.
 */
#include <chrono>
#include <vector>

#include "Core/ECS/Systems/Chunk/StreamingBudgets.h"

class Chunk;
class VoxModel;

namespace FarFieldBaker
{
	/* One far-field build, resumable. CHUNK_STREAMING_PLAN.md phase 4 (K5).
	 *
	 * `Build` below is 447 ms for Fishing_Village_Beat2 and it ran inside
	 * `World::Initialize` - which is off the frame loop, so it was 447 ms of a
	 * loading screen not animating and of the game not answering the
	 * compositor. It is the same walk in budgeted slices now, charged per
	 * *static root stamped*, driven from `ChunkSystem::Tick`.
	 *
	 * **The volume reports itself unbuilt for the whole of it**, which is what
	 * makes a partial build safe rather than merely bounded:
	 * `RenderContext::GetFarFieldGridSize` returns zero while `IsBuilt()` is
	 * false and every shader reads that as "no far field", so a half-filled
	 * volume is never sampled - not even the *previous level's*, which is the
	 * failure this ordering exists to prevent.
	 *
	 * The model pins are the piece with an ownership story: every model the
	 * level names is loaded once up front so a model shared by fifty entities
	 * is not read off disk fifty times, and they are released on completion
	 * *and* on cancellation. A build that is abandoned half-way must not leave
	 * the level's whole model set pinned for the rest of the session. */
	struct Progress
	{
		World* pWorld = nullptr;
		FarFieldVolume* pVolume = nullptr;

		/* Flattened once at Begin, so the walk has a stable order across
		   frames and does not depend on an unordered_map's iteration order
		   staying put while entities are being created. */
		std::vector<Chunk*> Chunks;
		std::vector<VoxModel*> Pinned;

		size_t uiChunk = 0;
		size_t uiRoot = 0;
		uint32_t uiRenderers = 0;
		uint32_t uiEntities = 0;

		std::chrono::steady_clock::time_point Start;
		bool bActive = false;
	};

	/* Resize, clear and pin. The volume is unbuilt from here until Continue
	   returns true. */
	void Begin(World* pWorld, FarFieldVolume& volume, Progress& progress);

	/* One budgeted slice, charged per static root. True when the whole level
	   has been stamped - at which point the pins are released and the volume
	   is marked built. */
	bool Continue(Progress& progress, StreamingBudget::Scope& budget);

	/* Abandon a build in progress: releases the pins and leaves the volume
	   unbuilt, i.e. not sampled. */
	void Cancel(Progress& progress);

	/* Rebuilds pVolume from every chunk in pWorld's ChunkSystem. Safe to call
	   with no chunk system or an empty level; both leave the volume unbuilt,
	   which the shader reads as "no far field". Main thread only - it
	   deserializes entities and loads models through the ResourceManager. */
	void Build(World* pWorld, FarFieldVolume& volume);

	/* Cross-checks placement against the chunks that are currently resident,
	   which hold the same geometry at full resolution, and names cells that
	   disagree. Returns the number of *phantom* cells - occupied in the far
	   field, empty in the chunk - which is placement being wrong. The reverse,
	   *missing*, is largely expected: the ground plane and every dynamic entity
	   are in a chunk's voxels and deliberately not in the far field.

	   Reads `Chunk::GetVoxelData`, not the voxel mapper. They hold the same
	   voxels, but the mapper is host-visible and uncached - likely VRAM over
	   PCIe, since it started preferring ReBAR - and sweeping the 75 million
	   voxels of a 768x128x768 window out of it takes long enough to look like
	   the editor has hung. It did; that was the first version of this. A chunk's
	   std::vector<Voxel> is ordinary cached memory. */
	uint32_t Validate(World* pWorld, const FarFieldVolume& volume);
}
