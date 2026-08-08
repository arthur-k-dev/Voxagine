#pragma once

#include "Core/ECS/Components/VoxRenderer.h"

class RenderContext;
class RenderSystem;
class PhysicsSystem;

class VoxelBaker
{
public:
	enum BakeType
	{
		E_OCCUPY,
		E_CLEAR
	};

	struct BakeCommand
	{
		BakeType Type;
		VoxRenderer* Renderer;
		VoxRenderer::BakeData* Data;
	};

	void Init(RenderSystem* pRenderSystem, PhysicsSystem* pPhysicsSystem);

	virtual void Bake();

	virtual uint32_t* Occupy(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData = nullptr);
	virtual void Clear(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData = nullptr);

	/* What Occupy would stamp, as a value: see VoxRenderer::BakeData::StampKey.
	   O(1) - it computes the stamp transform and reads three fields, it does
	   not walk the model. */
	VoxRenderer::BakeData::StampKey ComputeStampKey(VoxRenderer* pRenderer);

	/* Marks the dynamic renderers a static renderer's Clear may have erased, so
	   they stamp themselves again. See the definition - the asymmetry between
	   static and dynamic is the whole reason this is needed. */
	void NotifyClearedRegion(VoxRenderer* pCleared, const Vector3& v3GridMin, const Vector3& v3GridMax);

protected:
	RenderContext* m_pRenderContext = nullptr;
	RenderSystem* m_pRenderSystem = nullptr;
	PhysicsSystem* m_pPhysicsSystem = nullptr;
};