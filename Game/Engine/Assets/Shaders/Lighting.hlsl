#ifndef VOXAGINE_LIGHTING_HLSL
#define VOXAGINE_LIGHTING_HLSL

/* The surface shading model, in one place because three consumers have to
   agree on it exactly: VoxelRenderer.ps.hlsl and its ShadowLess variant shade
   the resident window, and FarField.hlsl shades both the far-field volume and
   the endless ground plane. The window and the far field meet along a visible
   boundary, so any disagreement between them reads as a hard brightness seam -
   that is how the previous far-field constant was found to be wrong (see
   RENDERING_PLAN.md phase 4).

   Two lights rather than one, which is the change:

     sun  - directional, warm, occluded by the shadow ray.
     sky  - hemispherical, cool from above and warm from below, occluded by
            ambient occlusion.

   What this replaces is a single `difference * (1 - AMBIENT) + AMBIENT` ramp
   whose floor was a flat grey constant, with AO applied afterwards as a
   multiplier over the whole result. That is wrong in the way that matters for
   the look: **AO is skylight occlusion, not sunlight occlusion**. A crevice in
   direct sun is not dark, and under the old model it was, because AO dimmed
   the sun term too. Splitting the two is most of why a path traced voxel
   render reads as lit by a place rather than by a lamp.

   Note this is still gamma space - RENDERING_PLAN.md 6.4 has not happened, and
   AmbientOcclusion.hlsl still hand-applies pow(x, 2.2) for that reason. Adding
   two light terms here is the same class of error that phase warns about for
   bounce light, and every constant below will want retuning once it lands.
   Doing it anyway because the shape of the lighting is what is wrong today,
   and that is visible from across the room. */

/* Sunlight reaching a surface facing it head on, unshadowed. Slightly warm. */
#define SUN_COLOR (float3(1.0, 0.96, 0.88) * 0.85)

/* Skylight from directly overhead. Tinted toward SKY_COLOR so that what the
   sky is drawn as and what it lights the world with are the same colour. */
#define SKY_LIGHT_COLOR (float3(0.588, 0.902, 1.0) * 0.34)

/* "Skylight" from directly below - really the ground bouncing sun back up,
   folded into the hemisphere so that downward faces are not black. Warm and
   dim; it is the cheapest possible stand in for the bounce that phase 7.3
   computes properly. */
#define SKY_GROUND_COLOR float3(0.20, 0.17, 0.13)

/* How much ambient occlusion is allowed to close down the sky term. At 1.0 a
   fully occluded crevice receives no skylight at all, which is correct and
   also the strongest reading; lower it if contact darkening is too heavy. */
#define SKY_AO_STRENGTH 1.0

/* Contrast curve on the raw sky visibility. Below 1 lightens, above 1 deepens.

   This is the knob for "AO used to be stronger", and the reason it needs one:
   the old model multiplied the *whole* shaded result - direct sun included - by
   a 0.54..1.0 remap of the same term. Occluding only the sky is the correct
   split (a crevice in full sun is not dark), but it costs AO roughly the sky's
   share of the light in apparent strength. AmbientOcclusion.hlsl also carried a
   pow(x, 1/3) softening that existed only to fit that old narrow remap, which
   compounded it.

   1.0 is the raw visibility. Raise toward 2 for heavier, more MagicaVoxel-like
   contact darkening; that is an art call, not a correctness one. */
#define SKY_AO_CURVE 1.35

/* Irradiance arriving from the sky hemisphere on a surface with this normal.
   A cosine-weighted integral of a two-colour hemisphere is a lerp on the
   normal's Y - upward faces see sky, downward faces see bounced ground, and a
   vertical wall sees the average of the two. */
float3 GetSkyLight(float3 v3Normal)
{
	return lerp(SKY_GROUND_COLOR, SKY_LIGHT_COLOR, v3Normal.y * 0.5 + 0.5);
}

/* fSunVisibility  - 1 lit, 0 fully shadowed. The shadow ray's transmittance.
   fSkyVisibility  - 1 open to the sky, 0 fully enclosed. Ambient occlusion,
                     and the term phase 7.1 replaces with a cone trace. */
float3 ShadeSurface(float3 v3Albedo, float3 v3Normal, float fSunVisibility, float fSkyVisibility)
{
	float fNdotL = clamp(dot(v3Normal, -lightDirection.xyz), 0.0, 1.0);

	float3 v3Sun = SUN_COLOR * fNdotL * fSunVisibility;
	float3 v3Sky = GetSkyLight(v3Normal) * lerp(1.0, pow(fSkyVisibility, SKY_AO_CURVE), SKY_AO_STRENGTH);

	return v3Albedo * (v3Sun + v3Sky);
}

#endif
