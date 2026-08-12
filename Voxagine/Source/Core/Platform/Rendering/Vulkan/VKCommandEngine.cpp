#include "VKCommandEngine.h"

#include "VKAllocator.h"
#include "VKDevice.h"
#include "VKLegacySync.h"
#include "VKTranslate.h"

#include "Core/Platform/Rendering/FrameProfiler.h"

#include <algorithm>
#include <cstdio>
#include <mutex>

VKCommandEngine::VKCommandEngine(VKDevice* pDevice, const VKAllocator* pAllocator, const Info& info)
	: CommandEngine(info)
	, m_pDevice(pDevice)
	, m_pAllocator(pAllocator)
{
}

VKCommandEngine::~VKCommandEngine()
{
	if (m_pDevice == nullptr || m_pDevice->Get() == VK_NULL_HANDLE)
		return;

	WaitForGPU();

	m_pUploadBuffer.reset();

	for (uint32_t i = 0; i < m_uiFrameCount; ++i)
	{
		if (m_Frames[i].m_CommandPool != VK_NULL_HANDLE)
			vkDestroyCommandPool(m_pDevice->Get(), m_Frames[i].m_CommandPool, nullptr);

		if (m_Frames[i].m_QueryPool != VK_NULL_HANDLE)
			vkDestroyQueryPool(m_pDevice->Get(), m_Frames[i].m_QueryPool, nullptr);
	}

	if (m_Timeline != VK_NULL_HANDLE)
		vkDestroySemaphore(m_pDevice->Get(), m_Timeline, nullptr);
}

bool VKCommandEngine::Initialize()
{
	/* One queue family for now. VKDevice only surfaces graphics and present;
	   E_COPY and E_COMPUTE would benefit from dedicated transfer/compute
	   families, which is a VKDevice change rather than one here. */
	m_Queue = m_pDevice->GetGraphicsQueue();
	m_uiQueueFamily = m_pDevice->GetGraphicsQueueFamily();

	VkSemaphoreTypeCreateInfo typeInfo{};
	typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
	typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	typeInfo.initialValue = 0;

	VkSemaphoreCreateInfo semaphoreInfo{};
	semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
	semaphoreInfo.pNext = &typeInfo;

	if (vkCreateSemaphore(m_pDevice->Get(), &semaphoreInfo, nullptr, &m_Timeline) != VK_SUCCESS)
	{
		fprintf(stderr, "[vulkan] timeline semaphore creation failed for '%s'\n",
		        m_Info.m_Name.c_str());
		return false;
	}

	/* RENDERING_PLAN.md Phase 0. Decided once here, not per-call: a query
	   pool that failed to create would otherwise make every later
	   WriteTimestamp* call re-check a handle for no reason. */
	m_bTimingEnabled = FrameProfiler::Get().IsEnabled() && m_pDevice->SupportsTimestamps();

	for (uint32_t i = 0; i < m_uiFrameCount; ++i)
	{
		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_uiQueueFamily;

		/* One pool per frame slot, reset wholesale. That is cheaper than
		   resetting buffers individually and matches how the D3D12 backend
		   recycled its allocators. */
		if (vkCreateCommandPool(m_pDevice->Get(), &poolInfo, nullptr, &m_Frames[i].m_CommandPool) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] command pool creation failed for '%s'\n",
			        m_Info.m_Name.c_str());
			return false;
		}

		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_Frames[i].m_CommandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = 1;

		if (vkAllocateCommandBuffers(m_pDevice->Get(), &allocInfo, &m_Frames[i].m_CommandBuffer) != VK_SUCCESS)
		{
			fprintf(stderr, "[vulkan] command buffer allocation failed for '%s'\n",
			        m_Info.m_Name.c_str());
			return false;
		}

		if (m_bTimingEnabled)
		{
			VkQueryPoolCreateInfo queryPoolInfo{};
			queryPoolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
			queryPoolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
			queryPoolInfo.queryCount = m_uiMaxTimestampsPerFrame;

			if (vkCreateQueryPool(m_pDevice->Get(), &queryPoolInfo, nullptr, &m_Frames[i].m_QueryPool) != VK_SUCCESS)
			{
				fprintf(stderr, "[vulkan] query pool creation failed for '%s'; GPU timing disabled\n",
				        m_Info.m_Name.c_str());
				m_bTimingEnabled = false;
			}
		}
	}

	m_pUploadBuffer = std::make_unique<VKUploadBuffer>(m_pDevice, m_pAllocator);

	return true;
}

VkCommandBuffer VKCommandEngine::GetCommandBuffer() const
{
	return m_Frames[m_uiFrameIndex].m_CommandBuffer;
}

void VKCommandEngine::Reset()
{
	FrameData& frame = m_Frames[m_uiFrameIndex];

	/* Wait only until this slot's own previous submission has retired. The
	   other slot may still be in flight, which is the entire point of double
	   buffering. */
	if (frame.m_uiSubmitValue > 0)
	{
		uint64_t uiCompleted = 0;
		vkGetSemaphoreCounterValue(m_pDevice->Get(), m_Timeline, &uiCompleted);

		if (uiCompleted < frame.m_uiSubmitValue)
		{
			VkSemaphoreWaitInfo waitInfo{};
			waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
			waitInfo.semaphoreCount = 1;
			waitInfo.pSemaphores = &m_Timeline;
			waitInfo.pValues = &frame.m_uiSubmitValue;

			vkWaitSemaphores(m_pDevice->Get(), &waitInfo, UINT64_MAX);
		}
	}

	/* The wait above guarantees this slot's previous submission has
	   retired, so its queries are ready - vkGetQueryPoolResults here never
	   stalls waiting on the GPU, it just reads results already sitting
	   there. This is the "one frame later" readback RENDERING_PLAN.md Phase
	   0 asks for. */
	if (m_bTimingEnabled && !frame.m_PendingQueries.empty())
	{
		std::vector<uint64_t> results(frame.m_uiQueryCursor, 0);

		const VkResult queryResult = vkGetQueryPoolResults(
			m_pDevice->Get(), frame.m_QueryPool, 0, frame.m_uiQueryCursor,
			results.size() * sizeof(uint64_t), results.data(), sizeof(uint64_t),
			VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);

		if (queryResult == VK_SUCCESS)
		{
			const float fTimestampPeriod = m_pDevice->GetTimestampPeriod();

			for (const PendingPassQuery& query : frame.m_PendingQueries)
			{
				const uint64_t uiBeginTicks = results[query.m_uiBeginIndex];
				const uint64_t uiEndTicks = results[query.m_uiEndIndex];

				if (uiEndTicks <= uiBeginTicks)
					continue;

				const double fMilliseconds =
					static_cast<double>(uiEndTicks - uiBeginTicks) * fTimestampPeriod / 1000000.0;

				FrameProfiler::Get().Report(query.m_Name, fMilliseconds);
			}
		}

		frame.m_PendingQueries.clear();
	}

	vkResetCommandPool(m_pDevice->Get(), frame.m_CommandPool, 0);

	/* Safe now that the GPU is done with this slot's upload pages. */
	if (m_pUploadBuffer != nullptr)
		m_pUploadBuffer->Reset();

	m_PendingBarriers.clear();
	m_bIsStarted = false;
	m_bRenderingOpen = false;
}

void VKCommandEngine::Start()
{
	if (m_bIsStarted)
		return;

	VkCommandBufferBeginInfo beginInfo{};
	beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
	beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

	vkBeginCommandBuffer(GetCommandBuffer(), &beginInfo);

	if (m_bTimingEnabled)
	{
		FrameData& frame = m_Frames[m_uiFrameIndex];

		/* Every query written by the last recording into this slot has
		   already been read back in Reset(); resetting the pool here is
		   what vkCmdWriteTimestamp2 requires before it will write again. */
		vkCmdResetQueryPool(GetCommandBuffer(), frame.m_QueryPool, 0, m_uiMaxTimestampsPerFrame);
		frame.m_uiQueryCursor = 0;
	}

	m_bIsStarted = true;
}

uint32_t VKCommandEngine::WriteTimestampBegin()
{
	if (!m_bTimingEnabled)
		return UINT32_MAX;

	FrameData& frame = m_Frames[m_uiFrameIndex];

	/* Need room for this query and its still-to-come End(). */
	if (frame.m_uiQueryCursor + 1 >= m_uiMaxTimestampsPerFrame)
	{
		static bool s_bWarned = false;

		if (!s_bWarned)
		{
			s_bWarned = true;
			fprintf(stderr, "[timing] '%s' ran out of timestamp queries for one frame; "
			                "some passes will be unmeasured\n", m_Info.m_Name.c_str());
		}

		return UINT32_MAX;
	}

	const uint32_t uiIndex = frame.m_uiQueryCursor++;
	vkCmdWriteTimestamp2(GetCommandBuffer(), VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, frame.m_QueryPool, uiIndex);

	return uiIndex;
}

void VKCommandEngine::WriteTimestampEnd(const std::string& name, uint32_t uiBeginIndex)
{
	if (!m_bTimingEnabled || uiBeginIndex == UINT32_MAX)
		return;

	FrameData& frame = m_Frames[m_uiFrameIndex];

	const uint32_t uiEndIndex = frame.m_uiQueryCursor++;
	vkCmdWriteTimestamp2(GetCommandBuffer(), VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, frame.m_QueryPool, uiEndIndex);

	frame.m_PendingQueries.push_back({ name, uiBeginIndex, uiEndIndex });
}

void VKCommandEngine::QueueBarrier(VKResource* pResource, PEResourceState newState)
{
	if (pResource == nullptr)
		return;

	/* An image that has never been transitioned is in VK_IMAGE_LAYOUT_UNDEFINED
	   regardless of what its tracked state says, so it still needs a barrier. */
	if (pResource->GetState() == newState && !pResource->IsLayoutUndefined())
		return;

	m_PendingBarriers.push_back({ pResource, newState });
}

void VKCommandEngine::ApplyBarriers()
{
	if (m_PendingBarriers.empty())
		return;

	if (!m_bIsStarted)
		Start();

	VkCommandBuffer cmd = GetCommandBuffer();

	/* VKResource::Transition records one barrier each. Batching them into a
	   single vkCmdPipelineBarrier2 would be better and is worth doing once the
	   passes exist to show which combinations actually occur. */
	for (const PendingBarrier& barrier : m_PendingBarriers)
		barrier.m_pResource->Transition(cmd, barrier.m_NewState);

	m_PendingBarriers.clear();
}

void VKCommandEngine::Wait(PCommandEngine* pEngine, uint64_t uiValue)
{
	if (pEngine == nullptr || pEngine == this)
		return;

	/* Recorded now, applied at the next Execute(). D3D12's
	   ID3D12CommandQueue::Wait was immediate; Vulkan attaches waits to a
	   submission, so they have to be deferred. */
	VkSemaphoreSubmitInfo waitInfo{};
	waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
	waitInfo.semaphore = pEngine->GetTimeline();
	waitInfo.value = uiValue;
	waitInfo.stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

	m_WaitSemaphores.push_back(waitInfo);
}

void VKCommandEngine::Execute()
{
	if (!m_bIsStarted)
		return;

	ApplyBarriers();

	VkCommandBuffer cmd = GetCommandBuffer();
	vkEndCommandBuffer(cmd);

	Signal();

	std::vector<VkSemaphore> waitSemaphores;
	std::vector<uint64_t> waitValues;
	std::vector<VkPipelineStageFlags> waitStages;
	for (const VkSemaphoreSubmitInfo& wait : m_WaitSemaphores)
	{
		waitSemaphores.push_back(wait.semaphore);
		waitValues.push_back(wait.value);
		waitStages.push_back(VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
	}
	const uint64_t signalValue = m_uiFenceValue;
	VkTimelineSemaphoreSubmitInfo timelineInfo{};
	timelineInfo.sType = VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO;
	timelineInfo.waitSemaphoreValueCount = static_cast<uint32_t>(waitValues.size());
	timelineInfo.pWaitSemaphoreValues = waitValues.data();
	timelineInfo.signalSemaphoreValueCount = 1;
	timelineInfo.pSignalSemaphoreValues = &signalValue;
	VkSubmitInfo submitInfo{};
	submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
	submitInfo.pNext = &timelineInfo;
	submitInfo.waitSemaphoreCount = static_cast<uint32_t>(waitSemaphores.size());
	submitInfo.pWaitSemaphores = waitSemaphores.data();
	submitInfo.pWaitDstStageMask = waitStages.data();
	submitInfo.commandBufferCount = 1;
	submitInfo.pCommandBuffers = &cmd;
	submitInfo.signalSemaphoreCount = 1;
	submitInfo.pSignalSemaphores = &m_Timeline;

	{
		/* Every engine shares one graphics queue, and resource loading submits
		   uploads from job threads while the main thread submits the frame. */
		std::lock_guard<std::mutex> lock(m_pDevice->GetQueueMutex());

		if (vkQueueSubmit(m_Queue, 1, &submitInfo, VK_NULL_HANDLE) != VK_SUCCESS)
			fprintf(stderr, "[vulkan] vkQueueSubmit failed for '%s'\n", m_Info.m_Name.c_str());
	}

	m_Frames[m_uiFrameIndex].m_uiSubmitValue = m_uiFenceValue;

	m_WaitSemaphores.clear();
	m_bIsStarted = false;

	/* No AdvanceFrame here. DXCommandEngine::Execute did not advance either,
	   and RenderContext::Present calls AdvanceFrame itself; doing both rotated
	   twice per frame. With two slots that lands back on the same one every
	   frame, so Reset always waited on the immediately preceding submission
	   and CPU and GPU ran fully serialised. */
}

void VKCommandEngine::AdvanceFrame()
{
	m_uiFrameIndex = (m_uiFrameIndex + 1) % m_uiFrameCount;
}

void VKCommandEngine::WaitForGPU()
{
	if (m_uiFenceValue == 0)
		return;

	VkSemaphoreWaitInfo waitInfo{};
	waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO;
	waitInfo.semaphoreCount = 1;
	waitInfo.pSemaphores = &m_Timeline;
	waitInfo.pValues = &m_uiFenceValue;

	vkWaitSemaphores(m_pDevice->Get(), &waitInfo, UINT64_MAX);
}

uint64_t VKCommandEngine::GetCompletedValue() const
{
	uint64_t uiValue = 0;
	const VkResult result = vkGetSemaphoreCounterValue(m_pDevice->Get(), m_Timeline, &uiValue);

	/* The result was discarded here, and that mattered more than it looks. On
	   VK_ERROR_DEVICE_LOST this leaves uiValue at 0, so GetCompletedValue() <
	   GetValue() stays true forever and RenderContext reports "GPU has not
	   completed for 600 frames" - for a device that no longer exists. A dead
	   GPU and a busy one were indistinguishable from inside the engine, which
	   is why every "the GPU is stuck on slow work" reading of that diagnostic
	   was unfalsifiable. Say which it is. */
	if (result != VK_SUCCESS && !m_bDeviceLostReported)
	{
		m_bDeviceLostReported = true;

		fprintf(stderr,
		        "[vulkan] '%s' timeline query failed (%d)%s - any [stall] line after this is a "
		        "dead device, not a slow frame\n",
		        m_Info.m_Name.c_str(), result,
		        result == VK_ERROR_DEVICE_LOST ? " VK_ERROR_DEVICE_LOST" : "");
	}

	return uiValue;
}

void VKCommandEngine::CopyResource(VKResource* pDest, VKResource* pSource)
{
	if (pDest == nullptr || pSource == nullptr)
		return;

	if (!m_bIsStarted)
		Start();

	/* D3D12 required the caller to have transitioned both resources already.
	   Doing it here means the copy cannot be issued against a wrong layout,
	   and QueueBarrier is a no-op when the state already matches. */
	QueueBarrier(pSource, E_STATE_COPY_SOURCE);
	QueueBarrier(pDest, E_STATE_COPY_DEST);
	ApplyBarriers();

	VkCommandBuffer cmd = GetCommandBuffer();

	if (pDest->GetKind() == VKResource::E_KIND_IMAGE &&
		pSource->GetKind() == VKResource::E_KIND_IMAGE)
	{
		VkImageCopy region{};
		region.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.srcSubresource.layerCount = 1;
		region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		region.dstSubresource.layerCount = 1;
		region.extent = pSource->GetExtent();

		vkCmdCopyImage(cmd,
		               pSource->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               pDest->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		               1, &region);
	}
	else if (pDest->GetKind() == VKResource::E_KIND_BUFFER &&
	         pSource->GetKind() == VKResource::E_KIND_BUFFER)
	{
		VkBufferCopy region{};
		region.size = pSource->GetSize() < pDest->GetSize() ? pSource->GetSize() : pDest->GetSize();

		vkCmdCopyBuffer(cmd, pSource->GetBuffer(), pDest->GetBuffer(), 1, &region);
	}
	else
	{
		fprintf(stderr, "[vulkan] CopyResource between an image and a buffer needs an explicit region\n");
	}
}

void VKCommandEngine::UploadImageRegions(VKResource* pImage, VKResource* pSource,
                                         const ImageRegion* pRegions, uint32_t uiRegionCount,
                                         uint32_t uiTexelSize)
{
	if (pImage == nullptr || pSource == nullptr || uiRegionCount == 0)
		return;

	if (pImage->GetKind() != VKResource::E_KIND_IMAGE ||
		pSource->GetKind() != VKResource::E_KIND_BUFFER)
		return;

	if (!m_bIsStarted)
		Start();

	const VkExtent3D extent = pImage->GetExtent();

	QueueBarrier(pImage, E_STATE_COPY_DEST);
	ApplyBarriers();

	VkCommandBuffer cmd = GetCommandBuffer();

	const uint32_t uiTimestampBeginIndex = WriteTimestampBegin();

	std::vector<VkBufferImageCopy> copies;
	copies.reserve(uiRegionCount);

	for (uint32_t i = 0; i < uiRegionCount; ++i)
	{
		const ImageRegion& region = pRegions[i];

		if (region.m_uiWidth == 0 || region.m_uiHeight == 0 || region.m_uiDepth == 0)
			continue;

		VkBufferImageCopy copy{};

		/* The buffer holds mip 0 in the image's own linearization, so the box
		   is named by its first texel plus the row and slice pitches rather
		   than being gathered into a contiguous staging region first. */
		copy.bufferOffset = static_cast<VkDeviceSize>(
			(static_cast<uint64_t>(region.m_uiZ) * extent.height + region.m_uiY) * extent.width
			+ region.m_uiX) * uiTexelSize;

		copy.bufferRowLength = extent.width;
		copy.bufferImageHeight = extent.height;

		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = 0;
		copy.imageSubresource.layerCount = 1;

		copy.imageOffset = { static_cast<int32_t>(region.m_uiX),
		                     static_cast<int32_t>(region.m_uiY),
		                     static_cast<int32_t>(region.m_uiZ) };

		copy.imageExtent = { region.m_uiWidth, region.m_uiHeight, region.m_uiDepth };

		copies.push_back(copy);
	}

	if (copies.empty())
	{
		WriteTimestampEnd("Voxel Pyramid Upload", uiTimestampBeginIndex);
		return;
	}

	vkCmdCopyBufferToImage(cmd, pSource->GetBuffer(), pImage->GetImage(),
	                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
	                       static_cast<uint32_t>(copies.size()), copies.data());

	const uint32_t uiMips = pImage->GetMipLevels();

	/* One level at a time, and each one has to wait for the level below it to
	   be written before it can be read - which is why this cannot be a single
	   barrier around the loop. */
	for (uint32_t uiLevel = 1; uiLevel < uiMips; ++uiLevel)
	{
		VkImageMemoryBarrier2 barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT | VK_PIPELINE_STAGE_2_BLIT_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = pImage->GetImage();
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = uiLevel - 1;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		VkDependencyInfo dependency{};
		dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers = &barrier;

		VKCmdPipelineBarrierLegacy(cmd, barrier);

		auto LevelExtent = [&extent](uint32_t uiL)
		{
			return VkOffset3D{
				static_cast<int32_t>(std::max(extent.width >> uiL, 1u)),
				static_cast<int32_t>(std::max(extent.height >> uiL, 1u)),
				static_cast<int32_t>(std::max(extent.depth >> uiL, 1u))
			};
		};

		VkImageBlit blit{};
		blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.srcSubresource.mipLevel = uiLevel - 1;
		blit.srcSubresource.layerCount = 1;
		blit.srcOffsets[1] = LevelExtent(uiLevel - 1);

		blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		blit.dstSubresource.mipLevel = uiLevel;
		blit.dstSubresource.layerCount = 1;
		blit.dstOffsets[1] = LevelExtent(uiLevel);

		vkCmdBlitImage(cmd,
		               pImage->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		               pImage->GetImage(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
		               1, &blit, VK_FILTER_LINEAR);
	}

	/* The last level is still a transfer destination while every other one is
	   a source, so it is moved across on its own and the whole image is then
	   in one layout again - which is what the tracked state has to describe
	   for the next ordinary barrier to be correct. */
	if (uiMips > 1)
	{
		VkImageMemoryBarrier2 barrier{};
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
		barrier.srcStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
		barrier.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
		barrier.dstStageMask = VK_PIPELINE_STAGE_2_BLIT_BIT;
		barrier.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
		barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
		barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.image = pImage->GetImage();
		barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		barrier.subresourceRange.baseMipLevel = uiMips - 1;
		barrier.subresourceRange.levelCount = 1;
		barrier.subresourceRange.layerCount = 1;

		VkDependencyInfo dependency{};
		dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
		dependency.imageMemoryBarrierCount = 1;
		dependency.pImageMemoryBarriers = &barrier;

		VKCmdPipelineBarrierLegacy(cmd, barrier);

		pImage->SetTrackedState(E_STATE_COPY_SOURCE);
	}

	WriteTimestampEnd("Voxel Pyramid Upload", uiTimestampBeginIndex);
}

bool VKCommandEngine::ReadbackImageMips(VKResource* pImage, VKResource* pDest, uint32_t uiTexelSize)
{
	if (pImage == nullptr || pDest == nullptr)
		return false;

	if (pImage->GetKind() != VKResource::E_KIND_IMAGE ||
		pDest->GetKind() != VKResource::E_KIND_BUFFER)
		return false;

	const VkExtent3D extent = pImage->GetExtent();
	const uint32_t uiMips = pImage->GetMipLevels();

	std::vector<VkBufferImageCopy> copies;
	copies.reserve(uiMips);

	VkDeviceSize uiOffset = 0;

	for (uint32_t uiLevel = 0; uiLevel < uiMips; ++uiLevel)
	{
		const uint32_t uiWidth = std::max(extent.width >> uiLevel, 1u);
		const uint32_t uiHeight = std::max(extent.height >> uiLevel, 1u);
		const uint32_t uiDepth = std::max(extent.depth >> uiLevel, 1u);

		VkBufferImageCopy copy{};
		copy.bufferOffset = uiOffset;
		copy.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		copy.imageSubresource.mipLevel = uiLevel;
		copy.imageSubresource.layerCount = 1;
		copy.imageExtent = { uiWidth, uiHeight, uiDepth };

		copies.push_back(copy);

		uiOffset += static_cast<VkDeviceSize>(uiWidth) * uiHeight * uiDepth * uiTexelSize;
	}

	if (uiOffset > pDest->GetSize())
		return false;

	Reset();
	Start();

	QueueBarrier(pImage, E_STATE_COPY_SOURCE);
	ApplyBarriers();

	vkCmdCopyImageToBuffer(GetCommandBuffer(), pImage->GetImage(),
	                       VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
	                       pDest->GetBuffer(),
	                       static_cast<uint32_t>(copies.size()), copies.data());

	/* Back to what the voxel pass expects, so the next frame does not have to
	   know a validation ran. */
	QueueBarrier(pImage, E_STATE_PIXEL_SHADER_RESOURCE);
	ApplyBarriers();

	Execute();
	WaitForGPU();

	return true;
}
