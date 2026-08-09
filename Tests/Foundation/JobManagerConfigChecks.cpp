#include "Framework/Check.h"

#include <array>
#include <cstdint>
#include <limits>

#include "Core/Threading/JobManagerConfig.h"

/* Worker count against hardware thread count, including the degenerate ends.
   The floor matters on a small machine - a job system that asks for zero
   workers never runs a job - and the ceiling matters because the count indexes
   fixed-size arrays. */
VOXAGINE_CHECK(JobManagerConfig, WorkerCountHasAFloorAndACeiling)
{
	constexpr std::array<uint32_t, 9> expected = { 3, 3, 3, 3, 3, 4, 5, 6, 7 };

	for (uint32_t uiHardwareThreads = 0; uiHardwareThreads < expected.size(); ++uiHardwareThreads)
		CHECK_EQ(JobManagerConfig::CalculateWorkerThreadCount(uiHardwareThreads), expected[uiHardwareThreads]);

	CHECK_EQ(JobManagerConfig::CalculateWorkerThreadCount(std::numeric_limits<uint32_t>::max()),
	         JobManagerConfig::MAX_WORKER_THREADS);
}
