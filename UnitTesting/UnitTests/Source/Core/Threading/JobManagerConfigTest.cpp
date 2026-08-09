#include "Core/Threading/JobManagerConfig.h"

#include <array>
#include <cstdint>
#include <limits>

int main()
{
	constexpr std::array<uint32_t, 9> expectedWorkerCounts = { 3, 3, 3, 3, 3, 4, 5, 6, 7 };

	for (uint32_t uiHardwareThreadCount = 0; uiHardwareThreadCount < static_cast<uint32_t>(expectedWorkerCounts.size()); ++uiHardwareThreadCount)
	{
		if (JobManagerConfig::CalculateWorkerThreadCount(uiHardwareThreadCount) != expectedWorkerCounts[uiHardwareThreadCount])
			return 1;
	}

	if (JobManagerConfig::CalculateWorkerThreadCount(std::numeric_limits<uint32_t>::max()) != JobManagerConfig::MAX_WORKER_THREADS)
		return 1;

	return 0;
}
