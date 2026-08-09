#include "pch.h"
#include "Core/Threading/JobQueue.h"

JobQueue::JobQueue(JobManager* pJobManager, uint32_t handle)
{
	m_pJobManager = pJobManager;
	m_QueueHandle = handle;

	m_JobQueue[JobType::JT_DEFAULT] = moodycamel::ConcurrentQueue<Job*>();
	m_JobQueue[JobType::JT_PHYSICS] = moodycamel::ConcurrentQueue<Job*>();
	m_JobQueue[JobType::JT_IO] = moodycamel::ConcurrentQueue<Job*>();
}

JobQueue::~JobQueue()
{
	CancelPendingJobs();
}

void JobQueue::Close()
{
	std::lock_guard<std::mutex> lock(m_SubmissionMutex);
	m_bAcceptingJobs = false;
}

void JobQueue::CancelPendingJobs()
{
	std::vector<Job*> pendingJobs;
	{
		std::lock_guard<std::mutex> lock(m_SubmissionMutex);
		m_bAcceptingJobs = false;

		for (auto& jobQueueIter : m_JobQueue)
		{
			Job* pJob = nullptr;
			while (jobQueueIter.second.try_dequeue(pJob))
				pendingJobs.push_back(pJob);
		}
	}

	for (Job* pJob : pendingJobs)
	{
		pJob->Canceled();
		delete pJob;
	}
}

void JobQueue::Enqueue(Job* pJob)
{
	TryEnqueueWithType(pJob, JT_DEFAULT);
}

void JobQueue::EnqueueBulk(const std::vector<Job*>& pJobs)
{
	{
		std::lock_guard<std::mutex> lock(m_SubmissionMutex);
		if (m_bAcceptingJobs)
		{
			for (Job* pJob : pJobs)
			{
				pJob->SetJobManager(m_pJobManager);
				pJob->SetQueueHandle(m_QueueHandle);
				pJob->m_Type = JT_DEFAULT;
			}

			m_JobQueue[JT_DEFAULT].enqueue_bulk(pJobs.begin(), pJobs.size());
			return;
		}
	}

	for (Job* pJob : pJobs)
	{
		pJob->Canceled();
		delete pJob;
	}
}

bool JobQueue::TryEnqueueWithType(Job* pJob, JobType jobType)
{
	{
		std::lock_guard<std::mutex> lock(m_SubmissionMutex);
		if (m_bAcceptingJobs)
		{
			pJob->m_Type = jobType;
			pJob->SetJobManager(m_pJobManager);
			pJob->SetQueueHandle(m_QueueHandle);
			m_JobQueue[jobType].enqueue(pJob);
			return true;
		}
	}

	pJob->Canceled();
	delete pJob;
	return false;
}

void JobQueue::EnqueueWithType(Job* pJob, JobType jobType)
{
	TryEnqueueWithType(pJob, jobType);
}

