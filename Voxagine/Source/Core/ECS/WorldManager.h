#pragma once
#include <chrono>
#include <cstdint>
#include <stack>
#include <functional>
#include <queue>
#include <string>
#include "Core/Event.h"
#include "Core/ECS/WorldStreamingReadiness.h"

class World;
class Application;
class GameTimer;
class WorldManager
{
public:
	WorldManager(Application* pApp);
	~WorldManager();

	Event<World*> WorldLoaded;
	Event<World*> WorldPopped;

	void ClearWorlds();

	void LoadWorld(World* pWorld);

	/* Initializes and streams a replacement *behind* the world that is on
	   screen - the loading screen - and swaps it in only once it has arrived.
	   The visible world keeps ticking and rendering throughout, which is the
	   whole point: a loading screen that cannot animate is a frozen frame with
	   artwork on it. Docs/CHUNK_STREAMING_PLAN.md phase 8.
	   Ownership of pWorld transfers here in every path, including refusal. */
	void LoadWorldAfterStreaming(World* pWorld);

	/* One display frame of progress for the pending world, and the activation
	   test. Called from Application::Run with the same fixed-step count the
	   active world was ticked with; does nothing when nothing is pending. */
	void UpdateStreamingWorld(const GameTimer& fixedTimer, uint32_t uiFixedSteps);

	/* Null unless a world is being brought up behind the visible one. It is not
	   in GetWorlds() and it is not GetTopWorld() - a world nobody can see is not
	   the world the game is in, and every existing caller of those means the
	   visible one. */
	World* GetStreamingWorld() const { return m_pStreamingWorld; }

	/* Substitutes how a pending world is advanced and how "it has arrived" is
	   answered. The checks suite is the only caller; see
	   WorldStreamingReadiness.h on why this seam exists. Null restores the
	   default. Does not take ownership. */
	void SetStreamingReadiness(IWorldStreamingReadiness* pReadiness);

	void PushWorld(World* pWorld);

	void PopWorld();
	void SwapWorlds();

	bool RequiresSwap() { return m_DeferredFuncs.size() > 0; }
	World* GetTopWorld();

	size_t GetWorldCount() const { return m_Worlds.size(); };
	const std::vector<World*>& GetWorlds() const { return m_Worlds; }

	std::vector<std::string> GetWorldFiles() const { return m_WorldFiles; }
	void SetWorldFiles(std::vector<std::string> m_vFiles) { m_WorldFiles = std::move(m_vFiles); }

	const std::string GetPreviousWorldName() const { return m_sPreviousWorld; }

private:
	Application* m_pApplication;
	std::vector<World*> m_Worlds;
	std::queue<std::function<void()>> m_DeferredFuncs;

	std::vector<std::string> m_WorldFiles;
	std::string m_sPreviousWorld;

	/* The one world that exists and is not in m_Worlds. Its defense is
	   ownership: it leaves by activation, by refusal, or with ClearWorlds, and
	   nothing else knows it is there. */
	World* m_pStreamingWorld = nullptr;

	/* Activation is a deferred transaction like every other world change, so
	   there is a window between queueing it and running it. Advancing the
	   pending world across that window would tick a world that is already
	   spoken for. */
	bool m_bStreamingActivationQueued = false;

	/* Display frames the visible world drew while the pending one arrived, and
	   when it started. Both are reported: the frame count is what says the
	   loading screen animated rather than froze, and the wall clock is the only
	   half that can be compared against the blocking load it replaced. */
	uint32_t m_uiStreamingFrames = 0;
	std::chrono::steady_clock::time_point m_StreamingBegan;

	DefaultWorldStreamingReadiness m_DefaultReadiness;
	IWorldStreamingReadiness* m_pReadiness = &m_DefaultReadiness;
};