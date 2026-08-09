#include "pch.h"
#include "Core/Threading/JobManager.h"
#include "Core/Threading/JobManagerConfig.h"

#include <chrono>
#include <queue>
#include "External/optick/optick.h"

JobManager::JobManager()
{
	m_uiMaxNumThreads = JobManagerConfig::CalculateWorkerThreadCount(std::thread::hardware_concurrency());
}

JobManager::~JobManager()
{
	
}

void JobManager::Initialize()
{
	if (!m_WorkerThreads.empty())
		return;

	{
		std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
		m_bAcceptingQueues = true;
		m_bKillThreads = false;
	}

	auto createWorker = [this](JobType jobType)
	{
		JobThread* pThread = new JobThread();
		pThread->SetJobType(jobType);
		pThread->SetThread(new std::thread(&JobManager::ThreadLoop, this, pThread));
		m_WorkerThreads.push_back(pThread);
		++m_uiNumActiveThreads;
	};

	// We always need one thread for each type at least
	createWorker(JT_PHYSICS);
	createWorker(JT_IO);
	createWorker(JT_DEFAULT);

	// Use extra thread slots for default jobs
	for (uint32_t i = JobManagerConfig::MIN_WORKER_THREADS; i < m_uiMaxNumThreads; ++i)
		createWorker(JT_DEFAULT);
}

void JobManager::Deinitialize()
{
	{
		std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
		m_bAcceptingQueues = false;

		for (auto& jobQueue : m_JobQueues)
			jobQueue.second->Close();
		for (auto& jobQueue : m_ShelvedJobQueues)
			jobQueue.second->Close();

		// This exclusive lock is also a barrier for every worker queue scan.
		m_bKillThreads = true;
	}

	for (JobThread* jobThread : m_WorkerThreads)
		jobThread->CancelRunningJob();

	// Waiting jobs never ran, so cancel and destroy them without calling Finish.
	for (auto& jobQueue : m_JobQueues)
		jobQueue.second->CancelPendingJobs();
	for (auto& jobQueue : m_ShelvedJobQueues)
		jobQueue.second->CancelPendingJobs();

	// Queue storage stays alive until no worker can read from it or publish a result.
	for (JobThread* jobThread : m_WorkerThreads)
		jobThread->Join();

	for (JobThread* jobThread : m_WorkerThreads)
		delete jobThread;
	m_WorkerThreads.clear();
	m_uiNumActiveThreads = 0;

	// Jobs that did run keep the existing policy: deliver Finish on this thread.
	// Closed queues remain alive while callbacks drain so late submissions fail safely.
	ProcessFinishedJobs();

	{
		std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
		for (auto& jobQueue : m_JobQueues)
			delete jobQueue.second;
		m_JobQueues.clear();

		for (auto& jobQueue : m_ShelvedJobQueues)
			delete jobQueue.second;
		m_ShelvedJobQueues.clear();
	}
}

void JobManager::ProcessFinishedJobs()
{
	OPTICK_EVENT();
	Job* pJob = nullptr;
	while (m_FinishedJobQueue.try_dequeue(pJob))
	{
		pJob->Finish();
		delete pJob;
	}
}

void JobManager::WaitForJob(Job* pJob)
{
	std::queue<Job*> dequeuedJobs;
	while (true)
	{
		Job* pTempJob = nullptr;
		while (m_FinishedJobQueue.try_dequeue(pTempJob))
		{
			if (pTempJob == pJob)
			{
				pJob->Finish();
				delete pJob;

				while (!dequeuedJobs.empty())
				{
					Job* topJob = dequeuedJobs.front();
					dequeuedJobs.pop();
					m_FinishedJobQueue.enqueue(topJob);
				}

				return;
			}

			dequeuedJobs.push(pTempJob);
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

JobQueue* JobManager::GetJobQueue(QueueHandle handle)
{
	std::shared_lock<std::shared_timed_mutex> lock(m_JobMutex);
	if (!m_bAcceptingQueues)
		return nullptr;

	auto jobQueueIter = m_JobQueues.find(handle);
	if (jobQueueIter != m_JobQueues.end())
		return jobQueueIter->second;
	return nullptr;
}

void JobManager::ThreadLoop(JobThread* pThread)
{
	while (!m_bKillThreads)
	{
		Job* newJob = nullptr;
		bool jobFound = false;

		/* The lock covers the queue scan only.
		 *
		 * It used to span the whole loop body, so every worker held a read
		 * lock while running a job AND while sleeping for 10ms. With several
		 * workers doing that the mutex was almost never free, and any writer -
		 * CreateJobQueue, DiscardJobQueue - had to wait for a moment when no
		 * reader held it at all.
		 *
		 * Windows' SRW locks are writer-preferring, so a waiting writer blocks
		 * new readers and eventually gets through. glibc's shared_timed_mutex
		 * is not, so the writer starved forever and World::Initialize hung on
		 * the first CreateJobQueue call. */
		{
			std::shared_lock<std::shared_timed_mutex> lock(m_JobMutex);
			if (m_bKillThreads)
				break;

			for (auto& jobQueueIter : m_JobQueues)
			{
				if (jobQueueIter.second->m_JobQueue[pThread->GetJobType()].try_dequeue(newJob))
				{
					jobFound = true;
					break;
				}
			}

			// Publish the running pointer before releasing the shutdown barrier.
			if (jobFound)
				pThread->SetRunningJob(newJob);
		}

		if (jobFound)
		{
			/* Outside the lock: a long job must not block queue creation. */
			newJob->SetWaiting(false);
			newJob->Run();
			pThread->SetRunningJob(nullptr);
			m_FinishedJobQueue.enqueue(newJob);
		}
		else
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(m_uiThreadSleepTime));
		}
	}
}

QueueHandle JobManager::CreateJobQueue()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
	if (!m_bAcceptingQueues)
		return UINT_MAX;

	QueueHandle handle = m_QueueHandleCtr++;
	m_JobQueues[handle] = new JobQueue(this, handle);
	return handle;
}

void JobManager::DiscardJobQueue(QueueHandle handle)
{
	JobQueue* pJobQueue = nullptr;
	{
		std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
		if (!m_bAcceptingQueues)
			return;

		auto jobQueueIter = m_JobQueues.find(handle);
		if (jobQueueIter == m_JobQueues.end())
			return;

		pJobQueue = jobQueueIter->second;
		pJobQueue->Close();
		m_JobQueues.erase(jobQueueIter);
	}

	// The erase lock is a worker-scan barrier, so every dequeued job is visible now.
	for (JobThread* pThread : m_WorkerThreads)
	{
		Job* pJob = pThread->GetRunningJob();
		if (pJob && pJob->m_JobQueueHandle == handle)
		{
			pJob->Canceled();
			WaitForJob(pJob);
		}
	}

	ProcessFinishedJobs();
	delete pJobQueue;
}

void JobManager::ShelveJobQueue(QueueHandle handle)
{
	JobQueue* pJobQueue = nullptr;
	{
		std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
		if (!m_bAcceptingQueues)
			return;

		auto jobQueueIter = m_JobQueues.find(handle);
		if (jobQueueIter == m_JobQueues.end())
			return;

		pJobQueue = jobQueueIter->second;
		m_JobQueues.erase(jobQueueIter);
		m_ShelvedJobQueues[handle] = pJobQueue;
	}

	// The erase lock is a worker-scan barrier, so every dequeued job is visible now.
	for (JobThread* pThread : m_WorkerThreads)
	{
		Job* pJob = pThread->GetRunningJob();
		if (pJob && pJob->m_JobQueueHandle == handle)
		{
			pJob->Canceled();
			WaitForJob(pJob);
		}
	}

	ProcessFinishedJobs();
}

void JobManager::UnShelveJobQueue(QueueHandle handle)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
	if (!m_bAcceptingQueues)
		return;

	auto jobQueueIter = m_ShelvedJobQueues.find(handle);
	if (jobQueueIter == m_ShelvedJobQueues.end()) return;

	JobQueue* pJobQueue = jobQueueIter->second;

	m_JobQueues[handle] = pJobQueue;
	m_ShelvedJobQueues.erase(handle);
}
