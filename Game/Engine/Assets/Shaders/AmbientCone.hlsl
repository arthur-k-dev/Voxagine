#ifndef VOXAGINE_AMBIENTCONE_HLSL
#define VOXAGINE_AMBIENTCONE_HLSL

/* Cone-traced ambient occlusion - RENDERING_PLAN.md 7.1b.
 *
 * Supplements GetSkyVisibility, which asks twelve immediately-adjacent voxels
 * whether they are solid. That only ever sees one voxel out, so it darkens the
 * inside corner of a step and knows nothing about a wall two metres away. The
 * result reads as an edge effect rather than as a place being enclosed, and
 * enclosure is most of what makes a voxel render look lit by a sky.
 *
 * The first cut of this marched the *brick* counts, and the answer was no - it
 * is on screen in the plan and it is what the coverage pyramid was built for.
 * Two failures with nothing in between: started nearer than ~8 voxels the cone
 * samples the 4-voxel bricks holding the wall it began on and the surface
 * occludes itself; started at 8, nothing measures occlusion between one voxel
 * and eight, which is the scale of the gaps between stones in a wall.
 *
 * Both come from the data, not the trace, and the pyramid answers both. Each
 * step samples the level whose cell matches the cone's own width at that
 * distance, filtered, so the near field is read at two voxels and a lattice the
 * geometry knows nothing about stops deciding the result.
 *
 * Route A kept the levels as counts in the brick buffer and filtered by hand,
 * eight fetches a sample; that cost 4.6x the whole rest of the cone and is what
 * sent this to route B. The pyramid is a 3D texture's mip chain now, so a step
 * is one SampleLevel and the filter is the sampler's.
 */

/* Whether this translation unit can ask the sun shadow map about a point.
   VoxelRenderer.ps.hlsl defines it and includes SunShadowLookup.hlsl first; the
   ShadowLess variant has no map at all, and its bounce is lit by an unoccluded
   sun exactly as its direct term is. */
#ifndef AO_CONE_HAS_SUN_SHADOW
	#define AO_CONE_HAS_SUN_SHADOW 0
#endif

/* The pyramid at a point, at a level: alpha is the occupied fraction of the
 * cell, 0..1, and RGB is its albedo in linear light already multiplied by that
 * fraction (RENDERING_PLAN.md 7.3).
 *
 * Premultiplied is what makes one fetch serve both terms. The occlusion wants
 * the fraction; the bounce wants albedo x fraction, because the light a step
 * picks up is the light leaving whatever matter that step just ran into. So the
 * two come out of the same texel with no divide between them.
 *
 * The coordinate is normalized over the whole window and so is the same at
 * every level, which is the second thing the texture buys: route A had to
 * recompute a per-level grid size and base offset per sample. Out of the window
 * reads as empty because the sampler's border is transparent black - past the
 * resident window there is only the endless ground plane, and an AO cone that
 * escapes the window has escaped into open sky by any reasonable reading. */
float4 PyramidSample(float3 v3Position, float3 v3InvWorld, float fLevel)
{
	return voxelPyramid.SampleLevel(pyramidSampler, v3Position * v3InvWorld, fLevel);
}

/* Light arriving at a bouncing cell, which is then reflected by its albedo.
   Sun plus sky, and neither can be evaluated the way a surface's would be: a
   pyramid cell is a density and a colour, with no normal to take an N.L or a
   hemisphere lerp against. GI_SUN_WRAP and GI_SKY_WRAP are the averages that
   stand in for both - see Defines.hlsl. */
float3 ConeIncidentLight(float3 v3Position)
{
#if AO_CONE_HAS_SUN_SHADOW
	float fSun = GetSunVisibilityAt(v3Position);
#else
	float fSun = 1.0;
#endif

	return SUN_COLOR * (fSun * GI_SUN_WRAP)
		+ (SKY_LIGHT_COLOR + SKY_GROUND_COLOR) * (0.5 * GI_SKY_WRAP);
}

/* The level whose cell is about as wide as the cone is at this distance. A
   cone sampling data finer than itself is a ray with extra steps; one sampling
   data coarser than itself is what the brick-only version was, and what put
   the wall's own bricks in front of it.

   Rounded rather than left continuous: the sampler filters point-wise across
   mips, so a fractional level would land on one of them anyway, and asking it
   to blend two is two fetches where the whole point of route B is one. */
float PyramidLevelForRadius(float fRadius)
{
	float fLevel = log2(max(fRadius * 2.0, 1.0) / float(1u << PYRAMID_FINE_SHIFT));

	return clamp(round(fLevel), 0.0, float(PYRAMID_LEVELS - 1));
}

/* One cone, front to back. Returns the occlusion it accumulated in w and the
   radiance it gathered on the way in xyz - RENDERING_PLAN.md 7.3.

   Steps grow geometrically. That is the whole economy of a cone trace: the
   footprint grows linearly with distance, so a fixed step count covers an
   exponentially growing range without ever undersampling relative to the
   cone's own width. A linear march would spend all its samples in the first
   few voxels and never reach the wall that actually encloses the point.

   The two terms share the traversal *and* the weight. What a step occludes is
   `(1 - occlusion) * density * strength`, and the light that reaches the eye
   from that step is what the matter it just ran into emits, attenuated by
   everything already in front of it - which is the same quantity times the
   matter's albedo. Since the texel is premultiplied, the density cancels and
   the bounce weight is just the transmittance times the strength. */
float4 AmbientConeTrace(float3 v3Origin, float3 v3Direction, float3 v3InvWorld)
{
	float3 v3Radiance = 0.0;
	float fOcclusion = 0.0;
	float fDistance = AO_CONE_START;

	for (uint i = 0; i < AO_CONE_STEPS; i++)
	{
		float3 v3Sample = v3Origin + v3Direction * fDistance;

		float fLevel = PyramidLevelForRadius(fDistance * AO_CONE_APERTURE);
		float4 v4Cell = PyramidSample(v3Sample, v3InvWorld, fLevel);

		/* Front-to-back: what is already occluded cannot be occluded again, and
		   near geometry matters more than far - which falls out of the order
		   rather than needing a distance weight. */
		float fTransmittance = 1.0 - fOcclusion;

#if GI_BOUNCE_ENABLED
		/* Skipped where there is nothing to bounce off, which is most steps of
		   most cones on open ground - and it is the shadow map tap inside
		   ConeIncidentLight that this is really skipping. */
		if (v4Cell.a > 0.0)
			v3Radiance += (fTransmittance * AO_CONE_STRENGTH) * v4Cell.rgb * ConeIncidentLight(v3Sample);
#endif

		fOcclusion += fTransmittance * v4Cell.a * AO_CONE_STRENGTH;

		if (fOcclusion > 0.99)
			break;

		fDistance *= AO_CONE_GROWTH;
	}

	return float4(v3Radiance, fOcclusion);
}

/* Environment specular, from one narrow cone along the reflected view ray -
   RENDERING_PLAN.md 7.4. Returns radiance to *add* to the shaded surface, not
   an irradiance the albedo multiplies: a reflection is light bouncing off the
   surface rather than into it.

   Narrow is the whole difference from the diffuse cones, and it is also the
   catch that killed the sun cone in 7.1's premise check: a narrow cone resolves
   to the finest pyramid level for most of its length, so it is close to a ray
   with extra steps. It is affordable here only because there is exactly one of
   it and its reach is short - SPEC_CONE_STEPS, not AO_CONE_STEPS.

   Schlick against a dielectric F0, so it is a rim effect that grows as a face
   turns edge-on and all but vanishes head-on. On voxel art that reads as the
   sheen along a wall seen at a glancing angle, which is the shape the faceting
   wants; a constant gain reads as the whole surface being wet. */
float3 GetConeSpecular(float3 v3Position, float3 v3Normal, float3 v3View)
{
#if SPEC_CONE_ENABLED
	float fNdotV = saturate(dot(v3Normal, -v3View));
	float fFresnel = SPEC_F0 + (1.0 - SPEC_F0) * pow(1.0 - fNdotV, 5.0);

	if (fFresnel < SPEC_CUTOFF)
		return 0.0;

	float3 v3InvWorld = 1.0 / float3(worldSize.xyz);
	float3 v3Reflected = reflect(v3View, v3Normal);

	/* Off the surface along the *normal*, as the diffuse cones are: pushing
	   along the reflected ray instead leaves a grazing reflection starting
	   inside the wall it is reflecting off. */
	float3 v3Origin = v3Position + v3Normal * AO_SURFACE_OFFSET;

	float3 v3Radiance = 0.0;
	float fOcclusion = 0.0;
	float fDistance = AO_CONE_START;

	for (uint i = 0; i < SPEC_CONE_STEPS; i++)
	{
		float3 v3Sample = v3Origin + v3Reflected * fDistance;

		float fLevel = PyramidLevelForRadius(fDistance * SPEC_CONE_APERTURE);
		float4 v4Cell = PyramidSample(v3Sample, v3InvWorld, fLevel);

		float fTransmittance = 1.0 - fOcclusion;

		if (v4Cell.a > 0.0)
			v3Radiance += (fTransmittance * AO_CONE_STRENGTH) * v4Cell.rgb * ConeIncidentLight(v3Sample);

		fOcclusion += fTransmittance * v4Cell.a * AO_CONE_STRENGTH;

		if (fOcclusion > 0.99)
			break;

		fDistance *= AO_CONE_GROWTH;
	}

	/* Whatever the cone did not run into is sky, and a reflection of the sky is
	   still a reflection - without this a wall against open ground has no
	   specular at all, which is the case a rim effect most wants one. */
	v3Radiance += (1.0 - fOcclusion) * SrgbToLinear(SKY_COLOR.rgb) * SKY_LIGHT_COLOR;

	return v3Radiance * (fFresnel * SPEC_STRENGTH);
#else
	return 0.0;
#endif
}

/* How open this point is to the sky, 0..1 - the same contract as the
   GetSkyVisibility it supplements - and, out of the same five cones, the
   diffuse irradiance bouncing onto it. The caller multiplies that by the
   receiver's own albedo, which is why it is returned as irradiance rather than
   as a colour: it is the light *arriving*, not the light leaving.

   Two terms, one trace. The plan budgeted these separately and put the bounce
   cones at quarter resolution to afford them; sharing the cones made that
   unnecessary - see GI_BOUNCE_ENABLED. */
float GetConeAmbient(float3 v3Position, float3 v3Normal, float2 v2ScreenPosition, out float3 v3Bounce)
{
	/* Off the surface along its normal, far enough to leave the finest cell it
	   is itself part of - otherwise every point in the world reads as half
	   enclosed by itself. One cell, not one brick: that is the whole
	   difference the pyramid makes to where a cone can start. */
	float3 v3Origin = v3Position + v3Normal * AO_SURFACE_OFFSET;

	/* Hoisted out of the cones: the reciprocal is the same for all thirty-five
	   samples, and a per-sample divide by worldSize is the one piece of
	   addressing the texture did not already delete. */
	float3 v3InvWorld = 1.0 / float3(worldSize.xyz);

	float3 v3Tangent = normalize(GetOrthogonal(v3Normal));
	float3 v3Bitangent = cross(v3Normal, v3Tangent);

	/* One cone straight up the normal, then a ring tilted away from it. The
	   ring is rotated per pixel so that what would otherwise be cell-shaped
	   steps in the result break up as dither instead. */
	float fRotation = frac(52.9829189 * frac(dot(v2ScreenPosition, float2(0.06711056, 0.00583715)))) * 6.2831853;

	float4 v4Sum = AmbientConeTrace(v3Origin, v3Normal, v3InvWorld);
	float fWeight = 1.0;

	for (uint i = 0; i < AO_RING_CONES; i++)
	{
		float fAngle = fRotation + float(i) * (6.2831853 / float(AO_RING_CONES));

		float3 v3Direction = normalize(
			v3Normal * AO_RING_COSINE +
			(v3Tangent * cos(fAngle) + v3Bitangent * sin(fAngle)) * AO_RING_SINE);

		/* Cosine weighted, because that is what irradiance on a surface is -
		   a cone leaning away from the normal contributes less of the sky. The
		   bounce wants the identical weighting for the identical reason, which
		   is the other half of why the two share a trace. */
		float fCosine = AO_RING_COSINE;

		v4Sum += AmbientConeTrace(v3Origin, v3Direction, v3InvWorld) * fCosine;
		fWeight += fCosine;
	}

	v3Bounce = v4Sum.rgb * (GI_STRENGTH / fWeight);

	return saturate(1.0 - v4Sum.a / fWeight);
}

#endif
