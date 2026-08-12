#include "pch.h"
#include "Core/ECS/WorldManager.h"

#include "Core/Application.h"
#include "Core/ECS/World.h"
#include "External/optick/optick.h"

#include <chrono>
#include <cstdio>

WorldManager::WorldManager(Application* pApp)
{
	m_pApplication = pApp;
}

WorldManager::~WorldManager()
{
	ClearWorlds();
}

void WorldManager::ClearWorlds()
{
	while (!m_Worlds.empty())
	{
		World* pWorld = m_Worlds.back();
		WorldPopped(pWorld);

		pWorld->Unload();

		delete pWorld;
		pWorld = nullptr;
		m_Worlds.pop_back();
	}
}

void WorldManager::LoadWorld(World* pWorld)
{
	m_DeferredFuncs.push([this, pWorld]()
	{
		/* Permanent, and one line rather than a profiler event, because this is
		   the single largest stall the game has and it happens off the frame
		   loop where the frame profiler cannot see it. Docs/CHUNK_STREAMING_PLAN.md
		   phase 4 is measured against these splits. */
		const auto begin = std::chrono::steady_clock::now();
		auto checkpoint = begin;
		double fUnloadMs = 0.0;
		double fDeleteMs = 0.0;

		const auto split = [&checkpoint]()
		{
			const auto now = std::chrono::steady_clock::now();
			const double fMs =
				std::chrono::duration<double, std::milli>(now - checkpoint).count();
			checkpoint = now;

			return fMs;
		};

		if (!m_Worlds.empty())
		{
			World* pTopWorld = m_Worlds.back();
			m_sPreviousWorld = pTopWorld->GetName();
			WorldPopped(pTopWorld);

			pTopWorld->Unload();
			fUnloadMs = split();

			delete pTopWorld;
			pTopWorld = nullptr;
			m_Worlds.pop_back();
			fDeleteMs = split();
		}

		m_Worlds.push_back(pWorld);
		pWorld->Initialize();
		const double fInitializeMs = split();

		WorldLoaded(pWorld);
		const double fLoadedMs = split();

		fprintf(stderr,
			"[world-switch] '%s': unload %.1f + delete %.1f + initialize %.1f + "
			"loaded %.1f = %.1f ms\n",
			pWorld->GetName().c_str(), fUnloadMs, fDeleteMs, fInitializeMs, fLoadedMs,
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - begin).count());
	});
}

void WorldManager::PushWorld(World* pWorld)
{
	m_DeferredFuncs.push([this, pWorld]()
	{
		m_sPreviousWorld = m_Worlds.back()->GetName();
		m_Worlds.back()->Pause();
		m_Worlds.push_back(pWorld);
		pWorld->Initialize();
		WorldLoaded(pWorld);
	});
}

void WorldManager::PopWorld()
{
	if (m_Worlds.empty()) return;

	m_DeferredFuncs.push([this]()
	{
		World* pWorld = m_Worlds.back();
		m_sPreviousWorld = pWorld->GetName();
		WorldPopped(pWorld);

		pWorld->Unload();

		delete pWorld;
		pWorld = nullptr;
		m_Worlds.pop_back();

		World* oldWorld = m_Worlds.back();
		if (oldWorld)
			oldWorld->Resume();
	});
}

void WorldManager::SwapWorlds()
{
	OPTICK_EVENT();
	while (!m_DeferredFuncs.empty())
	{
		std::function<void()>& func = m_DeferredFuncs.front();
		func();
		m_DeferredFuncs.pop();
	}
}

World* WorldManager::GetTopWorld()
{
	if (!m_Worlds.empty())
		return m_Worlds.back();
	return nullptr;
}
