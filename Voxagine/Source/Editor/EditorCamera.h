#pragma once

#include "Core/ECS/Entities/Camera.h"

class Editor;
class InputHandler;
class InputContextNew;

class EditorCamera : public Camera
{
public:
	EditorCamera(World* pWorld, Editor* pEditor);
	virtual ~EditorCamera();

	virtual void Awake() override;
	virtual void PreTick() override;
	virtual void PostTick(float fDelta) override;
	virtual void PostFixedTick(const GameTimer& timer) override;

	void LockThisFrame(bool bLock);

	Editor* GetEditor();

private:
	void ApplyPositionalCorrection();

	/* Orbit, pan and zoom from a multi-touch gesture. The two deltas are in
	   window pixels and the pinch is a ratio (1.0 = unchanged); see
	   SDLEventInput.h for which finger count produces which. */
	void ApplyTouchGesture(const Vector2& orbitDelta, const Vector2& panDelta,
	                       float fPinchScale);

	/* Distance to what the camera is looking at, which is the scale every
	   gesture is measured against. */
	float GetFocusDistance() const;

private:
	Editor* m_pEditor = nullptr;
	InputContextNew* m_pInputContext = nullptr;
	uint64_t m_uilBindIDMouseAction;

	bool m_bLeftClickAction = false;
	bool m_bLeftClickActionValid = false;

	bool m_bRightClickAction = false;
	bool m_bRightClickActionValid = false;

	float m_fRotationSpeed = 5.f;
	float m_fMaxRotationSpeed = 1000.f;

	float m_fTranslationSpeed = 200.f;
	float m_fMaxTranslationSpeed = 3000.f;

	float m_fScrollSpeed = 800.f;

	/* Touch. Degrees of orbit per pixel, and a multiplier on an otherwise
	   one-to-one zoom - both direct manipulation rather than the rates above,
	   so they are much smaller numbers than they look next to
	   m_fRotationSpeed and do not correspond to them. Pan has no constant at
	   all: it is derived from the field of view and the focus distance so the
	   world tracks the fingers exactly. */
	float m_fTouchOrbitSpeed = 0.25f;
	float m_fTouchZoomSpeed = 1.f;

	/* Bounds on GetFocusDistance. The floor stops a camera looking straight
	   down from a low altitude making every gesture microscopic; the ceiling
	   stops one looking at the horizon from flinging itself across the map. */
	static constexpr float k_fMinFocusDistance = 5.f;
	static constexpr float k_fMaxFocusDistance = 2000.f;

	/* Below this much downward tilt the view ray meets the ground so far away
	   that the intersection is meaningless, and the camera's height is the
	   better answer. */
	static constexpr float k_fMinDownwardComponent = 0.05f;

	/* A single frame may not more than halve or double the distance, however
	   fast the fingers moved or however long the frame was. */
	static constexpr float k_fMinPinchStep = 0.5f;
	static constexpr float k_fMaxPinchStep = 2.f;

	float m_fMaxWorldDistance = 250.f;

	Vector2 m_AccumulateMouseDelta = Vector2(0.f);
	float m_fAccumulateScrollDelta = 0.f;

	Vector2 m_AccumulateOrbitDelta = Vector2(0.f);
	Vector2 m_AccumulatePanDelta = Vector2(0.f);

	/* A ratio, so the identity is 1 and it accumulates by multiplication. */
	float m_fAccumulatePinchScale = 1.f;

	bool m_bFrameLock = false;
};