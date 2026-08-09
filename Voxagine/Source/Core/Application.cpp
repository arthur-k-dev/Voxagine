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

	LoadSettings();

#ifdef EDITOR
	/* The lock is a game presentation choice; the editor wants the whole
	   window. Play mode still uses the camera's own aspect ratio. */
	m_Settings.SetLockedAspectRatio(0.f);
#elif defined(VOXAGINE_MOBILE)
	/* Same on a phone, for a different reason. The lock letterboxes the frame
	   to 16:9 whatever the display is, and a modern phone is nearer 20:9 - so
	   a locked game would run with black bars down a fifth of the screen while
	   the device it is on has no bars anywhere else. Rendering at the device's
	   own ratio widens the view instead, which is the right trade for a
	   top-down game: it shows more of the arena rather than stretching it.

	   The screen is also the only place a phone has to put anything, so
	   spending a fifth of it on nothing is worse here than on a monitor. */
	m_Settings.SetLockedAspectRatio(0.f);

	/* Half resolution by default, and this is the single biggest performance
	   lever the engine has on a phone.
	 *
	 * Measured on a Galaxy S23 (Adreno 740) at native 2340x1080, in an arena:
	 * the Voxel pass alone is 108.8 ms of a ~138 ms frame - 79% of it - and it
	 * is fragment-bound, so it scales with pixel count almost linearly. Sun
	 * Shadow (a fixed 1024^2) and the full-resolution post and UI passes do
	 * not scale, which is why this is worth about 2.5x rather than 4x.
	 *
	 * Set from code rather than from Settings.vgs on purpose: that file is
	 * shared with the desktop build, and a half-resolution default is very
	 * much not wanted on a 4070. */
	m_Settings.SetResolutionScale(0.5f);
#endif

	m_JobManager.Initialize();
	m_Platform.Initialize();

	OnCreate();

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
