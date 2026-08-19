#pragma once

#include "Core/Platform/Window/WindowContext.h"

#include <Core/Platform/Rendering/Vulkan/VulkanAPI.h>

#include <atomic>
#include <string>
#include <vector>

struct SDL_Window;
union SDL_Event;

/* SDL3-backed window. Replaces WINWindowContext and its WndProc; SDL owns the
   event loop, so Poll() drains SDL's queue instead of PeekMessage. */
class SDLWindowContext : public WindowContext
{
public:
	SDLWindowContext(Platform* pPlatform);
	virtual ~SDLWindowContext();

	virtual void Initialize() override;

	/* The SDL_Window itself. Vulkan surface creation goes through
	   CreateSurface below rather than through a raw platform handle. */
	virtual void* GetHandle() override { return m_pWindow; }

	virtual void Poll() override;


	/* Events */
	virtual void OnMove() override;

	/* Instance extensions Vulkan needs to present to this window. */
	std::vector<const char*> GetRequiredInstanceExtensions() const;

	bool CreateSurface(VkInstance instance, VkSurfaceKHR* pSurface) const;

	bool ShouldClose() const { return m_bShouldClose; }

	/* Cursor in framebuffer pixels. SDL reports logical units, which differ
	   from pixels on a high-density window, and everything downstream wants
	   pixels. */
	static void GetMousePositionInPixels(float* pfX, float* pfY);

	/* True from the moment SDL reports a resize until the renderer clears it
	   by rebuilding the swapchain. */
	bool ConsumeResizeRequest();

	virtual bool IsBackgrounded() const override { return m_bBackgrounded.load(std::memory_order_acquire); }

	/* True exactly once, on the frame after the transition, then it clears
	   itself - an edge, not a level. The main loop polls these once per frame
	   rather than reacting inside the event watch below, because the watch can
	   fire from a thread that is not the one holding every Vulkan handle, and
	   there is exactly one thread that is allowed to touch those. */
	virtual bool ConsumeEnteredBackground() override;
	virtual bool ConsumeEnteredForeground() override;

private:
	/* Events */
	virtual void OnResize(uint32_t a_uiWidth, uint32_t a_uiHeight, IVector2 resolutionDelta) override;
	virtual void OnFullscreenChanged(bool bFullscreen) override;

	void CreateWindow();

	/* SDL's own documentation requires the four app-lifecycle events
	   (WILL/DID_ENTER_BACKGROUND, WILL/DID_ENTER_FOREGROUND) to be handled
	   through SDL_AddEventWatch rather than read out of the normal
	   SDL_PollEvent queue - because on some platforms the process can be
	   frozen before the main loop's next Poll() call ever runs, so an event
	   sitting in the ordinary queue may simply never be seen. The watch
	   callback only touches these two atomics; every actual consequence -
	   pausing audio, tearing down and rebuilding the Vulkan surface - happens
	   on the main thread in Application::Run, which is where those objects are
	   otherwise exclusively owned. */
	/* SDLCALL-decorated in the .cpp only, matching the definition SDL's
	   function-pointer type actually expects - this header does not include
	   SDL.h, so the macro is not available here. On every platform this
	   engine targets it is either __cdecl (already the default) or empty, so
	   the declaration and definition remain compatible without it. */
	static bool EventWatch(void* pUserData, SDL_Event* pEvent);

	SDL_Window* m_pWindow = nullptr;

	bool m_bShouldClose = false;
	bool m_bResizeRequested = false;

	std::atomic<bool> m_bBackgrounded{false};
	std::atomic<bool> m_bEnteredBackground{false};
	std::atomic<bool> m_bEnteredForeground{false};
};
