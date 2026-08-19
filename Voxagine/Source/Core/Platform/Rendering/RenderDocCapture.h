#pragma once

#include <cstdint>
#include <string>

/* Programmatic RenderDoc frame capture, for a renderer that is developed
 * headless.
 *
 * **Why this exists rather than "just run it under the RenderDoc UI".** Every
 * rendering measurement in this tree is taken with `--hidden` and a scripted
 * camera (CLAUDE.md, "Run the game headless") because the compositor decides
 * the window size otherwise and two runs of the same binary differ 4x in pixel
 * count. The RenderDoc UI wants to launch the process and have somebody press
 * F12 on the interesting frame; here nobody is watching, the interesting frame
 * is one specific frame of a scripted run - the chunk transition, the frame the
 * marcher runs out of budget in - and it may be three thousand frames in.
 *
 * So: the in-application API, driven by an environment variable like every
 * other diagnostic here.
 *
 *   VOXAGINE_RENDERDOC=1                  load librenderdoc.so ourselves
 *   VOXAGINE_RENDERDOC_CAPTURE=<n>        capture the first n presented frames
 *   VOXAGINE_RENDERDOC_PATH=<template>    where captures go (default ./captures/voxagine)
 *
 * `TriggerCapture` is also callable from code, which is the point: a phase that
 * wants "the frame the transition commits in" asks for it at the commit rather
 * than trying to hit F12 at 1000 fps.
 *
 * **Loading order is load-bearing.** RenderDoc hooks Vulkan by interposing on
 * the loader, so its library has to be in the process *before* `vkCreateInstance`
 * - which for this engine means before `Platform::Initialize`, since
 * `VKRenderContext::InitializeBackend` calls `volkInitialize` and creates the
 * instance there. `Initialize()` is called from `Application::Run` for exactly
 * that reason. If RenderDoc injected itself (the normal case, when its UI
 * launched the process) the library is already loaded and this only picks up
 * the API pointer - `RTLD_NOLOAD` first, always, because loading a second copy
 * of it is undefined.
 *
 * **Unverified on this machine, and that is recorded rather than hidden.**
 * RenderDoc is not installed here, so the "attached" half of every path below
 * has never run: what is tested is that its absence costs nothing and changes
 * nothing. The ABI is not guessed - `External/renderdoc/renderdoc_app.h` is
 * upstream's own header, MIT, vendored unmodified - so the risk is in the
 * calls, not in the layout.
 */
class RenderDocCapture
{
public:
	static RenderDocCapture& Get();

	/* Before anything creates a Vulkan instance. Safe to call twice; safe to
	   call when RenderDoc is not installed, which is the ordinary case. */
	void Initialize();

	/* Whether an API pointer was obtained. Everything below is a no-op when
	   this is false. */
	bool IsAttached() const { return m_pApi != nullptr; }

	/* Capture the next uiFrames presented frames. RenderDoc does the framing
	   itself from the present calls, so nothing has to be wrapped. */
	void TriggerCapture(uint32_t uiFrames = 1);

	/* Captures written so far - the number to print at exit so a headless run
	   says whether it produced anything. */
	uint32_t CaptureCount() const;

	/* Path template captures are written to, without an extension. */
	void SetCapturePathTemplate(const std::string& sTemplate);

private:
	RenderDocCapture() = default;

	/* RENDERDOC_API_1_6_0*, kept as void* so this header does not drag
	   renderdoc_app.h into everything that includes it. */
	void* m_pApi = nullptr;
	bool m_bInitialized = false;
};
