#include "pch.h"
#include "Core/Settings.h"

#include <rttr/registration>
#include <rttr/policy.h>

RTTR_REGISTRATION
{
	rttr::registration::enumeration<PlatformType>("E_PlatformType")
	(
		rttr::value("PT_LINUX", PlatformType::PT_LINUX),
		rttr::value("PT_WINDOWS", PlatformType::PT_WINDOWS),
		rttr::value("PT_SWITCH", PlatformType::PT_SWITCH),
		rttr::value("PT_ANDROID", PlatformType::PT_ANDROID)
	);

	rttr::registration::enumeration<AudioAPI>("E_AudioAPI")
	(
		rttr::value("AA_MINIAUDIO", AudioAPI::AA_MINIAUDIO),
		rttr::value("AA_NONE", AudioAPI::AA_NONE)
	);

	rttr::registration::enumeration<RenderingAPI>("E_RenderingAPI")
	(
		rttr::value("RA_VULKAN", RenderingAPI::RA_VULKAN)
	);

	rttr::registration::enumeration<ShadowQuality>("E_ShadowQuality")
	(
		rttr::value("SHQ_OFF", ShadowQuality::SHQ_OFF),
		rttr::value("SHQ_HARD", ShadowQuality::SHQ_HARD),
		rttr::value("SHQ_SOFT", ShadowQuality::SHQ_SOFT),
		rttr::value("SHQ_RAY", ShadowQuality::SHQ_RAY)
	);

	rttr::registration::enumeration<AmbientQuality>("E_AmbientQuality")
	(
		rttr::value("AMQ_OFF", AmbientQuality::AMQ_OFF),
		rttr::value("AMQ_SIMPLE", AmbientQuality::AMQ_SIMPLE),
		rttr::value("AMQ_CONE", AmbientQuality::AMQ_CONE)
	);

	rttr::registration::class_<Settings>("Settings")
		.constructor<>()(rttr::policy::ctor::as_object)
		.property("Title", &Settings::m_Title)
		.property("WorldFileExtension", &Settings::m_WorldFileExtension)
		.property("EngineAssetsPath", &Settings::m_EngineAssetsPath)
		.property("FontPath", &Settings::m_FontPath)
		.property("PlatformType", &Settings::m_PlatformType)
		.property("RenderApiType", &Settings::m_RenderApiType)
		.property("AudioApiType", &Settings::m_AudioApiType)
		.property("FixedTimeStep", &Settings::m_dFixedTimeStep)
		.property("FrameLimit", &Settings::m_dFrameLimit)
		.property("EnableVSync", &Settings::IsVSyncEnabled, &Settings::SetVSync)
		.property("FXAAEnabled", &Settings::IsFXAAEnabled, &Settings::SetFXAA)
		.property("ShadowsEnabled", &Settings::IsShadowEnabled, &Settings::SetShadowEnabled)
		.property("ShadowQuality", &Settings::m_ShadowQuality)
		.property("ShadowResolution", &Settings::GetSunShadowResolution, &Settings::SetSunShadowResolution)
		.property("ShadowRayDistance", &Settings::GetShadowRayDistance, &Settings::SetShadowRayDistance)
		.property("AmbientQuality", &Settings::m_AmbientQuality)
		.property("BounceLightEnabled", &Settings::IsBounceLightEnabled, &Settings::SetBounceLight)
		.property("ReflectionsEnabled", &Settings::IsReflectionEnabled, &Settings::SetReflections)
		.property("ResolutionScale", &Settings::GetResolutionScale, &Settings::SetResolutionScale)
		.property("LockedAspectRatio", &Settings::GetLockedAspectRatio, &Settings::SetLockedAspectRatio)
		.property("Fullscreen", &Settings::IsFullscreen, &Settings::SetFullscreen)
		.property("InitialWindowSize", &Settings::m_v2InitialWindowSize);
}

void Settings::SetFullscreen(bool bFullscreen)
{
	m_bFullscreen = bFullscreen;
	FullscreenChanged(bFullscreen);
}

/* The one place a platform decides what the renderer does by default.
 *
 * It has to be code rather than data, and that is not a shortcut: Settings.vgs
 * is one file shared by the desktop and mobile builds - the Android asset
 * staging copies the same bytes into the APK - so there is nowhere in it to say
 * "and on a phone, less of everything". Every value below is a *default*: it is
 * applied before the player's saved choices are restored, so a setting the
 * player has touched wins over it (see Application::LoadRenderSettings).
 *
 * The desktop side is deliberately absent rather than written out. The members'
 * own initializers are the desktop configuration - full suite, everything on -
 * and repeating them here would be a second place for them to drift.
 *
 * The mobile numbers come from the only measurement anyone has: a Galaxy S23
 * (Adreno 740) in an arena at ResolutionScale 0.5, 13 fps, with the Voxel pass
 * at 48.0 ms and Sun Shadow at ~21.5 ms of a ~77 ms frame. 60 fps is 16.6 ms
 * for the whole frame, so this is not a matter of trimming - the cone traces
 * and PCSS are three to five times the entire budget between them. What is left
 * on is the neighbour-based occlusion, a hard sun shadow at half the map size,
 * aerial perspective and the far field.
 *
 * **These have never been measured on a device.** They are chosen from what
 * each feature is known to cost per pixel, not from a phone that ran them - see
 * Docs/MOBILE_PORT_LOG.md. */
void Settings::ApplyPlatformRenderDefaults()
{
#if defined(VOXAGINE_MOBILE)
	/* 35%, not 50%. Measured on an S23 at exactly this settings combination
	   (Medium shadows, everything else off): the Voxel pass is fragment-bound
	   and close to linear in pixel count, and 35% is roughly half the pixels of
	   50% - (0.35/0.5)^2 = 0.49. The shadow map and the post/UI passes are a
	   fixed cost regardless, which is why this buys less than the pixel ratio
	   promises, but it is still the largest single lever left after Sun
	   Shadow's own size.
	 *
	 * This does not blur - PostProcessing.ps.hlsl point-samples the upscale
	   from the scene target now, so the visible effect is bigger voxels, not a
	   softer image, which is the right failure mode for this art. */
	SetResolutionScale(0.35f);

	SetShadowQuality(SHQ_HARD);

	/* 512, not 256, and this is a judged call rather than a measured one.
	 *
	 * The numbers, on an S23: 256 puts the Sun Shadow pass at 2.0 ms and 512 at
	 * 8.3 ms, so this default costs 6.3 ms - about a fifth of the frame - for
	 * nothing but shadow sharpness. It is chosen anyway because 256 was judged
	 * too coarse on screen: the map covers the whole 768-voxel resident window,
	 * so at 256 one texel is three voxels and a shadow edge stair-steps in
	 * chunks the eye reads as an artefact rather than as a style.
	 *
	 * The row is right there for anyone who would rather have the frame rate,
	 * which is the entire point of it being a setting.
	 *
	 * **The real answer to this trade is not on this line.** The map is sized to
	 * the resident window, but the camera never sees most of that window - fit
	 * the light-space rect to what is actually on screen and 256 texels resolve
	 * what 512 does now. That is the next thing worth building here. */
	SetSunShadowResolution(512);

	/* Only read at SHQ_RAY, which is not the mobile default - but if a player
	   picks it, an unbounded ray is 55 ms a frame on this hardware and 64 units
	   is roughly the contact shadows and nothing else. */
	SetShadowRayDistance(64.f);

	/* Off rather than AMQ_SIMPLE, on the evidence rather than on principle. The
	   twelve-neighbour term is genuinely cheap - measured on an S23, turning it
	   off moved the Voxel pass by well under a millisecond of seventeen - but
	   "cheap" is the wrong test when the frame is already over budget and the
	   thing it buys is contact darkening a player will not miss at half
	   resolution on a phone screen. It is one row in the settings menu for
	   anyone who wants it back. */
	SetAmbientQuality(AMQ_OFF);

	SetBounceLight(false);
	SetReflections(false);
	SetFXAA(false);
#endif
}