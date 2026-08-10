#pragma once

#include "Core/Platform/Rendering/RenderPassInc.h"
#include "Core/Platform/Rendering/RenderDefines.h"

class Shader;
class Buffer;
class Sampler;
class Mapper;

/* Light-space sun shadow map - RENDERING_PLAN.md 7.1a.
 *
 * A full-screen pass over a square R32_FLOAT target of
 * Settings::GetSunShadowResolution on a side, where every texel marches the
 * brick DDA once along `lightDirection` and records the depth of the first
 * blocker in its column.
 *
 * The point of it is what it is *not*: a per-pixel shadow ray. That costs
 * ~9.45 ms at 3840x2160 for a single sample, against a 3 ms budget for all
 * lighting, and it grows with pixel count. This pass costs the same whatever
 * the screen resolution is, and PCSS on lookup buys a distance-widening
 * penumbra without any rays at all.
 *
 * **It costs the same whatever the screen is doing, which cuts both ways.** On
 * a phone that made it the second largest item in the frame - ~21.5 ms of 77 -
 * while the pass it feeds had already been halved by ResolutionScale. Its only
 * lever is the map's size, which is why ShadowQuality picks one:
 * RenderContext::ApplyRenderSettings resizes this target when it changes, and
 * at SHQ_OFF RenderContext::Present does not draw the pass at all.
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
