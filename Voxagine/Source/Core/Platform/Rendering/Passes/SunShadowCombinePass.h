#pragma once

#include "Core/Platform/Rendering/RenderPassInc.h"
#include "Core/Platform/Rendering/RenderDefines.h"

class Shader;
class Buffer;

/* DYNAMIC_MODELS_PLAN.md phase 4. Full-screen min() of SunShadowPass's world
 * target and SunShadowModelPass's dynamic-renderer target, into one map
 * shaped exactly like SunShadowPass's own - so it can be bound as
 * `sunShadowMap` wherever that already was, with zero changes to
 * SunShadowLookup.hlsl's PCSS blocker search and filter. See
 * SunShadowModel.vs.hlsl's header for why this is a separate pass rather
 * than either producer writing into the other's target. */
class SunShadowCombinePass : public PRenderPass
{
public:
	SunShadowCombinePass(
		PRenderContext* pContext, Shader* pVertex, Shader* pPixel, Sampler* pSampler,
		Buffer* pCameraBuffer, View* pWorldShadowTexture, View* pModelShadowTexture
	);
};
