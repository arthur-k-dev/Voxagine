#pragma once

#include <algorithm>
#include <cstdint>

/* Which part of the model mesh store to copy to the GPU this time.
 *
 * Split out of RenderContext::SyncModelMeshStore and made pure, because the
 * whole of the defect it exists to prevent was arithmetic - no Vulkan call was
 * wrong, only the choice of *range*. That choice is now decidable without a
 * device, and Tests/Rendering/ModelMeshUploadChecks.cpp decides it against a
 * simulated allocator a few hundred thousand times.
 *
 * The rule that matters: a reallocation does not carry the old contents over
 * (Mapper::Resize drops both VkBuffers and creates new ones), so the upload has
 * to start from zero whenever one happens. Appending only the new range into a
 * fresh allocation leaves every previously meshed quad as uninitialised memory,
 * and since the uploaded count still claims they are present, nothing ever puts
 * them back - every model meshed before the growth silently stops being drawn,
 * for the rest of the session.
 */
namespace ModelMeshUpload
{
	/* VoxelMesher.h packs a quad into three uint32_t words, and the mapper's
	   element unit is one word - the same granularity every other Mapper here
	   uses. */
	static constexpr uint32_t k_uiWordsPerQuad = 3;

	/* Grown geometrically rather than to the exact length, because
	   Mapper::Resize destroys an allocation earlier submissions may still be
	   reading from: spare capacity means most newly meshed animation frames
	   need no reallocation at all. 2 M words is about 700 k quads, which is
	   several times what a level reaches. */
	static constexpr uint32_t k_uiMinCapacityWords = 1u << 21;

	struct Plan
	{
		bool bReallocate = false;

		uint32_t uiNewCapacityWords = 0;

		/* The half-open word range to copy. Empty when there is nothing to do. */
		uint32_t uiFirstWord = 0;
		uint32_t uiWordCount = 0;
	};

	inline Plan PlanUpload(uint32_t uiStoreQuads, uint32_t uiUploadedQuads, uint32_t uiCapacityWords)
	{
		Plan plan;
		plan.uiNewCapacityWords = uiCapacityWords;

		if (uiStoreQuads == uiUploadedQuads)
			return plan;

		const uint32_t uiRequiredWords = uiStoreQuads * k_uiWordsPerQuad;

		if (uiRequiredWords > uiCapacityWords)
		{
			uint32_t uiNewCapacity = std::max(k_uiMinCapacityWords, uiCapacityWords);

			while (uiNewCapacity < uiRequiredWords)
				uiNewCapacity *= 2u;

			plan.bReallocate = true;
			plan.uiNewCapacityWords = uiNewCapacity;
		}

		/* The store only ever appends, so this cannot go backwards today - but
		   the subtraction is unsigned, and a shrink would become a copy of
		   several gigabytes rather than a wrong image. Treat it as a full
		   re-upload, which is correct for any store contents whatsoever. */
		const bool bShrank = uiStoreQuads < uiUploadedQuads;

		plan.uiFirstWord = (plan.bReallocate || bShrank)
			? 0u
			: uiUploadedQuads * k_uiWordsPerQuad;
		plan.uiWordCount = uiRequiredWords - plan.uiFirstWord;

		return plan;
	}
}
