#pragma once

#include "Core/ECS/Systems/Rendering/Buffers/Structures/StructuredVoxelBuffer.h"
#include "Core/Platform/Rendering/RenderPassInc.h"
#include "Core/Platform/Rendering/RenderDefines.h"

class Shader;
class Buffer;

class ParticlePass : public PRenderPass
{
	friend class RenderContext;
	friend class PhysicsSystem;

public:
	ParticlePass(
		PRenderContext* pContext, Shader* pVertex, Shader* pPixel, Buffer* pCameraBuffer, Mapper* pMapper, Sampler* pSamplerPoint
	);

	virtual void Begin(PCommandEngine* pEngine) override;

protected:
	/* World deserialization can construct PhysicsSystem before the render pass
	   has finished initializing. Do not turn that startup ordering into an
	   out-of-bounds vector access; physics can run without GPU particles until
	   the mapper is available. */
	Mapper* GetMapper() { return m_Data.m_Mappers.empty() ? nullptr : m_Data.m_Mappers.front(); }
	uint32_t m_uiParticleCount = 0;
};
