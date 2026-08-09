#ifndef VOXAGINE_VOXELPYRAMID_HLSL
#define VOXAGINE_VOXELPYRAMID_HLSL

/* Coverage pyramid addressing - RENDERING_PLAN.md 7.1b.
 *
 * PYRAMID_LEVELS coverage values over the same window, each level's cell twice
 * the edge of the one below it, level 0 at 1 << PYRAMID_FINE_SHIFT.
 *
 * This is the *buffer* form, and after 7.1b route B the only thing that reads
 * it is the marcher, at PYRAMID_BRICK_LEVEL - it wants one exact count of one
 * cell, not a filtered neighbourhood, and it walks bricks by integer index.
 * That level is therefore the only one in the buffer, at element zero, and
 * these helpers reduce to a linearization. Route A put all five in here one
 * after another and had the offsets derived on both sides; the texture made
 * every level but this one unread, and 76.8 MiB of host-visible mirror with
 * it.
 *
 * Ambient occlusion reads the same pyramid out of the voxelPyramid 3D texture
 * instead, by a normalized coordinate that needs none of this - see
 * AmbientCone.hlsl.
 *
 * Everything here takes the grid dimensions as a parameter because two grids
 * use it: the resident window (worldSize) and the far field (farFieldSize).
 */

inline uint3 PyramidLevelGridSize(uint3 v3GridSize, uint uiLevel)
{
	uint uiShift = PYRAMID_FINE_SHIFT + uiLevel;

	return (v3GridSize + ((1u << uiShift) - 1u)) >> uiShift;
}

/* Linearization is the voxel convention (rule 4) scaled down:
   x + y*L.x + z*L.x*L.y. The buffer holds one level, so there is no base to
   add - C++ is VoxelBrickGrid::BrickID. */
inline uint PyramidCellID(uint3 v3GridSize, uint uiLevel, int3 v3Cell)
{
	uint3 v3Level = PyramidLevelGridSize(v3GridSize, uiLevel);

	return uint(v3Cell.x)
		+ uint(v3Cell.y) * v3Level.x
		+ uint(v3Cell.z) * v3Level.x * v3Level.y;
}

inline bool IsPyramidCellInWorld(uint3 v3GridSize, uint uiLevel, int3 v3Cell)
{
	int3 v3Level = int3(PyramidLevelGridSize(v3GridSize, uiLevel));

	return (
		v3Cell.x >= 0 && v3Cell.y >= 0 && v3Cell.z >= 0 &&
		v3Cell.x < v3Level.x && v3Cell.y < v3Level.y && v3Cell.z < v3Level.z
	);
}

#endif
