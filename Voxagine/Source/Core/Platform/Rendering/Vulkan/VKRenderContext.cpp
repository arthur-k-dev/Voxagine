#include "pch.h"
#include "VKRenderContext.h"

#include "Core/Application.h"
#include "Core/LoggingSystem/LoggingSystem.h"
#include "Core/Platform/Platform.h"
#include "Core/Platform/Window/SDL/SDLWindowContext.h"
#include "Core/Settings.h"

#include "Core/Resources/Formats/ShaderReference.h"

#include "Core/Platform/Rendering/FrameProfiler.h"
#include "Core/Platform/Rendering/Objects/View.h"
#include "Core/Platform/Rendering/Vulkan/VKCommandEngine.h"
#include "Core/Platform/Rendering/Vulkan/Managers/VKModelManager.h"
#include "Core/Platform/Rendering/Vulkan/Managers/VKTextureManager.h"

#include "External/imgui/imgui.h"

#include <cstdio>

namespace
{
void LogVulkanStartup(Platform* pPlatform, LogLevel level, const std::string& message)
{
	pPlatform->GetApplication()->GetLoggingSystem().Log(level, "Vulkan", message);
}
}

VKRenderContext::VKRenderContext(Platform* pPlatform) : RenderContext(pPlatform)
{
}

VKRenderContext::~VKRenderContext()
{
	Deinitialize();
}

bool VKRenderContext::InitializeBackend()
{
	m_StartupError.clear();
	SDLWindowContext* pWindow = static_cast<SDLWindowContext*>(m_pPlatform->GetWindowContext());

	if (pWindow == nullptr)
	{
		fprintf(stderr, "[vulkan] no window context to present to\n");
		m_StartupError = "No SDL window context is available.";
		LogVulkanStartup(m_pPlatform, LOGLEVEL_CRITICAL_ERROR, "Startup failed: no SDL window context.");
		return false;
	}

	bool bValidation = false;
#ifdef _DEBUG
	bValidation = true;
#endif

	/* VOXAGINE_VALIDATION forces the layers either way, same shape as
	   VOXAGINE_PROFILE above and for the same reason: the defect worth
	   validating only reproduces in Release. The Xid 109 hang has never once
	   been caught in Debug - it is timing dependent, and Debug is slow enough
	   and the layers serializing enough to hide it - so gating validation on
	   _DEBUG means the only build that faults is the only build that cannot
	   say why. Pair with
	   VK_LAYER_ENABLES=VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT
	   to get the write-after-read hazards, which is what a GPU stuck on work
	   no pass timer shows as slow would look like. */
	if (const char* pEnv = std::getenv("VOXAGINE_VALIDATION"))
		bValidation = (pEnv[0] != '0');

	if (!m_Device.CreateInstance(pWindow->GetRequiredInstanceExtensions(), bValidation))
	{
		m_StartupError = "Failed to create the Vulkan instance: " + m_Device.GetStartupError();
		LogVulkanStartup(m_pPlatform, LOGLEVEL_CRITICAL_ERROR,
			m_StartupError);
		return false;
	}

	if (!pWindow->CreateSurface(m_Device.GetInstance(), &m_Surface))
	{
		m_StartupError = "SDL could not create a Vulkan presentation surface.";
		LogVulkanStartup(m_pPlatform, LOGLEVEL_CRITICAL_ERROR, m_StartupError);
		return false;
	}

	if (!m_Device.CreateDevice(m_Surface))
	{
		m_StartupError = "Failed to create the Vulkan device: " + m_Device.GetStartupError();
		LogVulkanStartup(m_pPlatform, LOGLEVEL_CRITICAL_ERROR,
			m_StartupError);
		return false;
	}

	m_Allocator.Initialize(&m_Device);

	const UVector2 size = pWindow->GetSize();

	/* EnableVSync used to be stored and read by nothing; the swapchain took
	   mailbox unconditionally. See the present-mode choice in VKSwapchain. */
	if (!m_Swapchain.Create(&m_Device, m_Surface, size.x, size.y,
	                        m_pPlatform->GetApplication()->GetSettings().IsVSyncEnabled()))
	{
		m_StartupError = "Vulkan swapchain creation failed.";
		LogVulkanStartup(m_pPlatform, LOGLEVEL_CRITICAL_ERROR, m_StartupError);
		return false;
	}

	/* The editor shows this; it used to come from the DXGI adapter. */
	const std::string name = m_Device.GetDeviceName();
	m_pPlatform->GetApplication()->GetSettings().SetGPUName(
		std::wstring(name.begin(), name.end()).c_str());

	m_v2ScreenResolution = UVector2(m_Swapchain.GetExtent().width, m_Swapchain.GetExtent().height);

	printf("[vulkan] %s, %ux%u, %u swapchain images, %s\n", name.c_str(),
	       m_Swapchain.GetExtent().width, m_Swapchain.GetExtent().height,
	       m_Swapchain.GetImageCount(),
	       m_Swapchain.IsVSyncEnabled() ? "FIFO (vsync)" : "mailbox where offered");

	return true;
}

void VKRenderContext::Initialize()
{
	m_bBackendReady = InitializeBackend();

	if (!m_bBackendReady)
	{
		fprintf(stderr, "[vulkan] backend initialization failed; renderer is inert\n");
		LogVulkanStartup(m_pPlatform, LOGLEVEL_CRITICAL_ERROR,
			"Renderer is unavailable; world loading will not start.");
		return;
	}

	/* What the renderer is set to, before a single frame - a frame rate quoted
	   without this says nothing now that every one of them is a setting. */
	LogRenderSettings();

	/* Decided before any command engine is created: their Initialize()
	   reads this to decide whether to allocate query pools at all.

	   VOXAGINE_PROFILE forces it either way, because the costs worth
	   measuring - the bake, the chunk load - are the ones that behave
	   differently in Release, which is exactly where the default is off. */
	bool bProfiling = m_pPlatform->GetApplication()->GetSettings().IsGPUProfilingEnabled();

	if (const char* pEnv = std::getenv("VOXAGINE_PROFILE"))
		bProfiling = (pEnv[0] != '0');

	FrameProfiler::Get().SetEnabled(bProfiling);

	/* Same set of engines DX12RenderContext created, under the same names -
	   RenderContext::Present looks them up by string. */
	const CommandEngine::Info engines[] = {
		{ CommandEngine::E_COPY,    "Copy" },
		{ CommandEngine::E_DIRECT,  "Direct" },
		{ CommandEngine::E_DIRECT,  "Texture" },
		{ CommandEngine::E_DIRECT,  "VDirect" },
		{ CommandEngine::E_COMPUTE, "Compute" },
	};

	for (const CommandEngine::Info& info : engines)
	{
		std::unique_ptr<PCommandEngine> pEngine =
			std::make_unique<PCommandEngine>(&m_Device, &m_Allocator, info);

		if (!pEngine->Initialize())
		{
			fprintf(stderr, "[vulkan] command engine '%s' failed\n", info.m_Name.c_str());
			LogVulkanStartup(m_pPlatform, LOGLEVEL_CRITICAL_ERROR,
				"Startup failed: command engine " + info.m_Name + " could not initialize.");
			return;
		}

		m_pCommandEngines.emplace(info.m_Name, std::move(pEngine));
	}

	m_pTextureManager = std::make_unique<PTextureManager>(this);
	m_pModelManager = std::make_unique<PModelManager>(this);

	RenderContext::Initialize();

	/* Builds the buffers, samplers, mappers and the six passes. It had no
	   caller once DX12RenderContext was deleted, which is why the renderer
	   only ever cleared. */
	InitializeRenderLoop();
	LogVulkanStartup(m_pPlatform, LOGLEVEL_MESSAGE, "Renderer initialization complete.");
}

void VKRenderContext::Deinitialize()
{
	if (m_Device.Get() != VK_NULL_HANDLE)
		vkDeviceWaitIdle(m_Device.Get());

	/* These belong to RenderContext, whose members are destroyed after this
	   derived body runs - too late, they hold live device objects. Dependency
	   order: passes reference views, engines own the upload pages. */
	m_pRenderPasses.clear();
	m_pComputePasses.clear();
	m_pCommandEngines.clear();

	m_pTextureManager.reset();
	m_pModelManager.reset();

	m_mBuffers.clear();
	m_pMappers.clear();
	m_pSamplers.clear();
	m_pShaders.clear();
	m_pViews.clear();

	m_Swapchain.Destroy();

	if (m_Surface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_Device.GetInstance(), m_Surface, nullptr);
		m_Surface = VK_NULL_HANDLE;
	}

	m_Device.Destroy();
	m_bBackendReady = false;
}

void VKRenderContext::SuspendForBackground()
{
	if (!m_bBackendReady)
		return;

	/* Every command buffer this surface's images could still be referenced by
	   has to be retired before the surface goes away under it - the same
	   reason a resize waits, just triggered by onPause instead of a
	   WM_SIZE-equivalent. */
	WaitForGPU();

	m_Swapchain.Destroy();

	if (m_Surface != VK_NULL_HANDLE)
	{
		vkDestroySurfaceKHR(m_Device.GetInstance(), m_Surface, nullptr);
		m_Surface = VK_NULL_HANDLE;
	}

	/* Clear()/Present() already early-out on this - the same guard that
	   covers the window between InitializeBackend failing and the app giving
	   up, now covers the window between backgrounding and returning. */
	m_bBackendReady = false;

	printf("[vulkan] surface and swapchain released for the background\n");
}

bool VKRenderContext::ResumeFromBackground()
{
	SDLWindowContext* pWindow = static_cast<SDLWindowContext*>(m_pPlatform->GetWindowContext());

	if (pWindow == nullptr)
		return false;

	/* A fresh VkSurfaceKHR from whatever ANativeWindow the platform handed
	   SDL this time - not necessarily the one destroyed in
	   SuspendForBackground, and it does not need to be: nothing here assumes
	   continuity with the old one. */
	if (!pWindow->CreateSurface(m_Device.GetInstance(), &m_Surface))
	{
		fprintf(stderr, "[vulkan] could not recreate the surface on foreground re-entry\n");
		return false;
	}

	const UVector2 size = pWindow->GetSize();

	if (!m_Swapchain.Create(&m_Device, m_Surface, size.x, size.y,
	                        m_pPlatform->GetApplication()->GetSettings().IsVSyncEnabled()))
	{
		fprintf(stderr, "[vulkan] could not recreate the swapchain on foreground re-entry\n");

		vkDestroySurfaceKHR(m_Device.GetInstance(), m_Surface, nullptr);
		m_Surface = VK_NULL_HANDLE;

		return false;
	}

	m_v2ScreenResolution = UVector2(m_Swapchain.GetExtent().width, m_Swapchain.GetExtent().height);
	m_bBackendReady = true;

	/* The base implementation, deliberately not this class's own OnResize
	   override: that one starts by recreating the swapchain, which was just
	   built fresh above from a surface that did not exist a moment ago. This
	   only has to do the other half - resize render targets and fire
	   SizeChanged - and only if the size actually moved, which covers a
	   rotation that happened while the app had no surface to notice it with. */
	RenderContext::OnResize(m_v2ScreenResolution.x, m_v2ScreenResolution.y);

	printf("[vulkan] surface and swapchain rebuilt on returning from background, %ux%u\n",
	       m_v2ScreenResolution.x, m_v2ScreenResolution.y);

	return true;
}

void VKRenderContext::Clear()
{
	if (!m_bBackendReady)
		return;

	RenderContext::Clear();
}

bool VKRenderContext::Present()
{
	if (!m_bBackendReady)
		return false;

	if (!RenderContext::Present())
		return false;

	/* DX12 pointed the pass whose target type is E_STATE_PRESENT straight at
	   the swapchain buffers, so presenting was a flip. Here the pass owns its
	   own image and it is blitted across, which also rescales when the render
	   resolution differs from the window. */
	PRenderPass* pScreenPass = nullptr;

	for (auto& entry : m_pRenderPasses)
	{
		if (entry.second != nullptr && entry.second->GetData().m_TargetType == E_STATE_PRESENT)
		{
			pScreenPass = entry.second.get();
			break;
		}
	}

	if (pScreenPass == nullptr)
		return false;

	View* pSourceView = pScreenPass->GetTargetView(0);

	if (pSourceView == nullptr || pSourceView->GetNative() == nullptr)
		return false;

	/* Wait on the engine that drew the frame, not on the whole device. */
	PCommandEngine* pDirectEngine = m_pCommandEngines["Direct"].get();

	const VkSemaphore timeline = pDirectEngine != nullptr ? pDirectEngine->GetTimeline() : VK_NULL_HANDLE;
	const uint64_t uiValue = pDirectEngine != nullptr ? pDirectEngine->GetValue() : 0;

	if (!m_Swapchain.BlitAndPresent(pSourceView->GetNative(), timeline, uiValue))
	{
		const UVector2 size = m_pPlatform->GetWindowContext()->GetSize();

		if (size.x == 0 || size.y == 0)
			return false;

		if (!m_Swapchain.Recreate(size.x, size.y))
			return false;
	}

	m_uiFrameIndex = (m_uiFrameIndex + 1) % m_uiFrameCount;

	return true;
}

void VKRenderContext::SetVSyncEnabled(bool bEnabled)
{
	if (!m_bBackendReady || bEnabled == m_Swapchain.IsVSyncEnabled())
		return;

	/* Kept on Settings as well as on the swapchain: the swapchain is the only
	   thing that acts on it, but Settings is what is serialized and what the
	   next run reads. Letting the two disagree is what made this a dead
	   setting in the first place. */
	m_pPlatform->GetApplication()->GetSettings().SetVSync(bEnabled);

	if (!m_Swapchain.SetVSync(bEnabled))
		fprintf(stderr, "[vulkan] swapchain rebuild for vsync=%d failed\n", static_cast<int>(bEnabled));
}

bool VKRenderContext::OnResize(uint32_t uiWidth, uint32_t uiHeight)
{
	if (!m_bBackendReady || uiWidth == 0 || uiHeight == 0)
		return false;

	if (!m_Swapchain.Recreate(uiWidth, uiHeight))
		return false;

	m_v2ScreenResolution = UVector2(m_Swapchain.GetExtent().width, m_Swapchain.GetExtent().height);

	if (!RenderContext::OnResize(uiWidth, uiHeight))
		return false;

	/* The base class only notifies SizeChanged listeners (camera, window);
	   the render targets themselves are resized here. Sized from the render
	   resolution rather than the window: with an aspect ratio locked those
	   differ, and a target larger than the viewport the shaders divide by
	   pushes sample coordinates past 1, which a repeating sampler tiles. */
	const UVector2 resolution = GetRenderResolution();

	for (auto& entry : m_pRenderPasses)
	{
		PRenderPass* pPass = entry.second.get();

		if (pPass == nullptr || !pPass->GetData().m_bUseScreenResolution)
			continue;

		const float fScale = pPass->GetData().m_fRenderScale;

		pPass->Resize(UVector2(static_cast<uint32_t>(resolution.x * fScale),
		                       static_cast<uint32_t>(resolution.y * fScale)));
	}

	return true;
}

/* The two render settings that are image sizes rather than shader constants -
 * RenderContext::ApplyRenderSettings says which and why. Reached from
 * Settings::RenderQualityChanged, so it runs on any quality change and does
 * nothing for most of them, which is cheaper than working out which changed.
 *
 * Every Resize below idles the device before reallocating, so this is not
 * something to call per frame. It is called when a human moves a menu row. */
void VKRenderContext::ApplyRenderSettings()
{
	if (!m_bBackendReady)
		return;

	Settings& settings = m_pPlatform->GetApplication()->GetSettings();

	const float fResolutionScale = settings.GetResolutionScale();
	const UVector2 resolution = GetRenderResolution();

	for (auto& entry : m_pRenderPasses)
	{
		PRenderPass* pPass = entry.second.get();

		if (pPass == nullptr)
			continue;

		/* ResolutionScale. The Voxel and Particle passes carry a copy of it
		   taken at construction, and it has to be updated as well as acted on -
		   the window-resize path above re-reads it from the pass rather than
		   from Settings, so a stale copy would undo this on the next resize. */
		if (pPass->GetData().m_bFollowsResolutionScale)
		{
			pPass->SetRenderScale(fResolutionScale);

			const UVector2 target(
				static_cast<uint32_t>(resolution.x * fResolutionScale),
				static_cast<uint32_t>(resolution.y * fResolutionScale));

			pPass->Resize(target);

			/* Logged for the same reason VKRenderPass logs the size it creates
			   its attachments at: a pass time is only comparable against another
			   if you know how many pixels each was. A resolution change that
			   silently did not take is indistinguishable from one that did and
			   bought nothing, and both look like "the setting does nothing". */
			printf("[render] pass '%s' resized to %ux%u (scale %.3f)\n",
			       pPass->GetData().m_Name.c_str(), target.x, target.y, fResolutionScale);
		}
	}

	/* ShadowQuality, as the size of the map. Not skipped at SHQ_OFF: the pass
	   is simply not drawn in that mode and the target it is not drawing into
	   should be the small one.
	 *
	 * All three sun shadow targets, not just the world one - DYNAMIC_MODELS_PLAN.md
	 * phase 4. SunShadowCombine.ps.hlsl samples "Sun Shadow" and "Sun Shadow
	 * Models" texel for texel and only exists when shadows are enabled in the
	 * first place, so the three have to stay the same size as each other or
	 * the combine reads the wrong texels - and VoxelPass binds the combine
	 * pass's own target as its shadow map, so that one has to resize too. */
	for (const char* pShadowPassName : { "Sun Shadow", "Sun Shadow Models", "Sun Shadow Combine" })
	{
		auto shadowEntry = m_pRenderPasses.find(pShadowPassName);

		if (shadowEntry == m_pRenderPasses.end() || shadowEntry->second == nullptr)
			continue;

		const uint32_t uiResolution = settings.GetSunShadowResolution();

		shadowEntry->second->Resize(UVector2(uiResolution, uiResolution));

		printf("[render] pass '%s' resized to %ux%u\n", pShadowPassName, uiResolution, uiResolution);
	}

	LogRenderSettings();
}

/* One line saying what the renderer is actually doing, on every change and once
   at startup.
 *
 * It exists because a frame rate is meaningless without it. Every one of these
 * is the player's to change now, so "13 fps" and "60 fps" can be the same build,
 * the same device and the same scene - and on a phone the log is the only way to
 * ask what a setting ended up as, since there is no console and the answer lives
 * three layers down in PlayerPrefs. */
void VKRenderContext::LogRenderSettings() const
{
	Settings& settings = m_pPlatform->GetApplication()->GetSettings();

	static const char* const k_pShadow[] = { "off", "hard", "soft", "sharp" };
	static const char* const k_pAmbient[] = { "off", "simple", "cone" };

	const uint32_t uiShadow = static_cast<uint32_t>(settings.GetShadowQuality());
	const uint32_t uiAmbient = static_cast<uint32_t>(settings.GetAmbientQuality());

	printf("[render] shadows=%s (map %u) ao=%s bounce=%s reflections=%s fxaa=%s "
	       "scale=%.2f vsync=%s\n",
	       uiShadow < 4 ? k_pShadow[uiShadow] : "?",
	       settings.GetSunShadowResolution(),
	       uiAmbient < 3 ? k_pAmbient[uiAmbient] : "?",
	       settings.IsBounceLightEnabled() ? "on" : "off",
	       settings.IsReflectionEnabled() ? "on" : "off",
	       settings.IsFXAAEnabled() ? "on" : "off",
	       settings.GetResolutionScale(),
	       settings.IsVSyncEnabled() ? "on" : "off");
}

void VKRenderContext::LoadShader(ShaderReference* pShaderReference)
{
	/* Shaders are compiled to SPIR-V ahead of time by CMake/Shaders.cmake.
	   Wiring ShaderReference to those modules belongs with VKShader, which
	   lands alongside the pass layer. */
	VX_UNUSED(pShaderReference);
}

void VKRenderContext::DestroyShader(const ShaderReference* pShaderReference)
{
	VX_UNUSED(pShaderReference);
}

void RenderContext::Report()
{
	/* DX12 called DXGI's ReportLiveObjects here. Vulkan has no equivalent
	   built in - leaked handles are found with the validation layers'
	   object-lifetime tracking, which reports at instance destruction, so
	   there is nothing to trigger from here. Kept because Platform calls it
	   under _DEBUG. */
	printf("[vulkan] leak reporting is handled by the validation layers\n");
}

/* --- Frame capture (LaunchOptions.h, --screenshot) -------------------------
   Copies a pass's render target into a host-visible buffer and writes it out as
   a binary PPM. Stalls the GPU twice and allocates the size of the target, so
   it is a debugging facility only.

   PPM rather than PNG because the engine vendors stb_image but not
   stb_image_write, and a capture path is not worth a new dependency. Converting
   is one step outside the process.

   Why it exists: so a rendering change can be looked at without putting a
   window on the developer's display, and so an intermediate target can be
   inspected directly - "Sun Shadow" dumps the shadow map itself - instead of
   writing, compiling and then deleting a debug shader for it. */
void VKRenderContext::CaptureTarget(const std::string& passName, const std::string& path)
{
	auto found = m_pRenderPasses.find(passName);

	if (found == m_pRenderPasses.end() || found->second == nullptr)
	{
		fprintf(stderr, "[capture] no pass named '%s'\n", passName.c_str());
		return;
	}

	View* pView = found->second->GetTargetView();
	VKResource* pImage = pView != nullptr ? pView->GetNative() : nullptr;

	if (pImage == nullptr || pImage->GetImage() == VK_NULL_HANDLE)
	{
		fprintf(stderr, "[capture] pass '%s' has no target image\n", passName.c_str());
		return;
	}

	const VkExtent3D extent = pImage->GetExtent();
	const VkFormat format = pImage->GetFormat();

	/* Only the two the passes actually produce. Anything else would need its
	   own unpack below and would be silently wrong without one. */
	uint32_t uiBytesPerTexel = 0;

	if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_B8G8R8A8_UNORM)
		uiBytesPerTexel = 4;
	else if (format == VK_FORMAT_R32_SFLOAT)
		uiBytesPerTexel = 4;
	else
	{
		fprintf(stderr, "[capture] unsupported format %d on pass '%s'\n", (int)format, passName.c_str());
		return;
	}

	const VkDeviceSize uiSize =
		static_cast<VkDeviceSize>(extent.width) * extent.height * uiBytesPerTexel;

	VKResource staging;

	if (!staging.CreateBuffer(&m_Device, &m_Allocator, uiSize,
	                          VK_BUFFER_USAGE_TRANSFER_DST_BIT,
	                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
	{
		fprintf(stderr, "[capture] staging buffer allocation failed\n");
		return;
	}

	PCommandEngine* pEngine = m_pCommandEngines["Direct"].get();

	pEngine->WaitForGPU();
	pEngine->Reset();
	pEngine->Start();

	/* The target is left in whatever state the pass ended in; take it to
	   TRANSFER_SRC and put it back, or the next frame's render pass begins
	   against a layout it does not expect. */
	const PEResourceState previous = pImage->GetState();

	pEngine->QueueBarrier(pImage, E_STATE_COPY_SOURCE);
	pEngine->ApplyBarriers();

	VkBufferImageCopy region{};
	region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.imageSubresource.layerCount = 1;
	region.imageExtent = { extent.width, extent.height, 1 };

	vkCmdCopyImageToBuffer(pEngine->GetCommandBuffer(), pImage->GetImage(),
	                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, staging.GetBuffer(), 1, &region);

	pEngine->QueueBarrier(pImage, previous);
	pEngine->ApplyBarriers();

	pEngine->Execute();
	pEngine->WaitForGPU();

	const uint8_t* pData = static_cast<const uint8_t*>(staging.Map());

	if (pData == nullptr)
	{
		fprintf(stderr, "[capture] could not map staging buffer\n");
		staging.Destroy();
		return;
	}

	FILE* pFile = fopen(path.c_str(), "wb");

	if (pFile == nullptr)
	{
		fprintf(stderr, "[capture] could not open '%s'\n", path.c_str());
		staging.Unmap();
		staging.Destroy();
		return;
	}

	fprintf(pFile, "P6\n%u %u\n255\n", extent.width, extent.height);

	std::vector<uint8_t> row(static_cast<size_t>(extent.width) * 3);

	/* R32_FLOAT is the sun shadow map: a light-space depth, not a colour. It is
	   normalised against the largest finite value present so the result is
	   readable as an image at all - an absolute scale would be nearly black
	   everywhere, and the misses are 1e9. */
	float fMaxDepth = 1.0f;

	if (format == VK_FORMAT_R32_SFLOAT)
	{
		const float* pFloats = reinterpret_cast<const float*>(pData);

		for (VkDeviceSize i = 0; i < uiSize / 4; ++i)
		{
			if (pFloats[i] < 1.0e8f && pFloats[i] > fMaxDepth)
				fMaxDepth = pFloats[i];
		}
	}

	for (uint32_t y = 0; y < extent.height; ++y)
	{
		const uint8_t* pSource = pData + static_cast<size_t>(y) * extent.width * uiBytesPerTexel;

		for (uint32_t x = 0; x < extent.width; ++x)
		{
			uint8_t r, g, b;

			if (format == VK_FORMAT_R32_SFLOAT)
			{
				float fDepth;
				memcpy(&fDepth, pSource + static_cast<size_t>(x) * 4, sizeof(float));

				/* A miss reads as pure red, so "nothing in this column" is
				   distinguishable at a glance from "hit at depth zero". */
				if (fDepth > 1.0e8f)
				{
					r = 255; g = 0; b = 0;
				}
				else
				{
					const uint8_t uiValue = static_cast<uint8_t>(
						std::min(255.0f, std::max(0.0f, fDepth / fMaxDepth * 255.0f)));
					r = g = b = uiValue;
				}
			}
			else
			{
				const uint8_t* pTexel = pSource + static_cast<size_t>(x) * 4;

				if (format == VK_FORMAT_B8G8R8A8_UNORM)
				{
					b = pTexel[0]; g = pTexel[1]; r = pTexel[2];
				}
				else
				{
					r = pTexel[0]; g = pTexel[1]; b = pTexel[2];
				}
			}

			row[static_cast<size_t>(x) * 3 + 0] = r;
			row[static_cast<size_t>(x) * 3 + 1] = g;
			row[static_cast<size_t>(x) * 3 + 2] = b;
		}

		fwrite(row.data(), 1, row.size(), pFile);
	}

	fclose(pFile);

	staging.Unmap();
	staging.Destroy();

	fprintf(stderr, "[capture] wrote '%s' (%ux%u, pass '%s')\n",
	        path.c_str(), extent.width, extent.height, passName.c_str());
}
