#include "pch.h"
#include "VoxelPass.h"

#include "Core/Platform/Rendering/Objects/Buffer.h"

#include "Core/Platform/Rendering/RenderDefines.h"

#include "Core/Platform/Rendering/RenderContextInc.h"
#include "Core/Platform/Rendering/Managers/ModelManagerInc.h"
#include "../../Platform.h"
#include <Core/Application.h>
#include "Core/Settings.h"

VoxelPass::VoxelPass(
	PRenderContext* pContext, Shader* pVertex, Shader* pPixel,
	Sampler* pSampler, Sampler* pPyramidSampler,
	Mapper* pVoxelMapper, Mapper* pBrickMapper, Buffer* pCameraBuffer, Buffer* pAABBBuffer,
	View* pParticleTexture, View* pParticleDepthTexture,
	View* pModelTexture, View* pModelDepthTexture,
	View* pSunShadowTexture,
	View* pPyramidTexture
) : PRenderPass(pContext)
{
	// Creates a screen render target (for each buffer, m_uiFrameCount)
	RenderPass::Data RenderPassData;
	RenderPassData.m_Name = "Voxel";
	RenderPassData.m_TargetType = E_STATE_PIXEL_SHADER_RESOURCE;
	RenderPassData.m_TopologyType = E_PRIMITIVE_TOPOLOGY_TRIANGLE;
	RenderPassData.m_Topology = E_TOPOLOGY_TRIANGLESTRIP;
	RenderPassData.m_uiVertexCount = 14;
	RenderPassData.m_uiInstanceCount = 0;
	RenderPassData.m_fRenderScale = pContext->GetPlatform()->GetApplication()->GetSettings().GetResolutionScale();
	RenderPassData.m_bFollowsResolutionScale = true;
	RenderPassData.m_pVertexShader = pVertex;
	RenderPassData.m_pPixelShader = pPixel;
	RenderPassData.m_bEnableDepth = true;
	RenderPassData.m_CullType = E_CULL_FRONT;
	RenderPassData.m_uiBackBuffers = 1;

	RenderPassData.m_DepthClearValue = 1.f;

	/* s0 point, s1 linear-with-a-black-border for the coverage pyramid. Sampler
	   registers are assigned in push order, same contract as the rest. */
	RenderPassData.m_Samplers.push_back(pSampler);
	RenderPassData.m_Samplers.push_back(pPyramidSampler);

	/* Order is the SPIR-V contract: the mappers take u registers in the order
	   they are pushed, so the voxel buffer is u0 and the brick counts u1,
	   matching VoxelRenderer.ps.hlsl. */
	RenderPassData.m_Mappers.push_back(pVoxelMapper);
	RenderPassData.m_Mappers.push_back(pBrickMapper);

	RenderPassData.m_Buffers.push_back(pCameraBuffer);
	RenderPassData.m_Buffers.push_back(pAABBBuffer);

	/* Same contract on the t side: textures take t registers in push order, so
	   particles are t1 and t2. The shadowless variant reads neither particle
	   depth nor anything at t2, but must still declare it to keep the numbering
	   identical between the two. */
	RenderPassData.m_Textures.push_back(pParticleTexture);
	RenderPassData.m_Textures.push_back(pParticleDepthTexture);

	/* Dynamic model pass output, t3/t4 - DYNAMIC_MODELS_PLAN.md phase 2.
	   Unconditional like the particle pair above it, not gated on shadow
	   settings: both voxel pixel shader variants composite dynamic renderers
	   the same way regardless of ShadowQuality, so both declare these two and
	   the registers after them shift by two from what they were before this
	   pass existed. */
	RenderPassData.m_Textures.push_back(pModelTexture);
	RenderPassData.m_Textures.push_back(pModelDepthTexture);

	/* Sun shadow map at t5, when shadows are enabled. The ShadowLess variant
	   declares nothing at t5, and pushing a texture a shader never names would
	   put a descriptor in the layout with no binding to match it. */
	if (pSunShadowTexture != nullptr)
		RenderPassData.m_Textures.push_back(pSunShadowTexture);

	/* The coverage pyramid, last before the bindless array - so t6 with the
	   shadow map in front of it and t5 without, which is what each variant
	   declares. RENDERING_PLAN.md 7.1b route B. */
	RenderPassData.m_Textures.push_back(pPyramidTexture);

	RenderPassData.m_uiBindlessResourceCount = 1;
	RenderPassData.m_BindlessSource = RenderPass::E_BINDLESS_SOURCE_MODELS;

	Init(RenderPassData);
}

void VoxelPass::Begin(PCommandEngine* pEngine)
{
	m_Data.m_uiInstanceCount = m_Data.m_Buffers.back()->GetInstanceCount();
	PRenderPass::Begin(pEngine);
}
