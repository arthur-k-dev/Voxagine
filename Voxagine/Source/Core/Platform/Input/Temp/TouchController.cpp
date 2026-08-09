#include "pch.h"
#include "TouchController.h"

#include "Core/Platform/Window/WindowContext.h"

#include <SDL3/SDL.h>

#include <algorithm>

namespace
{
	/* Maps a point designed against the whole window into the safe area. */
	Vector2 ToSafeArea(const Vector2& v2Point, const Vector2& v2Origin, const Vector2& v2Size)
	{
		return v2Origin + v2Point * v2Size;
	}
}

void TouchController::UpdateEffectiveLayout()
{
	m_EffectiveLayout = m_Layout;

	SDL_Window* pWindow = GetWindowContext() != nullptr
		? static_cast<SDL_Window*>(GetWindowContext()->GetHandle())
		: nullptr;

	if (pWindow == nullptr)
		return;

	int iWidth = 0;
	int iHeight = 0;
	SDL_GetWindowSize(pWindow, &iWidth, &iHeight);

	SDL_Rect safe{};

	if (iWidth <= 0 || iHeight <= 0 || !SDL_GetWindowSafeArea(pWindow, &safe))
		return;

	/* SDL reports the whole window when the platform has no insets, which
	   makes this a no-op rather than a special case. */
	const Vector2 v2Origin(static_cast<float>(safe.x) / static_cast<float>(iWidth),
	                       static_cast<float>(safe.y) / static_cast<float>(iHeight));
	const Vector2 v2Size(static_cast<float>(safe.w) / static_cast<float>(iWidth),
	                     static_cast<float>(safe.h) / static_cast<float>(iHeight));

	if (v2Size.x <= 0.f || v2Size.y <= 0.f)
		return;

	Circle* pCircles[] = {
		&m_EffectiveLayout.Fire, &m_EffectiveLayout.Dash,
		&m_EffectiveLayout.Special, &m_EffectiveLayout.Pause
	};

	for (Circle* pCircle : pCircles)
	{
		pCircle->v2Centre = ToSafeArea(pCircle->v2Centre, v2Origin, v2Size);

		/* The radius shrinks with the tighter of the two axes so a control
		   stays inside the inset it was moved into, and stays round. */
		pCircle->fRadius *= std::min(v2Size.x, v2Size.y);
	}

	/* The stick zones follow the same rectangle. A finger outside the safe
	   area still steers - the zone test is a comparison, not a clip - which is
	   deliberate: the inset exists so controls are not *drawn* under a
	   cutout, not to make the edge of the screen dead. */
	m_EffectiveLayout.fStickZoneTop = v2Origin.y + m_Layout.fStickZoneTop * v2Size.y;
	m_EffectiveLayout.fZoneSplitX = v2Origin.x + m_Layout.fZoneSplitX * v2Size.x;
	m_EffectiveLayout.fStickRadius = m_Layout.fStickRadius * std::min(v2Size.x, v2Size.y);
}


void TouchController::OnInitialize()
{
	/* Deliberately connected from the start rather than when a touch device
	   appears. SDL only reports a touch device once it has been touched on
	   some drivers, and an input controller that reports disconnected is
	   skipped entirely by the binding processor - so waiting means the first
	   tap is swallowed. Nothing is reported until a finger arrives anyway. */
	SetConnected(true);
}

void TouchController::OnUninitialize()
{
	m_Fingers.clear();
}

void TouchController::InitializeButtons()
{
	/* The same keys a gamepad reports, which is the whole design: see the
	   header. Fire is RIGHTPADDOWN because that is what VoxApp binds Fire to,
	   and also what the menus bind Skip_UI to - so the fire button confirms in
	   menus without a second control. */
	AddInputKeyStateMap(IK_GAMEPADRIGHTPADDOWN);
	AddInputKeyStateMap(IK_GAMEPADRIGHTPADLEFT);
	AddInputKeyStateMap(IK_GAMEPADRIGHTPADUP);
	AddInputKeyStateMap(IK_GAMEPADRIGHTPADRIGHT);
	AddInputKeyStateMap(IK_GAMEPADOPTION);
	AddInputKeyStateMap(IK_GAMEPADSELECT);
}

void TouchController::InitializeAxises()
{
	AddInputKeyAxisMap(IK_GAMEPADLEFTSTICKAXISX);
	AddInputKeyAxisMap(IK_GAMEPADLEFTSTICKAXISY);
	AddInputKeyAxisMap(IK_GAMEPADRIGHTSTICKAXISX);
	AddInputKeyAxisMap(IK_GAMEPADRIGHTSTICKAXISY);
}

TouchController::Assignment TouchController::Classify(const Vector2& v2Position) const
{
	/* Buttons first: a finger that lands on one is a button press even though
	   it is also inside the aim stick's zone. */
	const Circle* pCircles[] = {
		&m_EffectiveLayout.Fire, &m_EffectiveLayout.Dash,
		&m_EffectiveLayout.Special, &m_EffectiveLayout.Pause
	};

	const Assignment eAssignments[] = {
		Assignment::Fire, Assignment::Dash, Assignment::Special, Assignment::Pause
	};

	for (size_t i = 0; i < 4; ++i)
	{
		const Vector2 v2Delta = v2Position - pCircles[i]->v2Centre;

		if (glm::dot(v2Delta, v2Delta) <= pCircles[i]->fRadius * pCircles[i]->fRadius)
			return eAssignments[i];
	}

	if (v2Position.y < m_EffectiveLayout.fStickZoneTop)
		return Assignment::None;

	return v2Position.x < m_EffectiveLayout.fZoneSplitX
		? Assignment::MoveStick
		: Assignment::AimStick;
}

Vector2 TouchController::StickValue(const Finger& finger) const
{
	Vector2 v2Delta = finger.v2Position - finger.v2Origin;

	/* Y down in touch coordinates, Y up on a thumbstick. */
	v2Delta.y = -v2Delta.y;

	const float fLength = glm::length(v2Delta);

	if (fLength <= m_EffectiveLayout.fStickRadius * m_EffectiveLayout.fStickDeadZone)
		return Vector2(0.f);

	if (fLength >= m_EffectiveLayout.fStickRadius)
		return v2Delta / fLength;

	return v2Delta / m_EffectiveLayout.fStickRadius;
}

void TouchController::OnUpdate()
{
	/* Every frame rather than on a resize event: a device rotation changes it,
	   and so does a keyboard appearing, and this is four floats. */
	UpdateEffectiveLayout();

	for (Finger& finger : m_Fingers)
		finger.bSeenThisFrame = false;

	int iDeviceCount = 0;
	SDL_TouchID* pDevices = SDL_GetTouchDevices(&iDeviceCount);

	for (int iDevice = 0; iDevice < iDeviceCount; ++iDevice)
	{
		int iFingerCount = 0;
		SDL_Finger** ppFingers = SDL_GetTouchFingers(pDevices[iDevice], &iFingerCount);

		if (ppFingers == nullptr)
			continue;

		for (int i = 0; i < iFingerCount; ++i)
		{
			const SDL_Finger* pFinger = ppFingers[i];

			if (pFinger == nullptr)
				continue;

			const Vector2 v2Position(pFinger->x, pFinger->y);
			const int64_t iId = static_cast<int64_t>(pFinger->id);

			auto found = std::find_if(m_Fingers.begin(), m_Fingers.end(),
				[iId](const Finger& f) { return f.iId == iId; });

			if (found == m_Fingers.end())
			{
				/* New contact: its assignment is decided once, here, and held
				   until it lifts. Reclassifying every frame would let a thumb
				   sliding off the fire button start steering. */
				Finger finger;
				finger.iId = iId;
				finger.v2Origin = v2Position;
				finger.eAssignment = Classify(v2Position);

				m_Fingers.push_back(finger);
				found = m_Fingers.end() - 1;

				m_bEverTouched = true;
			}

			found->v2Position = v2Position;
			found->bSeenThisFrame = true;
		}

		SDL_free(ppFingers);
	}

	SDL_free(pDevices);

	m_Fingers.erase(
		std::remove_if(m_Fingers.begin(), m_Fingers.end(),
			[](const Finger& f) { return !f.bSeenThisFrame; }),
		m_Fingers.end());

	/* Everything is released unless a finger says otherwise, which is what
	   makes a lifted finger stop firing without a separate up event. */
	Vector2 v2Move(0.f);
	Vector2 v2Aim(0.f);

	bool bFire = false;
	bool bDash = false;
	bool bSpecial = false;
	bool bPause = false;

	m_MoveStick.bActive = false;
	m_AimStick.bActive = false;

	for (const Finger& finger : m_Fingers)
	{
		switch (finger.eAssignment)
		{
		case Assignment::MoveStick:
			v2Move = StickValue(finger);
			m_MoveStick.bActive = true;
			m_MoveStick.v2Origin = finger.v2Origin;
			m_MoveStick.v2Position = finger.v2Position;
			break;

		case Assignment::AimStick:
			v2Aim = StickValue(finger);
			m_AimStick.bActive = true;
			m_AimStick.v2Origin = finger.v2Origin;
			m_AimStick.v2Position = finger.v2Position;
			break;

		case Assignment::Fire:    bFire = true; break;
		case Assignment::Dash:    bDash = true; break;
		case Assignment::Special: bSpecial = true; break;
		case Assignment::Pause:   bPause = true; break;

		default:
			break;
		}
	}

	UpdateAxisValue(IK_GAMEPADLEFTSTICKAXISX, v2Move.x);
	UpdateAxisValue(IK_GAMEPADLEFTSTICKAXISY, v2Move.y);
	UpdateAxisValue(IK_GAMEPADRIGHTSTICKAXISX, v2Aim.x);
	UpdateAxisValue(IK_GAMEPADRIGHTSTICKAXISY, v2Aim.y);

	UpdateKeyState(IK_GAMEPADRIGHTPADDOWN, bFire);
	UpdateKeyState(IK_GAMEPADRIGHTPADLEFT, bDash);
	UpdateKeyState(IK_GAMEPADRIGHTPADUP, bSpecial);
	UpdateKeyState(IK_GAMEPADOPTION, bPause);

	/* Nothing maps to these yet; reported so a binding added later behaves. */
	UpdateKeyState(IK_GAMEPADRIGHTPADRIGHT, false);
	UpdateKeyState(IK_GAMEPADSELECT, bFire);
}
