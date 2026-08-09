#pragma once

#include "Core/ECS/Component.h"

#include "Core/Math.h"

#include <rttr/type>

#include "Core/ECS/Systems/Physics/VoxelGrid.h"
#include "Core/ECS/Systems/Physics/Box.h"

class RenderSystem;

enum RenderLayer
{
	RL_DEFAULT,
	RL_ENTITY,
	RL_STATIC_ENTITY,
	RL_PARTICLES
};

enum RenderState {
	RS_DEFAULT,			// Shows voxels normally
	RS_GRID_LINES,		// Adds grid outline to voxels
	RS_SELECTION_LINES,	// Adds grid selection outline to voxels
};

/* --- The voxel word's tag byte (RENDERING_PLAN.md 7.4) ---------------------
 * A voxel in the GPU buffer is 0xTTBBGGRR: colour in the low three bytes and
 * this tag in the top one. **Occupancy is tag != 0** - rule 3 - which is why
 * the state is stored plus one and why nothing may claim the value zero.
 *
 * The audit that 7.4 was told to run before claiming a bit found that the byte
 * was not a tag at all: it was ORed onto a palette alpha of 255, so every voxel
 * carried 255 and the state never reached the buffer. It is masked in now. Two
 * consequences worth knowing:
 *
 *   - Nothing reads the state's *value* - VoxelBaker, the marcher, the brick
 *     grid, the far field and ambient occlusion all test occupancy alone - so
 *     the grid-lines and selection-lines states have had no rendering effect
 *     for the life of this branch. Fixing that is not this phase's business.
 *   - The bits above the state are genuinely free, and this is the first thing
 *     to take one. Keep VOXEL_STATE_MASK and VOXEL_EMISSIVE_TAG in step with
 *     Defines.hlsl's copies; they are a SPIR-V/C++ contract exactly as
 *     BRICK_SHIFT is.
 */
#define VOXEL_STATE_MASK 0x3Fu
#define VOXEL_EMISSIVE_TAG 0x80u

/* Occupancy is tag != 0, so the state is always stored plus one. */
inline uint32_t VoxelStateTag(RenderState state, bool bEmissive)
{
	const uint32_t uiState = (static_cast<uint32_t>(state) + 1u) & VOXEL_STATE_MASK;

	return (uiState | (bEmissive ? VOXEL_EMISSIVE_TAG : 0u)) << 24;
}

class VoxModel;
class VoxAnimator;
struct VoxFrame;

class VoxRenderer : public Component
{
public:
	friend class RenderSystem;
	friend class VoxAnimator;
	friend class VoxelBaker;
	friend class VoxFrameEmitter;
	friend class EditorRenderMapper;

	struct BakeData {
		uint32_t* Positions = nullptr;
		uint32_t Size = 0;

		Vector3 WorldOffset = Vector3(0.f, 0.f, 0.f);

		/* RenderContext::GetVoxelGeneration at the moment Positions was
		   written. Zero means "never baked", which never matches. */
		uint32_t Generation = 0;

		/* Everything the stamped voxels are a function of.
		   ForEachStampedVoxel reads exactly two things: the VoxelStampTransform
		   below, and three values off the renderer - its frame, its override
		   colour and its render state. So two stamps with equal keys produce
		   an identical sequence of (voxel, colour) pairs, and re-baking one
		   over the other writes what is already there.

		   Deliberately the stamp's own inputs rather than the transform's:
		   position, rotation and scale reach the stamp through quantization
		   and a floor, so a renderer can move without moving a single voxel,
		   which is the common case at a world load. */
		struct StampKey
		{
			Vector3 Origin = Vector3(0.f);
			Quaternion Rotation;
			Vector3 Scale = Vector3(1.f);
			Vector3 RoundedScale = Vector3(0.f);

			const VoxFrame* Frame = nullptr;
			uint32_t OverrideColor = 0;

			/* The whole tag byte the stamp writes, not just the render state:
			   Emissive rides in it too (RENDERING_PLAN.md 7.4) and toggling it
			   has to invalidate the key. */
			int32_t State = -1;

			bool operator==(const StampKey& other) const
			{
				return Frame == other.Frame &&
					OverrideColor == other.OverrideColor &&
					State == other.State &&
					Origin == other.Origin &&
					Scale == other.Scale &&
					RoundedScale == other.RoundedScale &&
					Rotation.x == other.Rotation.x &&
					Rotation.y == other.Rotation.y &&
					Rotation.z == other.Rotation.z &&
					Rotation.w == other.Rotation.w;
			}
		};

		/* Default RoundedScale of zero is a stamp no real one can equal, so a
		   BakeData that has never been through Occupy never matches. */
		StampKey Stamp;

		/* The grid-space box Occupy actually stamped into, in the grid the
		   WorldOffset above names. Recorded because VoxelBaker's repair pass
		   asks every renderer in the world "could this clear have erased any of
		   yours?" every time one of them clears, and deriving a box from the
		   transform matrix for that answer is eight matrix-vector products for
		   something the bake already knew. Empty - Min > Max - until a bake. */
		Vector3 StampMin = Vector3(1.f);
		Vector3 StampMax = Vector3(0.f);

		Vector3 LastLocation = Vector3(0.f, 0.f, 0.f);
		Vector3 LastScale = Vector3(1.f, 1.f, 1.f);
		Quaternion LastRotation;

		bool IsEnabled = true;
		bool IsStatic = false;
		bool Updated = false;

		/* Set by VoxelBaker::NotifyClearedRegion: this bake exists only to put
		   back voxels somebody else erased, so its own clear must not send a
		   third renderer round the same loop. Cleared by the bake it triggers. */
		bool RepairOnly = false;
	};

	Event<VoxRenderer*> FrameChanged;

	VoxRenderer(Entity* pOwner);
	~VoxRenderer();

	RenderLayer GetLayer() const { return m_RenderLayer; }
	RenderState GetState() const { return m_RenderState; }
	const VoxFrame* GetFrame() const { return m_pFrame; }

	std::string GetModelFilePath() const;

	void SetModelFilePath(std::string filePath);
	void SetFrame(const VoxFrame* pModel, bool bIncrementRef = true);

	void SetModel(const VoxModel* pModel, bool bIncrementRef = false);

	VColor GetOverrideColor() const { return m_OverrideColor; };
	void SetOverrideColor(VColor overrideColor);

	void SetLayer(RenderLayer layer) { m_RenderLayer = layer; }
	void SetState(RenderState state) { m_RenderState = state; }

	/* Determines whether the voxel positions should be rounded when rotated diagonally */
	bool IsAxisRounded() const { return m_bAxisRounded; }
	void SetAxisRounded(bool bAxisRounded) { m_bAxisRounded = bAxisRounded; }

	/* Determines whether the rotation angle should be limited to 90 degrees altogether */
	bool IsRotationAngleLimited() const { return m_uiRotationLimit != 0; }

	uint32_t GetRotationAngleLimit() const { return m_uiRotationLimit; }
	void SetRotationAngleLimit(uint32_t uiRotationAngleLimit) { m_uiRotationLimit = uiRotationAngleLimit % 360; }

	bool IsFrameChanged() const { return m_bIsFrameChanged; };
	void ResetFrameChanged() { m_bIsFrameChanged = false; };

	bool UpdateRequested() const { return m_bUpdateRequested; }
	void RequestUpdate() { m_bUpdateRequested = true; }

	bool DrawBoundsEnabled() const { return m_bDrawBounds; };
	Box GetBounds() const;

	/* Whether this model's voxels are light sources - RENDERING_PLAN.md 7.4.
	   An emissive voxel is drawn at its own colour instead of being shaded, and
	   it lights what is around it through the coverage pyramid. Per renderer
	   rather than per voxel because a .vox palette entry has nowhere to say so:
	   MagicaVoxel keeps emission in MATL chunks that VoxModel does not read.
	   The tag byte has room for a per-palette-index version later.

	   Changing it changes what the stamp writes, so the bake has to re-examine
	   this renderer - see BakeData::StampKey, which folds it in for exactly that
	   reason. */
	bool IsEmissive() const { return m_bEmissive; }
	void SetEmissive(bool bEmissive) { m_bEmissive = bEmissive; RequestUpdate(); }

	bool IsChunkInstanceLoaded() const { return m_bIsChunkInstanceLoaded; }
	void SetChunkInstanceLoaded(bool bChunkLoaded) { m_bIsChunkInstanceLoaded = bChunkLoaded; }

private:
	void ResetModel();

private:
	const VoxFrame* m_pFrame = nullptr;
	BakeData m_BakeData;

	VColor m_OverrideColor = VColor(0);

	RenderLayer m_RenderLayer = RL_DEFAULT;
	RenderState m_RenderState = RS_SELECTION_LINES;

	std::string m_modelFilePath = "";

	uint32_t m_uiRotationLimit = 90;

	bool m_bEmissive = false;

	bool m_bAxisRounded = false;
	bool m_bDrawBounds = true;

	bool m_bUpdateRequested = false;
	bool m_bIsFrameChanged = false;
	bool m_bIsChunkInstanceLoaded = false;

	RTTR_ENABLE(Component)
};