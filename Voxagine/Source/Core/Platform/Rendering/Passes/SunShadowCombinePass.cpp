#include "pch.h"
#include "SunShadowCombinePass.h"

#include "Core/Platform/Rendering/RenderDefines.h"
#include "Core/Platform/Rendering/RenderContextInc.h"
#include "../../Platform.h"
#include <Core/Application.h>
#include "Core/Settings.h"

SunShadowCombinePass::SunShadowCombinePass(
	PRenderContext* pContext, Shader* pVertex, Shader* pPixel, Sampler* pSampler,
	View* pWorldShadowTexture, View* pModelShadowTexture
) : PRenderPass(pContext)
{
	RenderPass::Data RenderPassData;
	RenderPassData.m_Name = "Sun Shadow Combine";
	RenderPassData.m_TargetType = E_STATE_PIXEL_SHADER_RESOURCE;

	RenderPassData.m_bUseScreenResolution = false;
	RenderPassData.m_TargetSize = Vector2(
		static_cast<float>(RenderContext::k_uiSunShadowResolution),
		static_cast<float>(RenderContext::k_uiSunShadowResolution));
	RenderPassData.m_fRenderScale = 1.0f;

	RenderPassData.m_TargetFormat = { E_R32_FLOAT };

	/* Every texel written unconditionally by the full-screen triangle -
	   same reasoning as SunShadowPass itself. */
	RenderPassData.m_bClearPerFrame = false;
	RenderPassData.m_bEnableDepth = false;

	RenderPassData.m_uiVertexCount = 3;

	RenderPassData.m_pVertexShader = pVertex;
	RenderPassData.m_pPixelShader = pPixel;

	RenderPassData.m_Samplers.push_back(pSampler);

	RenderPassData.m_Textures.push_back(pWorldShadowTexture);
	RenderPassData.m_Textures.push_back(pModelShadowTexture);

	Init(RenderPassData);
}
