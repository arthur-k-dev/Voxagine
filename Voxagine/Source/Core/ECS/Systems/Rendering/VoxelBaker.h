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

	/* One budgeted pass over the renderers that want re-stamping
	   (StreamingBudgets::VoxelBaking). Whatever it does not reach keeps its
	   Updated/UpdateRequested flags and is found again next frame, so the loop
	   is resumable without a cursor and without holding a pointer across a
	   frame - see the budget's comment.

	   Returns true when nothing is left wanting a stamp, which is what
	   RenderSystem::HasPendingVoxelBakes answers with. */
	virtual bool Bake();

virtual uint32_t* Occupy(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData = nullptr);

	/* bNotify false suppresses the repair pass below, and is how a repair is
	   stopped from provoking another one - see NotifyClearedRegion. */
	virtual void Clear(VoxRenderer* pRenderer, VoxRenderer::BakeData* pBakeData = nullptr, bool bNotify = true);

	/* Clear's counterpart for a renderer leaving with its chunk: forget what was
	   stamped without erasing a single voxel. Everything Clear resets is reset
	   here too, so a renderer that comes back with the chunk starts from "never
	   baked" rather than from a stale generation. */
	void ForgetChunkStamp(VoxRenderer* pRenderer);

	/* What Occupy would stamp, as a value: see VoxRenderer::BakeData::StampKey.
	   O(1) - it computes the stamp transform and reads three fields, it does
	   not walk the model. */
	VoxRenderer::BakeData::StampKey ComputeStampKey(VoxRenderer* pRenderer);

	/* Marks the dynamic renderers a Clear may have erased, so they stamp
	   themselves again. See the definition - the asymmetry between static and
	   dynamic is the whole reason this is needed. */
	void NotifyClearedRegion(VoxRenderer* pCleared, const Vector3& v3GridMin, const Vector3& v3GridMax);

protected:
	/* Milliseconds this bake spent in NotifyClearedRegion, accumulated across
	   every clear in the pass and reported once - the scan is per clear and
	   per renderer, so a per-call average would say nothing about the frame.
	   Only written while the profiler is on. */
	double m_fRepairMilliseconds = 0.0;

	RenderContext* m_pRenderContext = nullptr;
	RenderSystem* m_pRenderSystem = nullptr;
	PhysicsSystem* m_pPhysicsSystem = nullptr;
};