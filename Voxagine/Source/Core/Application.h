#pragma once
class Platform;

#include "ECS/WorldManager.h"
#include "Resources/ResourceManager.h"
#include "Core/Settings.h"
#include "Core/GameTimer.h"
#include "Platform/Platform.h"
#include "Core/JsonSerializer.h"
#include "Core/Threading/JobManager.h"
#include "Core/PlayerPrefs/PlayerPrefs.h"
#include "Core/LoggingSystem/LoggingSystem.h"

#ifdef EDITOR
#include "Editor/Editor.h"
#include <mutex>
#endif

class FileSystem;
class Application
{
public:
	Application();
	virtual ~Application();

	void Run();
	void Exit() { m_bExit = true; }

	Platform& GetPlatform() { return m_Platform; }
	WorldManager& GetWorldManager() { return m_WorldManager; }
	ResourceManager& GetResourceManager() { return m_ResourceManager; }
	Settings& GetSettings() { return m_Settings; }
	LoggingSystem& GetLoggingSystem() { return m_LoggingSystem; }
	JsonSerializer& GetSerializer() { return m_Serializer; }
	JobManager& GetJobManager() { return m_JobManager; }
	FileSystem* GetFileSystem() { return m_pFileSystem; }
	const GameTimer& GetTimer() const { return *m_Platform.GetGameTimer(); }

	/* Null until Platform::Initialize has run, which is never for an
	   Application that exists only to own a serializer and a job manager -
	   the streaming harness (CHUNK_STREAMING_PLAN.md T1). Anything that runs
	   both before startup and during the frame has to ask this way. */
	const GameTimer* TryGetTimer() const { return m_Platform.GetGameTimer(); }
	const GameTimer& GetFixedTimer() const { return *m_Platform.GetFixedTimer(); }

	bool IsSuspended() const { return m_bSuspended; }
	void SetSuspended(bool bSuspended) { m_bSuspended = bSuspended; }

	bool IsInEditor() const
	{
#ifdef EDITOR
		return m_Editor.GetEditorModus() == EditorModus::EM_EDITOR;
#else
		return false;
#endif
	}

	bool IsShuttingDown() const { return m_bExit; }

	/* Persist whatever the player changed in the settings menu, and restore it
	   at startup. See the definitions in Application.cpp for why this is
	   PlayerPrefs rather than Settings.vgs, and for the three-layer order.

	   SaveRenderSettings is public because the settings menu is the only thing
	   that calls it - it writes the whole set on leaving the screen rather than
	   on every keypress, so a player scrubbing through options is not writing a
	   file per frame. */
	void SaveRenderSettings();

protected:
	virtual void OnCreate() {};
	virtual void OnUpdate() {};
	virtual void OnDraw() {};
	virtual void OnExit() {};

	Platform m_Platform;
	WorldManager m_WorldManager;
	ResourceManager m_ResourceManager;
	Settings m_Settings;
	LoggingSystem m_LoggingSystem;
	JsonSerializer m_Serializer;
	JobManager m_JobManager;
	FileSystem* m_pFileSystem = nullptr;
	PlayerPrefs m_PlayerPrefs;
private:
	void LoadSettings();
	void LoadRenderSettings();

#ifdef EDITOR
 	Editor m_Editor;
#endif

	std::atomic_bool m_bSuspended;
	std::atomic_bool m_bExit;

	/* Rendered frames, for --frames. Only counted when a limit is set. */
	uint32_t m_uiRenderedFrames = 0;
};