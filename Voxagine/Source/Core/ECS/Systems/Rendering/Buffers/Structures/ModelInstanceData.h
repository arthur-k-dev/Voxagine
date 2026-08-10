#pragma once

#include "Core/Math.h"

/* DYNAMIC_MODELS_PLAN.md phase 2. One per dynamic (non-static) VoxRenderer
 * submitted this frame - VoxelModel.vs.hlsl's per-instance transform.
 *
 * Deliberately NOT the quantized VoxelStampTransform (VoxelStamp.h): that
 * transform exists to place a model on integer grid cells, which is the very
 * discretization this plan removes for dynamic renderers - see
 * DYNAMIC_MODELS_PLAN.md, "The one thing to check on screen". Rotation and
 * WorldOrigin here are the continuous (unrounded) equivalent of
 * ComputeVoxelStampTransform's own math, with the grid-alignment floor it
 * applies to a single-frame renderer's origin deliberately dropped: that
 * floor exists only to land the *voxelized* stamp on a grid cell, and has no
 * meaning for a rasterized mesh.
 *
 * Field order matches what RenderSystem::BuildModelInstance actually
 * computes and what VoxelModel.vs.hlsl reads in the same order - keep them
 * in step (rule 1, RENDERING_PLAN.md: -fvk-use-dx-layout means this is a
 * tightly packed C++ struct on the GPU, not a std430 one). */
struct ModelInstanceData
{
	Vector4 Rotation = Vector4(0.f, 0.f, 0.f, 1.f); // quaternion, xyzw
	Vector3 Scale = Vector3(1.f);
	float _Pad0 = 0.f;
	Vector3 WorldOrigin = Vector3(0.f);
	float _Pad1 = 0.f;

	/* Mirrors ForEachStampedVoxel's own colour resolution (VoxelStamp.h): a
	   quad's stored colour is used unless OverrideColor's alpha byte is
	   nonzero, in which case OverrideColor's RGB replaces it - both before
	   Tag is OR'd into the top byte. */
	uint32_t OverrideColor = 0;

	/* VoxelStateTag(state, emissive) - RENDERING_PLAN.md 7.4's tag byte
	   convention, computed on the CPU once per renderer per frame exactly as
	   VoxelBaker::Occupy already does, and combined with the quad's own
	   colour in the shader the same way ForEachStampedVoxel combines it with
	   a voxel's. */
	uint32_t Tag = 0;

	uint32_t _Pad2 = 0;
	uint32_t _Pad3 = 0;
};

/* One visible quad this frame: which renderer instance (index into the
 * ModelInstanceData buffer this frame) and which quad of ModelMeshStore's
 * shared store. A full uint32_t each rather than packed uint16_t - the mesh
 * store already runs past 65,536 quads over a whole session
 * (DYNAMIC_MODELS_PLAN.md phase 1 results), so a 16-bit quad index would
 * silently wrap. */
struct ModelQuadInstance
{
	uint32_t InstanceIndex = 0;
	uint32_t QuadIndex = 0;
};
