#pragma once

#include "VoxelMesher.h"

struct VoxFrame;

/* DYNAMIC_MODELS_PLAN.md phase 1. One growing CPU-side quad list shared by
 * every VoxFrame in the game - the GPU form (a Mapper, uploaded once and
 * never evicted, the same idiom m_pVoxelMapper already uses) is phase 2's
 * job once a pass exists to read it. Nothing reads this store yet; it exists
 * so the mesh a renderer needs is already built and cached by the time phase
 * 2 wires a pass to it.
 *
 * A process-wide singleton rather than a RenderContext member for now,
 * because meshing is pure CPU geometry with no GPU resource behind it at this
 * phase - RenderContext is where it moves once it owns a Mapper. */
class ModelMeshStore
{
public:
	static ModelMeshStore& Get();

	/* Builds pFrame's mesh on first call and caches the result on the frame
	 * itself (VoxFrame::m_uiMeshFirstQuad/m_uiMeshQuadCount); every later call
	 * for the same frame is one comparison. Safe to call for a null frame or
	 * one that meshes to nothing (an all-transparent palette) - both return a
	 * zero-count result and are not re-attempted. */
	VoxelMesher::Result EnsureMeshed(const VoxFrame* pFrame);

	const std::vector<uint32_t>& GetQuads() const { return m_Quads; }
	uint32_t GetTotalQuadCount() const { return static_cast<uint32_t>(m_Quads.size() / 3); }
	uint32_t GetMeshedFrameCount() const { return m_uiMeshedFrames; }

private:
	std::vector<uint32_t> m_Quads;
	uint32_t m_uiMeshedFrames = 0;
};
