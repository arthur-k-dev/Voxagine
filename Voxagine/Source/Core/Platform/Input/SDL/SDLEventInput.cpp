#include "pch.h"
#include "SDLEventInput.h"

#include "Core/Platform/Input/SDL/SDLMouse.h"

#include <SDL3/SDL.h>

#include <cmath>

namespace
{
	Vector2 Centroid(const Vector2* pPositions, size_t uiCount)
	{
		Vector2 sum(0.f);

		for (size_t i = 0; i < uiCount; ++i)
			sum += pPositions[i];

		return sum / static_cast<float>(uiCount);
	}
}

SDLEventInput& SDLEventInput::Get()
{
	static SDLEventInput s_Instance;
	return s_Instance;
}

static_assert(SDL_SCANCODE_COUNT <= 512, "the key-press latch is indexed by scancode");

void SDLEventInput::BeginFrame()
{
	m_Gesture.Drag = Vector2(0.f);
	m_Gesture.PinchScale = 1.f;

	for (bool& bPressed : m_bKeyPressedThisFrame)
		bPressed = false;

	/* Advance the tap replay. EndFrame is what puts the press and the release
	   into the reported state; this only moves it along, so each of the two
	   states is visible for exactly one whole frame. */
	switch (m_TapLatch)
	{
	case E_TAP_HOLD:
		m_TapLatch = E_TAP_RELEASE;
		break;

	case E_TAP_RELEASE:
		m_TapLatch = E_TAP_IDLE;
		break;

	default:
		break;
	}
}

void SDLEventInput::Handle(const SDL_Event& event)
{
	switch (event.type)
	{
	case SDL_EVENT_MOUSE_WHEEL:
		/* The one line that makes the wheel work at all. MouseController diffs
		   a running total, which is why this accumulates rather than reporting
		   a delta - see Mouse::AddScrollDelta.

		   This is also what a Magic Trackpad's and a Magic Keyboard trackpad's
		   two-finger scroll arrives as: iPadOS and macOS both report indirect
		   pointing devices as a mouse, so nothing here is touch-specific. */
		Mouse::Get().AddScrollDelta(event.wheel.y);
		break;

	case SDL_EVENT_TEXT_INPUT:
		/* Composed text, not scancodes: the keyboard snapshot in SDLKeyboard
		   knows a key is down but not what character a layout, a dead key or
		   an IME turned it into. ImGui text fields need the character. */
		m_TextInput += event.text.text;
		break;

	case SDL_EVENT_KEY_DOWN:
		if (event.key.scancode >= 0 &&
		    static_cast<size_t>(event.key.scancode) < k_uiScancodeCount)
			m_bKeyPressedThisFrame[event.key.scancode] = true;
		break;

	case SDL_EVENT_MOUSE_MOTION:
	case SDL_EVENT_MOUSE_BUTTON_DOWN:
		/* A real pointing device wants the cursor back. Checked against
		   SDL_TOUCH_MOUSEID even though main.cpp turns touch-to-mouse
		   synthesis off, because handing the pointer back to a *synthesized*
		   mouse event would undo the touch cursor on the very touch that
		   produced it. */
		if (event.motion.which != SDL_TOUCH_MOUSEID)
			m_bTouchOwnsPointer = false;
		break;

	case SDL_EVENT_FINGER_DOWN:
		OnFingerDown(event);
		break;

	case SDL_EVENT_FINGER_MOTION:
		OnFingerMotion(event);
		break;

	case SDL_EVENT_FINGER_UP:
	case SDL_EVENT_FINGER_CANCELED:
		OnFingerUp(event);
		break;

	default:
		break;
	}
}

std::string SDLEventInput::TakeTextInput()
{
	std::string taken;
	taken.swap(m_TextInput);

	return taken;
}

bool SDLEventInput::WasKeyPressedThisFrame(int iScancode) const
{
	if (iScancode < 0 || static_cast<size_t>(iScancode) >= k_uiScancodeCount)
		return false;

	return m_bKeyPressedThisFrame[iScancode];
}

SDLEventInput::Finger* SDLEventInput::FindFinger(int64_t id)
{
	for (size_t i = 0; i < m_uiFingerCount; ++i)
	{
		if (m_Fingers[i].Id == id)
			return &m_Fingers[i];
	}

	return nullptr;
}

Vector2 SDLEventInput::EventPositionInPixels(const SDL_Event& event) const
{
	/* SDL reports fingers in 0..1 across the window. Everything downstream -
	   the ImGui cursor, the editor's picking ray - works in window pixels, and
	   SDLWindowContext::GetMousePositionInPixels is what a real mouse goes
	   through, so match its units here rather than converting twice. */
	SDL_Window* pWindow = SDL_GetWindowFromID(event.tfinger.windowID);

	int iWidth = 0;
	int iHeight = 0;

	if (pWindow != nullptr)
		SDL_GetWindowSizeInPixels(pWindow, &iWidth, &iHeight);

	return Vector2(event.tfinger.x * static_cast<float>(iWidth),
	               event.tfinger.y * static_cast<float>(iHeight));
}

void SDLEventInput::OnFingerDown(const SDL_Event& event)
{
	m_bTouchOwnsPointer = true;

	if (m_uiFingerCount == 0)
		m_bPressReported = false;

	if (m_uiFingerCount < k_uiMaxFingers)
	{
		Finger& finger = m_Fingers[m_uiFingerCount++];
		finger.Id = static_cast<int64_t>(event.tfinger.fingerID);
		finger.Position = EventPositionInPixels(event);
	}

	if (m_uiFingerCount > 1)
		m_bPointerCancelled = true;

	OnFingerCountChanged();
}

void SDLEventInput::OnFingerMotion(const SDL_Event& event)
{
	Finger* pFinger = FindFinger(static_cast<int64_t>(event.tfinger.fingerID));

	/* A finger past k_uiMaxFingers was never recorded, so it has no slot to
	   update and is correctly ignored. */
	if (pFinger == nullptr)
		return;

	pFinger->Position = EventPositionInPixels(event);
}

void SDLEventInput::OnFingerUp(const SDL_Event& event)
{
	const int64_t id = static_cast<int64_t>(event.tfinger.fingerID);

	for (size_t i = 0; i < m_uiFingerCount; ++i)
	{
		if (m_Fingers[i].Id != id)
			continue;

		m_Fingers[i] = m_Fingers[m_uiFingerCount - 1];
		--m_uiFingerCount;
		break;
	}

	/* Only once the glass is empty. Lifting one finger of a two-finger orbit
	   leaves the other down, and that residue must not become a pointer press
	   mid-gesture. */
	if (m_uiFingerCount == 0)
	{
		/* A press nobody ever saw. Replay it over the next two frames - see
		   TapLatch. A press that was already reported needs none of this: the
		   release is simply the absence of fingers. */
		if (!m_bPointerCancelled && !m_bPressReported)
			m_TapLatch = E_TAP_HOLD;

		m_bPointerCancelled = false;
	}

	OnFingerCountChanged();
}

void SDLEventInput::OnFingerCountChanged()
{
	m_Gesture.Fingers = static_cast<int>(m_uiFingerCount);

	/* Rebuilt rather than carried across a change in finger count: a second
	   finger landing moves the centroid by half the distance between the two,
	   which as a delta would be an instant camera snap. */
	m_bAnchorValid = false;

	/* And the two-finger gesture starts over. Going 2 -> 3 -> 2 fingers is a
	   new gesture and gets to be a different one. */
	m_TwoFingerMode = E_TWO_FINGER_UNDECIDED;
	m_fCandidateOrbit = 0.f;
	m_fCandidatePinch = 0.f;

	if (m_uiFingerCount == 1)
		m_PointerPosition = m_Fingers[0].Position;
}

void SDLEventInput::EndFrame()
{
	/* The tap replay wins over the live state, because by definition there is
	   no live state left - the finger is already up. */
	if (m_TapLatch != E_TAP_IDLE)
	{
		m_bPointerPressed = m_TapLatch == E_TAP_HOLD;
		return;
	}

	m_bPointerPressed = m_uiFingerCount == 1 && !m_bPointerCancelled;

	if (m_bPointerPressed)
		m_bPressReported = true;

	if (m_uiFingerCount == 0)
	{
		m_bAnchorValid = false;
		return;
	}

	Vector2 positions[k_uiMaxFingers];

	for (size_t i = 0; i < m_uiFingerCount; ++i)
		positions[i] = m_Fingers[i].Position;

	const Vector2 centroid = Centroid(positions, m_uiFingerCount);

	/* Spread only means anything for a pinch, which is a two-finger gesture. */
	const float fSpread = m_uiFingerCount == 2
		? glm::distance(m_Fingers[0].Position, m_Fingers[1].Position)
		: 0.f;

	if (m_uiFingerCount == 1)
		m_PointerPosition = m_Fingers[0].Position;

	if (m_bAnchorValid)
	{
		const Vector2 movement = centroid - m_AnchorCentroid;

		if (m_uiFingerCount == 2)
		{
			m_fCandidateOrbit += glm::length(movement);
			m_fCandidatePinch += std::fabs(fSpread - m_AnchorSpread);

			if (m_TwoFingerMode == E_TWO_FINGER_UNDECIDED &&
			    std::fmax(m_fCandidateOrbit, m_fCandidatePinch) >= k_fTwoFingerDecisionPixels)
			{
				m_TwoFingerMode = m_fCandidatePinch > m_fCandidateOrbit
					? E_TWO_FINGER_PINCH
					: E_TWO_FINGER_ORBIT;
			}

			if (m_TwoFingerMode == E_TWO_FINGER_ORBIT)
			{
				m_Gesture.Drag += movement;
			}
			else if (m_TwoFingerMode == E_TWO_FINGER_PINCH &&
			         m_AnchorSpread > 1.f && fSpread > 1.f)
			{
				m_Gesture.PinchScale *= fSpread / m_AnchorSpread;
			}
		}
		else
		{
			/* One finger is a pointer drag and three are a pan. Neither needs
			   arbitrating, and the consumer tells them apart by Fingers. */
			m_Gesture.Drag += movement;
		}
	}

	m_AnchorCentroid = centroid;
	m_AnchorSpread = fSpread;
	m_bAnchorValid = true;
}
