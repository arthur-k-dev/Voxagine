#pragma once

#include "Core/Platform/Rendering/RenderPassInc.h"
#include "Core/Platform/Rendering/RenderDefines.h"

class Shader;
class Buffer;

/* DYNAMIC_MODELS_PLAN.md phase 2. Rasterizes every dynamic renderer's
 * greedy-meshed quads this frame, in their own free (unquantized) world
 * transform - VoxelModel.vs.hlsl - and shades them through the same lighting
 * building blocks the marched voxel pass uses (ShadeVoxelSurface.hlsl), but
 * with self-occlusion baked into the mesh instead of sampled from the world
 * voxel buffer - VoxelModel.ps.hlsl.
 *
 * Two render views, same shape as ParticlePass: colour and a linear
 * true-world-distance depth, which VoxelRenderer.ps.hlsl composites against
 * the same way it already composites particles - nearest of {model,
 * particle, world hit} wins. Its own real depth attachment (m_bEnableDepth)
 * resolves overlapping characters against each other in hardware.
 *
 * Drawn after the sun shadow pass and before the voxel pass - RenderContext's
 * per-frame instance ordering comment says why (the voxel pass samples this
 * pass's target, same constraint particles are already under). */
class VoxelModelPass : public PRenderPass
{
public:
	VoxelModelPass(
		PRenderContext* pContext, Shader* pVertex, Shader* pPixel,
		Sampler* pPointSampler, Sampler* pPyramidSampler,
		Mapper* pModelMeshMapper, Buffer* pCameraBuffer, Buffer* pInstanceBuffer, Buffer* pQuadInstanceBuffer,
		View* pPyramidTexture, View* pSunShadowTexture
	);

	virtual void Begin(PCommandEngine* pEngine) override;
};
