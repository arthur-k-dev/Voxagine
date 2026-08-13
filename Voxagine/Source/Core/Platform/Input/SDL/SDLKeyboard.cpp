#include "pch.h"
#include "SDLKeyboard.h"

#include <SDL3/SDL.h>

#include "Core/LaunchOptions.h"
#include "Core/Platform/Input/SDL/SDLEventInput.h"

#include <cctype>
#include <cstdio>
#include <string>
#include <vector>

/* --ui-script lives here rather than beside SDL's event queue, and the reason is
 * worth recording: this engine does not read the keyboard from events at all, it
 * takes a snapshot with SDL_GetKeyboardState every frame. Pushing synthetic
 * SDL_EVENT_KEY_DOWN events - the obvious approach, and the first one tried -
 * therefore does nothing whatsoever, silently. The state array is the boundary,
 * so the script writes into the state it returns.
 *
 * Everything above this line still runs for real: the binding maps, the canvas
 * callbacks, the focus walk. Only the two hundred microseconds where a physical
 * key would have been are replaced. See LaunchOptions::GetUIScript. */
namespace
{
	struct UIScript
	{
		std::vector<std::string> Tokens;
		size_t uiStep = 0;
		uint32_t uiFrame = 0;
		SDL_Scancode Held = SDL_SCANCODE_UNKNOWN;
		bool bParsed = false;
		bool bPaused = false;
		uint32_t uiPausedFrames = 0;
	};

	UIScript g_UIScript;

	SDL_Scancode ScancodeForToken(const std::string& token)
	{
		if (token == "up") return SDL_SCANCODE_UP;
		if (token == "down") return SDL_SCANCODE_DOWN;
		if (token == "left") return SDL_SCANCODE_LEFT;
		if (token == "right") return SDL_SCANCODE_RIGHT;
		if (token == "confirm") return SDL_SCANCODE_RETURN;
		if (token == "back") return SDL_SCANCODE_ESCAPE;
		if (token == "fire") return SDL_SCANCODE_P;
		if (token == "join") return SDL_SCANCODE_J;
		if (token == "wait") return SDL_SCANCODE_UNKNOWN;

		fprintf(stderr, "[ui] unknown script token \'%s\'\n", token.c_str());
		return SDL_SCANCODE_UNKNOWN;
	}

	void ParseUIScript()
	{
		const std::string& script = LaunchOptions::Get().GetUIScript();

		size_t start = 0;

		while (start <= script.size())
		{
			const size_t comma = script.find(',', start);
			const size_t end = comma == std::string::npos ? script.size() : comma;

			std::string token = script.substr(start, end - start);

			while (!token.empty() && isspace(static_cast<unsigned char>(token.front())))
				token.erase(token.begin());
			while (!token.empty() && isspace(static_cast<unsigned char>(token.back())))
				token.pop_back();

			if (!token.empty())
				g_UIScript.Tokens.push_back(token);

			if (comma == std::string::npos)
				break;

			start = comma + 1;
		}

		g_UIScript.bParsed = true;
	}

	/* One token per interval, held for a single frame. Held for exactly one is
	   what makes a press *and* a release happen: the menus bind some actions on
	   press and others on release, and a key that stayed down would fire the
	   repeat handlers instead.

	   forward-on/forward-off and backward-on/backward-off are the exception and
	   use Held deliberately: walking a player across a chunk boundary is what
	   the streaming gates script, and that takes many frames of one key being
	   down. Walking *back* is what makes a chunk unload and then reload, which
	   is the only way a script reaches the reload path at all - see
	   CHUNK_STREAMING_PLAN.md M7. */
	SDL_Scancode StepUIScript()
	{
		if (!LaunchOptions::Get().HasUIScript())
			return SDL_SCANCODE_UNKNOWN;

		if (!g_UIScript.bParsed)
			ParseUIScript();

		/* The clock does not run while gameplay is held. It counted display
		   frames from process start, and a world holds gameplay for as long as
		   its initial window takes to stream - 621 ticks in one Release run of
		   Fishing_Village_Beat2 and 2,930 in the next, on the same binary and
		   the same command line, because the only variable is how long the bake
		   takes. At the shipping interval of 60 that is the difference between a
		   script whose tokens land in gameplay and one that spends all twelve of
		   them before a single entity has ticked - which is exactly why `fire`
		   read as "the input never arrives" for four phases. Nothing has bound
		   an action yet: Player::Start has not run.

		   Frames, not ticks: this is the same clock the tokens are spent on.
		   See CHUNK_STREAMING_PLAN.md phase 11. */
		if (g_UIScript.bPaused)
		{
			++g_UIScript.uiPausedFrames;
			return SDL_SCANCODE_UNKNOWN;
		}

		const uint32_t uiInterval = LaunchOptions::Get().GetUIScriptInterval();

		if (g_UIScript.uiFrame++ % uiInterval != 0)
			return SDL_SCANCODE_UNKNOWN;

		if (g_UIScript.uiStep >= g_UIScript.Tokens.size())
			return SDL_SCANCODE_UNKNOWN;

		const std::string& token = g_UIScript.Tokens[g_UIScript.uiStep++];

		printf("[ui] step %zu: %s\n", g_UIScript.uiStep, token.c_str());

		if (token == "forward-on")
		{
			g_UIScript.Held = SDL_SCANCODE_W;
			return SDL_SCANCODE_UNKNOWN;
		}

		if (token == "backward-on")
		{
			g_UIScript.Held = SDL_SCANCODE_S;
			return SDL_SCANCODE_UNKNOWN;
		}

		if (token == "forward-off" || token == "backward-off")
		{
			g_UIScript.Held = SDL_SCANCODE_UNKNOWN;
			return SDL_SCANCODE_UNKNOWN;
		}

		return ScancodeForToken(token);
	}
}

Keyboard* Keyboard::s_pInstance = nullptr;

Keyboard::Keyboard()
{
	s_pInstance = this;
}

Keyboard::~Keyboard()
{
	if (s_pInstance == this)
		s_pInstance = nullptr;
}

void Keyboard::SetUIScriptPaused(bool bPaused)
{
	if (bPaused == g_UIScript.bPaused)
		return;

	g_UIScript.bPaused = bPaused;

	/* Printed on the transition only, and it is the line that tells a later
	   session whether a script that "did nothing" was ever running. */
	if (bPaused)
		printf("[ui] script paused: gameplay held\n");
	else
		printf("[ui] script resumed after %u held frames\n", g_UIScript.uiPausedFrames);

	if (!bPaused)
		g_UIScript.uiPausedFrames = 0;
}

Keyboard& Keyboard::Get()
{
	if (s_pInstance == nullptr)
		new Keyboard();

	return *s_pInstance;
}

Keyboard::State Keyboard::GetState() const
{
	State state;

	int iCount = 0;
	const bool* pKeys = SDL_GetKeyboardState(&iCount);

	if (pKeys == nullptr)
		return state;

	/* A key that went down *and* up inside one frame's events is not in the
	 * snapshot at all, because the snapshot is a photograph of now and the key
	 * is already back up. That is not hypothetical - it is why backspace did
	 * nothing in an ImGui text field on iOS, where SDL delivers it as a
	 * synthesized press and release together while text input is active.
	 *
	 * So the snapshot is merged with "was pressed at any point this frame"
	 * before anything reads it, which costs one pass over 512 bytes and means
	 * every consumer - ImGui, the binding maps, the editor shortcuts - sees
	 * the key. Held keys are unaffected: true OR true.
	 *
	 * Merged into a copy rather than SDL's array, which SDL owns. */
	static bool s_Merged[SDL_SCANCODE_COUNT];
	const int iMergeCount = iCount < SDL_SCANCODE_COUNT ? iCount : SDL_SCANCODE_COUNT;

	const SDLEventInput& eventInput = SDLEventInput::Get();

	for (int i = 0; i < iMergeCount; ++i)
		s_Merged[i] = pKeys[i] || eventInput.WasKeyPressedThisFrame(i);

	pKeys = s_Merged;

	/* --ui-script overlays one key onto the snapshot - see the top of this file.
	   Applied to the built state at the bottom rather than to SDL's array, which
	   SDL owns and which the rest of this function is reading. */
	const SDL_Scancode scripted = StepUIScript();

	state.A = pKeys[SDL_SCANCODE_A];
	state.Add = pKeys[SDL_SCANCODE_KP_PLUS];
	state.Apps = pKeys[SDL_SCANCODE_APPLICATION];
	state.B = pKeys[SDL_SCANCODE_B];
	state.Back = pKeys[SDL_SCANCODE_BACKSPACE];
	state.C = pKeys[SDL_SCANCODE_C];
	state.CapsLock = pKeys[SDL_SCANCODE_CAPSLOCK];
	state.D = pKeys[SDL_SCANCODE_D];
	state.D0 = pKeys[SDL_SCANCODE_0];
	state.D1 = pKeys[SDL_SCANCODE_1];
	state.D2 = pKeys[SDL_SCANCODE_2];
	state.D3 = pKeys[SDL_SCANCODE_3];
	state.D4 = pKeys[SDL_SCANCODE_4];
	state.D5 = pKeys[SDL_SCANCODE_5];
	state.D6 = pKeys[SDL_SCANCODE_6];
	state.D7 = pKeys[SDL_SCANCODE_7];
	state.D8 = pKeys[SDL_SCANCODE_8];
	state.D9 = pKeys[SDL_SCANCODE_9];
	state.Decimal = pKeys[SDL_SCANCODE_KP_PERIOD];
	state.Delete = pKeys[SDL_SCANCODE_DELETE];
	state.Divide = pKeys[SDL_SCANCODE_KP_DIVIDE];
	state.Down = pKeys[SDL_SCANCODE_DOWN];
	state.E = pKeys[SDL_SCANCODE_E];
	state.End = pKeys[SDL_SCANCODE_END];
	state.Enter = pKeys[SDL_SCANCODE_RETURN];
	state.Escape = pKeys[SDL_SCANCODE_ESCAPE];
	state.Execute = pKeys[SDL_SCANCODE_EXECUTE];
	state.F = pKeys[SDL_SCANCODE_F];
	state.F1 = pKeys[SDL_SCANCODE_F1];
	state.F10 = pKeys[SDL_SCANCODE_F10];
	state.F11 = pKeys[SDL_SCANCODE_F11];
	state.F12 = pKeys[SDL_SCANCODE_F12];
	state.F13 = pKeys[SDL_SCANCODE_F13];
	state.F14 = pKeys[SDL_SCANCODE_F14];
	state.F15 = pKeys[SDL_SCANCODE_F15];
	state.F16 = pKeys[SDL_SCANCODE_F16];
	state.F17 = pKeys[SDL_SCANCODE_F17];
	state.F18 = pKeys[SDL_SCANCODE_F18];
	state.F19 = pKeys[SDL_SCANCODE_F19];
	state.F2 = pKeys[SDL_SCANCODE_F2];
	state.F20 = pKeys[SDL_SCANCODE_F20];
	state.F21 = pKeys[SDL_SCANCODE_F21];
	state.F22 = pKeys[SDL_SCANCODE_F22];
	state.F23 = pKeys[SDL_SCANCODE_F23];
	state.F24 = pKeys[SDL_SCANCODE_F24];
	state.F3 = pKeys[SDL_SCANCODE_F3];
	state.F4 = pKeys[SDL_SCANCODE_F4];
	state.F5 = pKeys[SDL_SCANCODE_F5];
	state.F6 = pKeys[SDL_SCANCODE_F6];
	state.F7 = pKeys[SDL_SCANCODE_F7];
	state.F8 = pKeys[SDL_SCANCODE_F8];
	state.F9 = pKeys[SDL_SCANCODE_F9];
	state.G = pKeys[SDL_SCANCODE_G];
	state.H = pKeys[SDL_SCANCODE_H];
	state.Help = pKeys[SDL_SCANCODE_HELP];
	state.Home = pKeys[SDL_SCANCODE_HOME];
	state.I = pKeys[SDL_SCANCODE_I];
	state.Insert = pKeys[SDL_SCANCODE_INSERT];
	state.J = pKeys[SDL_SCANCODE_J];
	state.K = pKeys[SDL_SCANCODE_K];
	state.L = pKeys[SDL_SCANCODE_L];
	state.Left = pKeys[SDL_SCANCODE_LEFT];
	state.LeftAlt = pKeys[SDL_SCANCODE_LALT];
	state.LeftControl = pKeys[SDL_SCANCODE_LCTRL];
	state.LeftShift = pKeys[SDL_SCANCODE_LSHIFT];
	state.LeftWindows = pKeys[SDL_SCANCODE_LGUI];
	state.M = pKeys[SDL_SCANCODE_M];
	state.Multiply = pKeys[SDL_SCANCODE_KP_MULTIPLY];
	state.N = pKeys[SDL_SCANCODE_N];
	state.NumLock = pKeys[SDL_SCANCODE_NUMLOCKCLEAR];
	state.NumPad0 = pKeys[SDL_SCANCODE_KP_0];
	state.NumPad1 = pKeys[SDL_SCANCODE_KP_1];
	state.NumPad2 = pKeys[SDL_SCANCODE_KP_2];
	state.NumPad3 = pKeys[SDL_SCANCODE_KP_3];
	state.NumPad4 = pKeys[SDL_SCANCODE_KP_4];
	state.NumPad5 = pKeys[SDL_SCANCODE_KP_5];
	state.NumPad6 = pKeys[SDL_SCANCODE_KP_6];
	state.NumPad7 = pKeys[SDL_SCANCODE_KP_7];
	state.NumPad8 = pKeys[SDL_SCANCODE_KP_8];
	state.NumPad9 = pKeys[SDL_SCANCODE_KP_9];
	state.O = pKeys[SDL_SCANCODE_O];
	state.P = pKeys[SDL_SCANCODE_P];
	state.PageDown = pKeys[SDL_SCANCODE_PAGEDOWN];
	state.PageUp = pKeys[SDL_SCANCODE_PAGEUP];
	state.Pause = pKeys[SDL_SCANCODE_PAUSE];
	state.Print = pKeys[SDL_SCANCODE_PRINTSCREEN];
	state.PrintScreen = pKeys[SDL_SCANCODE_PRINTSCREEN];
	state.Q = pKeys[SDL_SCANCODE_Q];
	state.R = pKeys[SDL_SCANCODE_R];
	state.Right = pKeys[SDL_SCANCODE_RIGHT];
	state.RightAlt = pKeys[SDL_SCANCODE_RALT];
	state.RightControl = pKeys[SDL_SCANCODE_RCTRL];
	state.RightShift = pKeys[SDL_SCANCODE_RSHIFT];
	state.RightWindows = pKeys[SDL_SCANCODE_RGUI];
	state.S = pKeys[SDL_SCANCODE_S];
	state.Scroll = pKeys[SDL_SCANCODE_SCROLLLOCK];
	state.Select = pKeys[SDL_SCANCODE_SELECT];
	state.Separator = pKeys[SDL_SCANCODE_KP_COMMA];
	state.Space = pKeys[SDL_SCANCODE_SPACE];
	state.Subtract = pKeys[SDL_SCANCODE_KP_MINUS];
	state.T = pKeys[SDL_SCANCODE_T];
	state.Tab = pKeys[SDL_SCANCODE_TAB];
	state.U = pKeys[SDL_SCANCODE_U];
	state.Up = pKeys[SDL_SCANCODE_UP];
	state.V = pKeys[SDL_SCANCODE_V];
	state.W = pKeys[SDL_SCANCODE_W];
	state.X = pKeys[SDL_SCANCODE_X];
	state.Y = pKeys[SDL_SCANCODE_Y];
	state.Z = pKeys[SDL_SCANCODE_Z];

	/* Two scancodes now: the one this interval stepped to, and whatever
	   forward-on left held down across intervals. */
	const auto applyScriptedScancode = [&state](SDL_Scancode scancode)
	{
		switch (scancode)
		{
		case SDL_SCANCODE_UP:     state.Up = true; break;
		case SDL_SCANCODE_DOWN:   state.Down = true; break;
		case SDL_SCANCODE_LEFT:   state.Left = true; break;
		case SDL_SCANCODE_RIGHT:  state.Right = true; break;
		case SDL_SCANCODE_RETURN: state.Enter = true; break;
		case SDL_SCANCODE_ESCAPE: state.Escape = true; break;
		case SDL_SCANCODE_P:      state.P = true; break;
		case SDL_SCANCODE_J:      state.J = true; break;
		case SDL_SCANCODE_W:      state.W = true; break;
		case SDL_SCANCODE_S:      state.S = true; break;
		/* A token whose scancode is not listed here does nothing at all, in
		   silence, and the run still looks healthy - backward-on was added and
		   spent ninety scripted seconds not moving the player before that was
		   noticed. Add the case with the token. */
		default: break;
		}
	};

	applyScriptedScancode(scripted);
	applyScriptedScancode(g_UIScript.Held);

	return state;
}
