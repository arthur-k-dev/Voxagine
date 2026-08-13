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

	/* This world is going away; nothing it drew can be presented again.

	   Two things follow and both are the point. Every renderer's stamp is
	   *forgotten* rather than cleared - the same distinction a chunk unload
	   makes (see OnComponentDestroyed): the voxel window is about to be
	   replaced wholesale, so rewriting it a renderer at a time is thousands of
	   passes over storage no future frame can expose. And the renderer list is
	   emptied in one go rather than erased from the middle once per component
	   as World::Unload destroys entities.

	   bReleaseVoxelWindow is false only when the world being unloaded is *not*
	   the one whose voxels are in the window - the loading screen at the moment
	   its replacement is activated, whose own unload would otherwise wipe the
	   level that was just streamed in behind it. Docs/CHUNK_STREAMING_PLAN.md
	   phase 8. */
	void BeginWorldUnload(bool bReleaseVoxelWindow = true);

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

	/* Is any admitted renderer still waiting to be stamped into the voxel
	   window? CHUNK_STREAMING_PLAN.md phase 5: the stamp is budgeted now, so a
	   world can be fully admitted and still not fully *drawn*, and anything
	   asking "is this world settled" has to mean both.
	   ChunkSystem::IsStreaming folds this in. */
	bool HasPendingVoxelBakes() const { return m_bVoxelBakesPending; }

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

	struct LooseVoxelCell;

	/* Drops the recorded voxels a cell no longer has and re-tightens its box.
	   False when nothing survives. */
	bool ValidateLooseCell(uint32_t uiKey, LooseVoxelCell& cell, const Vector3& v3Offset);

	void EvictFarthestLooseCell();

	/* Loose voxels, bucketed into cells of this many voxels a side. Kept in
	   *level* space, not window space, so the registry survives the window
	   sliding and a chunk unloading and coming back - debris lives in the
	   chunk's voxels and returns with it, and a window-space registry would
	   have had to be shifted and would have lost anything that left and came
	   back. */
	static constexpr uint32_t k_uiLooseCellShift = 5;
	static constexpr uint32_t k_uiLooseCellSize = 1u << k_uiLooseCellShift;

	/* A cell records *which voxels* debris actually wrote, not just a box
	   around them (ledger L1). Retirement used to ask the brick grid whether
	   anything at all was still occupied nearby, which any writer satisfies -
	   so a cell overlapping static geometry could never retire and its box
	   re-tightened onto that geometry instead of onto its debris. Asking about
	   the exact voxels answers the actual question.

	   The offsets are cell-local, five bits an axis, so a voxel costs two bytes
	   and the list is bounded by the cell rather than by the level. Kept sorted
	   so re-registering the same voxel is idempotent. */
	struct LooseVoxelCell
	{
		Box Bounds;
		std::vector<uint16_t> Offsets;
	};

	std::unordered_map<uint32_t, LooseVoxelCell> m_LooseVoxelCells;

	/* Iteration order for the round-robin validator. An unordered_map's order
	   changes on rehash, so a cursor into it skipped cells arbitrarily and some
	   were never judged at all (ledger L3). This is rebuilt only when the map
	   changes size. */
	std::vector<uint32_t> m_LooseCellKeys;
	bool m_bLooseCellKeysDirty = true;

	/* Round-robin cursor and per-frame budget for retiring loose cells. The
	   registry is only bounded because of this - see SubmitLooseVoxelProxies. */
	static constexpr size_t k_uiLooseValidatePerFrame = 32;
	size_t m_LooseValidateCursor = 0;

	/* Cells outside the window cannot be judged - their voxels are not in the
	   buffer to test - so a level walked far enough would accumulate them
	   without bound (ledger L2). This is the backstop: past it, the cell
	   farthest from the window is dropped. Dropping loses a proxy, not the
	   debris; those voxels are still in the chunk and still drawn whenever some
	   other model's box covers them, which is the pre-registry behaviour rather
	   than a new defect. 8192 cells is a level 8x the largest one here, fully
	   littered. */
	static constexpr size_t k_uiMaxLooseCells = 8192;

	/* False until Start() has wiped and sized the voxel buffer. See
	   OnComponentAdded. */
	bool m_bStarted = false;

	/* See BeginWorldUnload. The second is what the destructor reads, so a
	   world unloaded without ever calling BeginWorldUnload behaves as it always
	   did. */
	bool m_bWorldUnloading = false;
	bool m_bReleaseVoxelWindow = true;

	bool m_bForcedUpdate = true;

	/* Renderers still waiting for a stamp because VoxelBaker::Bake ran out of
	   budget. CHUNK_STREAMING_PLAN.md phase 5 - HasPendingVoxelBakes is what
	   ChunkSystem::IsStreaming folds in, so "is this world settled" accounts
	   for geometry that has been admitted but not yet written. */
	bool m_bVoxelBakesPending = false;
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