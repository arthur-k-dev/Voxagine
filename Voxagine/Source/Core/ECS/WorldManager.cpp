#include "pch.h"
#include "Core/ECS/WorldManager.h"

#include "Core/Application.h"
#include "Core/ECS/World.h"
#include "Core/ECS/Systems/AudioSystem.h"
#include "Core/ECS/Systems/Rendering/RenderSystem.h"
#include "Core/Platform/Platform.h"
#include "Core/Platform/Rendering/RenderContext.h"
#include "External/optick/optick.h"

#include <chrono>
#include <cstdio>

namespace
{
	/* Sprite submissions legally span several display frames - FixedClear runs
	   from PostFixedTick, not from every frame - but they may not span a world
	   lifetime: unloading releases the textures those SpriteData entries
	   address, and a frame that runs no fixed tick would then draw freed
	   bindless IDs. Ledger E9's crash class, and the reason it is here rather
	   than inside the unload: only the world manager knows a world is leaving. */
	void DiscardActiveWorldSprites(Application* pApplication)
	{
		RenderContext* pRenderContext =
			pApplication != nullptr ? pApplication->GetPlatform().GetRenderContext() : nullptr;

		if (pRenderContext != nullptr)
			pRenderContext->FixedClear();
	}
}

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
	/* A pending world is not in m_Worlds, so it would otherwise survive a
	   shutdown or a ClearWorlds and leak - with its chunk jobs still queued
	   against a job manager that is going away. */
	if (m_pStreamingWorld != nullptr)
	{
		World* pStreamingWorld = m_pStreamingWorld;
		m_pStreamingWorld = nullptr;
		m_bStreamingActivationQueued = false;

		/* It owns the voxel window and the far-field build: it built them. */
		pStreamingWorld->Unload();
		delete pStreamingWorld;
	}

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
			DiscardActiveWorldSprites(m_pApplication);
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

void WorldManager::SetStreamingReadiness(IWorldStreamingReadiness* pReadiness)
{
	m_pReadiness = pReadiness != nullptr ? pReadiness : &m_DefaultReadiness;
}

void WorldManager::LoadWorldAfterStreaming(World* pWorld)
{
	if (pWorld == nullptr)
		return;

	m_DeferredFuncs.push([this, pWorld]()
	{
		/* There can be only one replacement behind the loading screen, and this
		   is not a cancellation API: a second request is a programming error, so
		   it is refused rather than allowed to replace a world that is already
		   half-streamed. Refusing still owns the world it was handed. */
		if (m_pStreamingWorld != nullptr)
		{
			fprintf(stderr,
				"[world-switch] refused a second pending streamed world '%s'; "
				"'%s' is already being prepared\n",
				pWorld->GetName().c_str(), m_pStreamingWorld->GetName().c_str());

			delete pWorld;
			return;
		}

		RenderContext* pRenderContext =
			m_pApplication->GetPlatform().GetRenderContext();

		const float fVisibleFade =
			pRenderContext != nullptr ? pRenderContext->GetFadeValue() : 1.f;

		/* Initialize builds the AudioSystem and ordinarily starts every autoplay
		   source with it. This world is behind the loading screen; its music
		   starts when it is the world you are looking at. */
		if (AudioSystem* pAudioSystem = pWorld->GetSystem<AudioSystem>())
			pAudioSystem->SetAutoPlayDeferred(true);

		const auto begin = std::chrono::steady_clock::now();

		m_pStreamingWorld = pWorld;
		m_uiStreamingFrames = 0;
		m_StreamingBegan = begin;
		pWorld->Initialize();

		/* RenderSystem::Start initialises a normal gameplay fade-in on the
		   *shared* render context. The visible world is still the loading
		   screen, so put its fade back - otherwise the screen everybody is
		   looking at fades to black as the world behind it starts up. */
		if (pRenderContext != nullptr)
			pRenderContext->SetFadeValue(fVisibleFade);

		fprintf(stderr,
			"[world-switch] '%s' initialized behind the loading screen in %.1f ms; "
			"streaming its first window\n",
			pWorld->GetName().c_str(),
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - begin).count());
	});
}

void WorldManager::UpdateStreamingWorld(
	const GameTimer& fixedTimer, uint32_t uiFixedSteps)
{
	World* pWorld = m_pStreamingWorld;

	if (pWorld == nullptr || m_bStreamingActivationQueued)
		return;

	++m_uiStreamingFrames;

	m_pReadiness->Advance(*pWorld, fixedTimer, uiFixedSteps);

	if (!m_pReadiness->HasArrived(*pWorld))
		return;

	m_bStreamingActivationQueued = true;

	/* Frames the loading screen actually drew while this world arrived. It is
	   the acceptance measurement rather than a curiosity: the failure this
	   phase exists to remove is a load that renders no frames at all, and a
	   count of one would say the world came up in a single blocking step. */
	const uint32_t uiStreamedFrames = m_uiStreamingFrames;
	const double fStreamedMs = std::chrono::duration<double, std::milli>(
		std::chrono::steady_clock::now() - m_StreamingBegan).count();

	/* The activation itself is a deferred transaction, run by SwapWorlds at the
	   top of a frame with the GPU already waited on - the same place every other
	   world change happens, and the reason this costs a fraction of a frame
	   rather than a frame of its own. */
	m_DeferredFuncs.push([this, pWorld, uiStreamedFrames, fStreamedMs]()
	{
		if (m_pStreamingWorld != pWorld)
			return;

		const auto begin = std::chrono::steady_clock::now();

		if (!m_Worlds.empty())
		{
			World* pVisibleWorld = m_Worlds.back();

			DiscardActiveWorldSprites(m_pApplication);

			m_sPreviousWorld = pVisibleWorld->GetName();
			WorldPopped(pVisibleWorld);

			/* The replacement already owns the voxel window and the far-field
			   build. An ordinary unload would cancel one and clear the other -
			   over the level that was just streamed in. */
			pVisibleWorld->Unload(false);
			delete pVisibleWorld;
			m_Worlds.pop_back();
		}

		m_Worlds.push_back(pWorld);
		m_pStreamingWorld = nullptr;
		m_bStreamingActivationQueued = false;

		if (RenderSystem* pRenderSystem = pWorld->GetRenderSystem())
			pRenderSystem->SetFadeValue(1.f);

		if (AudioSystem* pAudioSystem = pWorld->GetSystem<AudioSystem>())
			pAudioSystem->ActivateDeferredAutoPlay();

		WorldLoaded(pWorld);

		fprintf(stderr,
			"[world-switch] '%s' activated in %.2f ms, after %u frames / %.0f ms "
			"behind the loading screen\n",
			pWorld->GetName().c_str(),
			std::chrono::duration<double, std::milli>(
				std::chrono::steady_clock::now() - begin).count(),
			uiStreamedFrames, fStreamedMs);
	});
}

void WorldManager::PushWorld(World* pWorld)
{
	m_DeferredFuncs.push([this, pWorld]()
	{
		DiscardActiveWorldSprites(m_pApplication);
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
		DiscardActiveWorldSprites(m_pApplication);
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
