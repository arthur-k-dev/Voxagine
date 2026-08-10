#include "pch.h"

#include "Core/Platform/Rendering/Passes/ParticlePass.h"
#include "Core/Platform/Rendering/Objects/Buffer.h"

#include "Core/Platform/Rendering/RenderContextInc.h"

#include "../RenderContext.h"
#include "../../Platform.h"
#include <Core/Application.h>

ParticlePass::ParticlePass(
	PRenderContext* pContext, Shader* pVertex, Shader* pPixel, Buffer* pCameraBuffer, Mapper* pMapper, Sampler* pSamplerPoint
) : PRenderPass(pContext)
{
	// Creates a screen render target (for each buffer, m_uiFrameCount)
	RenderPass::Data RenderPassData;
	RenderPassData.m_Name = "Particles";
	RenderPassData.m_TargetType = E_STATE_PIXEL_SHADER_RESOURCE;
	RenderPassData.m_uiRenderViewCount = 2;
	RenderPassData.m_TargetFormat.push_back(E_R32_FLOAT);
	RenderPassData.m_TopologyType = E_PRIMITIVE_TOPOLOGY_TRIANGLE;
	RenderPassData.m_Topology = E_TOPOLOGY_TRIANGLELIST;
	/* 24 of the shader's 36 indices, which is four of a cube's six faces: the
	   bottom and the back are never drawn.
	 *
	 * DESTRUCTION_PLAN.md phase 3 asked for this to be either fixed or
	 * explained, and it is closer to a real artefact than to an optimization.
	 * Particles.vs.hlsl shades per face from a normal table, and the pass culls
	 * nothing - so from behind or below a particle is not a hole, it is the
	 * *inside* of its front and top faces, lit as though they were facing the
	 * camera. Depth and silhouette are right; only the shading is.
	 *
	 * Left at 24 deliberately. Going to 36 is one number, but it is +50% vertex
	 * invocations in a pass that can run 150,000 instances, and the difference
	 * it buys is a lighting change nobody has reported - which needs somebody
	 * watching the screen while the camera rotates to judge, not a unit test.
	 * See the phase 3 notes. */
	RenderPassData.m_uiVertexCount = 24;
	RenderPassData.m_uiInstanceCount = 0;
	RenderPassData.m_fRenderScale = pContext->GetPlatform()->GetApplication()->GetSettings().GetResolutionScale();

	/* Has to move with the Voxel pass: VoxelRenderer.ps.hlsl samples this
	   target at `viewport.xy * voxelRenderScale`, so a scale the two passes
	   disagree on reads the particle image at the wrong footprint. */
	RenderPassData.m_bFollowsResolutionScale = true;
	RenderPassData.m_pVertexShader = pVertex;
	RenderPassData.m_pPixelShader = pPixel;
	RenderPassData.m_bEnableDepth = true;
	RenderPassData.m_CullType = E_CULL_NONE;

	RenderPassData.m_Buffers.push_back(pCameraBuffer);
	RenderPassData.m_Mappers.push_back(pMapper);

	RenderPassData.m_Samplers.push_back(pSamplerPoint);

	Init(RenderPassData);
}

void ParticlePass::Begin(PCommandEngine* pEngine)
{
	m_Data.m_uiInstanceCount = m_pContext->m_uiParticleCount;
	PRenderPass::Begin(pEngine);
}
