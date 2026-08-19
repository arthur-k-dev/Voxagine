#include "pch.h"
#include "Core/LoggingSystem/LoggingSystem.h"

#include "Core/Application.h"
#include "Core/GameTimer.h"
#include "Core/System/MobileLog.h"

namespace
{
	const char* LogLevelName(LogLevel level)
	{
		switch (level)
		{
		case LOGLEVEL_MESSAGE: return "message";
		case LOGLEVEL_WARNING: return "warning";
		case LOGLEVEL_ERROR: return "error";
		case LOGLEVEL_CRITICAL_ERROR: return "critical";
		default: return "unknown";
		}
	}
}

LoggingSystem::LoggingSystem()
{
}

LoggingSystem::~LoggingSystem()
{
	for (auto it : m_LogEvents)
		delete it;
}

void LoggingSystem::Initialize(Application * pApplication)
{
	m_pApplication = pApplication;
}

void LoggingSystem::UnInitialize()
{
	// Doas nothing by default

	// In future, write logging events to log files
}

void LoggingSystem::Log(const LogEvent & newLogEvent)
{
	Log(newLogEvent.Level, newLogEvent.Category, newLogEvent.Description);
}

void LoggingSystem::Log(const LogLevel & level, const std::string & description)
{
	Log(level, "Unknown", description);
}

void LoggingSystem::Log(const LogLevel & level, const std::string & category, const std::string & description)
{
#ifdef _ORBIS
	return;
#endif

	LogEvent* event = new LogEvent();

	/* There is not always a clock. A log line can be emitted before
	   Platform::Initialize has created the game timer, and by a process that
	   never creates one at all - which used to be a null dereference inside the
	   logger, i.e. the one place least able to report it. */
	const GameTimer* pTimer = m_pApplication != nullptr ? m_pApplication->TryGetTimer() : nullptr;

	event->EventTime = pTimer != nullptr ? pTimer->GetCurrentSystemTime() : Time{ 0, 0, 0 };
	event->Level = level;
	event->Category = category;
	event->Description = description;

	LogEvent& NewLogEvent = *event;
	unsigned long NewLogEventID = 0;

	{
		std::lock_guard<std::recursive_mutex> lock(m_Mutex);

		m_LogEvents.push_back(event);
		NewLogEventID = static_cast<unsigned long>(m_LogEvents.size() - 1);

		CreateCategory(category);
		m_Categories[category].push_back(NewLogEventID);
	}

	/* The editor observes LogEventCreated, but a game on a phone has no editor
	   console. Publish the exact same event through the platform sink rather
	   than adding iOS/Android logging calls at individual failure sites. */
	MobileLog::Write(std::string("[") + LogLevelName(level) + "] [" + category + "] " + description);

	LogEventCreated.operator()(NewLogEvent, NewLogEventID);
}

void LoggingSystem::CreateCategory(const std::string & newCategory)
{
	std::lock_guard<std::recursive_mutex> lock(m_Mutex);

	std::unordered_map<std::string, std::vector<unsigned long>>::iterator found = m_Categories.find(newCategory);

	if (found == m_Categories.end())
	{
		m_Categories[newCategory];
		LogEventCategoryCreated.operator()(newCategory);
	}
}

const LogEvent * LoggingSystem::GetLogEvent(unsigned long logEventIndex)
{
	return m_LogEvents[logEventIndex];
}

unsigned long LoggingSystem::GetLogEventCount() const
{
	return static_cast<unsigned long>(m_LogEvents.size());
}

const std::vector<unsigned long>* LoggingSystem::GetEventLogCategoryIndices(const std::string & category)
{
	std::unordered_map<std::string, std::vector<unsigned long>>::iterator found = m_Categories.find(category);

	if (found != m_Categories.end())
		return &found->second;

	return nullptr;
}
