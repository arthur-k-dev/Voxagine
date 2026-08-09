#ifndef VOXAGINE_LIGHTING_HLSL
#define VOXAGINE_LIGHTING_HLSL

#include "Color.hlsl"

/* The surface shading model, in one place because four consumers have to
   agree on it exactly: VoxelRenderer.ps.hlsl and its ShadowLess variant shade
   the resident window, FarField.hlsl shades both the far-field volume and the
   endless ground plane, and Particles.vs.hlsl shades debris. The window and the
   far field meet along a visible boundary, so any disagreement between them
   reads as a hard brightness seam - that is how the previous far-field constant
   was found to be wrong (see RENDERING_PLAN.md phase 4).

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

   **This is linear light as of RENDERING_PLAN.md phase 7.2.** Albedo is decoded
   on the way in, the two light terms are summed as radiance, and the caller
   encodes the result - see Color.hlsl for where that boundary is and why it is
   not at present. Every constant below is therefore a *linear* quantity, and
   each one carries the gamma-space value it was converted from, because those
   were tuned by eye against the old pipeline and the numbers look wrong
   otherwise: a sky term of 0.095 is the same light as the 0.34 that was here
   before.

   What visibly changed on conversion, and it is the point of the phase: the sum
   sun + sky used to happen on encoded values, which over-brightens wherever
   both terms are significant. A sunlit, sky-open surface is now a few percent
   darker and the sun-to-shadow ramp is smoother; a shadowed one is unchanged,
   because a single term through the same curve is the same number. */

/* Sunlight reaching a surface facing it head on, unshadowed. Slightly warm.
   Was (1.0, 0.96, 0.88) * 0.85 in gamma space. The tint is more saturated in
   linear than the encoded number suggests, which is the curve, not a retune. */
#define SUN_COLOR (float3(1.0, 0.912, 0.750) * 0.692)

/* Skylight from directly overhead. Tinted toward SKY_COLOR so that what the
   sky is drawn as and what it lights the world with are the same colour - which
   is now literally SrgbToLinear(SKY_COLOR) scaled, where before it was the
   encoded sky colour scaled. Was (0.588, 0.902, 1.0) * 0.34. */
#define SKY_LIGHT_COLOR (float3(0.350, 0.809, 1.0) * 0.0946)

/* "Skylight" from directly below - really the ground bouncing sun back up,
   folded into the hemisphere so that downward faces are not black. Warm and
   dim; it is the cheapest possible stand in for the bounce that phase 7.3
   computes properly. Was (0.20, 0.17, 0.13). */
#define SKY_GROUND_COLOR float3(0.0331, 0.0245, 0.0153)

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

   **In linear this number means something different, and it had to move.** The
   curve shapes a visibility fraction that now multiplies radiance rather than
   an encoded value, and the encode afterwards is itself a ~1/2.2 power - so the
   old 1.35 would have arrived on screen as ~0.61 and AO would have all but
   vanished. 2.97 is 1.35 x 2.2: the same image as before this phase.

   1.0 is the raw visibility and, unlike in gamma space, it is now also the
   physically correct value - an unoccluded fraction of the sky delivers that
   fraction of the sky's radiance and nothing else. Everything above 1.0 is an
   art call, and this one is inherited rather than chosen. */
#define SKY_AO_CURVE 2.97

/* Irradiance arriving from the sky hemisphere on a surface with this normal.
   A cosine-weighted integral of a two-colour hemisphere is a lerp on the
   normal's Y - upward faces see sky, downward faces see bounced ground, and a
   vertical wall sees the average of the two. */
float3 GetSkyLight(float3 v3Normal)
{
	return lerp(SKY_GROUND_COLOR, SKY_LIGHT_COLOR, v3Normal.y * 0.5 + 0.5);
}

/* v3Albedo        - as stored: an 8-bit sRGB palette colour, decoded here.
   fSunVisibility  - 1 lit, 0 fully shadowed. The shadow map's transmittance.
   fSkyVisibility  - 1 open to the sky, 0 fully enclosed. Ambient occlusion,
                     from the cone trace of 7.1b.
   v3Bounce        - linear irradiance arriving from nearby lit geometry, from
                     the same cones (7.3). Zero for the callers that have no
                     pyramid to trace: the far field, the ground plane and
                     particles.

   Returns **linear radiance**, not something that can be written to a target;
   every caller passes it through EncodeSceneColor.

   The bounce is a third *irradiance*, summed with the other two inside the
   parentheses rather than added to the result - so it is reflected by this
   surface's albedo, which is what makes red light on a white wall read as red
   and on a blue wall read as nothing. Adding it outside would have painted the
   emitter's colour over the receiver regardless of what the receiver is.

   It is deliberately not occluded by fSkyVisibility. The two are alternatives,
   not factors: ambient occlusion is the admission that the flat sky term
   over-lights an enclosed place, and a bounce term computed by tracing that
   same enclosure already knows what is there. Multiplying by AO as well would
   darken a crevice twice for the one reason. */
float3 ShadeSurface(float3 v3Albedo, float3 v3Normal, float fSunVisibility, float fSkyVisibility, float3 v3Bounce)
{
	float fNdotL = clamp(dot(v3Normal, -lightDirection.xyz), 0.0, 1.0);

	float3 v3Sun = SUN_COLOR * fNdotL * fSunVisibility;
	float3 v3Sky = GetSkyLight(v3Normal) * lerp(1.0, pow(fSkyVisibility, SKY_AO_CURVE), SKY_AO_STRENGTH);

	return SrgbToLinear(v3Albedo) * (v3Sun + v3Sky + v3Bounce);
}

#endif
