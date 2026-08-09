#include "pch.h"
#include "SDLWindowContext.h"

#include "Core/LaunchOptions.h"

#include "Core/Application.h"
#include "Core/Platform/Platform.h"
#include "Core/Platform/Rendering/RenderContext.h"
#include "Core/Settings.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>

SDLWindowContext::SDLWindowContext(Platform* pPlatform) : WindowContext(pPlatform)
{
	if (!SDL_InitSubSystem(SDL_INIT_VIDEO))
	{
		fprintf(stderr, "[sdl] SDL_InitSubSystem(VIDEO) failed: %s\n", SDL_GetError());
		return;
	}

	CreateWindow();
}

SDLWindowContext::~SDLWindowContext()
{
	SDL_RemoveEventWatch(&SDLWindowContext::EventWatch, this);

	if (m_pWindow != nullptr)
	{
		SDL_DestroyWindow(m_pWindow);
		m_pWindow = nullptr;
	}

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

void SDLWindowContext::CreateWindow()
{
	Settings& settings = m_pPlatform->GetApplication()->GetSettings();

	/* Without HIGH_PIXEL_DENSITY a fractionally scaled display gives us a
	   logical-size surface that the compositor upscales, softening the whole
	   frame. With it, SDL_GetWindowSizeInPixels is the true size. */
	SDL_WindowFlags flags = SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY;
	if (settings.IsFullscreen())
		flags |= SDL_WINDOW_FULLSCREEN;

	UVector2 resolution = settings.m_v2InitialWindowSize;

	/* --hidden: render everything, display nothing (LaunchOptions.h). Vulkan
	   still gets a real surface, which is what SDL_VIDEODRIVER=offscreen cannot
	   provide on this driver.

	   Fullscreen has to come *off* with it, and --size only means anything
	   here: a mapped window is the compositor's to size - Hyprland tiles it and
	   ignores the request - but an unmapped one is not, so this is the only
	   place a measurement run can pin its own resolution. */
	const LaunchOptions& options = LaunchOptions::Get();

	if (options.IsHidden())
	{
		flags |= SDL_WINDOW_HIDDEN;
		flags &= ~SDL_WINDOW_FULLSCREEN;

		if (options.HasSize())
			resolution = UVector2(options.GetWidth(), options.GetHeight());
	}

	m_pWindow = SDL_CreateWindow(settings.GetTitle().c_str(),
	                             static_cast<int>(resolution.x),
	                             static_cast<int>(resolution.y),
	                             flags);

	if (m_pWindow == nullptr)
	{
		fprintf(stderr, "[sdl] SDL_CreateWindow failed: %s\n", SDL_GetError());
		return;
	}

	m_v2Size = resolution;
}

void SDLWindowContext::Initialize()
{
	WindowContext::Initialize();

	if (m_pWindow == nullptr)
		return;

	int iWidth = 0;
	int iHeight = 0;
	SDL_GetWindowSizeInPixels(m_pWindow, &iWidth, &iHeight);
	m_v2Size = UVector2(static_cast<uint32_t>(iWidth), static_cast<uint32_t>(iHeight));

	int iX = 0;
	int iY = 0;
	SDL_GetWindowPosition(m_pWindow, &iX, &iY);
	m_v2Position = UVector2(static_cast<uint32_t>(iX), static_cast<uint32_t>(iY));

	/* See the comment on EventWatch for why this has to be a watch rather
	   than a case in Poll()'s switch. `this` outlives the watch: it is removed
	   in the destructor before the object it points at stops existing. */
	SDL_AddEventWatch(&SDLWindowContext::EventWatch, this);
}

bool SDLCALL SDLWindowContext::EventWatch(void* pUserData, SDL_Event* pEvent)
{
	SDLWindowContext* pSelf = static_cast<SDLWindowContext*>(pUserData);

	switch (pEvent->type)
	{
	case SDL_EVENT_WILL_ENTER_BACKGROUND:
		/* Set going into the background, not coming out of it: this is the
		   point at which the surface is still valid and Vulkan calls are still
		   safe, which DID_ENTER_BACKGROUND does not promise - Android can
		   destroy the ANativeWindow at any point after onPause, and this is
		   the engine's only warning that onPause has happened. */
		pSelf->m_bBackgrounded.store(true, std::memory_order_release);
		pSelf->m_bEnteredBackground.store(true, std::memory_order_release);
		break;

	case SDL_EVENT_DID_ENTER_FOREGROUND:
		pSelf->m_bBackgrounded.store(false, std::memory_order_release);
		pSelf->m_bEnteredForeground.store(true, std::memory_order_release);
		break;

	default:
		break;
	}

	/* A watch never consumes the event; SDL_PollEvent's normal queue still
	   sees everything. */
	return true;
}

bool SDLWindowContext::ConsumeEnteredBackground()
{
	return m_bEnteredBackground.exchange(false, std::memory_order_acq_rel);
}

bool SDLWindowContext::ConsumeEnteredForeground()
{
	return m_bEnteredForeground.exchange(false, std::memory_order_acq_rel);
}

void SDLWindowContext::GetMousePositionInPixels(float* pfX, float* pfY)
{
	SDL_GetMouseState(pfX, pfY);

	/* Null when the cursor is outside our windows; logical is the best
	   answer there. */
	const float fDensity = SDL_GetWindowPixelDensity(SDL_GetMouseFocus());

	if (fDensity <= 0.f)
		return;

	*pfX *= fDensity;
	*pfY *= fDensity;
}

std::vector<const char*> SDLWindowContext::GetRequiredInstanceExtensions() const
{
	uint32_t uiCount = 0;
	const char* const* ppNames = SDL_Vulkan_GetInstanceExtensions(&uiCount);

	if (ppNames == nullptr)
	{
		fprintf(stderr, "[sdl] SDL_Vulkan_GetInstanceExtensions failed: %s\n", SDL_GetError());
		return {};
	}

	return std::vector<const char*>(ppNames, ppNames + uiCount);
}

bool SDLWindowContext::CreateSurface(VkInstance instance, VkSurfaceKHR* pSurface) const
{
	if (m_pWindow == nullptr)
		return false;

	if (!SDL_Vulkan_CreateSurface(m_pWindow, instance, nullptr, pSurface))
	{
		fprintf(stderr, "[sdl] SDL_Vulkan_CreateSurface failed: %s\n", SDL_GetError());
		return false;
	}

	return true;
}

void SDLWindowContext::Poll()
{
	SDL_Event event;

	while (SDL_PollEvent(&event))
	{
		switch (event.type)
		{
		case SDL_EVENT_QUIT:
		case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
			m_bShouldClose = true;
			m_pPlatform->GetApplication()->Exit();
			break;

		case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
		{
			const uint32_t uiWidth = static_cast<uint32_t>(event.window.data1);
			const uint32_t uiHeight = static_cast<uint32_t>(event.window.data2);


			const IVector2 delta(static_cast<int32_t>(uiWidth) - static_cast<int32_t>(m_v2Size.x),
			                     static_cast<int32_t>(uiHeight) - static_cast<int32_t>(m_v2Size.y));

			OnResize(uiWidth, uiHeight, delta);

			/* Drives the swapchain and render target recreation; under Win32
			   this went through the message pump instead. */
			if (m_pPlatform->GetRenderContext() != nullptr)
				m_pPlatform->GetRenderContext()->OnResize(uiWidth, uiHeight);

			break;
		}

		case SDL_EVENT_WINDOW_MOVED:
			OnMove();
			break;

		default:
			break;
		}
	}
}

void SDLWindowContext::OnMove()
{
	/* Only store non-fullscreen window position */
	if (m_pPlatform->GetApplication()->GetSettings().IsFullscreen())
		return;

	int iX = 0;
	int iY = 0;
	SDL_GetWindowPosition(m_pWindow, &iX, &iY);

	if (iX > 0 && iY > 0)
		m_v2Position = UVector2(static_cast<uint32_t>(iX), static_cast<uint32_t>(iY));
}

void SDLWindowContext::OnResize(uint32_t a_uiWidth, uint32_t a_uiHeight, IVector2 /*resolutionDelta*/)
{
	m_v2Size = UVector2(a_uiWidth, a_uiHeight);
	m_bResizeRequested = true;
}

void SDLWindowContext::OnFullscreenChanged(bool bFullscreen)
{
	if (m_pWindow == nullptr)
		return;

	SDL_SetWindowFullscreen(m_pWindow, bFullscreen);
	m_bResizeRequested = true;
}

bool SDLWindowContext::ConsumeResizeRequest()
{
	const bool bRequested = m_bResizeRequested;
	m_bResizeRequested = false;

	return bRequested;
}
