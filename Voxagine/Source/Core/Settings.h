#pragma once

#include <string>
#include "Core/Math.h"

#include "Core/Event.h"

#include <rttr/type>
#include <rttr/registration_friend> 

enum RenderingAPI
{
	RA_VULKAN
};

enum AudioAPI
{
	AA_MINIAUDIO,
	/* Deliberately silent. Useful headless, and the fallback when the engine
	   was built with VOXAGINE_AUDIO_BACKEND=NONE. */
	AA_NONE
};

enum PlatformType
{
	PT_LINUX,
	PT_WINDOWS,
	PT_SWITCH,
	PT_ANDROID
};

/* --- Render quality ---------------------------------------------------------
 * What the sun shadow costs, in two places at once: how large a map the Sun
 * Shadow pass marches, and how many taps a shaded pixel spends reading it.
 *
 *   SHQ_OFF   no map is marched at all and every surface is lit. The pass still
 *             exists and its target still holds whatever it last wrote - the
 *             lookup never reads it, so the contents do not matter.
 *   SHQ_RAY   no map at all: one exact shadow ray per lit pixel, marched
 *             through the same brick DDA the primary ray uses. Pixel-perfect
 *             silhouettes with no resolution, no depth bias and no acne - the
 *             three things a shadow map spends its complexity on. It fires more
 *             rays than the map has texels, but each starts *at the surface*
 *             and ends at the first thing it meets, where a map texel marches
 *             the whole depth of the window from the near plane whether or not
 *             anything is in that column.
 *   SHQ_HARD  a half-size map, read with one Gather: four depth comparisons
 *             blended by the bilinear weights. One shadow edge width
 *             everywhere, about a texel of the map wide.
 *   SHQ_SOFT  a full-size map read with percentage-closer soft shadows - a
 *             blocker search deciding a filter radius, so a shadow hardens
 *             where its caster touches the ground and softens with distance.
 *             Forty taps a pixel for that property.
 *
 * Measured on a Galaxy S23 in an arena, SHQ_SOFT's *map* alone was ~21.5 ms of
 * a 77 ms frame, which is why this is a player-facing setting on a phone rather
 * than a constant. */
enum ShadowQuality
{
	SHQ_OFF = 0,
	SHQ_HARD = 1,
	SHQ_SOFT = 2,

	/* Appended rather than inserted in menu order: the numbers are what
	   PlayerPrefs stores, so renumbering would silently change what a player
	   already chose. SettingsCanvas orders the row for reading. */
	SHQ_RAY = 3
};

/* How ambient occlusion is measured.
 *
 *   AMQ_OFF     no occlusion term; the sky lights every surface equally.
 *   AMQ_SIMPLE  the twelve-neighbour test in AmbientOcclusion.hlsl. It sees one
 *               voxel out, so it darkens the inside of a step and knows nothing
 *               about a wall two metres away.
 *   AMQ_CONE    five cones through the coverage pyramid (RENDERING_PLAN.md
 *               7.1b), which is what makes an enclosed place read as enclosed.
 *               ~20 dependent 3D texture samples a pixel.
 *
 * AMQ_SIMPLE is not a downgrade of AMQ_CONE, it is the term the cones multiply:
 * at AMQ_CONE both run. */
enum AmbientQuality
{
	AMQ_OFF = 0,
	AMQ_SIMPLE = 1,
	AMQ_CONE = 2
};

class Settings
{
public:
	const std::string& GetTitle() const { return m_Title; }
	const std::wstring GetFontPath() const { return std::wstring(m_FontPath.begin(), m_FontPath.end()); }
	const std::string& GetEngineAssetsPath() const { return m_EngineAssetsPath; }

	const std::wstring& GetGPUName() const { return m_GPUName; }
	void SetGPUName(const wchar_t* pGPUName) {
		m_GPUName = std::wstring(pGPUName);
	}

	PlatformType GetPlatformType() const { return m_PlatformType; }
	RenderingAPI GetRenderAPIType() const { return m_RenderApiType; }
	AudioAPI GetAudioAPIType() const { return m_AudioApiType; }

	bool IsVSyncEnabled() const { return m_bEnableVSync; }
	void SetVSync(bool bVSync) { m_bEnableVSync = bVSync; };

	bool IsTearingEnabled() const { return m_bEnableTearing; }
	void SetTearing(bool bTearing) { m_bEnableTearing = bTearing; };

	bool IsFXAAEnabled() const { return m_bFXAAEnabled; }
	void SetFXAA(bool bFXAA) { m_bFXAAEnabled = bFXAA; RenderQualityChanged(); };

	/* Whether the sun shadow *system* is built at all: the Sun Shadow pass and
	   the shadow-reading variant of the voxel pixel shader are both created
	   once, at startup, from this. It is engine configuration and is not in the
	   player-facing menu - ShadowQuality below is, and it is the runtime knob.

	   With this off, ShadowQuality is forced to SHQ_OFF and the menu row says
	   so, because there is no map for any quality to read. */
	bool IsShadowEnabled() const { return m_bShadowsEnabled; }
	void SetShadowEnabled(bool bEnabled) { m_bShadowsEnabled = bEnabled; }

	/* --- The player-facing render settings ---------------------------------
	   Every one of these is read by the shaders out of the camera constant
	   buffer each frame (see RenderContext::Present and CameraData.hlsl's
	   renderQuality), so changing one takes effect on the next frame with no
	   pipeline rebuild. The two that cannot be a uniform - the shadow map's size
	   and the scene target's - go through RenderContext::ApplyRenderSettings,
	   which is what RenderQualityChanged exists to trigger. */
	ShadowQuality GetShadowQuality() const
	{
		return m_bShadowsEnabled ? m_ShadowQuality : SHQ_OFF;
	}

	void SetShadowQuality(ShadowQuality quality)
	{
		m_ShadowQuality = quality;
		RenderQualityChanged();
	}

	AmbientQuality GetAmbientQuality() const { return m_AmbientQuality; }
	void SetAmbientQuality(AmbientQuality quality)
	{
		m_AmbientQuality = quality;
		RenderQualityChanged();
	}

	bool IsBounceLightEnabled() const { return m_bBounceLightEnabled; }
	void SetBounceLight(bool bEnabled) { m_bBounceLightEnabled = bEnabled; RenderQualityChanged(); }

	bool IsReflectionEnabled() const { return m_bReflectionsEnabled; }
	void SetReflections(bool bEnabled) { m_bReflectionsEnabled = bEnabled; RenderQualityChanged(); }

	/* Side of the square sun shadow map for the current quality. The Sun Shadow
	   pass marches one full traversal of the resident window per texel and
	   nothing about it scales with the screen, so its cost is exactly this
	   number squared - halving it is a quarter of the pass.

	   SHQ_OFF keeps the low size rather than dropping to nothing: the target is
	   still allocated, just never drawn into, and an allocation that changes
	   size when a setting merely turns something off is a resize nobody needs.

	   Paired with SUN_SHADOW_RESOLUTION reaching the shaders as
	   renderQuality.w - they are the same number and it now travels rather than
	   being compiled in on both sides. */
	uint32_t GetSunShadowResolution() const
	{
		return m_uiShadowResolution;
	}

	/* The side of the sun shadow map, and the only thing that moves that pass's
	   cost - it is one full march of the resident window per texel, so this
	   number squared *is* the cost.
	 *
	 * Its own setting rather than something ShadowQuality implies, because the
	 * two are independent questions - how sharp the filter is, and how much
	 * geometry the map can resolve - and on a phone the answer to the second is
	 * "as little as you can stand". Measured on an S23 at 512: 8.6 ms of a 32 ms
	 * frame. 256 is a quarter of that.
	 *
	 * Clamped to 128..2048. Powers of two are what the menu offers and what the
	 * sizes were reasoned about at, but nothing here requires one - the lookup
	 * divides by the resolution it is given. Past 2048 the pass costs more than
	 * the per-pixel shadow rays it replaced. */
	void SetSunShadowResolution(uint32_t uiResolution)
	{
		m_uiShadowResolution = std::min(2048u, std::max(128u, uiResolution));
		RenderQualityChanged();
	}

	/* Whether the Sun Shadow pass has to run at all. SHQ_RAY shades from the
	   marcher directly and SHQ_OFF shades everything lit, so in both cases the
	   pass is a full traversal of the window per texel that nothing will read. */
	bool NeedsSunShadowMap() const
	{
		const ShadowQuality quality = GetShadowQuality();

		return quality == SHQ_HARD || quality == SHQ_SOFT;
	}

	/* How far a SHQ_RAY shadow ray is allowed to march, in world units.
	 *
	 * The reason this mode needs a leash and the map does not: a shadow map
	 * texel marches once and is reused by every pixel that lands in it, so its
	 * length is amortised. A per-pixel ray is paid for by that pixel alone, and
	 * measured on an S23 an unbounded one put the Voxel pass at 55.8 ms - three
	 * times what the map costs, because rays from a jagged voxel surface all
	 * take different lengths and a whole wave waits for the longest.
	 *
	 * Capping it turns that into a fixed worst case, and the thing it gives up
	 * is the least valuable part of the result: a caster far from its receiver
	 * contributes a shadow nobody traces back to it, while contact shadows -
	 * which are what sells voxel geometry - are all within a few units.
	 * Everything past the limit reads as lit.
	 *
	 * Zero means unbounded, which is what the desktop wants. */
	float GetShadowRayDistance() const { return m_fShadowRayDistance; }
	void SetShadowRayDistance(float fDistance)
	{
		m_fShadowRayDistance = std::max(0.f, fDistance);
		RenderQualityChanged();
	}

	/* Platform defaults for everything above, applied after Settings.vgs is
	   read and before the player's own choices are restored - Settings.cpp has
	   the reasoning and the numbers. */
	void ApplyPlatformRenderDefaults();

	/* RENDERING_PLAN.md Phase 0: gates the per-pass GPU timestamp queries
	   and CPU frame breakdown (see FrameProfiler). Not RTTR-registered - a
	   dev/profiling knob has no business round-tripping through .vgs or
	   .vguser. */
	bool IsGPUProfilingEnabled() const { return m_bGPUProfilingEnabled; }
	void SetGPUProfilingEnabled(bool bEnabled) { m_bGPUProfilingEnabled = bEnabled; }

	float GetResolutionScale() const { return m_fResolutionScale; }
	void SetResolutionScale(float fResolutionScale)
	{
		m_fResolutionScale = std::max(0.1f, fResolutionScale);
		RenderQualityChanged();
	}

	/* The aspect ratio the game renders at regardless of window shape; the
	   result is letterboxed on present. Zero renders at the window's own
	   ratio, which is what a window manager that ignores size hints will
	   otherwise hand us. */
	float GetLockedAspectRatio() const { return m_fLockedAspectRatio; }
	void SetLockedAspectRatio(float fAspectRatio) { m_fLockedAspectRatio = std::max(0.f, fAspectRatio); }

	void SetFullscreen(bool bFullscreen);
	bool IsFullscreen() const { return m_bFullscreen; }

	const std::string& GetWorldFileExtension() const { return m_WorldFileExtension; }
	const std::string& GetPrefabFileExtension() const { return m_PrebabFileExtension; }

	double GetFixedTimeStep() const { return m_dFixedTimeStep; }
	double GetFrameLimit() const { return m_dFrameLimit; }

	/* Seconds per frame, or 0 for no limit. Only --uncapped sets it; the
	   serialized value is the one the game runs with. */
	void SetFrameLimit(double dSeconds) { m_dFrameLimit = dSeconds; }

	UVector2 m_v2InitialWindowSize = UVector2(1280, 720);

	/* Event list */
	Event<bool> FullscreenChanged;

	/* Fired by every render-quality setter. RenderContext subscribes and
	   re-applies the two things a per-frame uniform cannot carry: the size of
	   the sun shadow map, and the size of every target that scales with
	   ResolutionScale. Everything else is picked up on the next frame simply by
	   being uploaded.

	   Deliberately parameterless. The subscriber re-reads the whole of Settings
	   rather than acting on a delta, because several of these interact - shadow
	   quality decides the map's size, and the shadow system being off decides
	   whether there is one at all - and a per-setting delta would have to encode
	   that. */
	Event<> RenderQualityChanged;

private:
	/* Determines the window title */
#ifdef EDITOR
	std::string m_Title = "Voxagine";
#else
	std::string m_Title = "Bit Buster";
#endif
	std::wstring m_GPUName = L"Unknown";

	/* Set default paths */
	std::string m_WorldFileExtension = "wld";
	std::string m_PrebabFileExtension = "prefab";
	std::string m_EngineAssetsPath = "Engine/Assets";
	std::string m_FontPath = "Engine/Assets/Fonts/PressStart.spritefont";

	/* _WIN32, not _WINDOWS: the latter came from the old .vcxproj and nothing
	   defines it under CMake. Every Windows compiler defines _WIN32. */
#ifdef _WIN32
	PlatformType m_PlatformType = PT_WINDOWS;
#else
	PlatformType m_PlatformType = PT_LINUX;
#endif
	RenderingAPI m_RenderApiType = RA_VULKAN;

	AudioAPI m_AudioApiType = AA_MINIAUDIO;

	double m_dFixedTimeStep = 1.0 / 60.0;
	double m_dFrameLimit = 1.0 / 200.0;

	bool m_bEnableVSync = false;
	bool m_bEnableTearing = !m_bEnableVSync;

	bool m_bFXAAEnabled = true;

	bool m_bShadowsEnabled = true;
	float m_fResolutionScale = 1.f;

	/* Desktop values. ApplyPlatformRenderDefaults replaces them on a phone;
	   Settings.vgs is shared between the two builds, so it cannot. */
	ShadowQuality m_ShadowQuality = SHQ_SOFT;
	uint32_t m_uiShadowResolution = 1024;
	float m_fShadowRayDistance = 0.f;
	AmbientQuality m_AmbientQuality = AMQ_CONE;

	bool m_bBounceLightEnabled = true;
	bool m_bReflectionsEnabled = true;

	/* VOXAGINE_PROFILE_DEFAULT is for builds that are optimised but exist to be
	   measured - the Android `benchmark` variant, which is a release build with
	   a debug signature. VOXAGINE_PROFILE=1 still overrides at runtime on
	   desktop, but there is no practical way to set an environment variable for
	   a non-debuggable Android app, and a measurement build whose profiler is
	   off is not a measurement build. */
#if defined(_DEBUG) || defined(VOXAGINE_PROFILE_DEFAULT)
	bool m_bGPUProfilingEnabled = true;
#else
	bool m_bGPUProfilingEnabled = false;
#endif

	float m_fLockedAspectRatio = 16.f / 9.f;

	bool m_bFullscreen = false;

	RTTR_ENABLE();
	RTTR_REGISTRATION_FRIEND
};