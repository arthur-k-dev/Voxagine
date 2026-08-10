#include "pch.h"
#include "SunShadowModelPass.h"

#include "Core/Platform/Rendering/Objects/Buffer.h"
#include "Core/Platform/Rendering/RenderDefines.h"
#include "Core/Platform/Rendering/RenderContextInc.h"
#include "../../Platform.h"
#include <Core/Application.h>
#include "Core/Settings.h"

SunShadowModelPass::SunShadowModelPass(
	PRenderContext* pContext, Shader* pVertex, Shader* pPixel,
	Mapper* pModelMeshMapper, Buffer* pCameraBuffer, Buffer* pInstanceBuffer, Buffer* pQuadInstanceBuffer
) : PRenderPass(pContext)
{
	RenderPass::Data RenderPassData;
	RenderPassData.m_Name = "Sun Shadow Models";
	RenderPassData.m_TargetType = E_STATE_PIXEL_SHADER_RESOURCE;

	/* Same size as SunShadowPass's own target - the two are combined texel for
	   texel in SunShadowCombine.ps.hlsl. Settings::GetSunShadowResolution, not
	   a compile-time constant - it is a player-facing quality setting, and
	   VKRenderContext::ApplyRenderSettings resizes this pass (by name)
	   alongside "Sun Shadow" and "Sun Shadow Combine" whenever it changes at
	   runtime, not just at construction. */
	const uint32_t uiResolution = pContext->GetPlatform()->GetApplication()
		->GetSettings().GetSunShadowResolution();

	RenderPassData.m_bUseScreenResolution = false;
	RenderPassData.m_TargetSize = Vector2(
		static_cast<float>(uiResolution), static_cast<float>(uiResolution));
	RenderPassData.m_fRenderScale = 1.0f;

	RenderPassData.m_TargetFormat = { E_R32_FLOAT };

	/* SUN_SHADOW_FAR (Defines.hlsl) - keep the two in sync. A texel this
	   pass's geometry never covers must read as "nothing here", which the
	   combine pass's min() only gets right if the clear value is at least as
	   far as anything the world map could legitimately hold. */
	RenderPassData.m_ClearColor = Vector4(1.0e9f, 0.f, 0.f, 0.f);
	RenderPassData.m_bClearPerFrame = true;

	/* A real depth attachment, unlike SunShadowPass - that one is a single
	   full-screen triangle with nothing to resolve against itself. This one
	   draws many overlapping characters and needs hardware depth to pick the
	   nearest one per texel, same reasoning as VoxelModelPass. */
	RenderPassData.m_bEnableDepth = true;

	RenderPassData.m_pVertexShader = pVertex;
	RenderPassData.m_pPixelShader = pPixel;

	RenderPassData.m_TopologyType = E_PRIMITIVE_TOPOLOGY_TRIANGLE;
	RenderPassData.m_Topology = E_TOPOLOGY_TRIANGLELIST;
	RenderPassData.m_uiVertexCount = 6;
	RenderPassData.m_uiInstanceCount = 0;

	/* Winding is not a correctness question here - a "backwards" triangle
	   still covers the same texels at the same depth, there is nothing to
	   shade - so there is nothing to gain by culling and one less thing to
	   get wrong. Matches VoxelModelPass's own choice for the same reason. */
	RenderPassData.m_CullType = E_CULL_NONE;

	RenderPassData.m_Buffers.push_back(pCameraBuffer);
	RenderPassData.m_Buffers.push_back(pInstanceBuffer);
	RenderPassData.m_Buffers.push_back(pQuadInstanceBuffer);

	RenderPassData.m_Mappers.push_back(pModelMeshMapper);

	Init(RenderPassData);
}

void SunShadowModelPass::Begin(PCommandEngine* pEngine)
{
	m_Data.m_uiInstanceCount = m_Data.m_Buffers.back()->GetInstanceCount();
	PRenderPass::Begin(pEngine);
}
