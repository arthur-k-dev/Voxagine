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

	/* Which pass's target the capture reads. The default is the composited
	   image; naming an earlier pass is how an intermediate gets inspected
	   without a debug shader - "Sun Shadow" dumps the shadow map itself. */
	const std::string& GetScreenshotPass() const { return m_ScreenshotPass; }

private:
	std::string m_Map;
	std::string m_Screenshot;
	std::string m_ScreenshotPass = "Post Processing";

	uint32_t m_uiFrames = 0;
	uint32_t m_uiWidth = 0;
	uint32_t m_uiHeight = 0;

	bool m_bHidden = false;
};
