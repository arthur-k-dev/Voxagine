#include "VKDevice.h"

#include <cstdio>
#include <cstring>
#include <set>

namespace
{
	const char* const kValidationLayer = "VK_LAYER_KHRONOS_validation";
	const char* const kSwapchainExtension = VK_KHR_SWAPCHAIN_EXTENSION_NAME;

	VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
		VkDebugUtilsMessageSeverityFlagBitsEXT severity,
		VkDebugUtilsMessageTypeFlagsEXT,
		const VkDebugUtilsMessengerCallbackDataEXT* pData,
		void*)
	{
		if (severity >= VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT)
			fprintf(stderr, "[vulkan] %s\n", pData->pMessage);

		return VK_FALSE;
	}

	/* Everything this renderer cannot run without, checked up front so the
	   failure names itself.
	 *
	 * Before this, a device that was missing any of it got as far as
	 * vkCreateDevice and failed with "vkCreateDevice failed" - and on a phone
	 * that is the entire diagnostic the user or the crash report would carry.
	 * The floor is deliberately hard: MOBILE_PORT_PLAN.md phase 4 rejected
	 * building a reduced-feature path for older hardware, so the useful thing
	 * to do about an unsupported device is say precisely which capability is
	 * absent.
	 *
	 * Returns true if the device qualifies; otherwise fills `missing` with the
	 * names of what it lacks. */
	bool HasRequiredCapabilities(VkPhysicalDevice device, std::vector<const char*>& missing)
	{
		missing.clear();

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(device, &props);

		/* Timeline semaphores, descriptor indexing and buffer addresses are core
		   in 1.2. Synchronization uses the compatible legacy API below, while
		   dynamic rendering may be supplied through its KHR extension. */
		if (props.apiVersion < VK_API_VERSION_1_2)
			missing.push_back("Vulkan 1.2");

		VkPhysicalDeviceScalarBlockLayoutFeatures scalar{};
		scalar.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;

		VkPhysicalDeviceSynchronization2Features sync2{};
		sync2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SYNCHRONIZATION_2_FEATURES;
		sync2.pNext = &scalar;

		VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
		timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
		timeline.pNext = &sync2;

		VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{};
		bufferAddress.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
		bufferAddress.pNext = &timeline;

		VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{};
		descriptorIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
		descriptorIndexing.pNext = &bufferAddress;

		VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{};
		dynamicRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
		dynamicRendering.pNext = &descriptorIndexing;

		VkPhysicalDeviceFeatures2 features{};
		features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
		features.pNext = &dynamicRendering;

		vkGetPhysicalDeviceFeatures2(device, &features);

		if (!dynamicRendering.dynamicRendering)
			missing.push_back("dynamicRendering");

		if (!timeline.timelineSemaphore)
			missing.push_back("timelineSemaphore");

		if (!bufferAddress.bufferDeviceAddress)
			missing.push_back("bufferDeviceAddress");

		if (!descriptorIndexing.runtimeDescriptorArray ||
			!descriptorIndexing.descriptorBindingVariableDescriptorCount ||
			!descriptorIndexing.descriptorBindingPartiallyBound)
			missing.push_back("descriptorIndexing (bindless textures)");

		/* The shaders are compiled with -fvk-use-dx-layout because the engine
		   memcpys tightly packed C++ structs into structured buffers, and the
		   packed float3s that produces straddle 16-byte boundaries. Without
		   scalarBlockLayout every element after the first is read from the
		   wrong offset - which does not fail, it draws garbage. */
		if (!scalar.scalarBlockLayout)
			missing.push_back("scalarBlockLayout (needed by -fvk-use-dx-layout)");

		if (!features.features.shaderStorageImageWriteWithoutFormat)
			missing.push_back("shaderStorageImageWriteWithoutFormat");

		return missing.empty();
	}

	bool HasLayer(const char* pName)
	{
		uint32_t uiCount = 0;
		vkEnumerateInstanceLayerProperties(&uiCount, nullptr);

		std::vector<VkLayerProperties> layers(uiCount);
		vkEnumerateInstanceLayerProperties(&uiCount, layers.data());

		for (const VkLayerProperties& layer : layers)
		{
			if (strcmp(layer.layerName, pName) == 0)
				return true;
		}

		return false;
	}

	bool HasInstanceExtension(const char* pName)
	{
		uint32_t uiCount = 0;
		vkEnumerateInstanceExtensionProperties(nullptr, &uiCount, nullptr);

		std::vector<VkExtensionProperties> extensions(uiCount);
		vkEnumerateInstanceExtensionProperties(nullptr, &uiCount, extensions.data());

		for (const VkExtensionProperties& extension : extensions)
		{
			if (strcmp(extension.extensionName, pName) == 0)
				return true;
		}

		return false;
	}

	bool HasDeviceExtension(VkPhysicalDevice device, const char* pName)
	{
		uint32_t count = 0;
		vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr);
		std::vector<VkExtensionProperties> extensions(count);
		vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data());

		for (const VkExtensionProperties& extension : extensions)
		{
			if (strcmp(extension.extensionName, pName) == 0)
				return true;
		}

		return false;
	}
}

VKDevice::~VKDevice()
{
	Destroy();
}

bool VKDevice::CreateInstance(const std::vector<const char*>& a_InstanceExtensions, bool bEnableValidation)
{
	m_StartupError.clear();
	/* Nothing Vulkan can be called before this: with volk every entry point is
	   a null function pointer until the loader has been opened. See
	   VulkanAPI.h for why the engine loads Vulkan dynamically at all - the
	   short version is that Android's libvulkan.so exports nothing past 1.1. */
	if (volkInitialize() != VK_SUCCESS)
	{
		fprintf(stderr, "[vulkan] no Vulkan loader on this system\n");
		m_StartupError = "No Vulkan loader is available.";
		return false;
	}

	m_bValidationEnabled = bEnableValidation && HasLayer(kValidationLayer);

	if (bEnableValidation && !m_bValidationEnabled)
		printf("[vulkan] validation layer requested but not installed, continuing without it\n");

	VkApplicationInfo appInfo{};
	appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
	appInfo.pApplicationName = "Voxagine";
	appInfo.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.pEngineName = "Voxagine";
	appInfo.engineVersion = VK_MAKE_VERSION(1, 0, 0);
	appInfo.apiVersion = VK_API_VERSION_1_3;

	std::vector<const char*> extensions = a_InstanceExtensions;
	if (m_bValidationEnabled)
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);

	VkInstanceCreateFlags instanceFlags = 0;

	/* MoltenVK is a portability driver, and since loader 1.3.216 the loader
	   hides those from vkEnumeratePhysicalDevices unless the application opts
	   in here. Without it macOS fails at vkCreateInstance with "Found no
	   drivers!" even though MoltenVK is installed and working.

	   Only macOS goes through the loader: the iOS build links MoltenVK
	   statically as its own ICD, so the extension is absent there and this is
	   correctly skipped. Querying rather than #ifdef-ing keeps that automatic. */
	if (HasInstanceExtension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME))
	{
		extensions.push_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
		instanceFlags |= VK_INSTANCE_CREATE_ENUMERATE_PORTABILITY_BIT_KHR;
	}

	VkInstanceCreateInfo createInfo{};
	createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
	createInfo.flags = instanceFlags;
	createInfo.pApplicationInfo = &appInfo;
	createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
	createInfo.ppEnabledExtensionNames = extensions.data();

	if (m_bValidationEnabled)
	{
		createInfo.enabledLayerCount = 1;
		createInfo.ppEnabledLayerNames = &kValidationLayer;
	}

	if (vkCreateInstance(&createInfo, nullptr, &m_Instance) != VK_SUCCESS)
	{
		fprintf(stderr, "[vulkan] vkCreateInstance failed\n");
		m_StartupError = "vkCreateInstance failed.";
		return false;
	}

	/* Promotes the entry points from the loader's trampolines to this
	   instance's, and is what makes every 1.1+ instance-level function
	   non-null. Everything below this line depends on it. */
	volkLoadInstance(m_Instance);

	if (m_bValidationEnabled)
	{
		VkDebugUtilsMessengerCreateInfoEXT debugInfo{};
		debugInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
		debugInfo.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
		                            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debugInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
		                        VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
		                        VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debugInfo.pfnUserCallback = DebugCallback;

		PFN_vkCreateDebugUtilsMessengerEXT pCreate =
			reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(m_Instance, "vkCreateDebugUtilsMessengerEXT"));

		if (pCreate != nullptr)
			pCreate(m_Instance, &debugInfo, nullptr, &m_DebugMessenger);
	}

	return true;
}

bool VKDevice::HasSwapchainSupport(VkPhysicalDevice device) const
{
	uint32_t uiCount = 0;
	vkEnumerateDeviceExtensionProperties(device, nullptr, &uiCount, nullptr);

	std::vector<VkExtensionProperties> available(uiCount);
	vkEnumerateDeviceExtensionProperties(device, nullptr, &uiCount, available.data());

	for (const VkExtensionProperties& ext : available)
	{
		if (strcmp(ext.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0)
			return true;
	}

	return false;
}

bool VKDevice::FindQueueFamilies(VkPhysicalDevice device, VkSurfaceKHR surface,
                                 uint32_t& uiGraphics, uint32_t& uiPresent) const
{
	uint32_t uiCount = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(device, &uiCount, nullptr);

	std::vector<VkQueueFamilyProperties> families(uiCount);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &uiCount, families.data());

	uiGraphics = UINT32_MAX;
	uiPresent = UINT32_MAX;

	for (uint32_t i = 0; i < uiCount; ++i)
	{
		if (families[i].queueCount == 0)
			continue;

		if (uiGraphics == UINT32_MAX && (families[i].queueFlags & VK_QUEUE_GRAPHICS_BIT))
			uiGraphics = i;

		VkBool32 bPresent = VK_FALSE;
		vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface, &bPresent);

		if (uiPresent == UINT32_MAX && bPresent == VK_TRUE)
			uiPresent = i;
	}

	return uiGraphics != UINT32_MAX && uiPresent != UINT32_MAX;
}

bool VKDevice::PickPhysicalDevice(VkSurfaceKHR surface)
{
	uint32_t uiCount = 0;
	vkEnumeratePhysicalDevices(m_Instance, &uiCount, nullptr);

	if (uiCount == 0)
	{
		fprintf(stderr, "[vulkan] no physical devices\n");
		m_StartupError = "No Vulkan physical devices were found.";
		return false;
	}

	std::vector<VkPhysicalDevice> devices(uiCount);
	vkEnumeratePhysicalDevices(m_Instance, &uiCount, devices.data());

	VkPhysicalDevice fallback = VK_NULL_HANDLE;
	uint32_t uiFallbackGraphics = UINT32_MAX;
	uint32_t uiFallbackPresent = UINT32_MAX;

	/* Collected so that "no usable device" can say what each candidate was
	   short of, rather than just that there wasn't one. On a phone this
	   message is the whole bug report. */
	std::string rejections;

	for (VkPhysicalDevice device : devices)
	{
		uint32_t uiGraphics = 0;
		uint32_t uiPresent = 0;

		if (!HasSwapchainSupport(device))
			continue;

		if (!FindQueueFamilies(device, surface, uiGraphics, uiPresent))
			continue;

		VkPhysicalDeviceProperties props{};
		vkGetPhysicalDeviceProperties(device, &props);

		std::vector<const char*> missing;

		if (!HasRequiredCapabilities(device, missing))
		{
			rejections += std::string("\n  ") + props.deviceName + " lacks:";

			for (const char* pMissing : missing)
				rejections += std::string(" ") + pMissing + ";";

			continue;
		}

		/* Prefer a discrete GPU, but take whatever presents if there is none. */
		if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
		{
			m_PhysicalDevice = device;
			m_uiGraphicsFamily = uiGraphics;
			m_uiPresentFamily = uiPresent;
			m_DeviceName = props.deviceName;
			m_bTimestampsSupported = props.limits.timestampComputeAndGraphics == VK_TRUE &&
			                          props.limits.timestampPeriod > 0.0f;
			m_fTimestampPeriod = props.limits.timestampPeriod;
			return true;
		}

		if (fallback == VK_NULL_HANDLE)
		{
			fallback = device;
			uiFallbackGraphics = uiGraphics;
			uiFallbackPresent = uiPresent;
			m_DeviceName = props.deviceName;
			m_bTimestampsSupported = props.limits.timestampComputeAndGraphics == VK_TRUE &&
			                          props.limits.timestampPeriod > 0.0f;
			m_fTimestampPeriod = props.limits.timestampPeriod;
		}
	}

	if (fallback == VK_NULL_HANDLE)
	{
		if (rejections.empty())
		{
			fprintf(stderr, "[vulkan] no device can present to this surface\n");
			m_StartupError = "No Vulkan device can present to this surface.";
		}
		else
		{
			fprintf(stderr,
				"[vulkan] no device meets this renderer's requirements.%s\n"
				"[vulkan] Bit Buster needs Vulkan 1.3; there is no fallback path.\n",
				rejections.c_str());
			m_StartupError = "No Vulkan device meets this renderer's requirements:" + rejections;
		}

		return false;
	}

	m_PhysicalDevice = fallback;
	m_uiGraphicsFamily = uiFallbackGraphics;
	m_uiPresentFamily = uiFallbackPresent;

	return true;
}

bool VKDevice::CreateDevice(VkSurfaceKHR surface)
{
	if (!PickPhysicalDevice(surface))
		return false;

	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(m_PhysicalDevice, &properties);

	/* Dynamic rendering is core in 1.3, but this iPad exposes it as the
	   Vulkan 1.2 VK_KHR_dynamic_rendering extension. Querying the feature is
	   not sufficient: the extension has to be enabled on vkCreateDevice before
	   vkCmdBeginRendering may be used. */
	std::vector<const char*> deviceExtensions = { kSwapchainExtension };
	if (properties.apiVersion < VK_API_VERSION_1_3)
	{
		if (!HasDeviceExtension(m_PhysicalDevice, VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME))
		{
			m_StartupError = "The selected Vulkan 1.2 device does not expose VK_KHR_dynamic_rendering.";
			return false;
		}

		deviceExtensions.push_back(VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME);
	}

	/* Not optional: the spec says a device advertising VK_KHR_portability_subset
	   *must* have it enabled, and the loader's validation refuses vkCreateDevice
	   otherwise. MoltenVK advertises it on both macOS and iOS. */
	if (HasDeviceExtension(m_PhysicalDevice, "VK_KHR_portability_subset"))
		deviceExtensions.push_back("VK_KHR_portability_subset");

	const float fPriority = 1.f;

	/* Graphics and present are usually the same family; only ask once. */
	std::set<uint32_t> uniqueFamilies = { m_uiGraphicsFamily, m_uiPresentFamily };
	std::vector<VkDeviceQueueCreateInfo> queueInfos;

	for (uint32_t uiFamily : uniqueFamilies)
	{
		VkDeviceQueueCreateInfo queueInfo{};
		queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
		queueInfo.queueFamilyIndex = uiFamily;
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &fPriority;

		queueInfos.push_back(queueInfo);
	}

	/* Shaders are compiled with -fvk-use-dx-layout so structured buffers match
	   the tightly packed C++ structs the engine memcpys in (D3D's rules).
	   float3 members then straddle 16-byte boundaries, which Vulkan only
	   permits with scalarBlockLayout. */
	VkPhysicalDeviceScalarBlockLayoutFeatures scalarLayout{};
	scalarLayout.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SCALAR_BLOCK_LAYOUT_FEATURES;
	scalarLayout.scalarBlockLayout = VK_TRUE;

	/* CommandEngine exposes a monotonically increasing fence value that other
	   engines wait on - an ID3D12Fence. Timeline semaphores are the direct
	   equivalent. */
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{};
	timeline.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES;
	timeline.timelineSemaphore = VK_TRUE;
	timeline.pNext = &scalarLayout;

	/* Buffer::GetGPUAddress() returns a uint64 that used to be a
	   D3D12_GPU_VIRTUAL_ADDRESS. bufferDeviceAddress is the equivalent and
	   keeps every call site working unchanged. */
	VkPhysicalDeviceBufferDeviceAddressFeatures bufferAddress{};
	bufferAddress.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES;
	bufferAddress.bufferDeviceAddress = VK_TRUE;
	bufferAddress.pNext = &timeline;

	/* Bindless arrays: RenderPass::Data::m_uiBindlessResourceCount declares a
	   variable-sized texture array, which needs descriptor indexing. */
	VkPhysicalDeviceDescriptorIndexingFeatures descriptorIndexing{};
	descriptorIndexing.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DESCRIPTOR_INDEXING_FEATURES;
	descriptorIndexing.descriptorBindingVariableDescriptorCount = VK_TRUE;
	descriptorIndexing.descriptorBindingPartiallyBound = VK_TRUE;
	descriptorIndexing.runtimeDescriptorArray = VK_TRUE;
	descriptorIndexing.shaderSampledImageArrayNonUniformIndexing = VK_TRUE;
	descriptorIndexing.descriptorBindingSampledImageUpdateAfterBind = VK_TRUE;
	descriptorIndexing.pNext = &bufferAddress;

	VkPhysicalDeviceDynamicRenderingFeatures dynamicRendering{};
	dynamicRendering.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_FEATURES;
	dynamicRendering.dynamicRendering = VK_TRUE;
	dynamicRendering.pNext = &descriptorIndexing;

	/* Storage-image writes use format-less image stores. This is the only core
	   shader-storage feature the shipped shaders require: they do not perform
	   fragment or vertex-stage atomic operations, so requesting those features
	   would unnecessarily exclude iOS GPUs such as A12Z. */
	VkPhysicalDeviceFeatures coreFeatures{};
	coreFeatures.shaderStorageImageWriteWithoutFormat = VK_TRUE;

	VkDeviceCreateInfo deviceInfo{};
	deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
	deviceInfo.pNext = &dynamicRendering;
	deviceInfo.queueCreateInfoCount = static_cast<uint32_t>(queueInfos.size());
	deviceInfo.pQueueCreateInfos = queueInfos.data();
	deviceInfo.enabledExtensionCount = static_cast<uint32_t>(deviceExtensions.size());
	deviceInfo.ppEnabledExtensionNames = deviceExtensions.data();
	deviceInfo.pEnabledFeatures = &coreFeatures;

	if (vkCreateDevice(m_PhysicalDevice, &deviceInfo, nullptr, &m_Device) != VK_SUCCESS)
	{
		fprintf(stderr, "[vulkan] vkCreateDevice failed\n");
		m_StartupError = "vkCreateDevice failed after selecting " + m_DeviceName + ".";
		return false;
	}

	/* Swaps the device-level entry points for this device's own, skipping the
	   loader's dispatch on every draw call. Safe because this engine creates
	   exactly one device; a second one would need volkLoadDeviceTable instead. */
	volkLoadDevice(m_Device);

	vkGetDeviceQueue(m_Device, m_uiGraphicsFamily, 0, &m_GraphicsQueue);
	vkGetDeviceQueue(m_Device, m_uiPresentFamily, 0, &m_PresentQueue);

	return true;
}

void VKDevice::Destroy()
{
	if (m_Device != VK_NULL_HANDLE)
	{
		vkDestroyDevice(m_Device, nullptr);
		m_Device = VK_NULL_HANDLE;
	}

	if (m_DebugMessenger != VK_NULL_HANDLE)
	{
		PFN_vkDestroyDebugUtilsMessengerEXT pDestroy =
			reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(m_Instance, "vkDestroyDebugUtilsMessengerEXT"));

		if (pDestroy != nullptr)
			pDestroy(m_Instance, m_DebugMessenger, nullptr);

		m_DebugMessenger = VK_NULL_HANDLE;
	}

	if (m_Instance != VK_NULL_HANDLE)
	{
		vkDestroyInstance(m_Instance, nullptr);
		m_Instance = VK_NULL_HANDLE;
	}
}
