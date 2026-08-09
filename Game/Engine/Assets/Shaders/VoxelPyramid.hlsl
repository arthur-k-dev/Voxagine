#ifndef VOXAGINE_VOXELPYRAMID_HLSL
#define VOXAGINE_VOXELPYRAMID_HLSL

/* Coverage pyramid addressing - RENDERING_PLAN.md 7.1b.
 *
 * PYRAMID_LEVELS counts of occupied voxels over the same window, each level's
 * cell twice the edge of the one below it, level 0 at 1 << PYRAMID_FINE_SHIFT.
 * They live one after another in a single buffer - the brick buffer phase 2
 * already binds - so none of this needs a new descriptor or a change to the
 * SPIR-V/C++ contract.
 *
 * The offsets are *derived* on both sides rather than passed across, by the
 * same ceil-div, so there is no third place they can disagree. C++ is
 * VoxelBrickGrid::Resize and VoxelBrickGrid::GetLevelOffset; the failure this
 * shape avoids is not a crash but a cone quietly reading another level's data.
 *
 * Everything here takes the grid dimensions as a parameter because two grids
 * use it: the resident window (worldSize) and the far field (farFieldSize).
 */

inline uint3 PyramidLevelGridSize(uint3 v3GridSize, uint uiLevel)
{
	uint uiShift = PYRAMID_FINE_SHIFT + uiLevel;

	return (v3GridSize + ((1u << uiShift) - 1u)) >> uiShift;
}

/* First element of a level. Level 0 starts at zero and each one follows the
   last, so this is the running total of everything finer.

   The loop runs the full depth and masks, rather than stopping at uiLevel,
   because uiLevel varies per sample while the grid sizes do not: written this
   way every term is loop-invariant and the compiler hoists all five out of the
   cone, leaving five selects. Stopping early makes the trip count vary with
   the sample and the whole computation lands inside the inner loop - measured
   at 0.4 ms of the voxel pass at 4K. */
inline uint PyramidLevelOffset(uint3 v3GridSize, uint uiLevel)
{
	uint uiOffset = 0;

	[unroll]
	for (uint i = 0; i < PYRAMID_LEVELS; i++)
	{
		uint3 v3Level = PyramidLevelGridSize(v3GridSize, i);
		uiOffset += (i < uiLevel) ? (v3Level.x * v3Level.y * v3Level.z) : 0;
	}

	return uiOffset;
}

/* Linearization is the voxel convention (rule 4) scaled down:
   x + y*L.x + z*L.x*L.y, plus the level's base. */
inline uint PyramidCellID(uint3 v3GridSize, uint uiLevel, int3 v3Cell)
{
	uint3 v3Level = PyramidLevelGridSize(v3GridSize, uiLevel);

	return PyramidLevelOffset(v3GridSize, uiLevel)
		+ uint(v3Cell.x)
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
