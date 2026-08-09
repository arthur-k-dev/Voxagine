#include "pch.h"
#include "JobThread.h"

JobThread::~JobThread()
{
	Join();
	delete m_Thread;
}

void JobThread::CancelRunningJob()
{
	std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
	if (m_pRunningJob)
		m_pRunningJob->Canceled();
}

void JobThread::Join()
{
	if (m_Thread && m_Thread->joinable())
		m_Thread->join();
}

Job* JobThread::GetRunningJob()
{
	std::shared_lock<std::shared_timed_mutex> lock(m_JobMutex);
	return m_pRunningJob;
}

void JobThread::SetRunningJob(Job* pJob)
{
	std::unique_lock<std::shared_timed_mutex> lock(m_JobMutex);
	m_pRunningJob = pJob;
}
