#ifndef VOXAGINE_SUNSHADOWLOOKUP_HLSL
#define VOXAGINE_SUNSHADOWLOOKUP_HLSL

/* Reading the sun shadow map - RENDERING_PLAN.md 7.1a. Include after declaring
 *
 *     Texture2D<float> sunShadowMap;
 *     SamplerState s0;
 *
 * SunShadow.ps.hlsl writes it: one texel per light-space column of the window,
 * holding the light-space depth of the first blocker in that column.
 *
 * This is percentage-closer soft shadows. The blocker search finds how far in
 * front of this surface the occluding geometry actually is, and the filter
 * width is that distance times the sun's angular radius - so the penumbra
 * widens with distance to the occluder, and a box resting on the floor still
 * casts a hard line where it touches. Phase 6.2 got the same property by firing
 * a cone of rays and paying for it linearly in ray count; here it is a handful
 * of texture taps and the cost does not move with how soft the result is.
 */

/* Window-space position to light space: (u, v) across the map, w along the
   light. CameraData.hlsl documents the frame. */
float3 SunShadowToLightSpace(float3 v3Position)
{
	return float3(
		dot(v3Position, shadowTangent.xyz),
		dot(v3Position, shadowBitangent.xyz),
		dot(v3Position, lightDirection.xyz));
}

float2 SunShadowUV(float2 v2Light)
{
	return (v2Light - shadowRect.xy) / shadowRect.zw;
}

/* Vogel disc, the same construction GetSunVisibility used for its ray
   directions - equal-area radii and a golden-angle turn between samples. */
float2 SunShadowTap(uint uiIndex, uint uiCount, float fRotation)
{
	float fRadius = sqrt((float(uiIndex) + 0.5) / float(uiCount));
	float fAngle = float(uiIndex) * 2.39996323 + fRotation;

	return float2(cos(fAngle), sin(fAngle)) * fRadius;
}

/* 1 fully lit, 0 fully shadowed.

   v3Normal is the surface's face normal, and it is not decoration: the blocker
   recorded for a lit surface's own texel *is that surface*, at very nearly the
   same depth, so a depth bias alone has to be large enough to cover a whole
   texel's worth of light-space slope - which then leaks light under genuine
   contact shadows. Pushing the sample point out along the normal first moves it
   into the neighbouring column instead, so the bias only has to absorb what is
   left. Voxel faces are axis aligned, so one voxel of offset is exact rather
   than a tuned fudge. */
/* One tap, no blocker search, no filter: is this *point* lit? For the bounce
   cones of RENDERING_PLAN.md 7.3, which ask it of every cone sample and would
   pay for PCSS thirty-five times a pixel.

   A cone sample has no normal to offset along, so the bias is a tolerance along
   the light instead - see GI_SUN_TOLERANCE for why one is needed at all, and
   why it is not the receiver bias above. A hard 0-or-1 rather than a filtered
   edge is deliberate: the cone averages tens of these and the aggregate is
   smooth, where filtering each one would cost taps for softness nothing sees.
   Off the map is lit, exactly as the receiver lookup assumes. */
float GetSunVisibilityAt(float3 v3Position)
{
	float3 v3Light = SunShadowToLightSpace(v3Position);
	float2 v2UV = SunShadowUV(v3Light.xy);

	if (any(v2UV < 0.0) || any(v2UV > 1.0))
		return 1.0;

	float fDepth = sunShadowMap.SampleLevel(s0, v2UV, 0);

	return (v3Light.z - GI_SUN_TOLERANCE <= fDepth) ? 1.0 : 0.0;
}

float GetSunVisibility(float3 v3Position, float3 v3Normal, float2 v2ScreenPosition)
{
	/* Both the offset and the depth bias scale with how edge-on this surface is
	   to the light. See SUN_SHADOW_GRAZING_SCALE - a constant is only ever right
	   for a face pointing at the sun, and a vertical wall under a 48-degree sun
	   is nowhere near that. */
	float fNdotL = clamp(dot(v3Normal, -lightDirection.xyz), 0.0, 1.0);
	float fGrazing = 1.0 + SUN_SHADOW_GRAZING_SCALE * (1.0 - fNdotL);

	float3 v3Light = SunShadowToLightSpace(v3Position + v3Normal * (SUN_SHADOW_NORMAL_OFFSET * fGrazing));
	float2 v2UV = SunShadowUV(v3Light.xy);

	/* Outside the map is outside the resident window, where there is nothing
	   loaded to cast a shadow. Lit is the honest answer and it matches what the
	   far field and the endless ground plane already assume. */
	if (any(v2UV < 0.0) || any(v2UV > 1.0))
		return 1.0;

	/* Texels per world unit on each axis; shadowDepth.yz is its reciprocal. */
	float2 v2TexelsPerUnit = 1.0 / shadowDepth.yz;

	float fRotation = frac(52.9829189 * frac(dot(v2ScreenPosition, float2(0.06711056, 0.00583715)))) * 6.2831853;

	float fReceiver = v3Light.z - SUN_SHADOW_BIAS * fGrazing;

	/* --- Blocker search ---------------------------------------------------
	   Average depth of whatever is in front of this surface. The radius is the
	   widest penumbra a blocker could produce *for this receiver*, which is
	   bounded by how far the receiver is from the near plane - nothing can
	   occlude it from further away than that.

	   Sizing this off shadowDepth.w, the whole window's depth range, was the
	   first attempt and it is what made the penumbra speckle: ~1300 units of
	   range clamped the search to its maximum radius for every pixel, and a
	   handful of taps spread over that radius makes the average blocker depth -
	   and therefore the filter radius - jump from pixel to pixel. The noise was
	   never in the filter, it was in the estimate driving it. */
	float fReceiverDepth = max(fReceiver - shadowDepth.x, 0.0);

	float fSearchRadius = min(
		SUN_ANGULAR_RADIUS * fReceiverDepth * max(v2TexelsPerUnit.x, v2TexelsPerUnit.y),
		SUN_SHADOW_MAX_RADIUS);

	float fBlockerSum = 0.0;
	float fBlockerCount = 0.0;

	for (uint i = 0; i < SUN_SHADOW_BLOCKER_TAPS; i++)
	{
		float2 v2Offset = SunShadowTap(i, SUN_SHADOW_BLOCKER_TAPS, fRotation) * fSearchRadius;
		float fDepth = sunShadowMap.SampleLevel(s0, v2UV + v2Offset / SUN_SHADOW_RESOLUTION, 0);

		if (fDepth < fReceiver)
		{
			fBlockerSum += fDepth;
			fBlockerCount += 1.0;
		}
	}

	/* Nothing in front of it anywhere in the search radius. */
	if (fBlockerCount == 0.0)
		return 1.0;

	/* --- Filter -----------------------------------------------------------
	   Penumbra width from the receiver-to-blocker distance. This is the whole
	   trick: it is contact hardening, and it is free. */
	float fBlocker = fBlockerSum / fBlockerCount;

	/* Floored at SUN_SHADOW_MIN_RADIUS rather than letting PCSS's own estimate
	   reach zero - see that constant. A blocker essentially touching the
	   receiver still gets the filter loop, just at the smallest radius, so
	   contact reads as a hard line rather than being smeared by a wide
	   penumbra *and* never falls back to a single point sample of a map whose
	   own texel grid is coarser and diagonal to the geometry it is sampling. */
	float fRadius = clamp(
		SUN_ANGULAR_RADIUS * max(fReceiver - fBlocker, 0.0) * max(v2TexelsPerUnit.x, v2TexelsPerUnit.y),
		SUN_SHADOW_MIN_RADIUS,
		SUN_SHADOW_MAX_RADIUS);

	float fLit = 0.0;

	for (uint j = 0; j < SUN_SHADOW_FILTER_TAPS; j++)
	{
		float2 v2Offset = SunShadowTap(j, SUN_SHADOW_FILTER_TAPS, fRotation) * fRadius;
		float fDepth = sunShadowMap.SampleLevel(s0, v2UV + v2Offset / SUN_SHADOW_RESOLUTION, 0);

		fLit += (fDepth < fReceiver) ? 0.0 : 1.0;
	}

	return fLit / float(SUN_SHADOW_FILTER_TAPS);
}

#endif
