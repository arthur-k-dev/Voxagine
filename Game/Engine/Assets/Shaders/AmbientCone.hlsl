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
 * distance, trilinearly filtered, so the near field is read at two voxels and
 * a lattice the geometry knows nothing about stops deciding the result.
 */

/* Occupied fraction of one pyramid cell, 0..1. Out of the window reads as
   empty: past the resident window there is only the endless ground plane, and
   an AO cone that escapes the window has escaped into open sky by any
   reasonable reading. */
float PyramidCellDensity(int3 v3Cell, uint uiLevel, uint uiOffset, uint3 v3Grid, float fInvVolume)
{
	if (any(v3Cell < 0) || any(v3Cell >= int3(v3Grid)))
		return 0.0;

	uint uiID = uiOffset
		+ uint(v3Cell.x)
		+ uint(v3Cell.y) * v3Grid.x
		+ uint(v3Cell.z) * v3Grid.x * v3Grid.y;

	return float(voxelBrickData[uiID]) * fInvVolume;
}

/* Trilinearly filtered density at a level. Eight fetches by hand, which is the
   price of keeping the pyramid in the buffer the bricks already bind rather
   than in a 3D texture with hardware filtering - see 7.1b route A. The filter
   is the point of the level, not a nicety: a point sample of a 2-voxel lattice
   still reports whichever way a block happens to fall in it. */
float PyramidDensity(float3 v3Position, uint uiLevel)
{
	uint uiCellShift = PYRAMID_FINE_SHIFT + uiLevel;
	float fInvCell = 1.0 / float(1u << uiCellShift);

	uint3 v3Grid = PyramidLevelGridSize(worldSize.xyz, uiLevel);
	uint uiOffset = PyramidLevelOffset(worldSize.xyz, uiLevel);

	/* A cell's count is its whole volume when solid, so this is what turns one
	   into a 0..1 density. */
	float fInvVolume = 1.0 / float(1u << (3u * uiCellShift));

	/* Cell centres sit at (i + 0.5) cells, so the half-cell shift is what puts
	   the interpolation between centres rather than between corners. */
	float3 v3Cell = v3Position * fInvCell - 0.5;

#if !AO_CONE_TRILINEAR
	/* One fetch instead of eight, for pricing the filter rather than for
	   shipping: this is what a step would cost if the pyramid lived in a 3D
	   texture and the hardware did the interpolation (7.1b route B). The
	   image is the lattice artefact the pyramid exists to remove, so this is a
	   measurement switch, not a quality setting. */
	return PyramidCellDensity(int3(round(v3Cell)), uiLevel, uiOffset, v3Grid, fInvVolume);
#endif

	float3 v3Base = floor(v3Cell);
	float3 v3Frac = v3Cell - v3Base;

	int3 v3First = int3(v3Base);

	float fY0Z0 = lerp(
		PyramidCellDensity(v3First + int3(0, 0, 0), uiLevel, uiOffset, v3Grid, fInvVolume),
		PyramidCellDensity(v3First + int3(1, 0, 0), uiLevel, uiOffset, v3Grid, fInvVolume), v3Frac.x);

	float fY1Z0 = lerp(
		PyramidCellDensity(v3First + int3(0, 1, 0), uiLevel, uiOffset, v3Grid, fInvVolume),
		PyramidCellDensity(v3First + int3(1, 1, 0), uiLevel, uiOffset, v3Grid, fInvVolume), v3Frac.x);

	float fY0Z1 = lerp(
		PyramidCellDensity(v3First + int3(0, 0, 1), uiLevel, uiOffset, v3Grid, fInvVolume),
		PyramidCellDensity(v3First + int3(1, 0, 1), uiLevel, uiOffset, v3Grid, fInvVolume), v3Frac.x);

	float fY1Z1 = lerp(
		PyramidCellDensity(v3First + int3(0, 1, 1), uiLevel, uiOffset, v3Grid, fInvVolume),
		PyramidCellDensity(v3First + int3(1, 1, 1), uiLevel, uiOffset, v3Grid, fInvVolume), v3Frac.x);

	return lerp(lerp(fY0Z0, fY1Z0, v3Frac.y), lerp(fY0Z1, fY1Z1, v3Frac.y), v3Frac.z);
}

/* The level whose cell is about as wide as the cone is at this distance. A
   cone sampling data finer than itself is a ray with extra steps; one sampling
   data coarser than itself is what the brick-only version was, and what put
   the wall's own bricks in front of it. */
uint PyramidLevelForRadius(float fRadius)
{
	float fLevel = log2(max(fRadius * 2.0, 1.0) / float(1u << PYRAMID_FINE_SHIFT));

	return uint(clamp(round(fLevel), 0.0, float(PYRAMID_LEVELS - 1)));
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
		uint uiLevel = PyramidLevelForRadius(fDistance * AO_CONE_APERTURE);
		float fDensity = PyramidDensity(v3Origin + v3Direction * fDistance, uiLevel);

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

	float3 v3Tangent = normalize(GetOrthogonal(v3Normal));
	float3 v3Bitangent = cross(v3Normal, v3Tangent);

	/* One cone straight up the normal, then a ring tilted away from it. The
	   ring is rotated per pixel so that what would otherwise be cell-shaped
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
