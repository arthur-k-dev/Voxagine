#pragma once

#include "Core/Platform/Rendering/RenderPassInc.h"
#include "Core/Platform/Rendering/RenderDefines.h"

class Shader;
class Buffer;

/* DYNAMIC_MODELS_PLAN.md phase 4. Dynamic renderers' own light-space depth
 * target - same quad list and instance/quad-instance buffers VoxelModelPass
 * draws, projected into light-space clip instead of camera clip
 * (SunShadowModel.vs.hlsl). Its own real depth attachment resolves
 * overlapping characters against each other, same reasoning as
 * VoxelModelPass's. Combined with the world shadow map afterward
 * (SunShadowCombinePass) rather than drawn into that pass's own target - see
 * SunShadowModel.vs.hlsl's header on why.
 *
 * Built only when shadows are enabled, same gate SunShadowPass itself uses -
 * a target nothing reads is not worth the draw. */
class SunShadowModelPass : public PRenderPass
{
public:
	SunShadowModelPass(
		PRenderContext* pContext, Shader* pVertex, Shader* pPixel,
		Mapper* pModelMeshMapper, Buffer* pCameraBuffer, Buffer* pInstanceBuffer, Buffer* pQuadInstanceBuffer
	);

	virtual void Begin(PCommandEngine* pEngine) override;
};
