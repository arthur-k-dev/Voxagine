#include "pch.h"

#include "Core/Platform/Rendering/Passes/SunShadowPass.h"
#include "Core/Platform/Rendering/RenderPass.h"
#include "Core/Platform/Rendering/RenderContextInc.h"

SunShadowPass::SunShadowPass(
	PRenderContext* pContext, Shader* pVertex, Shader* pPixel, Sampler* pSampler,
	Buffer* pCameraBuffer, Mapper* pVoxelMapper, Mapper* pBrickMapper
) : PRenderPass(pContext)
{
	RenderPass::Data RenderPassData;
	RenderPassData.m_Name = "Sun Shadow";
	RenderPassData.m_TargetType = E_STATE_PIXEL_SHADER_RESOURCE;
	RenderPassData.m_uiVertexCount = 3;
	RenderPassData.m_pVertexShader = pVertex;
	RenderPassData.m_pPixelShader = pPixel;

	/* Fixed size, deliberately not the screen's. This is the entire reason the
	   pass exists - see the header. m_fRenderScale stays 1.0 so
	   Settings::ResolutionScale cannot drag it around either: rendering the
	   game at half resolution should not halve shadow quality, and rendering
	   at 4K should not quadruple the shadow cost. */
	RenderPassData.m_bUseScreenResolution = false;
	RenderPassData.m_TargetSize = Vector2(
		static_cast<float>(RenderContext::k_uiSunShadowResolution),
		static_cast<float>(RenderContext::k_uiSunShadowResolution));
	RenderPassData.m_fRenderScale = 1.0f;

	RenderPassData.m_TargetFormat = { E_R32_FLOAT };

	/* Every texel is written unconditionally - a miss stores SUN_SHADOW_FAR
	   rather than leaving the clear value - so there is nothing for a clear to
	   do, and skipping it saves a full-target write every frame. */
	RenderPassData.m_bClearPerFrame = false;
	RenderPassData.m_bEnableDepth = false;

	RenderPassData.m_Samplers.push_back(pSampler);

	/* Order is the SPIR-V contract: mappers take u registers in push order, so
	   the voxel buffer is u0 and the brick counts u1, matching what
	   SDFMarcher.hlsl expects of whoever includes it. */
	RenderPassData.m_Mappers.push_back(pVoxelMapper);
	RenderPassData.m_Mappers.push_back(pBrickMapper);

	RenderPassData.m_Buffers.push_back(pCameraBuffer);

	Init(RenderPassData);
}
