#pragma once

#include "Core/Platform/Rendering/RenderPassInc.h"
#include "Core/Platform/Rendering/RenderDefines.h"

class Shader;
class Buffer;
class Sampler;
class Mapper;

/* Light-space sun shadow map - RENDERING_PLAN.md 7.1a.
 *
 * A full-screen pass over a fixed k_uiSunShadowResolution^2 R32_FLOAT target,
 * where every texel marches the brick DDA once along `lightDirection` and
 * records the depth of the first blocker in its column.
 *
 * The point of it is what it is *not*: a per-pixel shadow ray. That costs
 * ~9.45 ms at 3840x2160 for a single sample, against a 3 ms budget for all
 * lighting, and it grows with pixel count. This pass costs the same whatever
 * the screen resolution is, and PCSS on lookup buys a distance-widening
 * penumbra without any rays at all.
 *
 * It runs before the Voxel pass, which samples its target at t3.
 */
class SunShadowPass : public PRenderPass
{
public:
	SunShadowPass(
		PRenderContext* pContext, Shader* pVertex, Shader* pPixel, Sampler* pSampler,
		Buffer* pCameraBuffer, Mapper* pVoxelMapper, Mapper* pBrickMapper
	);
};
