#include "Harness/VoxModelFile.h"

#include <algorithm>
#include <cstring>
#include <fstream>

#ifndef VOXAGINE_TEST_CONTENT_DIR
#define VOXAGINE_TEST_CONTENT_DIR "."
#endif

namespace
{
	int32_t ReadInt(const std::vector<char>& data, size_t uiOffset)
	{
		int32_t iValue = 0;
		std::memcpy(&iValue, data.data() + uiOffset, sizeof(int32_t));

		return iValue;
	}

	struct RawVoxel
	{
		uint8_t x = 0, y = 0, z = 0, palette = 0;
	};
}

bool VoxModelFile::LoadFromContent(const std::string& relativePath)
{
	return Load(std::string(VOXAGINE_TEST_CONTENT_DIR) + "/" + relativePath);
}

bool VoxModelFile::Load(const std::string& path)
{
	m_Positions.clear();
	m_Colors.clear();
	m_Error.clear();

	std::ifstream file(path, std::ios::binary);

	if (!file.is_open())
	{
		m_Error = "cannot open '" + path + "'";
		return false;
	}

	const std::vector<char> data((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

	if (data.size() < 8 || std::strncmp(data.data(), "VOX ", 4) != 0)
	{
		m_Error = "'" + path + "' is not a .vox";
		return false;
	}

	std::vector<std::vector<RawVoxel>> frames;
	std::vector<uint32_t> palette;

	size_t uiOffset = 8;

	while (uiOffset + 12 <= data.size())
	{
		char id[5] = {};
		std::memcpy(id, data.data() + uiOffset, 4);

		const int32_t iContent = ReadInt(data, uiOffset + 4);
		uiOffset += 12;

		if (std::strncmp(id, "XYZI", 4) == 0)
		{
			const int32_t iCount = ReadInt(data, uiOffset);

			std::vector<RawVoxel> frame;
			frame.reserve(static_cast<size_t>(iCount));

			for (int32_t i = 0; i < iCount; ++i)
			{
				const size_t uiVoxel = uiOffset + 4 + static_cast<size_t>(i) * 4;

				RawVoxel voxel;
				voxel.x = static_cast<uint8_t>(data[uiVoxel + 0]);
				voxel.y = static_cast<uint8_t>(data[uiVoxel + 1]);
				voxel.z = static_cast<uint8_t>(data[uiVoxel + 2]);
				voxel.palette = static_cast<uint8_t>(data[uiVoxel + 3]);

				frame.push_back(voxel);
			}

			frames.push_back(std::move(frame));
		}
		else if (std::strncmp(id, "RGBA", 4) == 0)
		{
			palette.resize(256);

			for (uint32_t i = 0; i < 256; ++i)
				std::memcpy(&palette[i], data.data() + uiOffset + i * 4, sizeof(uint32_t));
		}

		if (std::strncmp(id, "MAIN", 4) != 0)
			uiOffset += static_cast<size_t>(iContent);
	}

	if (frames.empty())
	{
		m_Error = "'" + path + "' has no voxels";
		return false;
	}

	m_uiFrameCount = static_cast<uint32_t>(frames.size());

	const std::vector<RawVoxel>& frame = frames.front();

	/* The fit, exactly as VoxModel::Read computes it: the engine's y is
	   MagicaVoxel's z, and the engine's z is MagicaVoxel's y. */
	uint8_t uiMinX = 255, uiMinY = 255, uiMinZ = 255;
	uint8_t uiMaxX = 0, uiMaxY = 0, uiMaxZ = 0;

	for (const RawVoxel& voxel : frame)
	{
		uiMinX = std::min(uiMinX, voxel.x);
		uiMinY = std::min(uiMinY, voxel.z);
		uiMinZ = std::min(uiMinZ, voxel.y);

		uiMaxX = std::max(uiMaxX, voxel.x);
		uiMaxY = std::max(uiMaxY, voxel.z);
		uiMaxZ = std::max(uiMaxZ, voxel.y);
	}

	m_v3FittedSize = Vector3(
		static_cast<float>(uiMaxX - uiMinX) + 1.f,
		static_cast<float>(uiMaxY - uiMinY) + 1.f,
		static_cast<float>(uiMaxZ - uiMinZ) + 1.f);

	m_v3FitSizeOffset = Vector3(
		static_cast<float>(uiMinX), static_cast<float>(uiMinY), static_cast<float>(uiMinZ));

	for (const RawVoxel& voxel : frame)
	{
		const uint32_t uiColor = palette.empty()
			? 0xFF808080u
			: palette[voxel.palette == 0 ? 0 : voxel.palette - 1];

		/* The loader drops anything the palette says is fully transparent, and
		   so must this - occupancy is alpha != 0. */
		if ((uiColor >> 24) == 0)
			continue;

		/* Swap, then flip x and z. Straight out of VoxModel::Read. */
		const float fX = m_v3FittedSize.x - 1.f - (static_cast<float>(voxel.x) - static_cast<float>(uiMinX));
		const float fY = static_cast<float>(voxel.z) - static_cast<float>(uiMinY);
		const float fZ = m_v3FittedSize.z - 1.f - (static_cast<float>(voxel.y) - static_cast<float>(uiMinZ));

		m_Positions.push_back(VColor(
			static_cast<unsigned char>(fX),
			static_cast<unsigned char>(fY),
			static_cast<unsigned char>(fZ),
			static_cast<unsigned char>(0)).inst.Color);

		m_Colors.push_back(uiColor);
	}

	SortVoxels();

	return true;
}

/* VoxModel::SortFrameVoxels, reproduced for the same reason everything else
 * here is: the order the voxels arrive in is not an implementation detail of
 * the loader, it decides which voxels the stamp *emits*.
 *
 * ForEachStampedVoxel skips a voxel whose rounded grid position equals the
 * previous one's, and a sorted model is exactly the input that makes that fire
 * - neighbours are adjacent, so a scaled or rotated stamp lands them on the
 * same cell. A test walking an unsorted model would never exercise the
 * duplicate-suppression state that CHUNK_STREAMING_PLAN.md phase 9 names as its
 * first suspect. */
void VoxModelFile::SortVoxels()
{
	const size_t uiCount = m_Positions.size();

	if (uiCount < 2)
		return;

	std::vector<uint64_t> order(uiCount);

	for (size_t i = 0; i < uiCount; ++i)
	{
		const VColor position(m_Positions[i]);

		const uint64_t uiKey =
			(static_cast<uint64_t>(position.inst.Colors.b) << 16) |
			(static_cast<uint64_t>(position.inst.Colors.g) << 8) |
			static_cast<uint64_t>(position.inst.Colors.r);

		order[i] = (uiKey << 32) | static_cast<uint64_t>(i);
	}

	std::sort(order.begin(), order.end());

	std::vector<uint32_t> positions(uiCount);
	std::vector<uint32_t> colors(uiCount);

	for (size_t i = 0; i < uiCount; ++i)
	{
		const size_t uiSource = static_cast<size_t>(order[i] & 0xFFFFFFFFull);

		positions[i] = m_Positions[uiSource];
		colors[i] = m_Colors[uiSource];
	}

	m_Positions.swap(positions);
	m_Colors.swap(colors);
}

VoxelStampModel VoxModelFile::Describe() const
{
	VoxelStampModel model;

	model.v3FittedSize = m_v3FittedSize;
	model.v3FitSizeOffset = m_v3FitSizeOffset;
	model.v3FirstFittedSize = m_v3FittedSize;
	model.v3FirstFitSizeOffset = m_v3FitSizeOffset;
	model.uiFrameCount = m_uiFrameCount;

	return model;
}

VoxelStampVoxels VoxModelFile::Voxels() const
{
	VoxelStampVoxels voxels;

	voxels.pPositions = m_Positions.data();
	voxels.pColors = m_Colors.data();
	voxels.uiCount = static_cast<uint32_t>(m_Positions.size());

	return voxels;
}
