#include "pch.h"
#include "Core/Application.h"
#include "Core/LaunchOptions.h"

#ifdef _ORBIS
#include "Core/System/ORBIS/ORBFileSystem.h"
#else
#include "Core/System/Posix/PosixFileSystem.h"
#endif

#include "Core/System/FileSystem.h"
#include "Platform/Window/WindowContext.h"
#include "Platform/Input/Temp/InputContextNew.h"
#include "Platform/Rendering/RenderContext.h"
#include "Editor/imgui/ImguiSystem.h"

#include "Core/ECS/World.h"
#include "ECS/Entities/Camera.h"

#include <chrono>
#include <iostream>
#include <thread>
#include <SDL3/SDL_messagebox.h>
#include "Core/GameTimer.h"
#include "ECS/WorldManager.h"
#include "ECS/Systems/Physics/PhysicsSystem.h"

#include "Core/Platform/Audio/AudioContext.h"
#include "External/optick/optick.h"

Application::Application() :
	m_Platform(this),
	m_WorldManager(this),
	m_ResourceManager(this),
	m_Serializer(m_Settings, m_LoggingSystem)
{
	m_bExit = false;
}

Application::~Application()
{

}

void Application::Run()
{
#ifdef _ORBIS
	m_pFileSystem = new ORBFileSystem(this);
	m_pFileSystem->Initialize();
#else
	m_pFileSystem = new PosixFileSystem(this);
	m_pFileSystem->Initialize();
#endif

	m_LoggingSystem.Initialize(this);
	m_Serializer.Initialize(m_pFileSystem);

	/* Before LoadSettings, because the render settings the player chose live in
	   here and LoadSettings is what restores them. It used to be initialized in
	   VoxApp::OnCreate, which runs *after* Platform::Initialize has already
	   built the render passes from Settings - too late to decide how large the
	   shadow map should be. Nothing else about it moved: it is an Application
	   member and always was. */
	m_PlayerPrefs.Initialize(&m_Serializer, "PlayerPrefs.vgprefs");

	LoadSettings();

#if defined(EDITOR) || defined(VOXAGINE_MOBILE)
	/* The 16:9 lock is a game presentation choice and neither of these wants
	   it, for two different reasons.

	   The editor wants the whole window; play mode still uses the camera's own
	   aspect ratio.

	   A phone is nearer 20:9, so a locked frame would run with black bars down
	   a fifth of a screen that has no bars anywhere else. Rendering at the
	   device's own ratio widens the view instead, which is the right trade for
	   a top-down game: it shows more of the arena rather than stretching it.

	   The render *quality* defaults that used to sit in this block are in
	   Settings::ApplyPlatformRenderDefaults now, where the player's own choices
	   can be layered on top of them. */
	m_Settings.SetLockedAspectRatio(0.f);
#endif

	m_JobManager.Initialize();
	m_Platform.Initialize();

	/* A window/input platform may initialize while its renderer rejects the
	   device. World startup requires RenderContext's voxel mapper, so stop here
	   with the renderer's already-recorded diagnostic instead of crashing later
	   in ChunkSystem or PhysicsSystem. */
	if (m_Platform.GetRenderContext()->IsReady())
	{
		OnCreate();
	}
	else
	{
		const std::string startupError = m_Platform.GetRenderContext()->GetStartupError();
		m_LoggingSystem.Log(LOGLEVEL_CRITICAL_ERROR, "Application",
			"Renderer initialization did not complete; application startup was stopped. " + startupError);
		SDL_ShowSimpleMessageBox(
			SDL_MESSAGEBOX_ERROR,
			"Bit Buster cannot start",
			("The selected renderer is not available on this device.\n\n" + startupError).c_str(),
			nullptr);
		m_bExit = true;
	}

#ifdef EDITOR
	m_Editor.Initialize(this);
#endif

	m_Platform.m_pGameTimer->ResetElapsedTime();
	m_Platform.m_pFixedGameTimer->ResetElapsedTime();


	while (!m_bExit)
	{

		m_Platform.m_pGameTimer->Update([this]()
		{
			OPTICK_FRAME("MainThread");
			float fElapsed = static_cast<float>(m_Platform.m_pGameTimer->GetElapsedSeconds());

			if (m_WorldManager.RequiresSwap())
			{
				m_Platform.GetRenderContext()->WaitForGPU();
				m_WorldManager.SwapWorlds();
			}

			m_Platform.GetWindowContext()->Poll();

			/* Android does not merely stop giving the app CPU time in the
			   background - it destroys the window surface the swapchain was
			   built from, and it may kill the process outright if it keeps
			   rendering and mixing audio anyway. Nothing here fires on
			   desktop; WindowContext's default ConsumeEntered* always return
			   false there. See WindowContext::IsBackgrounded and
			   VKRenderContext::SuspendForBackground for what each side is
			   actually protecting against. */
			if (m_Platform.GetWindowContext()->ConsumeEnteredBackground())
			{
				m_Platform.GetAudioContext()->PauseAll();
				m_Platform.GetRenderContext()->SuspendForBackground();
			}

			if (m_Platform.GetWindowContext()->ConsumeEnteredForeground())
			{
				m_Platform.GetRenderContext()->ResumeFromBackground();
				m_Platform.GetAudioContext()->ResumeAll();
			}

			if (m_Platform.GetWindowContext()->IsBackgrounded())
			{
				/* Nothing below this point has anything to draw to or, for
				   most of the frame, anything useful to simulate against - the
				   render context has no surface until ResumeFromBackground
				   runs. Sleeping here is what stops the main loop spinning
				   Poll() as fast as the CPU allows while there is nothing to
				   poll for but the foreground transition; GameTimer::Update
				   already gates *this* lambda by the frame limit, but with no
				   limit set (this game's default) that gate does nothing. */
				std::this_thread::sleep_for(std::chrono::milliseconds(100));
				return;
			}

			m_Platform.GetInputContext()->Update();
			m_Platform.GetImguiSystem().Update();

			m_Platform.GetRenderContext()->Clear();

			OnUpdate();

			m_JobManager.ProcessFinishedJobs();

			/* Update loop for world */
			World* activeWorld = m_WorldManager.GetTopWorld();

#ifdef EDITOR
			if (activeWorld)
			{
				m_Editor.WorldPreTick(activeWorld);
				m_Editor.WorldTick(activeWorld, fElapsed);
			}

			bool bFixedStep = false;

			m_Platform.m_pFixedGameTimer->Update([&]
			{
				bFixedStep = true;

				if (activeWorld)
				{
					m_Editor.WorldFixedTick(activeWorld, GetFixedTimer());
					m_Editor.WorldPostFixedTick(activeWorld, GetFixedTimer());
				}
			});

			if (activeWorld)
				m_Editor.WorldPostTick(activeWorld, fElapsed);

			Camera* pCamera = nullptr;
			if (activeWorld) {
				pCamera = activeWorld->GetMainCamera();
			}

			/* pCamera is null-checked into existence above and was then
			   dereferenced unconditionally. A world with no main camera - or
			   no world at all, before a level finishes loading - crashed here. */
			if (pCamera != nullptr)
			{
			m_Platform.GetRenderContext()->SetCameraData(
				CameraRenderData(
					pCamera->GetMVP(),
					pCamera->GetMV(),
					pCamera->GetView(),
					pCamera->GetProjection(),

					pCamera->GetProjectionValue(),
					pCamera->GetAspectRatio(),
					pCamera->IsOrthographic(),
					pCamera->IsUpdated(),

					Vector4(pCamera->GetTransform()->GetPosition(), 1.0f),
					Vector4(pCamera->GetCameraOffset(), 1.0f)
				)
			);

			pCamera->SetRecalculated(false);
			}

			if (activeWorld)
			{
				if (bFixedStep)
					m_Editor.WorldRender(activeWorld, GetFixedTimer());

				activeWorld->OnDrawGizmos(fElapsed);
			}

			m_Editor.Render(fElapsed);
#else
			if (activeWorld)
			{
				activeWorld->PreTick();
				activeWorld->Tick(fElapsed);
			}

			bool bFixedStep = false;

			m_Platform.m_pFixedGameTimer->Update([&, activeWorld]
			{
				bFixedStep = true;

				if (activeWorld)
				{
					activeWorld->FixedTick(*m_Platform.m_pFixedGameTimer);
					activeWorld->PostFixedTick(*m_Platform.m_pFixedGameTimer);
				}
			});

			if (activeWorld)
				activeWorld->PostTick(fElapsed);

			Camera* pCamera = nullptr;
			if (activeWorld) {
				pCamera = activeWorld->GetMainCamera();
			}

			/* Same block as the fixed-step path above: pCamera is null-checked
			   into existence and was then dereferenced regardless. */
			if (pCamera != nullptr)
			{
			m_Platform.GetRenderContext()->SetCameraData(
				CameraRenderData(
					pCamera->GetMVP(),
					pCamera->GetMV(),
					pCamera->GetView(),
					pCamera->GetProjection(),

					pCamera->GetProjectionValue(),
					pCamera->GetAspectRatio(),
					pCamera->IsOrthographic(),
					pCamera->IsUpdated(),

					Vector4(pCamera->GetTransform()->GetPosition(), 1.0f),
					Vector4(pCamera->GetCameraOffset(), 1.0f)
				)
			);

			pCamera->SetRecalculated(false);
			}

			if (activeWorld)
			{
				if (bFixedStep)
				{
					activeWorld->Render(*m_Platform.m_pFixedGameTimer);
				}

				activeWorld->OnDrawGizmos(fElapsed);
			}
#endif

			OnDraw();

			/* --frames / --screenshot (LaunchOptions.h). Counted here rather
			   than in the outer while loop because that one spins on the game
			   timer and iterates many times per rendered frame - counting there
			   would make --frames mean something other than frames.

			   The capture happens on the last frame and *before* Present, so
			   what it reads is a target the GPU has finished with rather than
			   one being composited. */
			{
				const LaunchOptions& options = LaunchOptions::Get();

				if (options.GetFrameLimit() > 0)
				{
					++m_uiRenderedFrames;

					if (m_uiRenderedFrames >= options.GetFrameLimit())
					{
						if (options.HasScreenshot())
						{
							m_Platform.GetRenderContext()->CaptureTarget(
								options.GetScreenshotPass(), options.GetScreenshot());
						}

						m_bExit = true;
					}
				}
			}

			m_Platform.GetRenderContext()->Present();

#ifdef _ORBIS
			uint32_t uiFPS = GetTimer().GetFramesPerSecond();
			const char* pFPS = (std::to_string(uiFPS) + "FPS\n").c_str();
			std::printf(pFPS);
#endif
		});
	}

	// Stop application-level producers while all of their dependencies are alive.
	OnExit();

	// Editor and world teardown can cancel jobs and emit log events.
#ifdef EDITOR
	m_Editor.UnInitialize();
#endif
	
	m_WorldManager.ClearWorlds();

	// Resource destruction still needs the platform and filesystem backends.
	m_ResourceManager.Unload();

	// No consumers remain, so the global services can now be stopped safely.
	m_JobManager.Deinitialize();
	m_LoggingSystem.UnInitialize();

	m_Platform.Deinitialize();

	m_pFileSystem->Deinitialize();
	delete m_pFileSystem;
}

void Application::LoadSettings()
{
	if (!GetSerializer().FromJsonFile(m_Settings, "Settings.vgs"))
	{
		GetSerializer().ToJsonFile(m_Settings, "Settings.vgs", true);
	}

	/* Render quality is three layers and the order is the whole design:
	 *
	 *   1. Settings.vgs, above  - shipped engine configuration, one file for
	 *      every platform.
	 *   2. the platform's defaults - what a phone should do differently, which
	 *      cannot live in a file shared with the desktop build.
	 *   3. the player's own choices from the settings menu, restored last so
	 *      that anything they have deliberately changed survives both.
	 *
	 * A key the player has never touched is simply absent from PlayerPrefs, so
	 * layer 3 is not "all settings" - it is exactly the ones they chose, which
	 * is what lets a future change to the mobile defaults reach everyone who
	 * never opened the menu. */
	m_Settings.ApplyPlatformRenderDefaults();
	LoadRenderSettings();

	/* --render-quality, after all three layers so that it overrides them, and
	   nothing is written back - the same contract --uncapped has. It exists so
	   that pricing the shading levers does not mean editing PlayerPrefs and
	   remembering to put it back; LaunchOptions.h has the full reasoning. */
	switch (LaunchOptions::Get().GetQualityPreset())
	{
	case LaunchOptions::QualityPreset::E_LOW:
		m_Settings.SetShadowQuality(SHQ_HARD);
		m_Settings.SetAmbientQuality(AMQ_OFF);
		m_Settings.SetBounceLight(false);
		m_Settings.SetReflections(false);
		m_Settings.SetFXAA(false);
		break;

	case LaunchOptions::QualityPreset::E_HIGH:
		m_Settings.SetShadowQuality(SHQ_SOFT);
		m_Settings.SetAmbientQuality(AMQ_CONE);
		m_Settings.SetBounceLight(true);
		m_Settings.SetReflections(true);
		m_Settings.SetFXAA(true);
		break;

	case LaunchOptions::QualityPreset::E_UNSET:
		break;
	}

	/* --uncapped, after the file so that it overrides it - RENDERING_PLAN.md
	   phase 0b, and it should have been there from the start.

	   Settings.vgs asks for FIFO at the display's 60 Hz, which means a headless
	   benchmark run leaves the GPU idle for most of every frame. That is not a
	   neutral way to measure: at 1080p the voxel pass is about two milliseconds
	   of a sixteen-millisecond frame, the card clocks down accordingly, and the
	   same pass at 4K - which keeps it busy - runs at a *higher* clock. Costs
	   measured that way are not comparable across resolutions, and comparing
	   them across resolutions is exactly what this plan does. 7.3 found it by
	   sweeping a cone whose cost came out flat over a 4x change in pixel count.

	   Nothing is written back, so the file is untouched - the whole point of
	   LaunchOptions. */
	if (LaunchOptions::Get().IsUncapped())
	{
		m_Settings.SetVSync(false);
		m_Settings.SetFrameLimit(0.0);
	}
}

/* The player's render choices, in PlayerPrefs rather than in Settings.vgs.
 *
 * Settings.vgs is shipped content - on Android it is a file inside the APK that
 * gets extracted once per install, and rewriting it would be rewriting an
 * asset. PlayerPrefs is already the per-install store this game writes to (the
 * level unlocks live there), it is already on a writable path on every
 * platform, and its "has the key" test is exactly the question layer 3 asks.
 *
 * The keys are prefixed and spelled out rather than generated from the RTTR
 * property names: a renamed C++ member should not silently lose a player's
 * settings, and a name in a save file is a compatibility promise. */
namespace
{
	const char* k_pShadowQualityKey = "Render_ShadowQuality";
	const char* k_pShadowResolutionKey = "Render_ShadowResolution";
	const char* k_pShadowRayDistanceKey = "Render_ShadowRayDistance";
	const char* k_pAmbientQualityKey = "Render_AmbientQuality";
	const char* k_pBounceKey = "Render_BounceLight";
	const char* k_pReflectionsKey = "Render_Reflections";
	const char* k_pFXAAKey = "Render_FXAA";
	const char* k_pResolutionScaleKey = "Render_ResolutionScale";
	const char* k_pVSyncKey = "Render_VSync";
}

void Application::LoadRenderSettings()
{
	if (PlayerPrefs::HasKey(k_pShadowQualityKey))
	{
		m_Settings.SetShadowQuality(static_cast<ShadowQuality>(
			PlayerPrefs::GetInt(k_pShadowQualityKey, static_cast<int32_t>(SHQ_SOFT))));
	}

	if (PlayerPrefs::HasKey(k_pShadowResolutionKey))
	{
		m_Settings.SetSunShadowResolution(static_cast<uint32_t>(
			PlayerPrefs::GetInt(k_pShadowResolutionKey, 1024)));
	}

	if (PlayerPrefs::HasKey(k_pShadowRayDistanceKey))
		m_Settings.SetShadowRayDistance(PlayerPrefs::GetFloat(k_pShadowRayDistanceKey, 0.f));

	if (PlayerPrefs::HasKey(k_pAmbientQualityKey))
	{
		m_Settings.SetAmbientQuality(static_cast<AmbientQuality>(
			PlayerPrefs::GetInt(k_pAmbientQualityKey, static_cast<int32_t>(AMQ_CONE))));
	}

	if (PlayerPrefs::HasKey(k_pBounceKey))
		m_Settings.SetBounceLight(PlayerPrefs::GetInt(k_pBounceKey, 1) != 0);

	if (PlayerPrefs::HasKey(k_pReflectionsKey))
		m_Settings.SetReflections(PlayerPrefs::GetInt(k_pReflectionsKey, 1) != 0);

	if (PlayerPrefs::HasKey(k_pFXAAKey))
		m_Settings.SetFXAA(PlayerPrefs::GetInt(k_pFXAAKey, 1) != 0);

	if (PlayerPrefs::HasKey(k_pResolutionScaleKey))
		m_Settings.SetResolutionScale(PlayerPrefs::GetFloat(k_pResolutionScaleKey, 1.f));

	if (PlayerPrefs::HasKey(k_pVSyncKey))
		m_Settings.SetVSync(PlayerPrefs::GetInt(k_pVSyncKey, 0) != 0);
}

void Application::SaveRenderSettings()
{
	PlayerPrefs::SetInt(k_pShadowQualityKey, static_cast<int32_t>(m_Settings.GetShadowQuality()));
	PlayerPrefs::SetInt(k_pShadowResolutionKey, static_cast<int32_t>(m_Settings.GetSunShadowResolution()));
	PlayerPrefs::SetFloat(k_pShadowRayDistanceKey, m_Settings.GetShadowRayDistance());
	PlayerPrefs::SetInt(k_pAmbientQualityKey, static_cast<int32_t>(m_Settings.GetAmbientQuality()));
	PlayerPrefs::SetInt(k_pBounceKey, m_Settings.IsBounceLightEnabled() ? 1 : 0);
	PlayerPrefs::SetInt(k_pReflectionsKey, m_Settings.IsReflectionEnabled() ? 1 : 0);
	PlayerPrefs::SetInt(k_pFXAAKey, m_Settings.IsFXAAEnabled() ? 1 : 0);
	PlayerPrefs::SetFloat(k_pResolutionScaleKey, m_Settings.GetResolutionScale());
	PlayerPrefs::SetInt(k_pVSyncKey, m_Settings.IsVSyncEnabled() ? 1 : 0);

	PlayerPrefs::Save();
}
