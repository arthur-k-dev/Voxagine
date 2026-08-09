#ifndef VOXAGINE_FOG_HLSL
#define VOXAGINE_FOG_HLSL

#include "Color.hlsl"

/* Aerial perspective - RENDERING_PLAN.md phase 6.1.
 *
 * Distant surfaces fade toward the sky. Three things it buys, and only the
 * first is what the phase was filed for:
 *
 *   - It hides the far-field seam. Phase 4's volume is the level at four voxels
 *     a cell with no shadow ray and no ambient occlusion, and the window's copy
 *     of the same hillside has both. The boundary is where those two readings
 *     meet, and fog is a weight that goes to zero on one side of it.
 *   - It sells the scale phase 4 built. A horizon two thousand units away that
 *     is exactly as crisp as the wall in front of the player does not read as
 *     distant, it reads as small.
 *   - The endless ground plane stops meeting the sky along a hard line. It runs
 *     to infinity, so without this there is a horizon edge with sky above and
 *     lit sand below and nothing in between.
 *
 * **Applied to linear radiance, before EncodeSceneColor.** Fog is in-scattered
 * light replacing transmitted light, which is an interpolation between two
 * radiances; doing it on encoded values is the same class of error 7.2 removed
 * from the sun-plus-sky sum. The fog colour is therefore the *decoded* sky, and
 * because the encode follows immediately, a fully fogged pixel comes out as
 * exactly SKY_COLOR - which is what the sky itself writes, so the horizon has
 * no step in it.
 *
 * Squared exponential rather than plain exponential, and offset by FOG_START:
 * both exist to keep the near field untouched. Gameplay happens within a couple
 * of hundred units of the camera and hazing it would cost contrast where the
 * player is actually looking, while the whole point is the other end of the
 * range.
 *
 * Deliberately not capped short of full. A cap would leave the endless ground
 * at some fixed fraction of the sky no matter how far away it is, which puts a
 * band of not-quite-sky along the horizon - the hard line this removes, moved
 * rather than deleted.
 */

/* 0 at the camera, 1 where nothing of the surface is left. Distance is from the
   camera in world units - not the marcher's own Distance, which is measured
   from wherever that ray started. */
float AerialPerspective(float fDistance)
{
#if FOG_ENABLED
	float fRange = max(fDistance - FOG_START, 0.0) * FOG_DENSITY;

	return 1.0 - exp(-fRange * fRange);
#else
	return 0.0;
#endif
}

/* Linear radiance in, linear radiance out. */
float3 ApplyAerialPerspective(float3 v3Radiance, float fDistance)
{
#if FOG_ENABLED
	return lerp(v3Radiance, SrgbToLinear(SKY_COLOR.rgb), AerialPerspective(fDistance));
#else
	return v3Radiance;
#endif
}

#endif
