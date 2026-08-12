#pragma once
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include <External/moodycamel/concurrentqueue.h>
#ifdef _MSC_VER
#pragma warning(pop)
#endif

#include "Core/Threading/Job.h"
#include "Core/Threading/GenericJob.h"
#include <functional>
#include <mutex>
#include <unordered_map>
#include <vector>

class JobManager;

class JobQueue 
{
public:
	friend class JobManager;
	friend class PhysicsSystem;
	friend class PosixFileSystem;
	friend class ORBFileSystem;

	JobQueue(JobManager* pJobManager, uint32_t handle);
	~JobQueue();

	template<typename T>
	Job* Enqueue(std::function<T()> backgroundFunc, std::function<void(T)> callback);

	/* Route work to one of the dedicated queues. Chunk decoding uses the single
	   IO worker so that six large volumes cannot occupy every default worker at
	   the exact moment the window moves and the game thread wants one. */
	template<typename T>
	Job* EnqueueWithType(std::function<T()> backgroundFunc, std::function<void(T)> callback, JobType jobType);

	void Enqueue(Job* pJob);
	void EnqueueBulk(const std::vector<Job*>& pJobs);

private:
	std::unordered_map<JobType, moodycamel::ConcurrentQueue<Job*>> m_JobQueue;
	JobManager* m_pJobManager = nullptr;
	uint32_t m_QueueHandle = UINT_MAX;
	std::mutex m_SubmissionMutex;
	bool m_bAcceptingJobs = true;

	void Close();
	void CancelPendingJobs();
	bool TryEnqueueWithType(Job* pJob, JobType jobType);
	void EnqueueWithType(Job* pJob, JobType jobType);
};

template<typename T>
Job* JobQueue::Enqueue(std::function<T()> backgroundFunc, std::function<void(T)> callback)
{
	Job* pJob = new GenericJob<T>(backgroundFunc, callback);
	return TryEnqueueWithType(pJob, JT_DEFAULT) ? pJob : nullptr;
}

template<typename T>
Job* JobQueue::EnqueueWithType(std::function<T()> backgroundFunc, std::function<void(T)> callback, JobType jobType)
{
	Job* pJob = new GenericJob<T>(backgroundFunc, callback);
	return TryEnqueueWithType(pJob, jobType) ? pJob : nullptr;
}
