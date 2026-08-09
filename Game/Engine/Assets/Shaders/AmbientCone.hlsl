#ifndef VOXAGINE_AMBIENTCONE_HLSL
#define VOXAGINE_AMBIENTCONE_HLSL

/* Cone-traced ambient occlusion - RENDERING_PLAN.md 7.1b.
 *
 * Replaces GetSkyVisibility, which asks twelve immediately-adjacent voxels
 * whether they are solid. That only ever sees one voxel out, so it darkens the
 * inside corner of a step and knows nothing about a wall two metres away. The
 * result reads as an edge effect rather than as a place being enclosed, and
 * enclosure is most of what makes a voxel render look lit by a sky.
 *
 * This marches a few cones over the hemisphere against the *brick* counts -
 * voxelBrickData, one occupied-voxel count per BRICK_SIZE^3 block. Two reasons
 * that is the right resolution here where it was the wrong one for shadows
 * (phase 6.2 rejected brick-density shadows as blotchy and shapeless):
 *
 *   - An AO cone is wide. At AO_CONE_APERTURE the footprint passes a brick's
 *     width within a few voxels of the surface, so the cone is sampling data
 *     coarser than itself almost immediately - which is exactly the condition
 *     the sun cone never met, and exactly when a cone beats a ray.
 *   - Ambient occlusion is a low-frequency term by nature. A shadow carries the
 *     silhouette of the thing that cast it and blockiness destroys that; AO
 *     carries no shape of its own, so the same coarseness is invisible.
 *
 * If it *is* visibly blocky, that is the signal to build the mip pyramid with
 * trilinear filtering that 7.1b was originally scoped as. Checking first,
 * because a cone trace against a structure that already exists costs nothing to
 * try and the pyramid is a week of GPU plumbing.
 */

/* Occupied fraction of the brick containing a world-space point, 0..1.
   Out of the window reads as empty: past the resident window there is only the
   endless ground plane, and an AO cone that escapes the window has escaped into
   open sky by any reasonable reading. */
float AmbientBrickDensity(float3 v3Position)
{
	int3 v3Brick = int3(floor(v3Position)) >> BRICK_SHIFT;

	if (!IsBrickInWorld(v3Brick))
		return 0.0;

	return float(voxelBrickData[PosToBrickID(v3Brick)]) * AO_INV_BRICK_VOLUME;
}

/* Occlusion accumulated along one cone, 0 open to 1 fully blocked.

   Steps grow geometrically. That is the whole economy of a cone trace: the
   footprint grows linearly with distance, so a fixed step count covers an
   exponentially growing range without ever undersampling relative to the
   cone's own width. A linear march would spend all its samples in the first
   few voxels and never reach the wall that actually encloses the point. */
float AmbientConeOcclusion(float3 v3Origin, float3 v3Direction)
{
	float fOcclusion = 0.0;
	float fDistance = AO_CONE_START;

	for (uint i = 0; i < AO_CONE_STEPS; i++)
	{
		float fDensity = AmbientBrickDensity(v3Origin + v3Direction * fDistance);

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
   GetSkyVisibility it replaces, so Lighting.hlsl is unchanged. */
float GetConeSkyVisibility(float3 v3Position, float3 v3Normal, float2 v2ScreenPosition)
{
	/* Start a voxel off the surface, or the first sample sits inside the
	   surface's own brick and every point in the world reads as half enclosed
	   by itself. */
	float3 v3Origin = v3Position + v3Normal * AO_SURFACE_OFFSET;

	float3 v3Tangent = normalize(GetOrthogonal(v3Normal));
	float3 v3Bitangent = cross(v3Normal, v3Tangent);

	/* One cone straight up the normal, then a ring tilted away from it. The
	   ring is rotated per pixel so that what would otherwise be brick-shaped
	   steps in the result break up as dither instead. */
	float fRotation = frac(52.9829189 * frac(dot(v2ScreenPosition, float2(0.06711056, 0.00583715)))) * 6.2831853;

	float fOcclusion = AmbientConeOcclusion(v3Origin, v3Normal);
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

		fOcclusion += AmbientConeOcclusion(v3Origin, v3Direction) * fCosine;
		fWeight += fCosine;
	}

	return saturate(1.0 - fOcclusion / fWeight);
}

#endif
