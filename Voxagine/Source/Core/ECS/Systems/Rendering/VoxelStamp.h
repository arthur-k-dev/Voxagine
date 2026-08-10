#pragma once

#include "Core/ECS/Components/VoxRenderer.h"
#include "Core/ECS/Components/Transform.h"
#include "Core/ECS/Entity.h"
#include "Core/Resources/Formats/VoxModel.h"

#include <cmath>

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
	Quaternion Rotation;
	Vector3 Scale = Vector3(1.f);
	Vector3 RoundedScale = Vector3(1.f);
};

/* False when the renderer has no frame to stamp. */
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

inline bool ComputeVoxelStampTransform(
	VoxRenderer* pRenderer, const Vector3& v3GridOrigin, float fInvVoxelSize,
	VoxelStampTransform& out)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return false;

	Transform* pTransform = pRenderer->GetTransform();

	Quaternion quat = pTransform->GetRotation();
	Vector3 originOffset(0.f);

	/* Both quantization modes below round the rotation to a multiple of a
	   limit, and nudge the origin by a voxel when the model has been flipped
	   far enough that the rounding lands it half a voxel off. */
	if (pRenderer->IsAxisRounded())
	{
		float fRotationLimit = 45.f;
		Vector3 rotation = StableEulerAnglesDegrees(pTransform->GetRotation());

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
	else if (pRenderer->IsRotationAngleLimited())
	{
		float fRotationLimit = static_cast<float>(pRenderer->GetRotationAngleLimit());
		Vector3 rotation = StableEulerAnglesDegrees(pTransform->GetRotation());

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

		quat = glm::quat(rotation);
	}

	const Vector3 scale = pTransform->GetScale();

	/* VoxelGrid::WorldToGrid, without the VoxelGrid: the grid's origin and
	   voxel size are the whole of what it does. */
	auto worldToGrid = [&v3GridOrigin, fInvVoxelSize](const Vector3& v3World)
	{
		return glm::floor((v3World - v3GridOrigin) * fInvVoxelSize);
	};

	const Vector3 size = pFrame->GetFittedSize();
	const Vector3 offset = -size * 0.5f;

	Vector3 origin;

	if (pFrame->GetModel()->GetFrameCount() > 1)
	{
		const VoxFrame* tFrame = pFrame->GetModel()->GetFrame(0);
		Vector3 offsetCen = -(tFrame->GetFitSizeOffset() - pFrame->GetFitSizeOffset()) * 0.5f
			+ ((pFrame->GetFitSizeOffset() + pFrame->GetFittedSize()) - (tFrame->GetFitSizeOffset() + tFrame->GetFittedSize())) * 0.5f;
		offsetCen.y *= -1.f;

		origin = worldToGrid(pTransform->GetPosition()) + scale * glm::rotate(quat, offset - offsetCen);
	}
	else
	{
		origin = worldToGrid(pTransform->GetPosition() + scale * glm::floor(glm::rotate(quat, offset)));
	}

	out.Origin = origin - originOffset;
	out.Rotation = quat;
	out.Scale = scale;
	out.RoundedScale = glm::ceil(glm::abs(scale));

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
inline bool ComputeStampedGridBounds(
	VoxRenderer* pRenderer, const VoxelStampTransform& stamp,
	Vector3& v3Min, Vector3& v3Max)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return false;

	/* Exactly the range of `modelPosition` in the walk below: model coordinates
	   run over [0, fittedSize - 1], multiplied by the scale - which may be
	   negative, hence the min/max rather than an assumption about which end is
	   which - and then offset by up to RoundedScale - 1 by the scale fill. */
	const Vector3 v3Scaled = (pFrame->GetFittedSize() - Vector3(1.f)) * stamp.Scale;

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

	return true;
}

/* Calls fn(const Vector3& v3GridPosition, uint32_t uiColor) once per voxel the
 * model puts into the grid. Positions are grid-space and already rounded, but
 * are neither bounds checked nor checked for being finite - the caller knows
 * its own grid's size, and the two callers do genuinely different things with
 * an out-of-range one. VoxelBaker names the entity behind a non-finite
 * position, which is the only diagnostic there is for the open NaN-transform
 * defect; the far-field build just drops it.
 *
 * Every caller must therefore write its range test as an in-range test rather
 * than a rejection test. A NaN compares false against everything, so a
 * rejection test lets it through, and static_cast<int32_t> of a NaN is
 * INT32_MIN.
 */
template <typename Fn>
void ForEachStampedVoxel(VoxRenderer* pRenderer, const VoxelStampTransform& stamp, Fn&& fn)
{
	const VoxFrame* pFrame = pRenderer->GetFrame();

	if (!pFrame)
		return;

	const uint32_t* pColors = pFrame->GetColors();
	const uint32_t* pPositions = pFrame->GetPositions();

	const uint32_t uiSolidVoxelCount = pFrame->GetSolidVoxelCount();

	const VColor overrideColor = pRenderer->GetOverrideColor();
	const bool bHasOverrideColor = overrideColor.inst.Colors.a > 0;

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
	const uint32_t uiTag = VoxelStateTag(pRenderer->GetState(), pRenderer->IsEmissive());

	Vector3 lastPosition(0.f);
	UVector3 scaleOffset(0, 0, 0);

	for (uint32_t i = 0; i < uiSolidVoxelCount; ++i)
	{
		for (scaleOffset.x = 0; scaleOffset.x < stamp.RoundedScale.x; ++scaleOffset.x)
		{
			for (scaleOffset.y = 0; scaleOffset.y < stamp.RoundedScale.y; ++scaleOffset.y)
			{
				for (scaleOffset.z = 0; scaleOffset.z < stamp.RoundedScale.z; ++scaleOffset.z)
				{
					// Translation
					const VColor vColPosition = VColor(pPositions[i]);
					Vector3 modelPosition(vColPosition.inst.Colors.r, vColPosition.inst.Colors.g, vColPosition.inst.Colors.b);

					// Scale
					modelPosition *= stamp.Scale;
					modelPosition += scaleOffset;

					// Grid space + rotation
					const Vector3 gridPosition = glm::round(stamp.Origin + glm::rotate(stamp.Rotation, modelPosition));

					// Check if position is different from last time
					if (lastPosition == gridPosition)
						continue;

					lastPosition = gridPosition;

					const uint32_t uiColor = bHasOverrideColor
						? ((overrideColor.inst.Color & 0x00FFFFFFu) | uiTag)
						: ((pColors[i] & 0x00FFFFFFu) | uiTag);

					fn(gridPosition, uiColor);
				}
			}
		}
	}
}
