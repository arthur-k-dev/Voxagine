#pragma once

#include <string>
#include <stdint.h>

/* Command-line options, parsed once in main() and read from wherever they are
 * needed - RENDERING_PLAN.md phase 0b.
 *
 * This exists because taking a GPU measurement used to mean *editing shared
 * files*: `ProjectSettings.vgps` to choose the level, `Settings.vgs` for vsync
 * and resolution, then launching the game into a window the compositor sizes,
 * sleeping, killing it and grepping stderr. Three things wrong with that, all
 * of which bit during phase 6.2:
 *
 *   - It churns files another agent may be holding, and it has to be reverted
 *     afterwards - every phase note in that plan carries such a caveat.
 *   - Hyprland picks the window size, so two runs of the same binary can differ
 *     4x in pixel count with nothing in the log saying so. That produced a
 *     Voxel pass reading *below* the same build's shadow-less floor.
 *   - It takes over the developer's display for the duration.
 *
 * A hidden window fixes the last two together: nothing appears on screen, and
 * the compositor does not tile what it is not showing, so the requested size is
 * actually honoured. `SDL_VIDEODRIVER=offscreen` was tried first and cannot
 * work here - SDL needs `VK_EXT_headless_surface` to give Vulkan a surface and
 * this driver does not expose it.
 *
 * A global rather than plumbing: these are process-wide facts fixed before
 * anything is constructed, and threading them through Application, Platform,
 * WindowContext and RenderContext constructors would touch far more than it is
 * worth. FrameProfiler is the same shape for the same reason.
 */
class LaunchOptions
{
public:
	static LaunchOptions& Get();

	/* Unrecognised arguments are reported and ignored rather than being fatal:
	   this is a developer convenience, and a typo should not stop the game
	   booting. Returns false only if the user asked for --help. */
	bool Parse(int argc, char** argv);

	/* World to load instead of ProjectSettings' DefaultMap. Empty when unset. */
	const std::string& GetMap() const { return m_Map; }
	bool HasMap() const { return !m_Map.empty(); }

	/* Frames to run before exiting. 0 runs until the window is closed. */
	uint32_t GetFrameLimit() const { return m_uiFrames; }

	/* Editor only: enter play mode after this many frames, as if Play had been
	   clicked. 0 leaves the editor in edit mode.

	   This exists because a defect in the editor's *play* path cost four rounds
	   of guessing: the camera stopped following the player, it reproduced
	   nowhere else, and there was no way to reach play mode without a human at
	   the machine - so every hypothesis had to be shipped to be tested. Being
	   able to observe beats being able to reason. Same argument as `--map` and
	   the `--ui-script` tokens. */
	uint32_t GetEditorPlayFrame() const { return m_uiEditorPlayFrame; }
	bool HasEditorPlay() const { return m_uiEditorPlayFrame > 0; }

	/* Create the window unmapped. Vulkan still renders; nothing is displayed. */
	bool IsHidden() const { return m_bHidden; }

	/* Window size override, and with it the render target size. Zero leaves
	   Settings::GetInitialWindowSize alone. Only meaningful alongside
	   --hidden, since a mapped window is the compositor's to size. */
	uint32_t GetWidth() const { return m_uiWidth; }
	uint32_t GetHeight() const { return m_uiHeight; }
	bool HasSize() const { return m_uiWidth > 0 && m_uiHeight > 0; }

	/* Where to write the final frame, as a binary PPM. Empty when unset.
	   PPM because the engine vendors stb_image but not stb_image_write, and a
	   capture path is not worth a new dependency - converting is one step
	   outside the process. */
	const std::string& GetScreenshot() const { return m_Screenshot; }
	bool HasScreenshot() const { return !m_Screenshot.empty(); }

	/* Ignore Settings' EnableVSync and FrameLimit and run the frame loop flat
	   out. For measurement: a vsync-locked run leaves the GPU idle most of the
	   frame, which lets it clock down, and a pass measured at two resolutions
	   is then measured at two clocks. See Application::LoadSettings. */
	bool IsUncapped() const { return m_bUncapped; }

	/* Which pass's target the capture reads. The default is the composited
	   image; naming an earlier pass is how an intermediate gets inspected
	   without a debug shader - "Sun Shadow" dumps the shadow map itself. */
	const std::string& GetScreenshotPass() const { return m_ScreenshotPass; }

	/* A whole render-quality preset, applied over the platform defaults and
	   *instead of* the player's saved settings - see
	   Application::LoadRenderSettings.
	 *
	 * This is a measurement tool and it is here for the same reason --map is:
	 * the alternative is editing PlayerPrefs.vgprefs, which is shared state
	 * another agent may be holding and which has to be put back afterwards.
	 * Every graphics setting is now the player's, so "measure the mobile
	 * configuration" and "measure the desktop one" stopped being two builds and
	 * became two runs - but only if there is a way to say which from outside.
	 *
	 * `low` is what Settings::ApplyPlatformRenderDefaults gives a phone, `high`
	 * is the full desktop suite. It does not touch ResolutionScale: the point of
	 * this switch is to price the *shading* levers against each other, and
	 * changing the pixel count at the same time would confound every one of
	 * them. Pass --size for that. */
	enum class QualityPreset
	{
		E_UNSET,
		E_LOW,
		E_HIGH
	};

	QualityPreset GetQualityPreset() const { return m_QualityPreset; }

	/* A scripted sequence of menu presses, fed into SDL's own event queue so
	 * that the whole real input path runs - binding maps, canvas callbacks,
	 * focus - rather than a shortcut that would prove nothing about either.
	 *
	 * This exists because menu bugs were being diagnosed by building an APK,
	 * installing it, and pressing buttons on a phone: a five minute loop for a
	 * one line question, and the class of bug in question (which canvas owns the
	 * input map after a world is popped) is invisible in a still frame. With
	 * this a whole trip through the menus is a headless run of a few seconds.
	 *
	 * Comma separated, one per --ui-script-interval frames:
	 *   up down left right  the four navigation directions
	 *   confirm             the menus' accept key
	 *   back                escape / pause
	 *   wait                do nothing this slot, for a world load to settle
	 *   fire                the player's weapon
	 *   join                press-to-join on the main menu, which is the only
	 *                       way a script reaches a level from the menus at all
	 *                       - see StartToJoinPlayerComponent::Awake
	 *   forward-on/-off     hold/release walk forward, for a window slide
	 *   backward-on/-off    hold/release walk back, which is what makes a chunk
	 *                       unload and then reload
	 *
	 * Turning it on also traces every focus change and binding map switch as
	 * [ui] lines, which is the actual output - the sequence is just how you get
	 * the engine into the state worth tracing. */
	const std::string& GetUIScript() const { return m_UIScript; }
	bool HasUIScript() const { return !m_UIScript.empty(); }

	uint32_t GetUIScriptInterval() const { return m_uiUIScriptInterval; }

private:
	std::string m_Map;
	std::string m_Screenshot;
	std::string m_ScreenshotPass = "Post Processing";

	uint32_t m_uiFrames = 0;
	uint32_t m_uiEditorPlayFrame = 0;
	uint32_t m_uiWidth = 0;
	uint32_t m_uiHeight = 0;

	bool m_bHidden = false;
	bool m_bUncapped = false;

	QualityPreset m_QualityPreset = QualityPreset::E_UNSET;

	std::string m_UIScript;
	uint32_t m_uiUIScriptInterval = 30;
};
