#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "Core/ECS/Systems/Rendering/VoxelStamp.h"

/* A MagicaVoxel `.vox` read without the engine's resource stack.
 *
 * `VoxModel::Load` needs a FileSystem, a ResourceManager and a render context,
 * so for as long as it was the only reader, no headless test could place a real
 * model anywhere. Every scenario therefore described boxes on flat ground - and
 * a bamboo pole sunk nine layers into the world, which is three 1-voxel stalks
 * joined only at a base layer that the sinking clipped away, is not remotely a
 * box.
 *
 * This reproduces exactly what `VoxModel::Read` produces and nothing else: the
 * axis swap (MagicaVoxel is z-up, the engine is y-up), the x/z flip, the fit to
 * the occupied bounding box, and the packing of a position into one uint32 the
 * way `ForEachStampedVoxel` unpacks it. It deliberately does *not* reimplement
 * placement - that is `ComputeVoxelStampTransform`, which the tests now call
 * directly.
 */
class VoxModelFile
{
public:
	bool Load(const std::string& path);

	/* Path relative to the game's content root, which CMake hands the tests as
	   VOXAGINE_TEST_CONTENT_DIR - so a scenario names a model the same way a
	   .wld does. */
	bool LoadFromContent(const std::string& relativePath);

	const std::string& Error() const { return m_Error; }

	VoxelStampModel Describe() const;
	VoxelStampVoxels Voxels() const;

	const Vector3& FittedSize() const { return m_v3FittedSize; }
	uint32_t Count() const { return static_cast<uint32_t>(m_Positions.size()); }

private:
	/* VoxModel::SortFrameVoxels' order, which the stamp's duplicate suppression
	   depends on - see the definition. */
	void SortVoxels();

	std::vector<uint32_t> m_Positions;
	std::vector<uint32_t> m_Colors;

	Vector3 m_v3FittedSize = Vector3(0.f);
	Vector3 m_v3FitSizeOffset = Vector3(0.f);

	uint32_t m_uiFrameCount = 1;

	std::string m_Error;
};
