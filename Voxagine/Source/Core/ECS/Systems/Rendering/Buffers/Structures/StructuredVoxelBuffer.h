#pragma once

#include "Core/Math.h"

#include <cstddef>

struct StructuredVoxelBuffer
{
	Vector3 Position;
	Vector3 Extents;
	uint32_t MapperID = 0;
	float Distance = 0.0;
};

/* GPU structured-buffer ABI, and the voxel pass's instance list: one element is
   one proxy box. VoxelRenderer.vs.hlsl's AABB spells these members as scalars
   so the shader agrees with this packed layout - float3 there would take
   std430's 16-byte alignment and stride 48, decoding every element after the
   first from the wrong offset and leaving the world unrendered.

   Asserted rather than commented because the failure is silent: it draws
   nothing, with no validation error and no build warning. */
static_assert(sizeof(StructuredVoxelBuffer) == 32, "AABB GPU stride changed");
static_assert(offsetof(StructuredVoxelBuffer, Extents) == 12, "AABB Extents offset changed");
static_assert(offsetof(StructuredVoxelBuffer, MapperID) == 24, "AABB MapperID offset changed");
static_assert(offsetof(StructuredVoxelBuffer, Distance) == 28, "AABB Distance offset changed");