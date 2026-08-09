#pragma once

#include <unordered_map>
#include "Core/ECS/ComponentSystem.h"
#include "Core/Math.h"

#include "DebugRenderer.h"
#include "VoxelBaker.h"
#include "Core/Voxels/VoxelEditBatch.h"

class SpriteRenderer;
class TextRenderer;

struct Voxel;

class VoxRenderer;
class VoxAnimator;

class RenderSystem : public ComponentSystem, public ILooseVoxelSink
{
	friend class PhysicsSystem;
	friend class World;
	friend class WorldManager;
	friend class Editor;
	friend class EditorRenderMapper;
	friend class SpriteRenderer;
	friend class ParticleSystem;
	friend class VoxelBaker;

public:
	RenderSystem(World* pWorld);
	virtual ~RenderSystem();

	virtual void Start() override;

	virtual bool CanProcessComponent(Component * pComponent) override;
	virtual void Tick(float fDeltaTime) override;
	virtual void PostTick(float fDeltaTime) override;

	virtual void FixedTick(const GameTimer& fixedTimer) override;
	virtual void PostFixedTick(const GameTimer& fixedTimer) override;

	void Render(const GameTimer& fixedTimer);

	DebugRenderer& GetDebugRenderer() { return m_DebugRenderer; }

	void ForceUpdate();
	void ForceCameraDataUpdate();
	void SetGroundPlane(const std::string& texturePath, bool bForce = false);

	/* RENDERING_PLAN.md phase 4d's acceptance test; see the definition.
	   Triggered by VOXAGINE_VOXEL_AUDIT=<seconds>. */
	void AuditVoxelRepresentation();

	/* DESTRUCTION_PLAN.md phase 0. Every representation of a voxel, checked
	   against every other one: CPU colour, mapped GPU word, occupancy bitmap
	   and brick count. Triggered periodically by VOXAGINE_SYNC_AUDIT=<seconds>
	   and available from the editor's View menu, which is where the brick half
	   of it already lived. See the definition. */
	void AuditRepresentationSync();

	void EnableDebugLines(bool bEnabled);

	void SetFadeTime(float fFadeTime);

	UVector2 GetRenderResolution() const { return m_pRenderContext->GetRenderResolution(); }
	UVector2 GetScreenResolution() const { return m_pRenderContext->GetScreenResolution(); }

	uint32_t GetVoxel(uint32_t uiVolumeId) const;
	uint32_t GetVoxel(int32_t x, int32_t y, int32_t z) const;

	/* Registers a voxel written into the world by something that is not a
	   VoxRenderer, so that the voxel pass has a proxy to rasterize for it.
	   Grid space, i.e. the same coordinates ModifyVoxel takes.

	   Only a renderer submits an AABB proxy, and the voxel pass rasterizes
	   nothing else, so a voxel from any other writer is drawn only when some
	   unrelated model's box happens to cover the pixel - which is a flicker
	   that comes and goes with the camera, not a steady absence. A particle
	   baking itself into the world where it landed is the writer this exists
	   for: PhysicsSystem, bake-on-impact. */
	void AddLooseVoxel(const Vector3& v3GridPosition) override;

	/* Everything a VoxelEditBatch needs to maintain every representation of a
	   voxel, assembled from the pieces only this system has together: the
	   physics grid's CPU voxels and owner slots, the render context's mapping
	   and brick grid, and itself as the loose-voxel sink.

	   Any destruction, island conversion or bake-on-impact write goes through a
	   batch built from this rather than through a pair of ModifyVoxel calls -
	   see DESTRUCTION_PLAN.md phase 1 on why the pair was the problem. */
	VoxelEditTarget MakeEditTarget();
	bool FindOccupiedBrickBounds(VoxelBrickGrid& brickGrid, const UVector3& v3BrickGrid,
	                             const Vector3& v3Min, const Vector3& v3Max,
	                             Vector3& o_v3Min, Vector3& o_v3Max) const;

	void Reveal() { m_bFaded = false; };
	void Fade() { m_bFaded = true; };

	bool IsFaded() const;
	bool IsFading() const;
	void SetFadeValue(float fValue) { return m_pRenderContext->SetFadeValue(fValue); }
	float GetFadeValue() const { return m_pRenderContext->m_fFader; }

protected:
	void OnWorldResumed(World* pWorld);

	virtual void OnComponentAdded(Component * pComponent) override;
	virtual void OnComponentDestroyed(Component * pComponent) override;

	inline bool ModifyVoxel(int32_t x, int32_t y, int32_t z, uint32_t uiColor, bool bOverwrite = true)
	{
		// Update world voxel
		return m_pRenderContext->ModifyVoxel(static_cast<uint32_t>(x + y * m_v3WorldSize.x + z * m_v3WorldSize.x * m_v3WorldSize.y), uiColor, bOverwrite);
	}

	inline bool ModifyVoxel(uint32_t uiVolumeId, uint32_t uiColor, bool bOverwrite = true)
	{
		// Update world voxel
		return m_pRenderContext->ModifyVoxel(uiVolumeId, uiColor, bOverwrite);
	}
	
	inline void ClearVoxels();

private:
	void CheckRendererChange(VoxRenderer* pRenderer);

	/* VOXAGINE_BOUNDS_AUDIT=1: reports how far the transform-derived AABB proxy
	   falls short of the voxels VoxelBaker actually stamps. Off - and free but
	   for one branch - unless the variable is set. See PostTick. */
	void AuditProxyBounds(VoxRenderer* pRenderer,
		const Vector3& v3ProxyMin, const Vector3& v3ProxyMax,
		const Vector3& v3StampMin, const Vector3& v3StampMax);

	/* VOXAGINE_COVERAGE_AUDIT=<seconds>: every that many seconds, counts the
	   occupied bricks of the resident window that no AABB proxy contains. A
	   voxel outside every proxy is never rasterized by the voxel pass, so it is
	   drawn only when some unrelated model's box happens to cover the pixel -
	   which is a flicker that comes and goes with the camera. Zero is the
	   invariant; see the definition. */
	void AuditProxyCoverage(float fDeltaTime);

	/* Grid-space proxy boxes submitted this frame, gathered only while the
	   coverage audit is on. */
	std::vector<Box> m_AuditProxies;

	/* One proxy per cell of loose voxels that overlaps the resident window. */
	void SubmitLooseVoxelProxies(bool bAudit);

	/* Loose voxels, bucketed into cells of this many voxels a side and stored
	   as the tight box of what actually landed in each. Kept in *level* space,
	   not window space, so the registry survives the window sliding and a chunk
	   unloading and coming back - debris lives in the chunk's voxels and
	   returns with it, and a window-space registry would have had to be shifted
	   and would have lost anything that left and came back. */
	static constexpr uint32_t k_uiLooseCellShift = 5;

	std::unordered_map<uint32_t, Box> m_LooseVoxelCells;

	/* Round-robin cursor and per-frame budget for retiring loose cells. The
	   registry is only bounded because of this - see SubmitLooseVoxelProxies. */
	static constexpr size_t k_uiLooseValidatePerFrame = 32;
	size_t m_LooseValidateCursor = 0;

	/* False until Start() has wiped and sized the voxel buffer. See
	   OnComponentAdded. */
	bool m_bStarted = false;

	bool m_bForcedUpdate = true;
	bool m_bShouldUpdateVoxelWorld = true;
	
	UVector3 m_v3WorldSize = UVector3(0, 0, 0);
	uint32_t m_uiMaxVoxels = 0;

	bool m_bFaded = false;

	RenderContext* m_pRenderContext = nullptr;

	DebugRenderer m_DebugRenderer;

	std::vector<VoxRenderer*> m_VoxRenderers;
	std::vector<VoxAnimator*> m_VoxAnimators;

	std::vector<TextRenderer*> m_TextRenderers;
	std::vector<SpriteRenderer*> m_SpriteRenderers;

	VoxelBaker m_VoxelBaker;
	PhysicsSystem* m_pPhysicsSystem = nullptr;
};