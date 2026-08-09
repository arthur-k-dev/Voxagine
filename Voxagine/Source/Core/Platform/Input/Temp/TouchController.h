#pragma once

#include "InputController.h"
#include "Core/Math.h"

#include <cstdint>
#include <vector>

/* Touch, presented to the rest of the engine as a gamepad.
 *
 * Bit Buster is a twin-stick game: VoxApp binds MoveRight/MoveForward to the
 * left stick, RotateRight/RotateUp to the right, and Fire/Dash/Special to face
 * buttons. So the useful thing for touch to be is *that gamepad*. This
 * controller reports the same IK_GAMEPAD* keys, which means every binding in
 * VoxApp.cpp and every reader in Player.cpp works with no change at all - and,
 * more importantly, keeps working when somebody adds a binding later without
 * thinking about touch.
 *
 * Two floating sticks and four buttons:
 *
 *  - a finger going down in the left zone becomes the **move** stick, centred
 *    wherever it landed. Floating rather than fixed because a fixed stick has
 *    to be found by looking, and a thumb on a phone is not looking.
 *  - a finger in the right zone, outside the buttons, becomes the **aim**
 *    stick, the same way.
 *  - **Fire**, **Dash**, **Special** and **Pause** are fixed circles, because
 *    a button that moves is not a button.
 *
 * A finger keeps whatever it was assigned to until it lifts, so sliding off a
 * button does not silently hand the touch to the aim stick.
 *
 * **The layout is public** (`GetLayout`) so that whatever eventually draws the
 * overlay draws exactly what the input layer is testing against, rather than a
 * second copy of the same numbers that drifts. Nothing draws it yet - see
 * MOBILE_PORT_LOG.md phase 5. */
class TouchController : public InputController
{
public:
	struct Circle
	{
		/* Normalised window coordinates, origin top left, and a radius in
		   units of the *smaller* window dimension so a control is the same
		   physical size in portrait and landscape. */
		Vector2 v2Centre = Vector2(0.f);
		float fRadius = 0.f;
	};

	struct Layout
	{
		/* Sticks live below this line; above it is the HUD's business. */
		float fStickZoneTop = 0.30f;

		/* Everything left of this is the move stick's; right of it is the aim
		   stick's, minus the buttons. */
		float fZoneSplitX = 0.45f;

		/* How far from where the finger landed counts as full deflection. */
		float fStickRadius = 0.14f;

		/* Below this, the stick reads as centred. A thumb resting on glass is
		   never perfectly still. */
		float fStickDeadZone = 0.14f;

		Circle Fire    = { Vector2(0.90f, 0.72f), 0.11f };
		Circle Dash    = { Vector2(0.72f, 0.84f), 0.09f };
		Circle Special = { Vector2(0.88f, 0.46f), 0.09f };
		Circle Pause   = { Vector2(0.95f, 0.07f), 0.05f };
	};

	/* Where the sticks currently are, for an overlay to draw. Only valid while
	   bActive. */
	struct StickState
	{
		bool bActive = false;
		Vector2 v2Origin = Vector2(0.f);
		Vector2 v2Position = Vector2(0.f);
	};

	/* The layout as designed, in a notional full-screen rectangle. */
	const Layout& GetLayout() const { return m_Layout; }
	void SetLayout(const Layout& layout) { m_Layout = layout; }

	/* The layout as actually used: the above mapped into the window's safe
	   area, so a fixed button never lands under a notch, a camera cutout or
	   the home indicator. On a device with no insets this is identical to
	   GetLayout(), which is why the design numbers can stay simple. */
	const Layout& GetEffectiveLayout() const { return m_EffectiveLayout; }

	const StickState& GetMoveStick() const { return m_MoveStick; }
	const StickState& GetAimStick() const { return m_AimStick; }

	/* True once a finger has ever touched the screen. The overlay should stay
	   hidden until then, so a device with a gamepad attached is not covered in
	   controls nobody is using. */
	bool HasBeenTouched() const { return m_bEverTouched; }

private:
	void OnInitialize() override;
	void OnUninitialize() override;
	void OnUpdate() override;

	void InitializeButtons() override;
	void InitializeAxises() override;

	/* What a given finger was assigned when it went down. */
	enum class Assignment
	{
		None,
		MoveStick,
		AimStick,
		Fire,
		Dash,
		Special,
		Pause
	};

	struct Finger
	{
		int64_t iId = 0;
		Assignment eAssignment = Assignment::None;
		Vector2 v2Origin = Vector2(0.f);
		Vector2 v2Position = Vector2(0.f);
		bool bSeenThisFrame = false;
	};

	Assignment Classify(const Vector2& v2Position) const;
	Vector2 StickValue(const Finger& finger) const;

	void UpdateEffectiveLayout();

	Layout m_Layout;
	Layout m_EffectiveLayout;

	std::vector<Finger> m_Fingers;

	StickState m_MoveStick;
	StickState m_AimStick;

	bool m_bEverTouched = false;
};
