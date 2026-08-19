#pragma once

#include "Core/Math.h"

#include <string>

union SDL_Event;

/* The event-driven half of input.
 *
 * This engine reads input by polling a snapshot: SDL_GetKeyboardState every
 * frame in SDLKeyboard, SDL_GetMouseState in SDLMouse. That works for anything
 * with a current *state* and is blind to everything that only exists as an
 * event - which is why the mouse wheel did nothing for the whole SDL port
 * (Mouse::AddScrollDelta was written and never called: there was no event
 * handler anywhere to call it), and why typing into an ImGui text field
 * produced no characters.
 *
 * Touch is the same problem in a larger size. There is no SDL_GetTouchState;
 * fingers exist only as SDL_EVENT_FINGER_DOWN/MOTION/UP, so a polled model
 * cannot see them at all.
 *
 * So this collects what the snapshot misses, once per frame, from
 * SDLWindowContext::Poll.
 *
 * ---------------------------------------------------------------------------
 * The touch mapping
 *
 * The editor is a mouse-and-keyboard tool and on an iPad it has to be usable
 * without either. Finger count selects what a gesture is:
 *
 *   1 finger    pointer. Position and left button, so ImGui menus, panels,
 *               sliders, the gizmo and click-to-select all work unchanged.
 *               A drag over a scrollable panel scrolls it instead - that
 *               arbitration needs to know whether the press grabbed a widget,
 *               so it lives in SDLImPlatform where ImGui can be asked.
 *   2 fingers   orbit *or* pinch-zoom, never both at once. See the mode lock
 *               below; doing both is what a first cut did and it made every
 *               zoom also swing the camera.
 *   3 fingers   pan.
 *
 * A paired mouse, trackpad or Magic Keyboard is not affected by any of it.
 * iPadOS delivers those as ordinary SDL mouse, wheel and key events, so they
 * arrive through the same path they do on desktop; the touch pointer only
 * takes over while fingers are actually down. Both directions of SDL's
 * touch/mouse synthesis are turned off in main.cpp so the two cannot fight.
 */
class SDLEventInput
{
public:
	/* A frame's worth of gesture. */
	struct Gesture
	{
		/* Movement of the centroid of whatever fingers are down, in pixels.
		   What it *means* is the caller's business, and depends on Fingers:
		   one finger is a pointer drag, two orbit, three pan. */
		Vector2 Drag = Vector2(0.f);

		/* How much the two fingers spread this frame, as a ratio: 1.0 is no
		   change, 2.0 is twice as far apart.
		 *
		 * A ratio and not a pixel delta, because that is what makes zoom feel
		 * right at any distance without a special case. Moving the camera to
		 * focusDistance / PinchScale means a pinch covers the same *fraction*
		 * of the distance to what you are looking at whether that is two
		 * metres away or two hundred - which is the behaviour every other
		 * touch application has, and the lack of it is why a fixed
		 * pixels-per-unit zoom feels wrong when far out. */
		float PinchScale = 1.f;

		/* How many fingers are down right now, so a consumer can tell "no
		   gesture" from "a gesture that happened not to move this frame". */
		int Fingers = 0;
	};

	static SDLEventInput& Get();

	/* Call at the top of the frame, before draining the event queue: clears
	   the per-frame deltas that the handler below accumulates into. */
	void BeginFrame();

	/* One SDL event. Safe to call for every event; unrecognised ones are
	   ignored. */
	void Handle(const SDL_Event& event);

	/* Call once the queue is drained. Two fingers moving produce two separate
	   motion events, so the centroid they define is only correct after both
	   have been applied - measuring per event would report half a gesture
	   twice. */
	void EndFrame();

	const Gesture& GetGesture() const { return m_Gesture; }

	/* True once touch has been used, and stays true after the finger lifts -
	 * until a real mouse moves, which hands the pointer straight back.
	 *
	 * The cursor has to persist, and getting this wrong broke the menu bar in
	 * a way that took a while to see. Releasing the pointer the moment the
	 * glass is empty sends io.MousePos back to SDL_GetMouseState, which on an
	 * iPad with no mouse attached is (0, 0) - the top-left corner, i.e. the
	 * menu bar. ImGui closes an open menu as soon as the cursor moves to a
	 * *different* item of the same menu bar, so every menu opened on the press
	 * frame and shut again one frame later when the cursor teleported off it.
	 *
	 * A cursor that stays where it was last put is also just what a pointer
	 * does; a mouse does not recentre itself when you let go of it. */
	bool HasPointer() const { return m_bTouchOwnsPointer; }

	/* Only meaningful when HasPointer(). Window pixels, matching what
	   SDLWindowContext::GetMousePositionInPixels reports for a real mouse.

	   Frozen, not averaged, while a two- or three-finger gesture is under way:
	   the cursor stays where the gesture began. That is deliberate and load
	   bearing - it is what decides whether a two-finger drag scrolls a panel
	   or orbits the camera, since both consumers ask what the cursor is over.
	   A cursor that tracked the centroid could start a scroll and finish it as
	   an orbit by drifting off the window edge. */
	Vector2 GetPointerPosition() const { return m_PointerPosition; }
	bool IsPointerPressed() const { return m_bPointerPressed; }

	/* Characters typed since the last drain, UTF-8. ImGui wants these as
	   codepoints and nothing else in the engine wants them at all, so they are
	   buffered as bytes here and decoded by the one consumer that cares. */
	std::string TakeTextInput();

	/* Whether this scancode saw a key-down event during this frame's events,
	 * regardless of whether it is still held by the time anyone looks.
	 *
	 * The same shape of bug as the tap: SDLKeyboard reads a snapshot with
	 * SDL_GetKeyboardState, so a key whose press and release both land inside
	 * one frame's event batch is never observed as down. Backspace on iOS is
	 * exactly that - with text input active SDL delivers it as a synthesized
	 * press and release together - so held keys worked and deleting a
	 * character did nothing.
	 *
	 * SDLKeyboard ORs this into the snapshot, so every consumer of the
	 * keyboard gets the fix rather than just ImGui. */
	bool WasKeyPressedThisFrame(int iScancode) const;

private:
	SDLEventInput() = default;

	struct Finger
	{
		int64_t Id = 0;
		Vector2 Position = Vector2(0.f);
	};

	/* Which of the two things a two-finger gesture turned out to be.
	 *
	 * Locked once, on the first movement large enough to tell them apart, and
	 * held until the finger count changes. Applying orbit and pinch together -
	 * which is what the raw deltas support, and what a first cut did - means
	 * every zoom also swings the camera, because no real pinch keeps its
	 * centroid perfectly still. Committing to one reads as intent rather than
	 * as drift. */
	enum TwoFingerMode
	{
		E_TWO_FINGER_UNDECIDED,
		E_TWO_FINGER_ORBIT,
		E_TWO_FINGER_PINCH
	};

	/* Where a tap that began and ended inside a single frame is replayed.
	 *
	 * A tap is 50-100 ms. A frame of this editor on an iPad is not reliably
	 * shorter than that, and consumers only ever see input *after* Poll has
	 * drained the whole queue - so a DOWN and its UP in the same batch left
	 * the button pressed for exactly zero observed frames and the tap did
	 * nothing at all. Sustained gestures were unaffected, which is why
	 * two-finger scrolling worked while tapping did not.
	 *
	 * So the release is deferred: one frame reporting the press at the tap
	 * position, then one reporting it released at the same position, which is
	 * the down-then-up ImGui and the editor's IKS_RELEASED binding both need
	 * to see. */
	enum TapLatch
	{
		E_TAP_IDLE,
		E_TAP_HOLD,
		E_TAP_RELEASE
	};

	void OnFingerDown(const SDL_Event& event);
	void OnFingerMotion(const SDL_Event& event);
	void OnFingerUp(const SDL_Event& event);

	void OnFingerCountChanged();

	Finger* FindFinger(int64_t id);
	Vector2 EventPositionInPixels(const SDL_Event& event) const;

	/* Three is all any gesture here uses, and a fourth finger resting on the
	   glass should not change what the other three mean. */
	static constexpr size_t k_uiMaxFingers = 3;

	/* How far two fingers have to move before the gesture commits to orbit or
	   to pinch. Small enough not to feel like a dead zone, large enough that
	   the noise in placing two fingers does not decide it. */
	static constexpr float k_fTwoFingerDecisionPixels = 16.f;

	Finger m_Fingers[k_uiMaxFingers];
	size_t m_uiFingerCount = 0;

	Gesture m_Gesture;

	/* Centroid and spread as of the previous frame, for the delta. Rebuilt
	   whenever the finger count changes so adding or lifting a finger does not
	   register as an enormous one-frame movement. */
	Vector2 m_AnchorCentroid = Vector2(0.f);
	float m_AnchorSpread = 0.f;
	bool m_bAnchorValid = false;

	TwoFingerMode m_TwoFingerMode = E_TWO_FINGER_UNDECIDED;
	float m_fCandidateOrbit = 0.f;
	float m_fCandidatePinch = 0.f;

	Vector2 m_PointerPosition = Vector2(0.f);

	/* Latched by the first touch, cleared by real mouse input. See
	   HasPointer() for why it is not simply "a finger is down". */
	bool m_bTouchOwnsPointer = false;
	bool m_bPointerPressed = false;

	/* Indexed by SDL_Scancode. Sized generously rather than by
	   SDL_SCANCODE_COUNT so this header does not have to include SDL; the
	   bound is asserted against the real count in the .cpp. */
	static constexpr size_t k_uiScancodeCount = 512;
	bool m_bKeyPressedThisFrame[k_uiScancodeCount] = {};

	TapLatch m_TapLatch = E_TAP_IDLE;

	/* Whether the press has already been visible to consumers for a frame. If
	   it has, the release needs no latch. */
	bool m_bPressReported = false;

	/* Set when a second finger lands, cleared when the glass is empty again.
	 *
	 * A two-finger gesture is two DOWN events, and unless they land in the
	 * same frame the first one has already been reported as a pointer press.
	 * Dropping the press the moment the second finger arrives is what stops an
	 * orbit that started over a button from also clicking it. It is not
	 * perfect - ImGui may have seen one frame of press-and-release - and the
	 * alternative, holding every tap back a few frames to see whether a second
	 * finger follows, puts latency on every single tap. Cancelling is the
	 * cheaper mistake. */
	bool m_bPointerCancelled = false;

	std::string m_TextInput;
};
