#include "pch.h"
#include "ModelMeshStore.h"

#include "Core/Resources/Formats/VoxModel.h"

#include <cstdio>

ModelMeshStore& ModelMeshStore::Get()
{
	static ModelMeshStore instance;
	return instance;
}

VoxelMesher::Result ModelMeshStore::EnsureMeshed(const VoxFrame* pFrame)
{
	if (pFrame == nullptr)
		return {};

	if (pFrame->m_uiMeshFirstQuad != UINT_MAX)
		return { pFrame->m_uiMeshFirstQuad, pFrame->m_uiMeshQuadCount };

	const VoxelMesher::Result result = VoxelMesher::BuildFrameMesh(pFrame, m_Quads);

	pFrame->m_uiMeshFirstQuad = result.m_uiFirstQuad;
	pFrame->m_uiMeshQuadCount = result.m_uiQuadCount;

	m_uiMeshedFrames++;

	/* One line per distinct frame the game ever loads - at most a few hundred
	 * over a whole session (DYNAMIC_MODELS_PLAN.md phase 0: 346 character
	 * frames total), so this is a one-shot log, not per-frame noise. It is
	 * phase 1's acceptance test until a real VOXAGINE_MESH_STORE_AUDIT exists:
	 * a quad count with no crash, for every model in the game, is what "the
	 * mesher runs correctly" means before anything renders it. */
	fprintf(stderr, "[mesh] frame #%u meshed: %u quads (store now %u quads over %u frames)\n",
	        m_uiMeshedFrames, result.m_uiQuadCount,
	        GetTotalQuadCount(), m_uiMeshedFrames);

	return result;
}
