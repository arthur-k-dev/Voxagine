#pragma once

#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Components/Transform.h"
#include "Core/ECS/Entity.h"
#include "Core/Resources/Formats/VoxModel.h"
#include "Core/ECS/Systems/Rendering/VoxelStampCursor.h"

#include <cmath>
#include <cstdint>
#include <utility>

/* Where a VoxRenderer's model lands in a voxel grid, and how to walk the voxels
 * it puts there.
 *
 * Extracted from VoxelBaker::Occupy so that the far-field volume
 * (RENDERING_PLAN.md phase 4) can stamp the same model at the same place in a
 * *different* grid. The two have to agree exactly or the far field and the
 * detail window disagree about where the level is, and the seam between them
 * moves as the window slides.
 *
 * The grid is parameterized only by its origin and voxel size, which is all
 * VoxelGrid::WorldToGrid ever used: the window passes its world offset, the
 * far field passes zero because level space *is* world space.
 */
struct VoxelStampTransform
{
	Vector3 Origin = Vector3(0.f);

	/* Identity, spelled out. GLM does not zero-initialise and this tree does
	   not set GLM_FORCE_CTOR_INIT, so `Quaternion Rotation;` is whatever was on
	   the stack - and a garbage rotation turns every stamped position into a
	   NaN, which the walk does not check for and the caller's in-range test
	   silently drops. It cost a CI failure that reproduced nowhere else: the
	   runner's stack garbage was non-finite and this machine's was not. */
	Quaternion Rotation = Quaternion(1.f, 0.f, 0.f, 0.f);

	Vector3 Scale = Vector3(1.f);
	Vector3 RoundedScale = Vector3(1.f);
};

/* Everything the placement math actually reads about a model, separated from
 * the object graph it normally comes from.
 *
 * A VoxRenderer needs a loaded VoxModel, which needs the ResourceManager, which
 * needs a render context - so for as long as placement was reachable only
 * through a renderer, nothing could check where a model lands without booting
 * the engine. Same move as pulling SphericalDestruction::Apply out of
 * PhysicsSystem: the game, the far field and the tests run one implementation.
 *
 * CHUNK_STREAMING_PLAN.md phase 9 is why it is worth doing now - the walk below
 * became resumable, and "resuming at every cursor value produces the identical
 * voxel set" is a check that needs a model and nothing else.
 */
struct VoxelStampModel
{
	Vector3 v3FittedSize = Vector3(0.f);
	Vector3 v3FitSizeOffset = Vector3(0.f);

	/* Frame 0's, for the multi-frame centring correction below. Equal to the
	   pair above on a single-frame model, which is what makes that branch a
	   no-op rather than a special case. */
	Vector3 v3FirstFittedSize = Vector3(0.f);
	Vector3 v3FirstFitSizeOffset = Vector3(0.f);

	uint32_t uiFrameCount = 1;
};

struct VoxelStampPose
{
	Vector3 v3Position = Vector3(0.f);

	/* Identity - see VoxelStampTransform::Rotation for what an uninitialised
	   one costs. */
	Quaternion Rotation = Quaternion(1.f, 0.f, 0.f, 0.f);

	Vector3 v3Scale = Vector3(1.f);

	/* Zero means "use the rotation as given". Anything else rounds it to a
	   multiple of that many degrees, which is what the renderer's axis-rounding
	   (45) and rotation-angle-limit modes do - and the rounding is load-bearing,
	   because the voxels land on a lattice and a free rotation does not. */
	float fRotationLimitDegrees = 0.f;
};

/* Euler angles in degrees, without the ±90° degeneracy that glm::eulerAngles
   has - and this is an architecture-dependent bug, not a theoretical one.
 *
 * glm computes pitch as atan2(2(yz + wx), w² - x² - y² + z²). For a pure yaw
 * the numerator is exactly zero and the denominator is cos(yaw), which at
 * exactly ±90° is *mathematically* zero. atan2(0, +0.0) is 0 and
 * atan2(0, -0.0) is π, so the sign of a floating-point epsilon decides between
 * an upright model and one rotated 180° in pitch *and* roll.
 *
 * x86-64 GCC and aarch64 Clang do not agree on that sign, because Clang
 * contracts w*w - y*y into an FMA and rounds differently. The symptom was a
 * character that appeared upside down when facing exactly left or right, on
 * Android only - and a round-trip test compiled on the desktop passes, which
 * is exactly why this went unnoticed for years of desktop play.
 *
 * The fix is not to make the epsilon come out positive. It is to stop asking
 * atan2 a question with no answer: when both of its arguments are within an
 * epsilon of zero the angle is genuinely undefined, and for a rotation that is
 * a pure yaw the right choice is zero. Note this only triggers *at* the
 * degeneracy - at yaw 135° the denominator is cos(135°) = -0.707, nowhere near
 * zero, so the legitimate (180, 45, 180) extraction is left alone. */
inline float StableAtan2(float fNumerator, float fDenominator)
{
	const float k_fEpsilon = 1e-6f;

	if (std::fabs(fNumerator) < k_fEpsilon && std::fabs(fDenominator) < k_fEpsilon)
		return 0.f;

	return std::atan2(fNumerator, fDenominator);
}

inline Vector3 StableEulerAnglesDegrees(const Quaternion& q)
{
	const float fX = q.x, fY = q.y, fZ = q.z, fW = q.w;

	const float fPitch = StableAtan2(2.f * (fY * fZ + fW * fX),
	                                 fW * fW - fX * fX - fY * fY + fZ * fZ);

	const float fYaw = std::asin(std::max(-1.f, std::min(1.f, -2.f * (fX * fZ - fW * fY))));

	const float fRoll = StableAtan2(2.f * (fX * fY + fW * fZ),
	                                fW * fW + fX * fX - fY * fY - fZ * fZ);

	return Vector3(fPitch, fYaw, fRoll) * RAD2DEG;
}

inline void ComputeVoxelStampTransform(
	const VoxelStampModel& model, const VoxelStampPose& pose,
	const Vector3& v3GridOrigin, float fInvVoxelSize,
	VoxelStampTransform& out)
{
	Quaternion quat = pose.Rotation;
	Vector3 originOffset(0.f);

	/* Both of the renderer's quantization modes reach here as one limit: round
	   the rotation to a multiple of it, and nudge the origin by a voxel when the
	   model has been flipped far enough that the rounding lands it half a voxel
	   off. */
	if (pose.fRotationLimitDegrees > 0.f)
	{
		const float fRotationLimit = pose.fRotationLimitDegrees;
		Vector3 rotation = StableEulerAnglesDegrees(pose.Rotation);

		rotation.x = std::fmod(rotation.x + 360.f, 360.f);
		rotation.y = std::fmod(rotation.y + 360.f, 360.f);
		rotation.z = std::fmod(rotation.z + 360.f, 360.f);

		if (abs(rotation.x) > 90 + fRotationLimit / 2.f)
			originOffset.z += 1;

		if (abs(rotation.z) > 90 + fRotationLimit / 2.f)
			originOffset.x += 1;

		rotation.x = round(rotation.x / fRotationLimit) * fRotationLimit * DEG2RAD;
		rotation.y = round(rotation.y / fRotationLimit) * fRotationLimit * DEG2RAD;
		rotation.z = round(rotation.z / fRotationLimit) * fRotationLimit * DEG2RAD;

		quat = glm::quat(glm::vec3(rotation.x, rotation.y, rotation.z));
	}

	const Vector3 scale = pose.v3Scale;

	/* VoxelGrid::WorldToGrid, without the VoxelGrid: the grid's origin and
	   voxel size are the whole of what it does. */
	auto worldToGrid = [&v3GridOrigin, fInvVoxelSize](const Vector3& v3World)
	{
		return glm::floor((v3World - v3GridOrigin) * fInvVoxelSize);
	};

	const Vector3 size = model.v3FittedSize;
	const Vector3 offset = -size * 0.5f;

	Vector3 origin;

	if (model.uiFrameCount > 1)
	{
		Vector3 offsetCen = -(model.v3FirstFitSizeOffset - model.v3FitSizeOffset) * 0.5f
			+ ((model.v3FitSizeOffset + model.v3FittedSize)
			   - (model.v3FirstFitSizeOffset + model.v3FirstFittedSize)) * 0.5f;
		offsetCen.y *= -1.f;

		origin = worldToGrid(pose.v3Position) + scale * glm::rotate(quat, offset - offsetCen);
	}
	else
	{
		origin = worldToGrid(pose.v3Position + scale * glm::floor(glm::rotate(quat, offset)));
	}

	out.Origin = origin - originOffset;
	out.Rotation = quat;
	out.Scale = scale;
	out.RoundedScale = glm::ceil(glm::abs(scale));
}

/* Reads the pair above off a VoxRenderer. False when it has no frame to stamp. */
inline bool DescribeVoxelStamp(VoxRenderer* pRenderer, VoxelStampModel& o_model, VoxelStampPose& o_pose)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return false;

	const VoxFrame* pFirst = pFrame->GetModel()->GetFrame(0);

	o_model.v3FittedSize = pFrame->GetFittedSize();
	o_model.v3FitSizeOffset = pFrame->GetFitSizeOffset();
	o_model.v3FirstFittedSize = pFirst ? pFirst->GetFittedSize() : o_model.v3FittedSize;
	o_model.v3FirstFitSizeOffset = pFirst ? pFirst->GetFitSizeOffset() : o_model.v3FitSizeOffset;
	o_model.uiFrameCount = pFrame->GetModel()->GetFrameCount();

	Transform* pTransform = pRenderer->GetTransform();

	o_pose.v3Position = pTransform->GetPosition();
	o_pose.Rotation = pTransform->GetRotation();
	o_pose.v3Scale = pTransform->GetScale();

	o_pose.fRotationLimitDegrees = pRenderer->IsAxisRounded()
		? 45.f
		: (pRenderer->IsRotationAngleLimited()
			? static_cast<float>(pRenderer->GetRotationAngleLimit())
			: 0.f);

	return true;
}

/* False when the renderer has no frame to stamp. */
inline bool ComputeVoxelStampTransform(
	VoxRenderer* pRenderer, const Vector3& v3GridOrigin, float fInvVoxelSize,
	VoxelStampTransform& out)
{
	VoxelStampModel model;
	VoxelStampPose pose;

	if (!DescribeVoxelStamp(pRenderer, model, pose))
		return false;

	ComputeVoxelStampTransform(model, pose, v3GridOrigin, fInvVoxelSize, out);

	return true;
}

/* DYNAMIC_MODELS_PLAN.md phase 2/3. The continuous (unrounded) equivalent of
 * ComputeVoxelStampTransform above, for a renderer that is drawn as a mesh
 * rather than stamped into a grid - no rotation quantization, no
 * grid-alignment floor (which exists above only to land a *voxelized* stamp
 * on an integer cell and has no meaning for a rasterized quad), no
 * RoundedScale fill loop (a mesh's vertex shader scales continuously; there
 * is no gap to fill).
 *
 * Two callers as of phase 3: RenderSystem::PostTick, which needs it to place
 * and bound the mesh, and VoxFrameEmitter, which needs it to place a dying
 * dynamic renderer's particles now that VoxelBaker never stamps one (so
 * BakeData::Positions - the old source - is always null for it). Kept here
 * rather than duplicated so the two can never disagree about where a
 * renderer's model actually is. */
/* Whether a dynamic renderer's yaw snaps to the nearest of 8 compass
 * directions (45 degree steps) rather than rendering the transform's true,
 * continuous heading. Settled with the user 2026-08-10: on by default -
 * these are voxel-art models authored with a handful of canonical facings in
 * mind, and a continuously turning character reads as unnatural between
 * them (faceting that was never meant to be seen head-on). Only yaw snaps;
 * pitch and roll (a knockback tumble, say) stay continuous.
 *
 * Not a Settings-exposed option yet - a function-local static reference is
 * the whole toggle, flip it with `ModelYawSnapEnabled() = false;` until
 * product wants a real setting. Function-local rather than a plain global to
 * sidestep static-initialization-order questions across translation units. */
inline bool& ModelYawSnapEnabled()
{
	static bool s_bEnabled = true;
	return s_bEnabled;
}

inline bool ComputeContinuousModelTransform(
	VoxRenderer* pRenderer, Vector3& o_v3WorldOrigin, Quaternion& o_qRotation, Vector3& o_v3Scale)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return false;

	Transform* pTransform = pRenderer->GetTransform();

	o_qRotation = pTransform->GetRotation();
	o_v3Scale = pTransform->GetScale();

	if (ModelYawSnapEnabled())
	{
		/* Same extraction ComputeVoxelStampTransform's own quantization uses
		   above - kept to a single well-understood idiom in this file rather
		   than a second one, just applied to one axis instead of three. */
		Vector3 v3Euler = StableEulerAnglesDegrees(o_qRotation);
		v3Euler.y = std::round(v3Euler.y / 45.f) * 45.f;

		o_qRotation = glm::quat(v3Euler * DEG2RAD);
	}

	const Vector3 size = pFrame->GetFittedSize();
	const Vector3 offset = -size * 0.5f;
	Vector3 offsetCen(0.f);

	if (pFrame->GetModel()->GetFrameCount() > 1)
	{
		const VoxFrame* pFirstFrame = pFrame->GetModel()->GetFrame(0);
		offsetCen = -(pFirstFrame->GetFitSizeOffset() - pFrame->GetFitSizeOffset()) * 0.5f
			+ ((pFrame->GetFitSizeOffset() + pFrame->GetFittedSize()) - (pFirstFrame->GetFitSizeOffset() + pFirstFrame->GetFittedSize())) * 0.5f;
		offsetCen.y *= -1.f;
	}

	o_v3WorldOrigin = pTransform->GetPosition() + o_v3Scale * glm::rotate(o_qRotation, offset - offsetCen);

	return true;
}

/* The grid-space box ForEachStampedVoxel below writes into, without walking a
 * voxel to find it: the model's own extent, scaled the way the walk scales it,
 * rotated by the same quantized rotation, placed at the same origin.
 *
 * This exists because the AABB proxy the voxel pass rasterizes has to *contain*
 * the stamp. The proxy used to be derived from the transform matrix instead
 * (VoxRenderer::GetBounds), which is a different quantity: it uses the true
 * rotation where the stamp uses one rounded to a multiple of the renderer's
 * limit, and it is centred on the transform where the stamp is placed by two
 * floors. Where the two disagree the proxy is *short*, and a voxel outside every
 * proxy is never rasterized - so it is missing from the image at the angles
 * where no other model's proxy happens to cover it, and comes back at the
 * angles where one does. See RenderSystem::PostTick.
 *
 * Bounds are inclusive of the last voxel's index; the caller adds the voxel's
 * own extent if it needs the far face.
 */
inline void ComputeStampedGridBounds(
	const Vector3& v3FittedSize, const VoxelStampTransform& stamp,
	Vector3& v3Min, Vector3& v3Max)
{
	/* Exactly the range of `modelPosition` in the walk below: model coordinates
	   run over [0, fittedSize - 1], multiplied by the scale - which may be
	   negative, hence the min/max rather than an assumption about which end is
	   which - and then offset by up to RoundedScale - 1 by the scale fill. */
	const Vector3 v3Scaled = (v3FittedSize - Vector3(1.f)) * stamp.Scale;

	const Vector3 v3ModelMin = glm::min(Vector3(0.f), v3Scaled);
	const Vector3 v3ModelMax = glm::max(Vector3(0.f), v3Scaled) + glm::max(stamp.RoundedScale - Vector3(1.f), Vector3(0.f));

	v3Min = Vector3(FLT_MAX);
	v3Max = Vector3(-FLT_MAX);

	for (uint32_t i = 0; i < 8; ++i)
	{
		const Vector3 corner(
			(i & 1) ? v3ModelMax.x : v3ModelMin.x,
			(i & 2) ? v3ModelMax.y : v3ModelMin.y,
			(i & 4) ? v3ModelMax.z : v3ModelMin.z);

		const Vector3 rotated = glm::rotate(stamp.Rotation, corner);

		v3Min = glm::min(v3Min, rotated);
		v3Max = glm::max(v3Max, rotated);
	}

	/* The walk rounds, and rounding is monotonic, so rounding the extremes of
	   the box bounds the rounded voxel set. */
	v3Min = glm::round(stamp.Origin + v3Min);
	v3Max = glm::round(stamp.Origin + v3Max);
}

inline bool ComputeStampedGridBounds(
	VoxRenderer* pRenderer, const VoxelStampTransform& stamp,
	Vector3& v3Min, Vector3& v3Max)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return false;

	ComputeStampedGridBounds(pFrame->GetFittedSize(), stamp, v3Min, v3Max);

	return true;
}

/* --- The walk -------------------------------------------------------------
 *
 * Calls fn(const Vector3& v3GridPosition, uint32_t uiColor) once per voxel the
 * model puts into the grid. Positions are grid-space and already rounded, but
 * are neither bounds checked nor checked for being finite - the caller knows
 * its own grid's size, and the callers do genuinely different things with an
 * out-of-range one. VoxelBaker names the entity behind a non-finite position,
 * which is the only diagnostic there is for the open NaN-transform defect; the
 * far-field build just drops it.
 *
 * Every caller must therefore write its range test as an in-range test rather
 * than a rejection test. A NaN compares false against everything, so a
 * rejection test lets it through, and static_cast<int32_t> of a NaN is
 * INT32_MIN.
 */

/* The model's own voxels, however they were obtained. A VoxFrame supplies them
   in the engine; a test can parse a .vox and supply them directly, which is why
   this overload exists at all. */
struct VoxelStampVoxels
{
	const uint32_t* pPositions = nullptr;
	const uint32_t* pColors = nullptr;
	uint32_t uiCount = 0;
};

/* One slice of the walk. Calls fn at most uiMaxSamples times - a *sample* is one
 * (model voxel, scale offset) pair, which is one iteration of the innermost loop
 * whether or not it emits, because that is what the work costs - and returns
 * true when the model is finished.
 *
 * An exhausted slice leaves the cursor on the sample it did not run, so calling
 * again with the same voxels, tag, colour and stamp continues exactly where it
 * stopped. All three of those are inputs rather than state for a reason: if the
 * stamp transform changes between slices, the second half of the model lands
 * somewhere the first half is not, and the caller - not this - is the only thing
 * that can notice. See VoxelBaker::Occupy, which restarts the stamp instead.
 */
template <typename Fn>
bool ForEachStampedVoxelRange(
	const VoxelStampVoxels& voxels, uint32_t uiStateTag, const uint32_t* pOverrideColor,
	const VoxelStampTransform& stamp, VoxelStampCursor& cursor, uint32_t uiMaxSamples, Fn&& fn)
{
	const uint32_t* pColors = voxels.pColors;
	const uint32_t* pPositions = voxels.pPositions;

	const uint32_t uiSolidVoxelCount = voxels.uiCount;

	const bool bHasOverrideColor = pOverrideColor != nullptr;
	const uint32_t uiOverrideColor = bHasOverrideColor ? (*pOverrideColor & 0x00FFFFFFu) | uiStateTag : 0u;

	uint32_t uiRemaining = uiMaxSamples;

	/* The counters are compared as floats against RoundedScale, exactly as the
	   unbounded walk did: it is ceil(abs(scale)), so a non-finite scale gives no
	   iterations here where a cast to an integer count would be undefined. */
	for (; cursor.uiVoxel < uiSolidVoxelCount; ++cursor.uiVoxel, cursor.uiScaleX = 0)
	{
		for (; static_cast<float>(cursor.uiScaleX) < stamp.RoundedScale.x; ++cursor.uiScaleX, cursor.uiScaleY = 0)
		{
			for (; static_cast<float>(cursor.uiScaleY) < stamp.RoundedScale.y; ++cursor.uiScaleY, cursor.uiScaleZ = 0)
			{
				for (; static_cast<float>(cursor.uiScaleZ) < stamp.RoundedScale.z; ++cursor.uiScaleZ)
				{
					if (uiRemaining == 0)
						return false;

					--uiRemaining;

					// Translation
					const VColor vColPosition = VColor(pPositions[cursor.uiVoxel]);
					Vector3 modelPosition(vColPosition.inst.Colors.r, vColPosition.inst.Colors.g, vColPosition.inst.Colors.b);

					// Scale
					modelPosition *= stamp.Scale;
					modelPosition += Vector3(
						static_cast<float>(cursor.uiScaleX),
						static_cast<float>(cursor.uiScaleY),
						static_cast<float>(cursor.uiScaleZ));

					// Grid space + rotation
					const Vector3 gridPosition = glm::round(stamp.Origin + glm::rotate(stamp.Rotation, modelPosition));

					// Check if position is different from last time
					if (cursor.LastPosition == gridPosition)
						continue;

					cursor.LastPosition = gridPosition;

					const uint32_t uiColor = bHasOverrideColor
						? uiOverrideColor
						: ((pColors[cursor.uiVoxel] & 0x00FFFFFFu) | uiStateTag);

					fn(gridPosition, uiColor);
				}
			}
		}
	}

	return true;
}

/* The whole model in one call. Deliberately the range walk with no budget
   rather than a second copy of the loop: phase 9's acceptance is that the two
   agree voxel for voxel, and the cheapest way to guarantee that is for there to
   be only one of them. */
template <typename Fn>
void ForEachStampedVoxel(const VoxelStampVoxels& voxels, uint32_t uiStateTag,
                         const uint32_t* pOverrideColor, const VoxelStampTransform& stamp, Fn&& fn)
{
	VoxelStampCursor cursor;

	ForEachStampedVoxelRange(voxels, uiStateTag, pOverrideColor, stamp, cursor,
	                         UINT32_MAX, std::forward<Fn>(fn));
}

/* What a renderer contributes to the walk beyond its placement: its model's
   voxels, its override colour and the tag byte its state and emissive flag
   pack. False when it has no frame to stamp. */
inline bool DescribeVoxelStampVoxels(
	VoxRenderer* pRenderer, VoxelStampVoxels& o_voxels, uint32_t& o_uiStateTag,
	uint32_t& o_uiOverrideColor, bool& o_bHasOverrideColor)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return false;

	o_voxels.pPositions = pFrame->GetPositions();
	o_voxels.pColors = pFrame->GetColors();
	o_voxels.uiCount = pFrame->GetSolidVoxelCount();

	const VColor overrideColor = pRenderer->GetOverrideColor();

	o_uiOverrideColor = overrideColor.inst.Color;
	o_bHasOverrideColor = overrideColor.inst.Colors.a > 0;

	/* The voxel word's tag byte - RENDERING_PLAN.md 7.4, and read that phase's
	   audit before touching it.

	   This used to be `colour | (rendererState + 1) << 24`, an OR onto a colour
	   whose alpha the palette had already set to 255. So the tag never appeared
	   in the buffer at all: **every voxel in the world carried alpha 255**, and
	   rule 3's "the alpha byte packs rendererState + 1" described an intention
	   rather than the data. Nothing noticed because no consumer anywhere reads
	   the value - all of them test `> 0` for occupancy - which is also what made
	   it safe to fix.

	   Masked rather than ORed now, so the byte holds what it claims to and the
	   bits above the state are actually free. VOXEL_EMISSIVE_TAG is the first
	   one claimed. */
	o_uiStateTag = VoxelStateTag(pRenderer->GetState(), pRenderer->IsEmissive());

	return true;
}

template <typename Fn>
void ForEachStampedVoxel(VoxRenderer* pRenderer, const VoxelStampTransform& stamp, Fn&& fn)
{
	VoxelStampVoxels voxels;
	uint32_t uiStateTag = 0;
	uint32_t uiOverrideColor = 0;
	bool bHasOverrideColor = false;

	if (!DescribeVoxelStampVoxels(pRenderer, voxels, uiStateTag, uiOverrideColor, bHasOverrideColor))
		return;

	ForEachStampedVoxel(voxels, uiStateTag, bHasOverrideColor ? &uiOverrideColor : nullptr,
	                    stamp, std::forward<Fn>(fn));
}
