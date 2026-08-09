#pragma once

#include <cstdint>

namespace JobManagerConfig
{
	// Physics, IO, and default work each retain a dedicated worker.
	constexpr uint32_t MIN_WORKER_THREADS = 3;
	// Bound bad CPU reports and avoid excessive oversubscription on large hosts.
	constexpr uint32_t MAX_WORKER_THREADS = 64;

	constexpr uint32_t CalculateWorkerThreadCount(uint32_t uiHardwareThreadCount)
	{
		const uint32_t uiAvailableWorkerThreads = uiHardwareThreadCount > 0
			? uiHardwareThreadCount - 1
			: 0;

		if (uiAvailableWorkerThreads < MIN_WORKER_THREADS)
			return MIN_WORKER_THREADS;
		if (uiAvailableWorkerThreads > MAX_WORKER_THREADS)
			return MAX_WORKER_THREADS;

		return uiAvailableWorkerThreads;
	}
}
