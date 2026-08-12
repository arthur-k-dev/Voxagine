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
#include "Core/Threading/JobThread.h"
#include "Core/Threading/JobQueue.h"
#include <vector>
#include <unordered_map>
#include <atomic>
#include <shared_mutex>
#include <mutex>

typedef uint32_t QueueHandle;

class JobManager
{
public:
	friend class Application;
	friend class PhysicsSystem;
	friend class PosixFileSystem;
	friend class ORBFileSystem;
	friend class JobQueue;
	friend class Job;

	JobManager();
	~JobManager();

	QueueHandle CreateJobQueue();
	JobQueue* GetJobQueue(QueueHandle handle);
	void DiscardJobQueue(QueueHandle handle);
	void ShelveJobQueue(QueueHandle handle);
	void UnShelveJobQueue(QueueHandle handle);

	/* The worker pool's lifetime and the point at which completion callbacks
	   run on this thread. Public because Application::Run is not the only
	   legitimate driver of a job system any more: the streaming harness runs
	   one with no Application loop behind it (CHUNK_STREAMING_PLAN.md T1), and
	   "when do callbacks fire" is exactly the question a deterministic test has
	   to be able to answer. */
	void Initialize();
	void Deinitialize();
	void ProcessFinishedJobs();

private:
	moodycamel::ConcurrentQueue<Job*> m_FinishedJobQueue;
	std::vector<JobThread*> m_WorkerThreads;

	uint32_t m_uiNumActiveThreads = 0;
	uint32_t m_uiMaxNumThreads = 0;
	uint32_t m_uiThreadSleepTime = 10;
	std::atomic_bool m_bKillThreads = { false };
	bool m_bAcceptingQueues = true;

	void WaitForJob(Job* pJob);

	void ThreadLoop(JobThread* pThrea);

private:
	std::shared_timed_mutex m_JobMutex;
	uint32_t m_QueueHandleCtr = 0;
	std::unordered_map<QueueHandle, JobQueue*> m_JobQueues;
	std::unordered_map<QueueHandle, JobQueue*> m_ShelvedJobQueues;
};
