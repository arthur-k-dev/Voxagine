#pragma once

/* The one place Vulkan's headers are included.
 *
 * This used to be a direct `vulkan/vulkan.h` include in thirteen files, linking
 * against the loader's exported symbols. **That does not work on Android.**
 * Android's platform `libvulkan.so` exports the 1.0/1.1 core and nothing
 * newer, so a link that names `vkCmdPipelineBarrier2`, `vkQueueSubmit2`,
 * `vkCmdWriteTimestamp2`, `vkCmdBeginRendering` or `vkCmdEndRendering` - all
 * core in 1.3, all used by this renderer - fails with five undefined symbols.
 * There is no Android NDK setting that changes that: post-1.1 entry points are
 * reached through vkGetInstanceProcAddr/vkGetDeviceProcAddr, full stop.
 *
 * volk is the standard answer: it declares every entry point as a function
 * pointer of the same name and fills them in from the loader, so call sites are
 * unchanged and any 1.2/1.3/1.4 function used in the future works the same way.
 * The alternative - hand-loading the five that happen to be needed today -
 * fails again, as a link error on one platform only, the next time somebody
 * uses a sixth.
 *
 * Three calls make it work, all in VKRenderContext/VKDevice:
 *   volkInitialize()            before vkCreateInstance
 *   volkLoadInstance(instance)  after it
 *   volkLoadDevice(device)      after device creation, which swaps the
 *                               pointers for device-specific ones (one fewer
 *                               dispatch per call, and this engine has exactly
 *                               one device)
 *
 * volk.h defines VK_NO_PROTOTYPES itself, so including <vulkan/vulkan.h>
 * directly anywhere after this is a hard error rather than a silent
 * double-definition - which is the behaviour we want. */

#if defined(VOXAGINE_IOS)
/* MoltenVK is linked statically on iOS and exports the native Vulkan entry
 * points directly. volk's dynamic-loader TU would define colliding function
 * pointer symbols, so use the SDK prototypes and no-op its dispatch loads. */
#include <vulkan/vulkan.h>
inline VkResult volkInitialize() { return VK_SUCCESS; }
inline void volkLoadInstance(VkInstance) {}
inline void volkLoadDevice(VkDevice) {}
#else
#include <External/volk/volk.h>
#endif
