#pragma once

#include <mutex>
#include <vector>
#include <unordered_map>

#include "Core/LoggingSystem/LogEvent.h"
#include "Core/Event.h"

class Application;

class LoggingSystem
{
public:
	LoggingSystem();
	~LoggingSystem();

	void Initialize(Application* pApplication);
	void UnInitialize();

	void Log(const LogEvent& newLogEvent);
	void Log(const LogLevel& level, const std::string& description);
	void Log(const LogLevel& level, const std::string& category, const std::string& description);

	void CreateCategory(const std::string& newCategory);

	const LogEvent* GetLogEvent(unsigned long logEventIndex);
	unsigned long GetLogEventCount() const;
	const std::unordered_map<std::string, std::vector<unsigned long>>& GetCategories() { return m_Categories; }

	const std::vector<unsigned long>* GetEventLogCategoryIndices(const std::string& category);

public:
	Event<const LogEvent&, unsigned long> LogEventCreated;
	Event<const std::string> LogEventCategoryCreated;
private:
	Application* m_pApplication;

	std::vector<LogEvent*> m_LogEvents;
	std::unordered_map<std::string, std::vector<unsigned long>> m_Categories;

	/* Log is reached from job threads - a resource that fails to load on the
	   asynchronous world loader, a serializer refusing a root, anything on the
	   IO worker - while the main thread is logging too, and two push_backs into
	   one vector is a corrupted heap. Chunk streaming phase 14; the same class
	   as the reference managers and the file system.

	   It covers the two containers only. Subscribers are called after it is
	   released, because a lock held across arbitrary observer code is a deadlock
	   waiting for a subscriber that logs; recursive for the one that does it
	   anyway, through CreateCategory. */
	std::recursive_mutex m_Mutex;
};