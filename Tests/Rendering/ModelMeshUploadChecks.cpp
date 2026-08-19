#include "Framework/Check.h"

#include "Core/Platform/Rendering/ModelMeshUpload.h"
#include "Core/Utils/DeterministicRandom.h"

#include <cstdint>
#include <vector>

/* The model mesh store is uploaded incrementally, and the failure that costs is
 * not a wrong pixel: it is a model that stops being drawn and never comes back,
 * part-way through a session, reading as content rather than as corruption.
 *
 * It happened. RenderContext::SyncModelMeshStore appended only the newly meshed
 * range, which is right until the buffer has to grow - Mapper::Resize drops both
 * VkBuffers and creates new ones rather than copying, so appending into a fresh
 * allocation left every previously meshed quad as uninitialised memory while the
 * uploaded count still claimed it was present. Every character meshed before
 * that moment vanished.
 *
 * These checks own that invariant. The simulation below is the important one:
 * it models the allocator the way Vulkan actually behaves - a new allocation
 * contains garbage, never the old contents - and asserts after every single
 * upload that the buffer equals the store. That is the assertion the real
 * VOXAGINE_MODEL_MESH_AUDIT makes in-game against the real mapper; this one
 * makes it a few hundred thousand times, in 0.1 s, with no GPU.
 */
namespace
{
	/* Distinctive, and deliberately not zero: a fresh allocation full of zeroes
	   would accidentally match any store word that happens to be zero, which is
	   exactly the sort of luck that lets a broken upload pass. */
	constexpr uint32_t k_uiGarbage = 0xDEADBEEFu;

	/* Everything the real function owns except the Vulkan calls: a store that
	   only grows, a buffer that is replaced (not resized) when capacity runs
	   out, and the uploaded-count bookkeeping. */
	class UploadSimulation
	{
	public:
		void GrowStoreBy(uint32_t uiQuads)
		{
			for (uint32_t i = 0; i < uiQuads; ++i)
			{
				const uint32_t uiQuad = static_cast<uint32_t>(m_Store.size()) /
					ModelMeshUpload::k_uiWordsPerQuad;

				/* Contents that depend on the index, so a word copied from the
				   wrong place is caught as well as one never copied at all. */
				m_Store.push_back(uiQuad * 3u + 1u);
				m_Store.push_back(uiQuad * 3u + 2u);
				m_Store.push_back(uiQuad * 3u + 3u);
			}
		}

		void Sync()
		{
			const uint32_t uiStoreQuads = StoreQuads();

			const ModelMeshUpload::Plan plan = ModelMeshUpload::PlanUpload(
				uiStoreQuads, m_uiUploadedQuads, m_uiCapacityWords);

			if (plan.bReallocate)
			{
				++m_uiReallocations;

				/* The whole point: the old buffer is gone, and the new one has
				   never been written. */
				m_Buffer.assign(plan.uiNewCapacityWords, k_uiGarbage);
				m_uiCapacityWords = plan.uiNewCapacityWords;
			}

			for (uint32_t i = 0; i < plan.uiWordCount; ++i)
				m_Buffer[plan.uiFirstWord + i] = m_Store[plan.uiFirstWord + i];

			m_uiUploadedQuads = uiStoreQuads;
		}

		/* Every word the GPU would read must equal the store. */
		bool Agrees(uint32_t& o_uiFirstDisagreement) const
		{
			const uint32_t uiWords = static_cast<uint32_t>(m_Store.size());

			if (m_Buffer.size() < uiWords)
			{
				o_uiFirstDisagreement = static_cast<uint32_t>(m_Buffer.size());
				return false;
			}

			for (uint32_t i = 0; i < uiWords; ++i)
			{
				if (m_Buffer[i] == m_Store[i])
					continue;

				o_uiFirstDisagreement = i;
				return false;
			}

			return true;
		}

		uint32_t StoreQuads() const
		{
			return static_cast<uint32_t>(m_Store.size()) / ModelMeshUpload::k_uiWordsPerQuad;
		}

		uint32_t Reallocations() const { return m_uiReallocations; }

	private:
		std::vector<uint32_t> m_Store;
		std::vector<uint32_t> m_Buffer;

		uint32_t m_uiCapacityWords = 0;
		uint32_t m_uiUploadedQuads = 0;
		uint32_t m_uiReallocations = 0;
	};
}

/* The regression itself, in the smallest form that shows it: upload something,
   then grow past capacity. Before the fix the plan appended, and every quad
   below the append point was left as whatever the new allocation contained. */
VOXAGINE_CHECK(ModelMeshUpload, AReallocationReUploadsEverything)
{
	const uint32_t uiCapacity = ModelMeshUpload::k_uiMinCapacityWords;
	const uint32_t uiQuadsThatFit = uiCapacity / ModelMeshUpload::k_uiWordsPerQuad;

	// Within capacity: append only, starting where the last upload stopped.
	const ModelMeshUpload::Plan append =
		ModelMeshUpload::PlanUpload(1000u, 400u, uiCapacity);

	CHECK_FALSE(append.bReallocate);
	CHECK_EQ(append.uiFirstWord, 400u * ModelMeshUpload::k_uiWordsPerQuad);
	CHECK_EQ(append.uiWordCount, 600u * ModelMeshUpload::k_uiWordsPerQuad);

	// Past capacity: the allocation is replaced, so the copy starts at zero.
	const ModelMeshUpload::Plan grow =
		ModelMeshUpload::PlanUpload(uiQuadsThatFit + 1u, uiQuadsThatFit, uiCapacity);

	CHECK_TRUE(grow.bReallocate);
	CHECK_EQ(grow.uiFirstWord, 0u) << "a reallocation invalidates everything already uploaded";
	CHECK_EQ(grow.uiWordCount, (uiQuadsThatFit + 1u) * ModelMeshUpload::k_uiWordsPerQuad);
	CHECK_GE(grow.uiNewCapacityWords, grow.uiWordCount);
}

VOXAGINE_CHECK(ModelMeshUpload, NothingToUploadWhenTheStoreHasNotMoved)
{
	const ModelMeshUpload::Plan plan =
		ModelMeshUpload::PlanUpload(5000u, 5000u, ModelMeshUpload::k_uiMinCapacityWords);

	CHECK_FALSE(plan.bReallocate);
	CHECK_EQ(plan.uiWordCount, 0u);
}

VOXAGINE_CHECK(ModelMeshUpload, TheFirstUploadAllocatesAtLeastTheMinimum)
{
	const ModelMeshUpload::Plan plan = ModelMeshUpload::PlanUpload(1u, 0u, 0u);

	CHECK_TRUE(plan.bReallocate);
	CHECK_EQ(plan.uiFirstWord, 0u);
	CHECK_EQ(plan.uiNewCapacityWords, ModelMeshUpload::k_uiMinCapacityWords)
		<< "growth is geometric from a floor, not sized to the first request";
}

/* Capacity doubles rather than tracking the request, because Mapper::Resize
   destroys an allocation earlier submissions may still be reading from - so the
   thing to keep rare is the reallocation, not the slack. */
VOXAGINE_CHECK(ModelMeshUpload, CapacityDoublesPastTheMinimum)
{
	const uint32_t uiCapacity = ModelMeshUpload::k_uiMinCapacityWords;
	const uint32_t uiQuads = (uiCapacity / ModelMeshUpload::k_uiWordsPerQuad) + 1u;

	const ModelMeshUpload::Plan plan = ModelMeshUpload::PlanUpload(uiQuads, 0u, uiCapacity);

	CHECK_EQ(plan.uiNewCapacityWords, uiCapacity * 2u);
}

/* The store only appends today. If that ever changes, the unsigned subtraction
   this used to do would become a copy of several gigabytes rather than a wrong
   image, so a shrink is defined as a full re-upload. */
VOXAGINE_CHECK(ModelMeshUpload, AShrunkStoreIsReUploadedWholeRatherThanUnderflowing)
{
	const ModelMeshUpload::Plan plan =
		ModelMeshUpload::PlanUpload(100u, 5000u, ModelMeshUpload::k_uiMinCapacityWords);

	CHECK_EQ(plan.uiFirstWord, 0u);
	CHECK_EQ(plan.uiWordCount, 100u * ModelMeshUpload::k_uiWordsPerQuad);
	CHECK_LE(plan.uiWordCount, 100u * ModelMeshUpload::k_uiWordsPerQuad)
		<< "an unsigned underflow would show here as a colossal count";
}

/* The one that would have caught it. A long session of animation frames being
   meshed at unpredictable moments, through several reallocations, with the
   buffer-equals-store invariant asserted after every upload. */
VOXAGINE_CHECK(ModelMeshUpload, TheBufferAlwaysEqualsTheStoreAcrossManyGrowths)
{
	UploadSimulation sim;
	DeterministicRandom random(0x5EED1234u);

	// Enough to cross the minimum capacity several times over.
	const uint32_t uiTargetQuads = (ModelMeshUpload::k_uiMinCapacityWords /
		ModelMeshUpload::k_uiWordsPerQuad) * 5u;

	uint32_t uiSyncs = 0;

	while (sim.StoreQuads() < uiTargetQuads)
	{
		/* A newly meshed model frame is a few hundred quads; occasionally a
		   whole level's worth arrives at once, which is what a world load
		   looks like. */
		const bool bWorldLoad = (random.Next() % 100ull) == 0ull;
		const uint32_t uiBurst = bWorldLoad
			? static_cast<uint32_t>(20000ull + random.Next() % 40000ull)
			: static_cast<uint32_t>(1ull + random.Next() % 500ull);

		sim.GrowStoreBy(uiBurst);
		sim.Sync();
		++uiSyncs;

		uint32_t uiFirstDisagreement = 0;
		if (!sim.Agrees(uiFirstDisagreement))
		{
			REQUIRE_TRUE(false)
				<< "buffer disagrees with the store at word " << uiFirstDisagreement
				<< " after sync " << uiSyncs
				<< " (" << sim.StoreQuads() << " quads, "
				<< sim.Reallocations() << " reallocations)";
		}
	}

	CHECK_GT(sim.Reallocations(), 1u)
		<< "the simulation has to actually reallocate more than once, or it "
		   "proves nothing about the case that broke";
	CHECK_GT(uiSyncs, 10u);
}
