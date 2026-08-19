#include "VKSwapchain.h"

#include "VKResource.h"
#include "VKLegacySync.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

VKSwapchain::~VKSwapchain()
{
	Destroy();
}

bool VKSwapchain::Create(VKDevice* pDevice, VkSurfaceKHR surface, uint32_t uiWidth, uint32_t uiHeight, bool bVSync)
{
	m_pDevice = pDevice;
	m_Surface = surface;
	m_bVSync = bVSync;

	if (!CreateSwapchain(uiWidth, uiHeight))
		return false;

	if (!CreateImageViews())
		return false;

	return CreateFrameResources();
}

bool VKSwapchain::CreateSwapchain(uint32_t uiWidth, uint32_t uiHeight)
{
	VkPhysicalDevice physical = m_pDevice->GetPhysicalDevice();

	VkSurfaceCapabilitiesKHR caps{};
	vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, m_Surface, &caps);

	uint32_t uiFormatCount = 0;
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, m_Surface, &uiFormatCount, nullptr);

	if (uiFormatCount == 0)
	{
		fprintf(stderr, "[vulkan] surface reports no formats\n");
		return false;
	}

	std::vector<VkSurfaceFormatKHR> formats(uiFormatCount);
	vkGetPhysicalDeviceSurfaceFormatsKHR(physical, m_Surface, &uiFormatCount, formats.data());

	/* Must be UNORM, not SRGB, and this is not a preference - it is the one
	   thing standing between the engine's colours and a second gamma encode.
	 *
	 * Everything upstream renders into R_DEF_RESOURCE_FORMAT targets, which is
	 * E_R8G8B8A8_UNORM, and every shader writes values that are *already*
	 * gamma encoded: the art is authored in sRGB, lighting is applied to it
	 * directly, and AmbientOcclusion.hlsl hand-applies a pow(x, 2.2) precisely
	 * because the pipeline is in gamma space. Present is a vkCmdBlitImage from
	 * that target into the swapchain image, and a blit converts formats: an
	 * sRGB destination takes the source value as linear and encodes it. So an
	 * sRGB swapchain encodes a second time - 0.5 presents as 0.86 - which
	 * washes out and desaturates the entire frame, ImGui and the editor chrome
	 * along with the voxels, since they all composite into the same target
	 * before the blit.
	 *
	 * The comment that used to be here claimed R_DEF_RESOURCE_FORMAT was
	 * E_R8G8B8A8_UNORM_SRGB and picked the swapchain to match. It is not, and
	 * never was; that mistake is the whole bug, and it arrived with the port -
	 * D3D12 presented from a UNORM back buffer.
	 *
	 * Doing this properly means moving lighting to linear and encoding once,
	 * deliberately, on the way out - RENDERING_PLAN.md phase 6.4. Until then
	 * the correct present is a straight copy of bytes the shaders already
	 * encoded. */
	VkSurfaceFormatKHR chosen = formats[0];
	bool bFoundUnorm = false;

	for (const VkSurfaceFormatKHR& format : formats)
	{
		if ((format.format == VK_FORMAT_B8G8R8A8_UNORM || format.format == VK_FORMAT_R8G8B8A8_UNORM) &&
			format.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
		{
			chosen = format;
			bFoundUnorm = true;
			break;
		}
	}

	if (!bFoundUnorm)
	{
		/* Every desktop driver offers one, so this is close to unreachable -
		   but if it ever is reached the frame will be visibly washed out, and
		   that is worth naming rather than leaving to be rediscovered from a
		   screenshot. */
		fprintf(stderr, "[vulkan] surface offers no 8-bit UNORM format; presenting through %d, "
		                "which will gamma-encode the frame a second time\n",
		        static_cast<int>(chosen.format));
	}

	uint32_t uiPresentModeCount = 0;
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical, m_Surface, &uiPresentModeCount, nullptr);

	std::vector<VkPresentModeKHR> presentModes(uiPresentModeCount);
	vkGetPhysicalDeviceSurfacePresentModesKHR(physical, m_Surface, &uiPresentModeCount, presentModes.data());

	/* FIFO is the only mode guaranteed present.
	 *
	 * Mailbox is the lower-latency choice and used to be taken unconditionally,
	 * which made Settings::EnableVSync a setting that was serialized, stored
	 * and read by nothing. It is not a free win: mailbox presents every frame
	 * the engine finishes and the display shows whichever is newest at its
	 * vblank, so at 200 fps on a 60 Hz panel the displayed frames advance the
	 * world by 15 ms, then 20, then 15 - a beat, because 200/60 is not an
	 * integer. Every frame is delivered on time and the motion still reads as
	 * skipping. FIFO paces the engine to the display instead, so each shown
	 * frame carries an equal slice of world time.
	 *
	 * Which of latency and smoothness matters more is a judgement, so it is a
	 * setting rather than a constant - and it now behaves like one. */
	VkPresentModeKHR presentMode = VK_PRESENT_MODE_FIFO_KHR;

#if !defined(VOXAGINE_MOBILE)
	if (!m_bVSync)
	{
		for (VkPresentModeKHR mode : presentModes)
		{
			if (mode == VK_PRESENT_MODE_MAILBOX_KHR)
			{
				presentMode = mode;
				break;
			}
		}
	}
#else
	/* Mobile takes FIFO regardless of the setting. Mailbox means rendering
	   frames the panel will never show, which on a phone is heat and battery
	   spent on nothing - and a device that thermally throttles gets *worse*
	   latency than the one it was chasing. The setting stays meaningful on
	   desktop, where the cost is only a fan. */
	(void)m_bVSync;
#endif

	if (caps.currentExtent.width != UINT32_MAX)
	{
		m_Extent = caps.currentExtent;
	}
	else
	{
		m_Extent.width = std::clamp(uiWidth, caps.minImageExtent.width, caps.maxImageExtent.width);
		m_Extent.height = std::clamp(uiHeight, caps.minImageExtent.height, caps.maxImageExtent.height);
	}

	if (m_Extent.width == 0 || m_Extent.height == 0)
		return false;

	uint32_t uiImageCount = caps.minImageCount + 1;
	if (caps.maxImageCount > 0 && uiImageCount > caps.maxImageCount)
		uiImageCount = caps.maxImageCount;

	VkSwapchainCreateInfoKHR createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
	createInfo.surface = m_Surface;
	createInfo.minImageCount = uiImageCount;
	createInfo.imageFormat = chosen.format;
	createInfo.imageColorSpace = chosen.colorSpace;
	createInfo.imageExtent = m_Extent;
	createInfo.imageArrayLayers = 1;
	createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	/* preTransform is a promise, not a request: it tells the presentation
	 * engine what rotation the app has *already applied* to its own rendering.
	 *
	 * This was `caps.currentTransform`, which is correct on desktop for the
	 * boring reason that currentTransform is always IDENTITY there - so the
	 * promise was true by accident. On Android it is not. A phone whose natural
	 * orientation is portrait reports ROTATE_90 while running a landscape app,
	 * and the engine renders upright regardless, so claiming ROTATE_90 hands
	 * the compositor an upright image labelled as pre-rotated. The result is
	 * the entire game displayed on its side: caught on the first Android run,
	 * with the menu text reading bottom to top.
	 *
	 * Asking for IDENTITY makes the promise true again - the compositor then
	 * does the rotation, which costs it a full-screen pass. The faster answer
	 * on mobile is to keep currentTransform and rotate the projection to match,
	 * which is why the extension exists at all; that is a real change to every
	 * camera matrix and to the letterbox maths, and it should be done with a
	 * device to measure it against. Correct first. */
	createInfo.preTransform = (caps.supportedTransforms & VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR)
		? VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR
		: caps.currentTransform;

	m_bSuboptimalIsExpected = createInfo.preTransform != caps.currentTransform;

	if (m_bSuboptimalIsExpected && !m_bReportedTransform)
	{
		/* Once, not every swapchain: this is the compositor doing work every
		   frame that the app could have done for free inside its own
		   projection. */
		m_bReportedTransform = true;

		printf("[vulkan] surface wants transform %u; presenting untransformed and "
		       "letting the compositor rotate\n",
		       static_cast<unsigned>(caps.currentTransform));
	}
	createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
	createInfo.presentMode = presentMode;
	createInfo.clipped = VK_TRUE;
	createInfo.oldSwapchain = VK_NULL_HANDLE;

	const uint32_t uiFamilies[] = { m_pDevice->GetGraphicsQueueFamily(), m_pDevice->GetPresentQueueFamily() };

	if (uiFamilies[0] != uiFamilies[1])
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
		createInfo.queueFamilyIndexCount = 2;
		createInfo.pQueueFamilyIndices = uiFamilies;
	}
	else
	{
		createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
	}

	if (vkCreateSwapchainKHR(m_pDevice->Get(), &createInfo, nullptr, &m_Swapchain) != VK_SUCCESS)
	{
		fprintf(stderr, "[vulkan] vkCreateSwapchainKHR failed\n");
		return false;
	}

	m_Format = chosen.format;

	uint32_t uiActualCount = 0;
	vkGetSwapchainImagesKHR(m_pDevice->Get(), m_Swapchain, &uiActualCount, nullptr);

	m_Images.resize(uiActualCount);
	vkGetSwapchainImagesKHR(m_pDevice->Get(), m_Swapchain, &uiActualCount, m_Images.data());

	return true;
}

bool VKSwapchain::CreateImageViews()
{
	m_ImageViews.resize(m_Images.size(), VK_NULL_HANDLE);

	for (size_t i = 0; i < m_Images.size(); ++i)
	{
		VkImageViewCreateInfo viewInfo{};
		viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
		viewInfo.image = m_Images[i];
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
		viewInfo.format = m_Format;
		viewInfo.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		viewInfo.subresourceRange.levelCount = 1;
		viewInfo.subresourceRange.layerCount = 1;

		if (vkCreateImageView(m_pDevice->Get(), &viewInfo, nullptr, &m_ImageViews[i]) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] vkCreateImageView failed\n");
			return false;
		}
	}

	/* One present semaphore per swapchain image. */
	m_RenderFinished.resize(m_Images.size(), VK_NULL_HANDLE);

	for (size_t i = 0; i < m_RenderFinished.size(); ++i)
	{
		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		if (vkCreateSemaphore(m_pDevice->Get(), &semaphoreInfo, nullptr, &m_RenderFinished[i]) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] vkCreateSemaphore failed\n");
			return false;
		}
	}

	return true;
}

bool VKSwapchain::CreateFrameResources()
{
	VkCommandPoolCreateInfo poolInfo{};
	poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
	poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	poolInfo.queueFamilyIndex = m_pDevice->GetGraphicsQueueFamily();

	if (vkCreateCommandPool(m_pDevice->Get(), &poolInfo, nullptr, &m_CommandPool) != VK_SUCCESS)
	{
		fprintf(stderr, "[vulkan] vkCreateCommandPool failed\n");
		return false;
	}

	for (uint32_t i = 0; i < m_uiFramesInFlight; ++i)
	{
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_CommandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		if (vkAllocateCommandBuffers(m_pDevice->Get(), &allocInfo, &m_Frames[i].m_CommandBuffer) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] vkAllocateCommandBuffers failed\n");
			return false;
		}

		VkSemaphoreCreateInfo semaphoreInfo{};
		semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		/* Signalled, so the first frame does not block on a fence nothing submitted to. */
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

		if (vkCreateSemaphore(m_pDevice->Get(), &semaphoreInfo, nullptr, &m_Frames[i].m_ImageAvailable) != VK_SUCCESS ||
			vkCreateFence(m_pDevice->Get(), &fenceInfo, nullptr, &m_Frames[i].m_InFlight) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] frame sync object creation failed\n");
			return false;
		}
	}

	return true;
}

void VKSwapchain::TransitionImage(VkCommandBuffer cmd, VkImage image,
                                  VkImageLayout oldLayout, VkImageLayout newLayout) const
{
	VkImageMemoryBarrier2 barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image;
	barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	barrier.subresourceRange.levelCount = 1;
	barrier.subresourceRange.layerCount = 1;

	/* ALL_TRANSFER rather than CLEAR: the presented image is written by a clear
	   and a blit, and a clear-only scope does not cover the blit. */
	if (newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
	{
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_NONE;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
	}
	else
	{
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_ALL_TRANSFER_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_NONE;
	}

	VkDependencyInfo dependency{};
	dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
	dependency.imageMemoryBarrierCount = 1;
	dependency.pImageMemoryBarriers = &barrier;

	VKCmdPipelineBarrierLegacy(cmd, barrier);
}

bool VKSwapchain::ClearAndPresent(const float a_fColor[4])
{
	VkDevice device = m_pDevice->Get();
	FrameData& frame = m_Frames[m_uiFrameIndex];

	/* Wait only for this slot's previous submission, not for the whole GPU. */
	vkWaitForFences(device, 1, &frame.m_InFlight, VK_TRUE, UINT64_MAX);

	uint32_t uiImageIndex = 0;
	VkResult acquire = vkAcquireNextImageKHR(device, m_Swapchain, UINT64_MAX,
	                                         frame.m_ImageAvailable, VK_NULL_HANDLE, &uiImageIndex);

	if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
		return false;

	if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
	{
		fprintf(stderr, "[vulkan] vkAcquireNextImageKHR failed (%d)\n", acquire);
		return false;
	}

	/* Reset only after we know we are going to submit, or the fence stays
	   unsignalled and the next wait on this slot hangs. */
	vkResetFences(device, 1, &frame.m_InFlight);
	vkResetCommandBuffer(frame.m_CommandBuffer, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(frame.m_CommandBuffer, &beginInfo);

	TransitionImage(frame.m_CommandBuffer, m_Images[uiImageIndex],
	                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	VkClearColorValue clearColor{};
	clearColor.float32[0] = a_fColor[0];
	clearColor.float32[1] = a_fColor[1];
	clearColor.float32[2] = a_fColor[2];
	clearColor.float32[3] = a_fColor[3];

	VkImageSubresourceRange range{};
	range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	range.levelCount = 1;
	range.layerCount = 1;

	vkCmdClearColorImage(frame.m_CommandBuffer, m_Images[uiImageIndex],
	                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clearColor, 1, &range);

	TransitionImage(frame.m_CommandBuffer, m_Images[uiImageIndex],
	                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	vkEndCommandBuffer(frame.m_CommandBuffer);

	VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;

	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.waitSemaphoreCount = 1;
	submitInfo.pWaitSemaphores = &frame.m_ImageAvailable;
	submitInfo.pWaitDstStageMask = &waitStage;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &frame.m_CommandBuffer;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &m_RenderFinished[uiImageIndex];

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &m_RenderFinished[uiImageIndex];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &m_Swapchain;
	presentInfo.pImageIndices = &uiImageIndex;

	VkResult present = VK_SUCCESS;

	{
		std::lock_guard<std::mutex> lock(m_pDevice->GetQueueMutex());

		if (vkQueueSubmit(m_pDevice->GetGraphicsQueue(), 1, &submitInfo, frame.m_InFlight) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] vkQueueSubmit failed\n");
			return false;
		}

		present = vkQueuePresentKHR(m_pDevice->GetPresentQueue(), &presentInfo);
	}

	m_uiFrameIndex = (m_uiFrameIndex + 1) % m_uiFramesInFlight;

	/* OUT_OF_DATE always means rebuild. SUBOPTIMAL means "this still works but
	   is not ideal", and when the swapchain was deliberately created with a
	   preTransform the surface disagrees with, *every* present is suboptimal -
	   so acting on it rebuilds the swapchain forever, at the frame rate. That
	   is not hypothetical: it is what a Galaxy S23 does, and the emulator never
	   reported suboptimal at all. A genuine resize still arrives as
	   OUT_OF_DATE, and SDL's own resize event is what the engine actually
	   listens to (SDLWindowContext::ConsumeResizeRequest). */
	if (present == VK_SUBOPTIMAL_KHR && m_bSuboptimalIsExpected)
		return true;

	return present != VK_ERROR_OUT_OF_DATE_KHR && present != VK_SUBOPTIMAL_KHR;
}

bool VKSwapchain::BlitAndPresent(VKResource* pSource, VkSemaphore waitTimeline, uint64_t uiWaitValue)
{
	if (pSource == nullptr || pSource->GetKind() != VKResource::E_KIND_IMAGE)
		return false;

	VkDevice device = m_pDevice->Get();
	FrameData& frame = m_Frames[m_uiFrameIndex];

	vkWaitForFences(device, 1, &frame.m_InFlight, VK_TRUE, UINT64_MAX);

	uint32_t uiImageIndex = 0;
	VkResult acquire = vkAcquireNextImageKHR(device, m_Swapchain, UINT64_MAX,
	                                         frame.m_ImageAvailable, VK_NULL_HANDLE, &uiImageIndex);

	if (acquire == VK_ERROR_OUT_OF_DATE_KHR)
		return false;

	if (acquire != VK_SUCCESS && acquire != VK_SUBOPTIMAL_KHR)
	{
		fprintf(stderr, "[vulkan] vkAcquireNextImageKHR failed (%d)\n", acquire);
		return false;
	}

	vkResetFences(device, 1, &frame.m_InFlight);
	vkResetCommandBuffer(frame.m_CommandBuffer, 0);

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	VkCommandBuffer cmd = frame.m_CommandBuffer;
	vkBeginCommandBuffer(cmd, &beginInfo);

	TransitionImage(cmd, m_Images[uiImageIndex],
	                VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);

	/* The pass left this as a shader resource; the blit needs it readable as
	   a transfer source. VKResource tracks its own state, so this is a no-op
	   when it already matches. */
	pSource->Transition(cmd, E_STATE_COPY_SOURCE);

	const VkExtent3D srcExtent = pSource->GetExtent();

	/* Fit the source into the swapchain without distorting it; usually a 1:1
	   copy into a centred rect with black bars either side. Integer and
	   rounded, because a float ratio loses the exact fit for some window sizes
	   and a one-pixel mismatch resamples the whole frame. */
	uint32_t uiDstWidth = m_Extent.width;
	uint32_t uiDstHeight = m_Extent.height;

	if (srcExtent.width > 0 && srcExtent.height > 0)
	{
		const uint64_t uiTargetCross = static_cast<uint64_t>(m_Extent.width) * srcExtent.height;
		const uint64_t uiSourceCross = static_cast<uint64_t>(m_Extent.height) * srcExtent.width;

		if (uiTargetCross > uiSourceCross)
		{
			/* Window is wider than the image: pillarbox. */
			uiDstWidth = static_cast<uint32_t>((uiSourceCross + srcExtent.height / 2) / srcExtent.height);
		}
		else
		{
			uiDstHeight = static_cast<uint32_t>((uiTargetCross + srcExtent.width / 2) / srcExtent.width);
		}
	}

	uiDstWidth = std::min(std::max(uiDstWidth, 1u), m_Extent.width);
	uiDstHeight = std::min(std::max(uiDstHeight, 1u), m_Extent.height);

	const int32_t iOffsetX = static_cast<int32_t>((m_Extent.width - uiDstWidth) / 2);
	const int32_t iOffsetY = static_cast<int32_t>((m_Extent.height - uiDstHeight) / 2);

	/* Anything but a 1:1 blit resamples the whole frame; report it once. */
	{
		static uint32_t s_uiReportedWidth = 0;
		static uint32_t s_uiReportedHeight = 0;

		if (srcExtent.width != uiDstWidth || srcExtent.height != uiDstHeight)
		{
			if (s_uiReportedWidth != srcExtent.width || s_uiReportedHeight != srcExtent.height)
			{
				s_uiReportedWidth = srcExtent.width;
				s_uiReportedHeight = srcExtent.height;

				fprintf(stderr, "[vulkan] present rescales %ux%u to %ux%u in a %ux%u window\n",
				        srcExtent.width, srcExtent.height, uiDstWidth, uiDstHeight,
				        m_Extent.width, m_Extent.height);
			}
		}
	}

	/* The bars are never written by the blit, and a swapchain image is
	   recycled with whatever the last frame left in it. */
	if (iOffsetX != 0 || iOffsetY != 0)
	{
		VkClearColorValue black{};

		VkImageSubresourceRange range{};
		range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		range.levelCount = 1;
		range.layerCount = 1;

		vkCmdClearColorImage(cmd, m_Images[uiImageIndex],
		                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

		/* The clear covers the whole image and the blit then writes the centre
		   of it, so the two transfer writes overlap and nothing ordered them -
		   "vkCmdBlitImage writes to VkImage ... previously written by
		   vkCmdClearColorImage". Same layout either side, so this is a pure
		   execution and memory dependency, not a transition. */
		VkImageMemoryBarrier2 clearBarrier{};
		clearBarrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		clearBarrier.srcStageMask = VK_PIPELINE_STAGE_2_CLEAR_BIT;
		clearBarrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		clearBarrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
		clearBarrier.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		clearBarrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		clearBarrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		clearBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		clearBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		clearBarrier.image = m_Images[uiImageIndex];
		clearBarrier.subresourceRange = range;

		VkDependencyInfo clearDependency{};
		clearDependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		clearDependency.imageMemoryBarrierCount = 1;
		clearDependency.pImageMemoryBarriers = &clearBarrier;

		VKCmdPipelineBarrierLegacy(cmd, clearBarrier);
	}

	VkImageBlit region{};
	region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.srcSubresource.layerCount = 1;
	region.srcOffsets[1] = { static_cast<int32_t>(srcExtent.width),
	                         static_cast<int32_t>(srcExtent.height), 1 };
	region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
	region.dstSubresource.layerCount = 1;
	region.dstOffsets[0] = { iOffsetX, iOffsetY, 0 };
	region.dstOffsets[1] = { iOffsetX + static_cast<int32_t>(uiDstWidth),
	                         iOffsetY + static_cast<int32_t>(uiDstHeight), 1 };

	vkCmdBlitImage(cmd,
	               pSource->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	               m_Images[uiImageIndex], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	               1, &region, VK_FILTER_LINEAR);

	TransitionImage(cmd, m_Images[uiImageIndex],
	                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);

	vkEndCommandBuffer(cmd);

	/* Wait for the acquired image and, when rendering used a command engine,
	   for its timeline value. Legacy submission carries timeline values in its
	   pNext structure and works on Vulkan 1.2/MoltenVK as well as 1.3. */
	VkSemaphore waits[2] = { frame.m_ImageAvailable, waitTimeline };
	VkPipelineStageFlags waitStages[2] = {
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		VK_PIPELINE_STAGE_TRANSFER_BIT
	};
	uint64_t waitValues[2] = { 0, uiWaitValue };
	const uint32_t waitCount = waitTimeline != VK_NULL_HANDLE ? 2 : 1;
	const uint64_t signalValue = 0;
	VkTimelineSemaphoreSubmitInfo timelineInfo{};
	timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	timelineInfo.waitSemaphoreValueCount = waitCount;
	timelineInfo.pWaitSemaphoreValues = waitValues;
	timelineInfo.signalSemaphoreValueCount = 1;
	timelineInfo.pSignalSemaphoreValues = &signalValue;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pNext = &timelineInfo;
	submitInfo.waitSemaphoreCount = waitCount;
	submitInfo.pWaitSemaphores = waits;
	submitInfo.pWaitDstStageMask = waitStages;
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &m_RenderFinished[uiImageIndex];

	VkPresentInfoKHR presentInfo{};
	presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
	presentInfo.waitSemaphoreCount = 1;
	presentInfo.pWaitSemaphores = &m_RenderFinished[uiImageIndex];
	presentInfo.swapchainCount = 1;
	presentInfo.pSwapchains = &m_Swapchain;
	presentInfo.pImageIndices = &uiImageIndex;

	VkResult present = VK_SUCCESS;

	{
		/* Submit and present as one critical section: a job thread uploading a
		   texture shares this queue. */
		std::lock_guard<std::mutex> lock(m_pDevice->GetQueueMutex());

		if (vkQueueSubmit(m_pDevice->GetGraphicsQueue(), 1, &submitInfo, frame.m_InFlight) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] blit submit failed\n");
			return false;
		}

		present = vkQueuePresentKHR(m_pDevice->GetPresentQueue(), &presentInfo);
	}

	m_uiFrameIndex = (m_uiFrameIndex + 1) % m_uiFramesInFlight;

	/* OUT_OF_DATE always means rebuild. SUBOPTIMAL means "this still works but
	   is not ideal", and when the swapchain was deliberately created with a
	   preTransform the surface disagrees with, *every* present is suboptimal -
	   so acting on it rebuilds the swapchain forever, at the frame rate. That
	   is not hypothetical: it is what a Galaxy S23 does, and the emulator never
	   reported suboptimal at all. A genuine resize still arrives as
	   OUT_OF_DATE, and SDL's own resize event is what the engine actually
	   listens to (SDLWindowContext::ConsumeResizeRequest). */
	if (present == VK_SUBOPTIMAL_KHR && m_bSuboptimalIsExpected)
		return true;

	return present != VK_ERROR_OUT_OF_DATE_KHR && present != VK_SUBOPTIMAL_KHR;
}

bool VKSwapchain::SetVSync(bool bVSync)
{
	if (bVSync == m_bVSync)
		return true;

	m_bVSync = bVSync;

	/* Same rebuild a resize does, at the same extent. */
	return Recreate(m_Extent.width, m_Extent.height);
}

bool VKSwapchain::Recreate(uint32_t uiWidth, uint32_t uiHeight)
{
	vkDeviceWaitIdle(m_pDevice->Get());

	DestroySwapchainObjects();

	if (!CreateSwapchain(uiWidth, uiHeight))
		return false;

	return CreateImageViews();
}

void VKSwapchain::DestroySwapchainObjects()
{
	VkDevice device = m_pDevice->Get();

	for (VkSemaphore semaphore : m_RenderFinished)
	{
		if (semaphore != VK_NULL_HANDLE)
			vkDestroySemaphore(device, semaphore, nullptr);
	}
	m_RenderFinished.clear();

	for (VkImageView view : m_ImageViews)
	{
		if (view != VK_NULL_HANDLE)
			vkDestroyImageView(device, view, nullptr);
	}
	m_ImageViews.clear();
	m_Images.clear();

	if (m_Swapchain != VK_NULL_HANDLE)
	{
		vkDestroySwapchainKHR(device, m_Swapchain, nullptr);
		m_Swapchain = VK_NULL_HANDLE;
	}
}

void VKSwapchain::Destroy()
{
	if (m_pDevice == nullptr || m_pDevice->Get() == VK_NULL_HANDLE)
		return;

	VkDevice device = m_pDevice->Get();
	vkDeviceWaitIdle(device);

	DestroySwapchainObjects();

	for (uint32_t i = 0; i < m_uiFramesInFlight; ++i)
	{
		if (m_Frames[i].m_ImageAvailable != VK_NULL_HANDLE)
			vkDestroySemaphore(device, m_Frames[i].m_ImageAvailable, nullptr);

		if (m_Frames[i].m_InFlight != VK_NULL_HANDLE)
			vkDestroyFence(device, m_Frames[i].m_InFlight, nullptr);

		m_Frames[i] = FrameData{};
	}

	if (m_CommandPool != VK_NULL_HANDLE)
	{
		vkDestroyCommandPool(device, m_CommandPool, nullptr);
		m_CommandPool = VK_NULL_HANDLE;
	}

	m_pDevice = nullptr;
}
