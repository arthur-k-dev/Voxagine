#include "pch.h"
#include "VoxelModelPass.h"

#include "Core/Platform/Rendering/Objects/Buffer.h"
#include "Core/Platform/Rendering/RenderDefines.h"
#include "Core/Platform/Rendering/RenderContextInc.h"
#include "../../Platform.h"
#include <Core/Application.h>
#include "Core/Settings.h"

VoxelModelPass::VoxelModelPass(
	PRenderContext* pContext, Shader* pVertex, Shader* pPixel,
	Sampler* pPointSampler, Sampler* pPyramidSampler,
	Mapper* pModelMeshMapper, Buffer* pCameraBuffer, Buffer* pInstanceBuffer, Buffer* pQuadInstanceBuffer,
	View* pPyramidTexture, View* pSunShadowTexture
) : PRenderPass(pContext)
{
	RenderPass::Data RenderPassData;
	RenderPassData.m_Name = "Voxel Models";
	RenderPassData.m_TargetType = E_STATE_PIXEL_SHADER_RESOURCE;

	/* Second target is a linear true-world-camera distance, same shape as
	   ParticlePass's - VoxelRenderer.ps.hlsl's composite branch reads both the
	   same way it already reads the particle pair. */
	RenderPassData.m_uiRenderViewCount = 2;
	RenderPassData.m_TargetFormat.push_back(E_R32_FLOAT);

	RenderPassData.m_TopologyType = E_PRIMITIVE_TOPOLOGY_TRIANGLE;
	RenderPassData.m_Topology = E_TOPOLOGY_TRIANGLELIST;
	RenderPassData.m_uiVertexCount = 6;
	RenderPassData.m_uiInstanceCount = 0;

	/* Moves with Settings::ResolutionScale for the same reason particles do -
	   this pass's output is sampled by the voxel pass, which is itself scaled,
	   and the two have to agree on footprint (VoxelRenderer.ps.hlsl divides by
	   voxelRenderScale when sampling the particle pair; the model pair is read
	   the same way). m_bFollowsResolutionScale is what makes that hold at
	   runtime too, not just at construction: RenderContext::ApplyRenderSettings
	   (VKRenderContext.cpp) re-reads and resizes every pass with this flag set
	   when the player changes the setting, same as ParticlePass/VoxelPass. */
	RenderPassData.m_fRenderScale = pContext->GetPlatform()->GetApplication()->GetSettings().GetResolutionScale();
	RenderPassData.m_bFollowsResolutionScale = true;

	RenderPassData.m_pVertexShader = pVertex;
	RenderPassData.m_pPixelShader = pPixel;
	RenderPassData.m_bEnableDepth = true;

	/* Winding is derived (VoxelModel.vs.hlsl's comment on d1 x d2), not
	   verified on screen yet - DYNAMIC_MODELS_PLAN.md phase 2 notes says to
	   confirm it and switch this to a real cull mode. Left open rather than
	   guessed: the wrong guess would make every dynamic renderer disappear,
	   which is a worse failure than a few extra back-face shades on meshes
	   this small. */
	RenderPassData.m_CullType = E_CULL_NONE;

	RenderPassData.m_Buffers.push_back(pCameraBuffer);
	RenderPassData.m_Buffers.push_back(pInstanceBuffer);

	/* Last, so Begin can read its live instance count off m_Buffers.back() -
	   same pattern VoxelPass uses for its own AABB buffer. */
	RenderPassData.m_Buffers.push_back(pQuadInstanceBuffer);

	RenderPassData.m_Mappers.push_back(pModelMeshMapper);

	RenderPassData.m_Textures.push_back(pPyramidTexture);

	/* DYNAMIC_MODELS_PLAN.md phase 4 follow-up: self-shadowing. Both null
	   together, when Settings::IsShadowEnabled() is false - VoxelModel.
	   ShadowLess.ps.hlsl is selected instead (RenderContext.cpp) and declares
	   neither a point sampler nor t4, same rule 8 pattern VoxelPass's own
	   shadow texture already follows. Order matters: s0 has to be the point
	   sampler and s1 the pyramid one to match both shaders' register
	   declarations, so the point sampler is pushed first only when it
	   exists - never leaving a gap between the two. */
	if (pSunShadowTexture != nullptr)
	{
		RenderPassData.m_Textures.push_back(pSunShadowTexture);
		RenderPassData.m_Samplers.push_back(pPointSampler);
	}

	RenderPassData.m_Samplers.push_back(pPyramidSampler);

	Init(RenderPassData);
}

void VoxelModelPass::Begin(PCommandEngine* pEngine)
{
	m_Data.m_uiInstanceCount = m_Data.m_Buffers.back()->GetInstanceCount();
	PRenderPass::Begin(pEngine);
}
