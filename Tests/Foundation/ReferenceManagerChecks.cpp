#include "Framework/Check.h"

#include "Core/Resources/ReferenceManager.h"

#include <atomic>
#include <string>
#include <thread>
#include <vector>

/* Chunk streaming phase 14, and the other half of M9.
 *
 * World::OpenWorldAsync deserializes the incoming level on a job thread, and
 * deserializing an entity loads its model, its texture and its sound - through
 * the process-wide ReferenceManagers, while the outgoing level's chunk staging
 * is doing the same thing on the main thread. Nothing there was synchronised:
 * two threads inserting into one unordered_map, and two threads both finding
 * the same resource unloaded and both running Load on it. The stress fixture
 * died of the second one with `double free or corruption (!prev)` inside
 * VoxModel::Reset, on a job thread, in the middle of the level switch.
 *
 * These run against ReferenceManager itself rather than ResourceManager,
 * because ResourceManager::LoadVox needs a render context and this suite has
 * none - and because the mechanism is entirely in the manager: acquire-or-
 * create and first-time-load have to be one decision or the second thread runs
 * Load against a half-loaded object.
 *
 * A race check cannot fail deterministically. What it can do is drive the exact
 * shape at a contention rate high enough that the unsynchronised version dies
 * under it, and assert the invariants that hold whatever the interleaving was.
 * That was measured rather than assumed: the pre-fix manager and these two
 * loops, reduced to one standalone translation unit, segfaulted on five runs
 * out of five, and the locked one is clean on five out of five. So a failure
 * here is far more likely to be a crash than a reported mismatch - which is
 * fine, and is what the ASan/UBSan lane is for.
 */

namespace
{
	/* The smallest thing ReferenceManager will hold. Load is deliberately not
	   trivial: it writes a buffer and then publishes m_bIsLoaded, so two
	   concurrent Loads of one object are visible as a count rather than only as
	   whatever the allocator does about it. */
	class CountedReference final : public ReferenceObject
	{
	public:
		explicit CountedReference(std::string refPath)
			: ReferenceObject(std::move(refPath))
		{
		}

		bool Load(const std::string&) override
		{
			s_uiLoads.fetch_add(1, std::memory_order_relaxed);

			m_Payload.assign(256, static_cast<char>(s_uiLoads.load(std::memory_order_relaxed)));
			m_bIsLoaded = true;

			return true;
		}

		static void ResetLoadCount() { s_uiLoads.store(0, std::memory_order_relaxed); }
		static uint64_t LoadCount() { return s_uiLoads.load(std::memory_order_relaxed); }

	protected:
		void Free() override { m_Payload.clear(); }

	private:
		std::vector<char> m_Payload;

		static std::atomic<uint64_t> s_uiLoads;
	};

	std::atomic<uint64_t> CountedReference::s_uiLoads{ 0 };

	/* Enough names that the threads collide on the map's buckets, and few enough
	   that they collide on the same *object* constantly. Both are the real
	   pattern: two levels of one game share most of their models. */
	std::string NameFor(uint32_t uiIndex)
	{
		return "Content/Models/Model_" + std::to_string(uiIndex % 8) + ".vox";
	}
}

/* The one that crashed: a resource is loaded exactly once however many threads
   ask for it at the same instant. */
VOXAGINE_CHECK(ReferenceManager, ConcurrentAcquisitionLoadsEachResourceOnce)
{
	CountedReference::ResetLoadCount();

	ReferenceManager<CountedReference> manager;

	constexpr uint32_t k_uiThreads = 4;
	constexpr uint32_t k_uiAcquisitionsPerThread = 2000;

	std::atomic<uint32_t> uiUnloadedOnReturn{ 0 };
	std::vector<std::thread> threads;

	for (uint32_t uiThread = 0; uiThread < k_uiThreads; ++uiThread)
	{
		threads.emplace_back([&manager, &uiUnloadedOnReturn]()
		{
			for (uint32_t uiIndex = 0; uiIndex < k_uiAcquisitionsPerThread; ++uiIndex)
			{
				const std::string sName = NameFor(uiIndex);

				CountedReference* pRef = manager.AddReferenceAndLoad(sName,
					[&sName](CountedReference* pNewRef) { pNewRef->Load(sName); });

				if (!pRef->IsLoaded())
					++uiUnloadedOnReturn;
			}
		});
	}

	for (std::thread& thread : threads)
		thread.join();

	CHECK_EQ(CountedReference::LoadCount(), 8u)
		<< "a resource was loaded more than once, which is two threads running "
		   "Load over each other on one object";

	CHECK_EQ(uiUnloadedOnReturn.load(), 0u)
		<< "an acquisition returned a resource that was not loaded yet";
}

/* The map itself: acquiring and releasing from several threads leaves the
   reference counts and the map consistent. This is the half that corrupts the
   allocator rather than the payload. */
VOXAGINE_CHECK(ReferenceManager, ConcurrentAcquireAndReleaseLeavesTheMapIntact)
{
	CountedReference::ResetLoadCount();

	ReferenceManager<CountedReference> manager;

	constexpr uint32_t k_uiThreads = 4;
	constexpr uint32_t k_uiCyclesPerThread = 2000;

	std::vector<std::thread> threads;

	for (uint32_t uiThread = 0; uiThread < k_uiThreads; ++uiThread)
	{
		threads.emplace_back([&manager]()
		{
			for (uint32_t uiIndex = 0; uiIndex < k_uiCyclesPerThread; ++uiIndex)
			{
				const std::string sName = NameFor(uiIndex);

				manager.AddReferenceAndLoad(sName,
					[&sName](CountedReference* pNewRef) { pNewRef->Load(sName); });

				manager.RemoveReference(sName);
			}
		});
	}

	for (std::thread& thread : threads)
		thread.join();

	/* Every acquisition was matched by a release, so the map is empty - and the
	   objects it held were deleted exactly once, which is what the sanitizer
	   lane is here to say. */
	for (uint32_t uiIndex = 0; uiIndex < 8; ++uiIndex)
	{
		CHECK_TRUE(manager.GetReference(NameFor(uiIndex)) == nullptr)
			<< "a reference survived a balanced acquire/release: " << NameFor(uiIndex);
	}
}
