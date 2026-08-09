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

/* Occupied fraction of the pyramid at a point, 0..1, at a level.
 *
 * The coordinate is normalized over the whole window and so is the same at
 * every level, which is the second thing the texture buys: route A had to
 * recompute a per-level grid size and base offset per sample. Out of the window
 * reads as empty because the sampler's border is transparent black - past the
 * resident window there is only the endless ground plane, and an AO cone that
 * escapes the window has escaped into open sky by any reasonable reading. */
float PyramidDensity(float3 v3Position, float3 v3InvWorld, float fLevel)
{
	return voxelPyramid.SampleLevel(pyramidSampler, v3Position * v3InvWorld, fLevel);
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

/* Occlusion accumulated along one cone, 0 open to 1 fully blocked.

   Steps grow geometrically. That is the whole economy of a cone trace: the
   footprint grows linearly with distance, so a fixed step count covers an
   exponentially growing range without ever undersampling relative to the
   cone's own width. A linear march would spend all its samples in the first
   few voxels and never reach the wall that actually encloses the point. */
float AmbientConeOcclusion(float3 v3Origin, float3 v3Direction, float3 v3InvWorld)
{
	float fOcclusion = 0.0;
	float fDistance = AO_CONE_START;

	for (uint i = 0; i < AO_CONE_STEPS; i++)
	{
		float fLevel = PyramidLevelForRadius(fDistance * AO_CONE_APERTURE);
		float fDensity = PyramidDensity(v3Origin + v3Direction * fDistance, v3InvWorld, fLevel);

		/* Front-to-back: what is already occluded cannot be occluded again, and
		   near geometry matters more than far - which falls out of the order
		   rather than needing a distance weight. */
		fOcclusion += (1.0 - fOcclusion) * fDensity * AO_CONE_STRENGTH;

		if (fOcclusion > 0.99)
			break;

		fDistance *= AO_CONE_GROWTH;
	}

	return fOcclusion;
}

/* How open this point is to the sky, 0..1. Same contract as the
   GetSkyVisibility it supplements, so Lighting.hlsl is unchanged. */
float GetConeSkyVisibility(float3 v3Position, float3 v3Normal, float2 v2ScreenPosition)
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

	float fOcclusion = AmbientConeOcclusion(v3Origin, v3Normal, v3InvWorld);
	float fWeight = 1.0;

	for (uint i = 0; i < AO_RING_CONES; i++)
	{
		float fAngle = fRotation + float(i) * (6.2831853 / float(AO_RING_CONES));

		float3 v3Direction = normalize(
			v3Normal * AO_RING_COSINE +
			(v3Tangent * cos(fAngle) + v3Bitangent * sin(fAngle)) * AO_RING_SINE);

		/* Cosine weighted, because that is what irradiance on a surface is -
		   a cone leaning away from the normal contributes less of the sky. */
		float fCosine = AO_RING_COSINE;

		fOcclusion += AmbientConeOcclusion(v3Origin, v3Direction, v3InvWorld) * fCosine;
		fWeight += fCosine;
	}

	return saturate(1.0 - fOcclusion / fWeight);
}

#endif
