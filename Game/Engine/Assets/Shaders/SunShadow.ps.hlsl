#include "Defines.hlsl"
#include "CameraData.hlsl" // register(b0)

/* Sun shadow map - RENDERING_PLAN.md 7.1a.
 *
 * One texel per light-space column of the resident window; the value is the
 * distance along `lightDirection`, measured from the window's near plane in
 * light space, at which that column first hits something. A miss writes a
 * value past the far plane, which every comparison then reads as "nothing in
 * the way".
 *
 * Why this exists rather than more shadow rays per pixel: a shadow ray per
 * pixel costs ~9.45 ms at 3840x2160 against a 3 ms budget for all lighting,
 * and it scales with pixel count. This pass is a fixed
 * k_uiSunShadowResolution^2 marches whatever the screen is doing. Phase 6.2's
 * notes have the arithmetic.
 *
 * Why not a cone trace through the phase 7.1b mip pyramid: the sun cone is
 * ~1.15 degrees, so its radius is under two voxels out to 100 units, which is
 * finer than the pyramid's finest level over the whole range that matters. A
 * cone is only cheaper than a ray when it is wider than the data it samples,
 * and this one never is. The pyramid is for AO and bounce, whose cones are
 * tens of degrees.
 */

SamplerState s0 : register(s0);

VOXEL_RW_BUFFER voxelWorldData : register(u0);
RW_STRUCTURED_BUFFER(uint) voxelBrickData : register(u1);

#include "SDFMarcher.hlsl"

float main(float4 v4Position : POS_OUT) : TAR_OUT
{
	/* Light-space UV of this texel's centre, then the world-space point it
	   names on the window's near plane. shadowRect is (minU, minV, sizeU,
	   sizeV) and shadowDepth.x is the near plane's own light-space depth. */
	/* SUN_SHADOW_RESOLUTION is Settings::GetSunShadowResolution arriving in the
	   constant buffer now, not a compile-time constant - the pass's target is
	   resized live when the shadow quality changes. It is always the size this
	   draw is actually covering, because RenderContext resizes the target and
	   uploads the number in the same place. */
	float2 v2UV = v4Position.xy / SUN_SHADOW_RESOLUTION;

	float2 v2Light = shadowRect.xy + v2UV * shadowRect.zw;

	float3 v3Origin =
		shadowTangent.xyz * v2Light.x +
		shadowBitangent.xyz * v2Light.y +
		lightDirection.xyz * shadowDepth.x;

	/* MarchDiffuse rather than MarchBricks: this origin is on the window's
	   bounding plane and usually outside the window itself, and MarchDiffuse is
	   the entry point that walks to the box first and reports Distance relative
	   to the origin it was given. MarchBricks assumes it starts inside. */
	int iMaxBrickSteps = int(length(float3(worldSize.xyz)) * BRICK_INV_SIZE) + 2;

	MarchResult hit = MarchDiffuse(v3Origin, lightDirection.xyz, iMaxBrickSteps);

	/* A miss has to land beyond anything a receiver can report, and the far
	   plane is not enough on its own - a receiver's own depth is computed from
	   its world position and can sit a fraction past it through rounding. */
	if (hit.Color.a == 0.0)
		return SUN_SHADOW_FAR;

	return shadowDepth.x + hit.Distance;
}
