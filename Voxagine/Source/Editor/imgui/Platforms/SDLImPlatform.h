#pragma once

#include "Editor/imgui/Platforms/ImPlatform.h"

#include "Core/Platform/Input/SDL/SDLEventInput.h"

struct ImGuiIO;
class SDLWindowContext;

/* Feeds ImGui its display size and input, replacing W32ImPlatform's WndProc
   hooks with SDL events. */
class SDLImPlatform : public ImPlatform
{
public:
	SDLImPlatform(SDLWindowContext* pWindow);

	virtual void Initialize() override;
	virtual void NewFrame() override;
	virtual float GetDisplayScale() const override;

private:
	/* Starts and stops SDL text input to follow ImGui's io.WantTextInput. On
	   iOS that is also what raises and dismisses the on-screen keyboard. */
	void UpdateTextInput();

	/* Turns a finger drag over an ImGui window into scrolling, unless the
	   press grabbed a widget. */
	void UpdateTouchScroll(ImGuiIO& io, const SDLEventInput::Gesture& gesture);

	/* What the finger currently down turned out to be doing. Decided once per
	   gesture and held, so a flick that passes over a button keeps scrolling
	   rather than flickering between the two. */
	enum TouchDrag
	{
		E_TOUCH_DRAG_NONE,
		E_TOUCH_DRAG_UNDECIDED,
		E_TOUCH_DRAG_SCROLL,
		E_TOUCH_DRAG_WIDGET
	};

	SDLWindowContext* m_pWindow = nullptr;

	bool m_bTextInputActive = false;

	TouchDrag m_TouchDrag = E_TOUCH_DRAG_NONE;
	float m_fTouchDragDistance = 0.f;
};
