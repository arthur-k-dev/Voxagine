#pragma once

#include <Core/Platform/Rendering/Vulkan/VulkanAPI.h>

/* The renderer's resource-state model is expressed in synchronization2
   types, but its dependencies do not need the extra precision.  Keeping the
   compatibility conversion here lets Vulkan 1.2 devices (notably MoltenVK on
   A12Z) use the same resource code.  ALL_COMMANDS/MEMORY_READ|WRITE is
   deliberately conservative: correctness and portable startup matter more
   than trimming a barrier on the mobile fallback. */
inline void VKCmdPipelineBarrierLegacy(VkCommandBuffer commandBuffer,
                                       const VkImageMemoryBarrier2& source)
{
	VkImageMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
	barrier.srcAccessMask = source.oldLayout == VK_IMAGE_LAYOUT_UNDEFINED
		? 0 : VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.oldLayout = source.oldLayout;
	barrier.newLayout = source.newLayout;
	barrier.srcQueueFamilyIndex = source.srcQueueFamilyIndex;
	barrier.dstQueueFamilyIndex = source.dstQueueFamilyIndex;
	barrier.image = source.image;
	barrier.subresourceRange = source.subresourceRange;

	vkCmdPipelineBarrier(commandBuffer,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
}

inline void VKCmdPipelineBarrierLegacy(VkCommandBuffer commandBuffer,
                                       const VkBufferMemoryBarrier2& source)
{
	VkBufferMemoryBarrier barrier{};
	barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
	barrier.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	barrier.srcQueueFamilyIndex = source.srcQueueFamilyIndex;
	barrier.dstQueueFamilyIndex = source.dstQueueFamilyIndex;
	barrier.buffer = source.buffer;
	barrier.offset = source.offset;
	barrier.size = source.size;

	vkCmdPipelineBarrier(commandBuffer,
		VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 1, &barrier, 0, nullptr);
}
