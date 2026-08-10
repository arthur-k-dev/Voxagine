#include "pch.h"
#include "VoxelMesher.h"

#include "Core/Resources/Formats/VoxModel.h"

namespace
{
	/* Dense per-frame occupancy: 0 means empty, else the voxel's raw palette
	 * colour (see VoxelMesher.h on why that is not a tag word). Small enough
	 * to build per call - the largest model in the game is 40x40x60 = 96,000
	 * cells, 384 KB as uint32_t, a single load-time allocation and never
	 * resident afterwards. */
	struct OccupancyGrid
	{
		std::vector<uint32_t> m_Cells;
		int m_Dim[3] = { 0, 0, 0 };

		int Index(int x, int y, int z) const
		{
			return x + y * m_Dim[0] + z * m_Dim[0] * m_Dim[1];
		}

		bool InBounds(int x, int y, int z) const
		{
			return x >= 0 && y >= 0 && z >= 0 && x < m_Dim[0] && y < m_Dim[1] && z < m_Dim[2];
		}

		uint32_t At(int x, int y, int z) const
		{
			return InBounds(x, y, z) ? m_Cells[Index(x, y, z)] : 0u;
		}

		/* Model-local occupancy for AO: out of bounds is simply empty. Unlike
		 * AmbientOcclusion.hlsl's IsVoxel, there is no ground-plane special
		 * case here - that is a world-marcher concern (the endless ground
		 * plane), and a standalone model volume has nothing outside it. */
		bool IsSolid(int x, int y, int z) const { return At(x, y, z) != 0u; }
	};

	/* Port of AmbientOcclusion.hlsl's GetVertexAO, kept in the same shape so
	 * it can be checked line for line against the shader. Booleans in,
	 * {0,1,2,3} out - see VoxelMesher.h on why that range is exact rather
	 * than a quantization: sideA && sideB always yields 3 (the shader's
	 * early-return branch, "ao = 1.0" i.e. maximally occluded before the
	 * final 1-ao inversion the shader applies); otherwise it is
	 * sideA + sideB + corner, and sideA/sideB cannot both be true in that
	 * branch, so the sum only ever reaches 0, 1 or 2. */
	int GetVertexAOLevel(bool bSideA, bool bSideB, bool bCorner)
	{
		if (bSideA && bSideB)
			return 3;

		return static_cast<int>(bSideA) + static_cast<int>(bSideB) + static_cast<int>(bCorner);
	}

	/* One exposed voxel face's four corner AO levels, in (axis, u, v) space
	 * with u = (axis+1)%3, v = (axis+2)%3 - the same convention MeshSlice
	 * uses for the mask it builds. `pos` is the empty cell one step outside
	 * the face along its own normal, exactly AmbientOcclusion.hlsl's
	 * `position - mask * srDirection` (== position + normal, since
	 * normal = -mask*srDirection there): GetVoxelAO tests only that outside
	 * layer, never the coplanar neighbour that would be part of the same
	 * merged run, which is what makes the merge-key equality check below
	 * exact rather than approximate - see VoxelMesher.h. */
	struct CornerAO
	{
		/* Indexed (u,v) in {0,1}: 0 = minus, 1 = plus - so mm/pm/mp/pp reads
		 * as (u,v) sign pairs, matching the packing comment in
		 * VoxelMesher.h. */
		int m_Levels[2][2] = {};
	};

	CornerAO ComputeFaceAO(const OccupancyGrid& grid, const int (&pos)[3], const int (&d1)[3], const int (&d2)[3])
	{
		auto solidAt = [&](int dx1, int dx2)
		{
			return grid.IsSolid(
				pos[0] + d1[0] * dx1 + d2[0] * dx2,
				pos[1] + d1[1] * dx1 + d2[1] * dx2,
				pos[2] + d1[2] * dx1 + d2[2] * dx2);
		};

		const bool bUPlus = solidAt(1, 0);
		const bool bUMinus = solidAt(-1, 0);
		const bool bVPlus = solidAt(0, 1);
		const bool bVMinus = solidAt(0, -1);

		const bool bCornerPP = solidAt(1, 1);
		const bool bCornerMP = solidAt(-1, 1);
		const bool bCornerMM = solidAt(-1, -1);
		const bool bCornerPM = solidAt(1, -1);

		CornerAO ao;
		ao.m_Levels[0][0] = GetVertexAOLevel(bUMinus, bVMinus, bCornerMM); // (-u,-v)
		ao.m_Levels[1][0] = GetVertexAOLevel(bVMinus, bUPlus, bCornerPM);  // (+u,-v)
		ao.m_Levels[0][1] = GetVertexAOLevel(bVPlus, bUMinus, bCornerMP);  // (-u,+v)
		ao.m_Levels[1][1] = GetVertexAOLevel(bUPlus, bVPlus, bCornerPP);   // (+u,+v)
		return ao;
	}

	bool SameAO(const CornerAO& a, const CornerAO& b)
	{
		return a.m_Levels[0][0] == b.m_Levels[0][0] && a.m_Levels[1][0] == b.m_Levels[1][0] &&
		       a.m_Levels[0][1] == b.m_Levels[0][1] && a.m_Levels[1][1] == b.m_Levels[1][1];
	}

	/* One exposed cell of the 2D mask MeshSlice builds. Two cells merge only
	 * if both the colour and the full four-corner AO signature agree - see
	 * VoxelMesher.h and ComputeFaceAO's comment on why that condition is
	 * exact rather than a heuristic cutoff. */
	struct MaskCell
	{
		uint32_t m_uiColour = 0u;
		CornerAO m_AO;
		bool m_bExposed = false;
	};

	bool SameMaskKey(const MaskCell& a, const MaskCell& b)
	{
		return a.m_uiColour == b.m_uiColour && SameAO(a.m_AO, b.m_AO);
	}

	/* Greedy merge of one axis-aligned slice's exposed faces. Standard "grow
	 * width, then grow height" sweep over a 2D mask, same shape as the
	 * reference greedy voxel mesher, extended to key on AO as well as
	 * colour. */
	void MeshSlice(
		const OccupancyGrid& grid, int axis, int sign, int w,
		int dimU, int dimV, std::vector<uint32_t>& outQuads)
	{
		const int u = (axis + 1) % 3;
		const int v = (axis + 2) % 3;

		int d1[3] = { 0, 0, 0 };
		int d2[3] = { 0, 0, 0 };
		d1[u] = 1;
		d2[v] = 1;

		std::vector<MaskCell> mask(static_cast<size_t>(dimU) * dimV);

		int pos[3];
		int neighbour[3];
		int aoPos[3];

		for (int a = 0; a < dimU; a++)
		{
			for (int b = 0; b < dimV; b++)
			{
				pos[axis] = w;
				pos[u] = a;
				pos[v] = b;

				const uint32_t colour = grid.At(pos[0], pos[1], pos[2]);
				if (colour == 0u)
					continue;

				neighbour[axis] = w + sign;
				neighbour[u] = a;
				neighbour[v] = b;

				/* Exposed iff the neighbour one step along the face normal is
				 * empty - including "outside the model", which InBounds/At
				 * already answers as empty. */
				if (grid.At(neighbour[0], neighbour[1], neighbour[2]) != 0u)
					continue;

				aoPos[0] = neighbour[0];
				aoPos[1] = neighbour[1];
				aoPos[2] = neighbour[2];

				MaskCell& cell = mask[static_cast<size_t>(a) * dimV + b];
				cell.m_uiColour = colour;
				cell.m_AO = ComputeFaceAO(grid, aoPos, d1, d2);
				cell.m_bExposed = true;
			}
		}

		std::vector<uint8_t> consumed(static_cast<size_t>(dimU) * dimV, 0u);

		for (int a = 0; a < dimU; a++)
		{
			for (int b = 0; b < dimV; b++)
			{
				const size_t startIdx = static_cast<size_t>(a) * dimV + b;

				if (consumed[startIdx] || !mask[startIdx].m_bExposed)
					continue;

				const MaskCell& key = mask[startIdx];

				int extentU = 1;
				while (a + extentU < dimU)
				{
					const size_t idx = static_cast<size_t>(a + extentU) * dimV + b;
					if (consumed[idx] || !mask[idx].m_bExposed || !SameMaskKey(mask[idx], key))
						break;
					extentU++;
				}

				int extentV = 1;
				bool bCanGrow = true;
				while (bCanGrow && b + extentV < dimV)
				{
					for (int k = 0; k < extentU; k++)
					{
						const size_t idx = static_cast<size_t>(a + k) * dimV + (b + extentV);
						if (consumed[idx] || !mask[idx].m_bExposed || !SameMaskKey(mask[idx], key))
						{
							bCanGrow = false;
							break;
						}
					}

					if (bCanGrow)
						extentV++;
				}

				for (int k = 0; k < extentU; k++)
					for (int l = 0; l < extentV; l++)
						consumed[static_cast<size_t>(a + k) * dimV + (b + l)] = 1u;

				pos[axis] = w;
				pos[u] = a;
				pos[v] = b;

				const uint32_t word0 =
					(static_cast<uint32_t>(pos[0]) & 0xFFu) |
					((static_cast<uint32_t>(pos[1]) & 0xFFu) << 8) |
					((static_cast<uint32_t>(pos[2]) & 0xFFu) << 16) |
					((static_cast<uint32_t>(axis) & 0x3u) << 24) |
					((sign > 0 ? 1u : 0u) << 26);

				/* Every cell absorbed by this run shares one AO signature
				 * (SameMaskKey required it), so the run's own is the merged
				 * quad's true outer-corner value - see ComputeFaceAO. */
				const uint32_t word1 =
					(static_cast<uint32_t>(extentU) & 0xFFu) |
					((static_cast<uint32_t>(extentV) & 0xFFu) << 8) |
					((static_cast<uint32_t>(key.m_AO.m_Levels[0][0]) & 0x3u) << 16) |
					((static_cast<uint32_t>(key.m_AO.m_Levels[1][0]) & 0x3u) << 18) |
					((static_cast<uint32_t>(key.m_AO.m_Levels[0][1]) & 0x3u) << 20) |
					((static_cast<uint32_t>(key.m_AO.m_Levels[1][1]) & 0x3u) << 22);

				outQuads.push_back(word0);
				outQuads.push_back(word1);
				outQuads.push_back(key.m_uiColour);
			}
		}
	}
}

namespace VoxelMesher
{
	Result BuildFrameMesh(const VoxFrame* pFrame, std::vector<uint32_t>& pOutQuads)
	{
		Result result;
		result.m_uiFirstQuad = static_cast<uint32_t>(pOutQuads.size() / 3);

		if (pFrame == nullptr)
			return result;

		const Vector3& v3Fitted = pFrame->GetFittedSize();

		OccupancyGrid grid;
		grid.m_Dim[0] = static_cast<int>(v3Fitted.x + 0.5f);
		grid.m_Dim[1] = static_cast<int>(v3Fitted.y + 0.5f);
		grid.m_Dim[2] = static_cast<int>(v3Fitted.z + 0.5f);

		/* Origin/extent are packed into 8 bits each (VoxelMesher.h). Every
		 * character model in the game fits well inside that (largest is
		 * 40x40x60 - Docs/DYNAMIC_MODELS_PLAN.md phase 0); a frame that does
		 * not is skipped rather than silently wrapping its coordinates. */
		if (grid.m_Dim[0] <= 0 || grid.m_Dim[1] <= 0 || grid.m_Dim[2] <= 0 ||
		    grid.m_Dim[0] > 255 || grid.m_Dim[1] > 255 || grid.m_Dim[2] > 255)
		{
			return result;
		}

		grid.m_Cells.assign(
			static_cast<size_t>(grid.m_Dim[0]) * grid.m_Dim[1] * grid.m_Dim[2], 0u);

		const uint32_t* pPositions = pFrame->GetPositions();
		const uint32_t* pColors = pFrame->GetColors();
		const uint32_t uiSolidCount = pFrame->GetSolidVoxelCount();

		for (uint32_t i = 0; i < uiSolidCount; i++)
		{
			/* Packed (x, y, z, 0) the same way VoxModel::Read packs it -
			 * Core/Math.h's VColor puts the first component in the low byte. */
			const uint32_t packedPos = pPositions[i];
			const int x = static_cast<int>(packedPos & 0xFFu);
			const int y = static_cast<int>((packedPos >> 8) & 0xFFu);
			const int z = static_cast<int>((packedPos >> 16) & 0xFFu);

			if (!grid.InBounds(x, y, z))
				continue;

			const uint32_t colour = pColors[i];

			/* A colour word of exactly 0 would read back as "empty" - the
			 * grid's own sentinel - so it is nudged to a value the alpha byte
			 * still reports as opaque. Loading the palette already rejects
			 * alpha == 0 (VoxModel.cpp:565), so this can only touch an RGB of
			 * pure black, and moving it by one blue bit is invisible. */
			grid.m_Cells[grid.Index(x, y, z)] = (colour != 0u) ? colour : 0x01000000u;
		}

		for (int axis = 0; axis < 3; axis++)
		{
			const int dimAxis = grid.m_Dim[axis];
			const int dimU = grid.m_Dim[(axis + 1) % 3];
			const int dimV = grid.m_Dim[(axis + 2) % 3];

			for (int sign = -1; sign <= 1; sign += 2)
				for (int w = 0; w < dimAxis; w++)
					MeshSlice(grid, axis, sign, w, dimU, dimV, pOutQuads);
		}

		result.m_uiQuadCount = static_cast<uint32_t>(pOutQuads.size() / 3) - result.m_uiFirstQuad;
		return result;
	}
}
