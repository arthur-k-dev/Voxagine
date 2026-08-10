#include "pch.h"

#include "Core/Platform/Rendering/Passes/PostProcessingPass.h"
#include "Core/Platform/Rendering/RenderPass.h"

PostProcessingPass::PostProcessingPass(
	PRenderContext* pContext, Shader* pVertex, Shader* pPixel,
	Sampler* pSampler, Sampler* pPointSampler,
	Buffer* pCameraBuffer, Mapper* pVoxelMapper,
	Mapper* pFarFieldMapper, Mapper* pFarFieldBrickMapper,
	std::vector<View*> pTextures
) : PRenderPass(pContext)
{
	// Creates a screen render target (for each buffer, m_uiFrameCount)
	RenderPass::Data RenderPassData;
	RenderPassData.m_Name = "Post Processing";
	RenderPassData.m_TargetType = E_STATE_PRESENT;
	RenderPassData.m_uiVertexCount = 3;
	RenderPassData.m_pVertexShader = pVertex;
	RenderPassData.m_fRenderScale = 1.0;
	RenderPassData.m_pPixelShader = pPixel;
	RenderPassData.m_bClearPerFrame = false;

#ifdef _ORBIS
	RenderPassData.m_TargetFormat = E_R8G8B8A8_UNORM_SRGB;
#endif

	RenderPassData.m_Buffers.push_back(pCameraBuffer);
	/* s0 linear, s1 point. FXAA is built on bilinear taps and needs the first;
	   the *upscale* of the scene target wants the second, because this is a
	   voxel game rendered at a fraction of the window and bilinear turns a hard
	   voxel edge into a gradient. See PostProcessing.ps.hlsl. */
	RenderPassData.m_Samplers.push_back(pSampler);
	RenderPassData.m_Samplers.push_back(pPointSampler);
	/* Order is the SPIR-V contract: mappers take u registers in the order they
	   are pushed, so the voxel buffer stays at u0 and the far-field volume and
	   its bricks land at u1/u2 - matching PostProcessing.ps.hlsl. Appended
	   rather than inserted, so nothing that was already bound moves. */
	RenderPassData.m_Mappers.push_back(pVoxelMapper);
	RenderPassData.m_Mappers.push_back(pFarFieldMapper);
	RenderPassData.m_Mappers.push_back(pFarFieldBrickMapper);

	for (View* pTexture : pTextures)
	{
		RenderPassData.m_Textures.push_back(pTexture);
	}

	Init(RenderPassData);
}