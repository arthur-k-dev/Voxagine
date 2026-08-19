#pragma once

#include <cstdint>
#include <vector>

struct VoxFrame;

/* DYNAMIC_MODELS_PLAN.md phase 1. Turns one VoxFrame's solid voxels into
 * greedy-meshed quads: one quad per maximal same-colour, same-AO rectangle of
 * exposed faces, instead of one cube per voxel. Purely CPU, run once per
 * distinct frame the first time a dynamic VoxRenderer needs it - a model
 * never changes its own voxels at runtime (DYNAMIC_MODELS_PLAN.md, "Why
 * meshes and not ray marching"), so the result is cached forever on the
 * VoxFrame itself.
 *
 * Each quad is 3 packed uint32_t words, matching the granularity every other
 * Mapper in this engine already uses (see ModelMeshStore in RenderContext):
 *
 *   word0  bits 0-7   origin.x   (voxel-space, the rectangle's lower corner)
 *          bits 8-15  origin.y
 *          bits 16-23 origin.z
 *          bits 24-25 axis       (0 = x, 1 = y, 2 = z)
 *          bit  26    sign       (0 = -1, 1 = +1 - the face's outward normal)
 *   word1  bits 0-7   extentU    (voxel-space size along (axis+1)%3)
 *          bits 8-15  extentV    (voxel-space size along (axis+2)%3)
 *          bits 16-17 AO(-u,-v)  corner occlusion level, 0 (open) .. 3 (closed)
 *          bits 18-19 AO(+u,-v)
 *          bits 20-21 AO(-u,+v)
 *          bits 22-23 AO(+u,+v)
 *   word2  colour, 0xAABBGGRR straight from the model's palette.
 *
 * The four AO levels are the quad's actual geometric corners, in (u,v) space
 * with u=(axis+1)%3, v=(axis+2)%3 - a vertex shader bilinearly interpolating
 * them across the quad reproduces AmbientOcclusion.hlsl's GetSkyVisibility
 * exactly, because merging only ever joins voxels whose own corner AO already
 * agrees (see VoxelMesher.cpp) - the merged quad's outer corners equal every
 * interior voxel's identical corner value, so nothing is approximated by
 * merging. Each level is one of exactly {0, 1, 2, 3}: GetVertexAO's three
 * boolean inputs (two sides, one diagonal) only ever produce those four
 * sums, which is what makes 2 bits exact rather than a quantization.
 *
 * word2 is deliberately NOT the world voxel buffer's tag-byte word (rule 3,
 * RENDERING_PLAN.md). A mesh quad's existence already says "this face is
 * solid" - there is no occupancy test left to encode - so its alpha is the
 * palette's real alpha (usually 255), not a VoxelStateTag. Anything that
 * later reads this colour must not run it through VOXEL_STATE_MASK or
 * IsEmissiveVoxel, which assume the other convention.
 *
 * A voxel at grid index v occupies [v, v+1) - RENDERING_PLAN.md phase 2's
 * convention, kept here. So the face plane sits at origin[axis] for a
 * negative-sign face and at origin[axis]+1 for a positive one; the vertex
 * shader that eventually consumes this decides how to turn that into a
 * quad's four corners. Nothing here assumes a particular vertex shader. */
namespace VoxelMesher
{
	struct Result
	{
		uint32_t m_uiFirstQuad = 0;
		uint32_t m_uiQuadCount = 0;
	};

	/* Greedy-meshes pFrame's solid voxels and appends the quads to
	 * pOutQuads (3 words each, see above). Returns where they landed.
	 * pOutQuads is never cleared or resized down - callers share one growing
	 * store across every resident model frame. */
	Result BuildFrameMesh(const VoxFrame* pFrame, std::vector<uint32_t>& pOutQuads);
}
