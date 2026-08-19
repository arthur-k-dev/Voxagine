#include "pch.h"
#include "SDLImPlatform.h"

#include "Core/Platform/Input/SDL/SDLEventInput.h"
#include "Core/Platform/Input/SDL/SDLMouse.h"
#include "Core/Platform/Window/SDL/SDLWindowContext.h"
#include "External/imgui/imgui.h"

#include <SDL3/SDL.h>

#include <cmath>

namespace
{
	/* Pixels of drag per wheel notch. ImGui scrolls by whole notches, so this
	   only has to feel like the rest of the system: a short flick moves a list
	   a line or two. */
	constexpr float k_fTouchScrollPixelsPerNotch = 40.f;

	/* How far a finger travels before an undecided drag becomes a scroll.
	   Below this it is still a tap, and a tap has to be able to land on a
	   button without the button sliding away first. */
	constexpr float k_fTouchScrollThresholdPixels = 8.f;
}

SDLImPlatform::SDLImPlatform(SDLWindowContext* pWindow) : m_pWindow(pWindow)
{
}

void SDLImPlatform::Initialize()
{
	ImGuiIO& io = ImGui::GetIO();
	io.BackendPlatformName = "Voxagine SDL3";
}

void SDLImPlatform::UpdateTextInput()
{
	/* SDL emits SDL_EVENT_TEXT_INPUT only between StartTextInput and
	   StopTextInput, and nothing in this engine had ever called either - so
	   every ImGui text field was unusable on every platform. Names, paths,
	   any numeric field typed rather than dragged: the keys registered as
	   KeysDown but produced no characters.
	 *
	 * Driven by io.WantTextInput rather than switched on once at startup, and
	 * on iOS that distinction is the whole game: SDL_StartTextInput is also
	 * what raises the on-screen keyboard, so leaving it on permanently would
	 * cover half the editor with a keyboard nobody asked for. Following the
	 * flag means the keyboard appears when a field is focused and leaves when
	 * it is not - and with a Magic Keyboard attached iOS suppresses the
	 * on-screen one by itself, so the same code is right either way.
	 *
	 * WantTextInput is set during the previous frame's Render, so this acts on
	 * it one frame late. That is invisible next to the keyboard's own
	 * animation. */
	if (m_pWindow == nullptr)
		return;

	SDL_Window* pWindow = static_cast<SDL_Window*>(m_pWindow->GetHandle());

	if (pWindow == nullptr)
		return;

	const bool bWanted = ImGui::GetIO().WantTextInput;

	if (bWanted == m_bTextInputActive)
		return;

	if (bWanted)
		SDL_StartTextInput(pWindow);
	else
		SDL_StopTextInput(pWindow);

	m_bTextInputActive = bWanted;
}

void SDLImPlatform::NewFrame()
{
	if (m_pWindow == nullptr)
		return;

	ImGuiIO& io = ImGui::GetIO();

	const UVector2 size = m_pWindow->GetSize();
	io.DisplaySize = ImVec2(static_cast<float>(size.x), static_cast<float>(size.y));

	/* Through Mouse rather than SDL_GetMouseState directly, so the touch
	   pointer reaches ImGui by the same route it reaches the rest of the
	   editor. Reading SDL here and Mouse everywhere else is how the cursor and
	   the click end up disagreeing about where the user is. */
	const Mouse::State mouse = Mouse::Get().GetState();

	io.MousePos = ImVec2(static_cast<float>(mouse.x), static_cast<float>(mouse.y));
	io.MouseDown[0] = mouse.leftButton;
	io.MouseDown[1] = mouse.rightButton;
	io.MouseDown[2] = mouse.middleButton;

	UpdateTextInput();

	SDLEventInput& eventInput = SDLEventInput::Get();

	/* UTF-8 in, codepoints out. Decoding this a byte at a time would turn one
	   accented character into two Latin-1 ones. */
	const std::string text = eventInput.TakeTextInput();

	if (!text.empty())
		io.AddInputCharactersUTF8(text.c_str());

	UpdateTouchScroll(io, eventInput.GetGesture());
}

void SDLImPlatform::UpdateTouchScroll(ImGuiIO& io, const SDLEventInput::Gesture& gesture)
{
	/* Dragging a list with one finger scrolls it. That is what a finger does
	 * to a list on every touch platform, and the first cut of this required
	 * two - which is a trackpad idiom, not a touchscreen one.
	 *
	 * The catch is that one finger is also how you drag a slider, move a
	 * window by its title bar and operate the gizmo, and those have to keep
	 * working. So the gesture is not classified by where it started but by
	 * what it grabbed: ImGui decides on the press frame whether the touch
	 * landed on something interactive, and IsAnyItemActive() reports that on
	 * the frame after. If nothing took it and the finger has since moved, the
	 * drag was over inert content and becomes a scroll.
	 *
	 * Committed once per gesture, so a scroll that drifts over a button
	 * mid-flick keeps scrolling. */
	if (gesture.Fingers == 0)
	{
		m_TouchDrag = E_TOUCH_DRAG_NONE;
		return;
	}

	/* Two fingers still scroll, unconditionally and without the arbitration:
	   nothing is being dragged with two fingers, so there is nothing to lose
	   to it. Over the viewport this same gesture orbits instead - EditorCamera
	   reads it too and skips it while the cursor is over editor windows, so
	   only one of the two ever fires. */
	if (gesture.Fingers == 2)
	{
		if (io.WantCaptureMouse)
			io.MouseWheel += -gesture.Drag.y / k_fTouchScrollPixelsPerNotch;

		return;
	}

	if (gesture.Fingers != 1 || !io.WantCaptureMouse)
	{
		m_TouchDrag = E_TOUCH_DRAG_NONE;
		return;
	}

	if (m_TouchDrag == E_TOUCH_DRAG_NONE)
	{
		m_TouchDrag = E_TOUCH_DRAG_UNDECIDED;
		m_fTouchDragDistance = 0.f;
	}

	if (m_TouchDrag == E_TOUCH_DRAG_UNDECIDED)
	{
		m_fTouchDragDistance += std::fabs(gesture.Drag.x) + std::fabs(gesture.Drag.y);

		if (ImGui::IsAnyItemActive())
			m_TouchDrag = E_TOUCH_DRAG_WIDGET;
		else if (m_fTouchDragDistance >= k_fTouchScrollThresholdPixels)
			m_TouchDrag = E_TOUCH_DRAG_SCROLL;
	}

	if (m_TouchDrag != E_TOUCH_DRAG_SCROLL)
		return;

	/* Let go of the button, or the list scrolls under a finger that is still
	   notionally pressing whatever it started on. */
	io.MouseDown[0] = false;

	/* Inverted, because dragging content upwards scrolls a list downwards. */
	io.MouseWheel += -gesture.Drag.y / k_fTouchScrollPixelsPerNotch;
	io.MouseWheelH += -gesture.Drag.x / k_fTouchScrollPixelsPerNotch;
}

float SDLImPlatform::GetDisplayScale() const
{
	if (m_pWindow == nullptr)
		return 1.0f;

	SDL_Window* pWindow = static_cast<SDL_Window*>(m_pWindow->GetHandle());
	if (pWindow == nullptr)
		return 1.0f;

	const float fDisplayScale = SDL_GetWindowDisplayScale(pWindow);
	return fDisplayScale > 0.0f ? fDisplayScale : 1.0f;
}

float SDLImPlatform::GetDisplayScale() const
{
	if (m_pWindow == nullptr)
		return 1.0f;

	SDL_Window* pWindow = static_cast<SDL_Window*>(m_pWindow->GetHandle());
	if (pWindow == nullptr)
		return 1.0f;

	const float fDisplayScale = SDL_GetWindowDisplayScale(pWindow);
	return fDisplayScale > 0.0f ? fDisplayScale : 1.0f;
}
