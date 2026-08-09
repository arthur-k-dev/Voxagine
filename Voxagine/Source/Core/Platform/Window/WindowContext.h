#pragma once

#include <stdint.h>
#include "Core/Math.h"

#include "Core/Event.h"

class Settings;
class Platform;

class WindowContext
{
public:
	WindowContext(Platform* pPlatform);
	virtual ~WindowContext();

	virtual void Initialize();

	virtual void* GetHandle() = 0;

	const UVector2& GetPosition() const;
	const UVector2& GetSize() const;

	virtual	void Poll() = 0;

	/* Events */
	virtual void OnMove() = 0;

	/* True between the OS taking the app out of the foreground and bringing it
	   back - Android's onPause/onResume, iOS's equivalent. Always false on
	   desktop, which has no such concept; SDLWindowContext is the only
	   override, and only on mobile does anything ever set it.

	   What actually rides on this, and why it is not cosmetic: Android
	   destroys the app's ANativeWindow while backgrounded, which is the same
	   handle the live VkSurfaceKHR was created from - so the surface does not
	   merely become suboptimal the way a resize does, it becomes invalid, and
	   presenting to it or even querying its capabilities is undefined. See
	   RenderContext::SuspendForBackground/ResumeFromBackground. */
	virtual bool IsBackgrounded() const { return false; }

	/* Each true exactly once, on the frame after the transition - an edge, not
	   a level, which is what makes it safe for Application::Run to act on
	   without a second flag to say "already handled this one". */
	virtual bool ConsumeEnteredBackground() { return false; }
	virtual bool ConsumeEnteredForeground() { return false; }
	
	Platform* GetPlatform() const { return m_pPlatform; };

protected:

	/* Events */
	virtual void OnResize(uint32_t a_uiWidth, uint32_t a_uiHeight, IVector2 resolutionDelta) = 0;
	virtual void OnFullscreenChanged(bool bFullscreen) = 0;

	Platform* m_pPlatform;

	UVector2 m_v2Position;
	UVector2 m_v2Size;
};