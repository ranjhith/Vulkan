/*
* Vulkan Example - Minimal headless compute example
*
* Copyright (C) 2017 by Sascha Willems - www.saschawillems.de
*
* This code is licensed under the MIT license (MIT) (http://opensource.org/licenses/MIT)
*/

/*
 * Minimal scheduleviz compute (derived from Minimal headless compute example)
 */

// TODO: separate transfer queue (if not supported by compute queue) including buffer ownership transfer

#if defined(_WIN32)
#	pragma comment(linker, "/subsystem:console")
#elif defined(VK_USE_PLATFORM_ANDROID_KHR)
#	include "VulkanAndroid.h"
#	include <android/asset_manager.h>
#	include <android/log.h>
#	include <android/native_activity.h>
#	include <android_native_app_glue.h>
#endif

#include <algorithm>
#include <assert.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>

#include "VulkanTools.h"
#include <vulkan/vulkan.h>

#include "VulkanTexture.hpp"

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
android_app *androidapp;
#endif

#define DEBUG (!NDEBUG)

#define WIDTH 56
#define HEIGHT 32
#define COMPONENTS_PP 4
#define BYTES_PP (4 * (COMPONENTS_PP))
#define WORKGROUP_SZ_X 1
#define WORKGROUP_SZ_Y 1
#define BUFFER_ELEMENTS 32

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
#	define LOG(...) ((void) __android_log_print(ANDROID_LOG_INFO, "vulkanExample", __VA_ARGS__))
#else
#	define LOG(...) printf(__VA_ARGS__)
#endif

static VKAPI_ATTR VkBool32 VKAPI_CALL debugMessageCallback(
    VkDebugReportFlagsEXT      flags,
    VkDebugReportObjectTypeEXT objectType,
    uint64_t                   object,
    size_t                     location,
    int32_t                    messageCode,
    const char *               pLayerPrefix,
    const char *               pMessage,
    void *                     pUserData)
{
	LOG("[VALIDATION]: %s - %s\n", pLayerPrefix, pMessage);
	return VK_FALSE;
}

class VulkanExample
{
  public:
	VkInstance            instance;
	VkPhysicalDevice      physicalDevice;
	VkDevice              device;
	uint32_t              queueFamilyIndex;
	VkPipelineCache       pipelineCache;
	VkQueue               queue;
	VkCommandPool         commandPool;
	VkCommandBuffer       commandBuffer;
	VkFence               fence;
	VkDescriptorPool      descriptorPool;
	VkDescriptorSetLayout descriptorSetLayout;
	VkDescriptorSet       descriptorSet;
	VkPipelineLayout      pipelineLayout;
	VkPipeline            pipeline;
	VkShaderModule        shaderModule;

	VkDebugReportCallbackEXT debugReportCallback{};

	VkResult createBuffer(VkBufferUsageFlags usageFlags, VkMemoryPropertyFlags memoryPropertyFlags, VkBuffer *buffer, VkDeviceMemory *memory, VkDeviceSize size, void *data = nullptr)
	{
		// Create the buffer handle
		VkBufferCreateInfo bufferCreateInfo = vks::initializers::bufferCreateInfo(usageFlags, size);
		bufferCreateInfo.sharingMode        = VK_SHARING_MODE_EXCLUSIVE;
		VK_CHECK_RESULT(vkCreateBuffer(device, &bufferCreateInfo, nullptr, buffer));

		// Create the memory backing up the buffer handle
		VkPhysicalDeviceMemoryProperties deviceMemoryProperties;
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemoryProperties);
		VkMemoryRequirements memReqs;
		VkMemoryAllocateInfo memAlloc = vks::initializers::memoryAllocateInfo();
		vkGetBufferMemoryRequirements(device, *buffer, &memReqs);
		memAlloc.allocationSize = memReqs.size;
		// Find a memory type index that fits the properties of the buffer
		bool memTypeFound = false;
		for (uint32_t i = 0; i < deviceMemoryProperties.memoryTypeCount; i++)
		{
			if ((memReqs.memoryTypeBits & 1) == 1)
			{
				if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & memoryPropertyFlags) == memoryPropertyFlags)
				{
					memAlloc.memoryTypeIndex = i;
					memTypeFound             = true;
				}
			}
			memReqs.memoryTypeBits >>= 1;
		}
		assert(memTypeFound);
		VK_CHECK_RESULT(vkAllocateMemory(device, &memAlloc, nullptr, memory));

		if (data != nullptr)
		{
			void *mapped;
			VK_CHECK_RESULT(vkMapMemory(device, *memory, 0, size, 0, &mapped));
			memcpy(mapped, data, size);
			vkUnmapMemory(device, *memory);
		}

		VK_CHECK_RESULT(vkBindBufferMemory(device, *buffer, *memory, 0));

		return VK_SUCCESS;
	}

	uint32_t getMemoryTypeIndex(uint32_t typeBits, VkMemoryPropertyFlags properties)
	{
		VkPhysicalDeviceMemoryProperties deviceMemoryProperties;
		vkGetPhysicalDeviceMemoryProperties(physicalDevice, &deviceMemoryProperties);
		for (uint32_t i = 0; i < deviceMemoryProperties.memoryTypeCount; i++)
		{
			if ((typeBits & 1) == 1)
			{
				if ((deviceMemoryProperties.memoryTypes[i].propertyFlags & properties) == properties)
				{
					return i;
				}
			}
			typeBits >>= 1;
		}
		return 0;
	}

	// End the command buffer and submit it to the queue
	// Uses a fence to ensure command buffer has finished executing before deleting it
	void flushCommandBuffer(VkCommandBuffer commandBuffer)
	{
		assert(commandBuffer != VK_NULL_HANDLE);

		VK_CHECK_RESULT(vkEndCommandBuffer(commandBuffer));

		VkSubmitInfo submitInfo       = {};
		submitInfo.sType              = VK_STRUCTURE_TYPE_SUBMIT_INFO;
		submitInfo.commandBufferCount = 1;
		submitInfo.pCommandBuffers    = &commandBuffer;

		// Create fence to ensure that the command buffer has finished executing
		VkFenceCreateInfo fenceCreateInfo = {};
		fenceCreateInfo.sType             = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceCreateInfo.flags             = 0;
		VkFence fence;
		VK_CHECK_RESULT(vkCreateFence(device, &fenceCreateInfo, nullptr, &fence));

		// Submit to the queue
		VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence));
		// Wait for the fence to signal that command buffer has finished executing
		VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, DEFAULT_FENCE_TIMEOUT));

		vkDestroyFence(device, fence, nullptr);
		vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
	}

	void saveOutputAsImage(uint32_t width, uint32_t height, uint32_t components_pp, std::vector<uint32_t> &data)
	{
		/*
			Save host visible framebuffer image to disk (ppm format)
		*/

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
		const char *filename = strcat(getenv("EXTERNAL_STORAGE"), "/scheduleviz.ppm");
#else
		const char *filename           = "scheduleviz.ppm";
#endif
		std::ofstream file(filename, std::ios::out | std::ios::binary);

		// ppm header
		file << "P6\n"
		     << width << " "
		     << height << "\n"
		     << 255 << "\n";

		// ppm binary pixel data
		for (int32_t y = 0; y < height; y += 1)
		{
			for (int32_t x = 0; x < width; x += 1)
			{
				char     component;
				uint32_t offset = (y * width * components_pp) + (x * components_pp);
				for (int32_t z = 0; z < (components_pp - 1); z += 1)
				{
					component = data.at(offset + z);
					file.write(&component, 1);
				}
			}
		}
		file.close();

		LOG("\nImage saved to %s\n", filename);
	}

	VulkanExample()
	{
		LOG("Running scheduleviz compute\n");

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
		LOG("loading vulkan lib");
		vks::android::loadVulkanLibrary();
#endif

		VkApplicationInfo appInfo = {};
		appInfo.sType             = VK_STRUCTURE_TYPE_APPLICATION_INFO;
		appInfo.pApplicationName  = "Vulkan scheduleviz";
		appInfo.pEngineName       = "VulkanExample";
		appInfo.apiVersion        = VK_API_VERSION_1_0;

		/*
			Vulkan instance creation (without surface extensions)
		*/
		VkInstanceCreateInfo instanceCreateInfo = {};
		instanceCreateInfo.sType                = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
		instanceCreateInfo.pApplicationInfo     = &appInfo;

		uint32_t layerCount = 0;
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
		const char *validationLayers[] = {"VK_LAYER_GOOGLE_threading", "VK_LAYER_LUNARG_parameter_validation", "VK_LAYER_LUNARG_object_tracker", "VK_LAYER_LUNARG_core_validation", "VK_LAYER_LUNARG_swapchain", "VK_LAYER_GOOGLE_unique_objects"};
		layerCount                     = 6;
#else
		const char *validationLayers[] = {"VK_LAYER_LUNARG_standard_validation"};
		layerCount                     = 1;
#endif
#if DEBUG
		// Check if layers are available
		uint32_t instanceLayerCount;
		vkEnumerateInstanceLayerProperties(&instanceLayerCount, nullptr);
		std::vector<VkLayerProperties> instanceLayers(instanceLayerCount);
		vkEnumerateInstanceLayerProperties(&instanceLayerCount, instanceLayers.data());

		bool layersAvailable = true;
		for (auto layerName : validationLayers)
		{
			bool layerAvailable = false;
			for (auto instanceLayer : instanceLayers)
			{
				if (strcmp(instanceLayer.layerName, layerName) == 0)
				{
					layerAvailable = true;
					break;
				}
			}
			if (!layerAvailable)
			{
				layersAvailable = false;
				break;
			}
		}

		if (layersAvailable)
		{
			instanceCreateInfo.ppEnabledLayerNames     = validationLayers;
			const char *validationExt                  = VK_EXT_DEBUG_REPORT_EXTENSION_NAME;
			instanceCreateInfo.enabledLayerCount       = layerCount;
			instanceCreateInfo.enabledExtensionCount   = 1;
			instanceCreateInfo.ppEnabledExtensionNames = &validationExt;
		}
#endif
		VK_CHECK_RESULT(vkCreateInstance(&instanceCreateInfo, nullptr, &instance));

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
		vks::android::loadVulkanFunctions(instance);
#endif
#if DEBUG
		if (layersAvailable)
		{
			VkDebugReportCallbackCreateInfoEXT debugReportCreateInfo = {};
			debugReportCreateInfo.sType                              = VK_STRUCTURE_TYPE_DEBUG_REPORT_CALLBACK_CREATE_INFO_EXT;
			debugReportCreateInfo.flags                              = VK_DEBUG_REPORT_ERROR_BIT_EXT | VK_DEBUG_REPORT_WARNING_BIT_EXT;
			debugReportCreateInfo.pfnCallback                        = (PFN_vkDebugReportCallbackEXT) debugMessageCallback;

			// We have to explicitly load this function.
			PFN_vkCreateDebugReportCallbackEXT vkCreateDebugReportCallbackEXT = reinterpret_cast<PFN_vkCreateDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugReportCallbackEXT"));
			assert(vkCreateDebugReportCallbackEXT);
			VK_CHECK_RESULT(vkCreateDebugReportCallbackEXT(instance, &debugReportCreateInfo, nullptr, &debugReportCallback));
		}
#endif

		/*
			Vulkan device creation
		*/
		// Physical device (always use first)
		uint32_t deviceCount = 0;
		VK_CHECK_RESULT(vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr));
		std::vector<VkPhysicalDevice> physicalDevices(deviceCount);
		VK_CHECK_RESULT(vkEnumeratePhysicalDevices(instance, &deviceCount, physicalDevices.data()));
		physicalDevice = physicalDevices[0];

		VkPhysicalDeviceProperties deviceProperties;
		vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
		LOG("GPU: %s\n", deviceProperties.deviceName);

		// Request a single compute queue
		const float             defaultQueuePriority(0.0f);
		VkDeviceQueueCreateInfo queueCreateInfo = {};
		uint32_t                queueFamilyCount;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, nullptr);
		std::vector<VkQueueFamilyProperties> queueFamilyProperties(queueFamilyCount);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount, queueFamilyProperties.data());
		for (uint32_t i = 0; i < static_cast<uint32_t>(queueFamilyProperties.size()); i++)
		{
			if (queueFamilyProperties[i].queueFlags & VK_QUEUE_COMPUTE_BIT)
			{
				queueFamilyIndex                 = i;
				queueCreateInfo.sType            = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
				queueCreateInfo.queueFamilyIndex = i;
				queueCreateInfo.queueCount       = 1;
				queueCreateInfo.pQueuePriorities = &defaultQueuePriority;
				break;
			}
		}
		// Create logical device
		VkDeviceCreateInfo deviceCreateInfo   = {};
		deviceCreateInfo.sType                = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
		deviceCreateInfo.queueCreateInfoCount = 1;
		deviceCreateInfo.pQueueCreateInfos    = &queueCreateInfo;
		VK_CHECK_RESULT(vkCreateDevice(physicalDevice, &deviceCreateInfo, nullptr, &device));

		// Get a compute queue
		vkGetDeviceQueue(device, queueFamilyIndex, 0, &queue);

		// Compute command pool
		VkCommandPoolCreateInfo cmdPoolInfo = {};
		cmdPoolInfo.sType                   = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		cmdPoolInfo.queueFamilyIndex        = queueFamilyIndex;
		cmdPoolInfo.flags                   = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VK_CHECK_RESULT(vkCreateCommandPool(device, &cmdPoolInfo, nullptr, &commandPool));

		/*
			Prepare storage buffers
		*/
		std::vector<uint32_t> computeInput(WIDTH * HEIGHT * COMPONENTS_PP);
		std::vector<uint32_t> computeOutput(WIDTH * HEIGHT * COMPONENTS_PP);

		// Fill input data
		uint32_t n = 0;
		std::generate(computeInput.begin(), computeInput.end(), [&n] { return 0; });

		const VkDeviceSize bufferSize = computeInput.size() * sizeof(computeInput.at(0));
		VkBuffer           deviceBuffer, hostBuffer;
		VkDeviceMemory     deviceMemory, hostMemory;

		const VkDeviceSize uniformSize = 2 * sizeof(uint32_t);
		VkBuffer           uniformBuffer;
		VkDeviceMemory     uniformMemory;

		vks::Texture2D textureComputeTarget;

		const VkDeviceSize lockSize = sizeof(uint32_t);
		VkBuffer           lockBuffer;
		VkDeviceMemory     lockMemory;

		// Copy input data to VRAM using a staging buffer
		{
			createBuffer(
			    VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT,
			    &hostBuffer,
			    &hostMemory,
			    bufferSize,
			    computeInput.data());

			// Flush writes to host visible buffer
			void *mapped;
			vkMapMemory(device, hostMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
			VkMappedMemoryRange mappedRange = vks::initializers::mappedMemoryRange();
			mappedRange.memory              = hostMemory;
			mappedRange.offset              = 0;
			mappedRange.size                = VK_WHOLE_SIZE;
			vkFlushMappedMemoryRanges(device, 1, &mappedRange);
			vkUnmapMemory(device, hostMemory);

			createBuffer(
			    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			    &deviceBuffer,
			    &deviceMemory,
			    bufferSize);

			// Copy to staging buffer
			VkCommandBufferAllocateInfo cmdBufAllocateInfo = vks::initializers::commandBufferAllocateInfo(commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
			VkCommandBuffer             copyCmd;
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &copyCmd));
			VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();
			VK_CHECK_RESULT(vkBeginCommandBuffer(copyCmd, &cmdBufInfo));

			VkBufferCopy copyRegion = {};
			copyRegion.size         = bufferSize;
			vkCmdCopyBuffer(copyCmd, hostBuffer, deviceBuffer, 1, &copyRegion);
			VK_CHECK_RESULT(vkEndCommandBuffer(copyCmd));

			VkSubmitInfo submitInfo       = vks::initializers::submitInfo();
			submitInfo.commandBufferCount = 1;
			submitInfo.pCommandBuffers    = &copyCmd;
			VkFenceCreateInfo fenceInfo   = vks::initializers::fenceCreateInfo(VK_FLAGS_NONE);
			VkFence           fence;
			VK_CHECK_RESULT(vkCreateFence(device, &fenceInfo, nullptr, &fence));

			// Submit to the queue
			VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &submitInfo, fence));
			VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

			vkDestroyFence(device, fence, nullptr);
			vkFreeCommandBuffers(device, commandPool, 1, &copyCmd);
		}

		{
			createBuffer(
			    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			    &uniformBuffer,
			    &uniformMemory,
			    uniformSize);

			uint32_t *dimensions = NULL;
			vkMapMemory(device, uniformMemory, 0, uniformSize, 0, (void **) &dimensions);
			dimensions[0] = WIDTH;
			dimensions[1] = HEIGHT;
			vkUnmapMemory(device, uniformMemory);
		}

#if 0
		// Prepare a texture target that is used to store compute shader calculations
		//void prepareTextureTarget(, uint32_t width, uint32_t height, VkFormat format)
		{
			vks::Texture *     tex = &textureComputeTarget;
			VkFormatProperties formatProperties;

			// Get device properties for the requested texture format
			vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R32G32B32A32_UINT, &formatProperties);
			// Check if requested image format supports image storage operations
			assert(formatProperties.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT);

			// Prepare blit target texture
			tex->width  = WIDTH;
			tex->height = HEIGHT;

			VkImageCreateInfo imageCreateInfo = vks::initializers::imageCreateInfo();
			imageCreateInfo.imageType         = VK_IMAGE_TYPE_2D;
			imageCreateInfo.format            = VK_FORMAT_R32G32B32A32_UINT;
			imageCreateInfo.extent            = {WIDTH, HEIGHT, 1};
			imageCreateInfo.mipLevels         = 1;
			imageCreateInfo.arrayLayers       = 1;
			imageCreateInfo.samples           = VK_SAMPLE_COUNT_1_BIT;
			imageCreateInfo.tiling            = VK_IMAGE_TILING_OPTIMAL;
			// Image will be sampled in the fragment shader and used as storage target in the compute shader
			imageCreateInfo.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
			                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
			imageCreateInfo.flags = 0;
			// Sharing mode exclusive means that ownership of the image does not need to be explicitly transferred between the compute and graphics queue
			imageCreateInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

			VkMemoryAllocateInfo memAllocInfo = vks::initializers::memoryAllocateInfo();
			VkMemoryRequirements memReqs;

			VK_CHECK_RESULT(vkCreateImage(device, &imageCreateInfo, nullptr, &tex->image));

			vkGetImageMemoryRequirements(device, tex->image, &memReqs);
			memAllocInfo.allocationSize  = memReqs.size;
			memAllocInfo.memoryTypeIndex = getMemoryTypeIndex(memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
			VK_CHECK_RESULT(vkAllocateMemory(device, &memAllocInfo, nullptr, &tex->deviceMemory));
			VK_CHECK_RESULT(vkBindImageMemory(device, tex->image, tex->deviceMemory, 0));

			VkCommandBuffer             layoutCmd;
			VkCommandBufferAllocateInfo cmdBufAllocateInfo =
			    vks::initializers::commandBufferAllocateInfo(commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &layoutCmd));

			tex->imageLayout = VK_IMAGE_LAYOUT_GENERAL;
			vks::tools::setImageLayout(
			    layoutCmd, tex->image,
			    VK_IMAGE_ASPECT_COLOR_BIT,
			    VK_IMAGE_LAYOUT_UNDEFINED,
			    tex->imageLayout);

			flushCommandBuffer(layoutCmd);

			// Create sampler
			VkSamplerCreateInfo sampler = vks::initializers::samplerCreateInfo();
			sampler.magFilter           = VK_FILTER_LINEAR;
			sampler.minFilter           = VK_FILTER_LINEAR;
			sampler.mipmapMode          = VK_SAMPLER_MIPMAP_MODE_LINEAR;
			sampler.addressModeU        = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
			sampler.addressModeV        = sampler.addressModeU;
			sampler.addressModeW        = sampler.addressModeU;
			sampler.mipLodBias          = 0.0f;
			sampler.maxAnisotropy       = 1.0f;
			sampler.compareOp           = VK_COMPARE_OP_NEVER;
			sampler.minLod              = 0.0f;
			sampler.maxLod              = tex->mipLevels;
			sampler.borderColor         = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
			VK_CHECK_RESULT(vkCreateSampler(device, &sampler, nullptr, &tex->sampler));

			// Create image view
			VkImageViewCreateInfo view = vks::initializers::imageViewCreateInfo();
			view.image                 = VK_NULL_HANDLE;
			view.viewType              = VK_IMAGE_VIEW_TYPE_2D;
			view.format                = VK_FORMAT_R32G32B32A32_UINT;
			view.components            = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B, VK_COMPONENT_SWIZZLE_A};
			view.subresourceRange      = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			view.image                 = tex->image;
			VK_CHECK_RESULT(vkCreateImageView(device, &view, nullptr, &tex->view));

			// Initialize a descriptor for later use
			tex->descriptor.imageLayout = tex->imageLayout;
			tex->descriptor.imageView   = tex->view;
			tex->descriptor.sampler     = tex->sampler;

			// TODO: What to assign for device? tex->device = device;
		}
#endif

		{
			createBuffer(
			    VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
			    VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
			    &lockBuffer,
			    &lockMemory,
			    lockSize);

			// Ensure the lock is ZERO to start with
			uint32_t *lock = NULL;
			vkMapMemory(device, lockMemory, 0, lockSize, 0, (void **) &lock);
			*lock = 0;
			vkUnmapMemory(device, lockMemory);
		}

		/*
			Prepare compute pipeline
		*/
		{
			std::vector<VkDescriptorPoolSize> poolSizes = {
			    vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1),
			    vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
			    vks::initializers::descriptorPoolSize(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1),
			};

			VkDescriptorPoolCreateInfo descriptorPoolInfo =
			    vks::initializers::descriptorPoolCreateInfo(poolSizes, 3);
			VK_CHECK_RESULT(vkCreateDescriptorPool(device, &descriptorPoolInfo, nullptr, &descriptorPool));

			std::vector<VkDescriptorSetLayoutBinding> setLayoutBindings = {
			    vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 0),
			    vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 1),
			    vks::initializers::descriptorSetLayoutBinding(VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, VK_SHADER_STAGE_COMPUTE_BIT, 2),
			};

			VkDescriptorSetLayoutCreateInfo descriptorLayout =
			    vks::initializers::descriptorSetLayoutCreateInfo(setLayoutBindings);
			VK_CHECK_RESULT(vkCreateDescriptorSetLayout(device, &descriptorLayout, nullptr, &descriptorSetLayout));

			VkPipelineLayoutCreateInfo pipelineLayoutCreateInfo =
			    vks::initializers::pipelineLayoutCreateInfo(&descriptorSetLayout, 1);
			VK_CHECK_RESULT(vkCreatePipelineLayout(device, &pipelineLayoutCreateInfo, nullptr, &pipelineLayout));

			VkDescriptorSetAllocateInfo allocInfo =
			    vks::initializers::descriptorSetAllocateInfo(descriptorPool, &descriptorSetLayout, 1);
			VK_CHECK_RESULT(vkAllocateDescriptorSets(device, &allocInfo, &descriptorSet));

			VkDescriptorBufferInfo            bufferDescriptor           = {deviceBuffer, 0, VK_WHOLE_SIZE};
			VkDescriptorBufferInfo            uniformDescriptor          = {uniformBuffer, 0, VK_WHOLE_SIZE};
			VkDescriptorBufferInfo            lockDescriptor             = {lockBuffer, 0, VK_WHOLE_SIZE};
			std::vector<VkWriteDescriptorSet> computeWriteDescriptorSets = {
			    vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 0, &uniformDescriptor),
			    vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, &bufferDescriptor),
			    vks::initializers::writeDescriptorSet(descriptorSet, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2, &lockDescriptor),
			};
			vkUpdateDescriptorSets(device, static_cast<uint32_t>(computeWriteDescriptorSets.size()), computeWriteDescriptorSets.data(), 0, NULL);

			VkPipelineCacheCreateInfo pipelineCacheCreateInfo = {};
			pipelineCacheCreateInfo.sType                     = VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO;
			VK_CHECK_RESULT(vkCreatePipelineCache(device, &pipelineCacheCreateInfo, nullptr, &pipelineCache));

			// Create pipeline
			VkComputePipelineCreateInfo computePipelineCreateInfo = vks::initializers::computePipelineCreateInfo(pipelineLayout, 0);

			// Pass SSBO size via specialization constant

			struct SpecializationData
			{
				uint32_t bufferElementCount = WIDTH * HEIGHT * COMPONENTS_PP;
				uint32_t local_size_x       = WORKGROUP_SZ_X;
				uint32_t local_size_y       = WORKGROUP_SZ_Y;
				uint32_t local_size_z       = 1;
			} specializationData;

			std::vector<VkSpecializationMapEntry> specializationMapEntries;
			specializationMapEntries.push_back(vks::initializers::specializationMapEntry(0, offsetof(SpecializationData, bufferElementCount), sizeof(uint32_t)));
			specializationMapEntries.push_back(vks::initializers::specializationMapEntry(1, offsetof(SpecializationData, local_size_x), sizeof(uint32_t)));
			specializationMapEntries.push_back(vks::initializers::specializationMapEntry(2, offsetof(SpecializationData, local_size_y), sizeof(uint32_t)));
			specializationMapEntries.push_back(vks::initializers::specializationMapEntry(3, offsetof(SpecializationData, local_size_z), sizeof(uint32_t)));

			VkSpecializationInfo specializationInfo =
			    vks::initializers::specializationInfo(static_cast<uint32_t>(specializationMapEntries.size()), specializationMapEntries.data(), sizeof(specializationData), &specializationData);

			// TODO: There is no command line arguments parsing (nor Android settings) for this
			// example, so we have no way of picking between GLSL or HLSL shaders.
			// Hard-code to glsl for now.
			const std::string shadersPath = getAssetPath() + "shaders/glsl/computescheduleviz/";

			VkPipelineShaderStageCreateInfo shaderStage = {};
			shaderStage.sType                           = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
			shaderStage.stage                           = VK_SHADER_STAGE_COMPUTE_BIT;
#if defined(VK_USE_PLATFORM_ANDROID_KHR)
			shaderStage.module = vks::tools::loadShader(androidapp->activity->assetManager, (shadersPath + "scheduleviz.comp.spv").c_str(), device);
#else
            shaderStage.module = vks::tools::loadShader((shadersPath + "scheduleviz.comp.spv").c_str(), device);
#endif
			shaderStage.pName               = "main";
			shaderStage.pSpecializationInfo = &specializationInfo;
			shaderModule                    = shaderStage.module;

			assert(shaderStage.module != VK_NULL_HANDLE);
			computePipelineCreateInfo.stage = shaderStage;
			VK_CHECK_RESULT(vkCreateComputePipelines(device, pipelineCache, 1, &computePipelineCreateInfo, nullptr, &pipeline));

			// Create a command buffer for compute operations
			VkCommandBufferAllocateInfo cmdBufAllocateInfo =
			    vks::initializers::commandBufferAllocateInfo(commandPool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1);
			VK_CHECK_RESULT(vkAllocateCommandBuffers(device, &cmdBufAllocateInfo, &commandBuffer));

			// Fence for compute CB sync
			VkFenceCreateInfo fenceCreateInfo = vks::initializers::fenceCreateInfo(VK_FENCE_CREATE_SIGNALED_BIT);
			VK_CHECK_RESULT(vkCreateFence(device, &fenceCreateInfo, nullptr, &fence));
		}

		/*
			Command buffer creation (for compute work submission)
		*/
		{
			VkCommandBufferBeginInfo cmdBufInfo = vks::initializers::commandBufferBeginInfo();

			VK_CHECK_RESULT(vkBeginCommandBuffer(commandBuffer, &cmdBufInfo));

			// Barrier to ensure that input buffer transfer is finished before compute shader reads from it
			VkBufferMemoryBarrier bufferBarrier = vks::initializers::bufferMemoryBarrier();
			bufferBarrier.buffer                = deviceBuffer;
			bufferBarrier.size                  = VK_WHOLE_SIZE;
			bufferBarrier.srcAccessMask         = VK_ACCESS_HOST_WRITE_BIT;
			bufferBarrier.dstAccessMask         = VK_ACCESS_SHADER_READ_BIT;
			bufferBarrier.srcQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED;
			bufferBarrier.dstQueueFamilyIndex   = VK_QUEUE_FAMILY_IGNORED;

			vkCmdPipelineBarrier(
			    commandBuffer,
			    VK_PIPELINE_STAGE_HOST_BIT,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    VK_FLAGS_NONE,
			    0, nullptr,
			    1, &bufferBarrier,
			    0, nullptr);

			vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
			vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, pipelineLayout, 0, 1, &descriptorSet, 0, 0);

			vkCmdDispatch(commandBuffer, (uint32_t) ceil(WIDTH / float(WORKGROUP_SZ_X)), (uint32_t) ceil(HEIGHT / float(WORKGROUP_SZ_Y)), 1);

			// Barrier to ensure that shader writes are finished before buffer is read back from GPU
			bufferBarrier.srcAccessMask       = VK_ACCESS_SHADER_WRITE_BIT;
			bufferBarrier.dstAccessMask       = VK_ACCESS_TRANSFER_READ_BIT;
			bufferBarrier.buffer              = deviceBuffer;
			bufferBarrier.size                = VK_WHOLE_SIZE;
			bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

			vkCmdPipelineBarrier(
			    commandBuffer,
			    VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
			    VK_PIPELINE_STAGE_TRANSFER_BIT,
			    VK_FLAGS_NONE,
			    0, nullptr,
			    1, &bufferBarrier,
			    0, nullptr);

			// Read back to host visible buffer
			VkBufferCopy copyRegion = {};
			copyRegion.size         = bufferSize;
			vkCmdCopyBuffer(commandBuffer, deviceBuffer, hostBuffer, 1, &copyRegion);

			// Barrier to ensure that buffer copy is finished before host reading from it
			bufferBarrier.srcAccessMask       = VK_ACCESS_TRANSFER_WRITE_BIT;
			bufferBarrier.dstAccessMask       = VK_ACCESS_HOST_READ_BIT;
			bufferBarrier.buffer              = hostBuffer;
			bufferBarrier.size                = VK_WHOLE_SIZE;
			bufferBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
			bufferBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;

			vkCmdPipelineBarrier(
			    commandBuffer,
			    VK_PIPELINE_STAGE_TRANSFER_BIT,
			    VK_PIPELINE_STAGE_HOST_BIT,
			    VK_FLAGS_NONE,
			    0, nullptr,
			    1, &bufferBarrier,
			    0, nullptr);

			VK_CHECK_RESULT(vkEndCommandBuffer(commandBuffer));

			// Submit compute work
			vkResetFences(device, 1, &fence);
			const VkPipelineStageFlags waitStageMask     = VK_PIPELINE_STAGE_TRANSFER_BIT;
			VkSubmitInfo               computeSubmitInfo = vks::initializers::submitInfo();
			computeSubmitInfo.pWaitDstStageMask          = &waitStageMask;
			computeSubmitInfo.commandBufferCount         = 1;
			computeSubmitInfo.pCommandBuffers            = &commandBuffer;
			VK_CHECK_RESULT(vkQueueSubmit(queue, 1, &computeSubmitInfo, fence));
			VK_CHECK_RESULT(vkWaitForFences(device, 1, &fence, VK_TRUE, UINT64_MAX));

			// Make device writes visible to the host
			void *mapped;
			vkMapMemory(device, hostMemory, 0, VK_WHOLE_SIZE, 0, &mapped);
			VkMappedMemoryRange mappedRange = vks::initializers::mappedMemoryRange();
			mappedRange.memory              = hostMemory;
			mappedRange.offset              = 0;
			mappedRange.size                = VK_WHOLE_SIZE;
			vkInvalidateMappedMemoryRanges(device, 1, &mappedRange);

			// Copy to output
			memcpy(computeOutput.data(), mapped, bufferSize);
			vkUnmapMemory(device, hostMemory);
		}

		vkQueueWaitIdle(queue);

		saveOutputAsImage(WIDTH, HEIGHT, COMPONENTS_PP, computeOutput);

		// Clean up
		vkDestroyBuffer(device, deviceBuffer, nullptr);
		vkFreeMemory(device, deviceMemory, nullptr);

		vkDestroyBuffer(device, hostBuffer, nullptr);
		vkFreeMemory(device, hostMemory, nullptr);

		vkDestroyBuffer(device, uniformBuffer, nullptr);
		vkFreeMemory(device, uniformMemory, nullptr);

		vkDestroyBuffer(device, lockBuffer, nullptr);
		vkFreeMemory(device, lockMemory, nullptr);

#if 0
		{
			vkDestroyImageView(device, textureComputeTarget.view, nullptr);
			vkDestroyImage(device, textureComputeTarget.image, nullptr);
			if (textureComputeTarget.sampler)
			{
				vkDestroySampler(device, textureComputeTarget.sampler, nullptr);
			}
			vkFreeMemory(device, textureComputeTarget.deviceMemory, nullptr);
		}
#endif

	}

	~VulkanExample()
	{
		vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
		vkDestroyDescriptorSetLayout(device, descriptorSetLayout, nullptr);
		vkDestroyDescriptorPool(device, descriptorPool, nullptr);
		vkDestroyPipeline(device, pipeline, nullptr);
		vkDestroyPipelineCache(device, pipelineCache, nullptr);
		vkDestroyFence(device, fence, nullptr);
		vkDestroyCommandPool(device, commandPool, nullptr);
		vkDestroyShaderModule(device, shaderModule, nullptr);
		vkDestroyDevice(device, nullptr);
#if DEBUG
		if (debugReportCallback)
		{
			PFN_vkDestroyDebugReportCallbackEXT vkDestroyDebugReportCallback = reinterpret_cast<PFN_vkDestroyDebugReportCallbackEXT>(vkGetInstanceProcAddr(instance, "vkDestroyDebugReportCallbackEXT"));
			assert(vkDestroyDebugReportCallback);
			vkDestroyDebugReportCallback(instance, debugReportCallback, nullptr);
		}
#endif
		vkDestroyInstance(instance, nullptr);
	}
};

#if defined(VK_USE_PLATFORM_ANDROID_KHR)
void handleAppCommand(android_app *app, int32_t cmd)
{
	if (cmd == APP_CMD_INIT_WINDOW)
	{
		VulkanExample *vulkanExample = new VulkanExample();
		delete (vulkanExample);
		ANativeActivity_finish(app->activity);
	}
}
void android_main(android_app *state)
{
	androidapp           = state;
	androidapp->onAppCmd = handleAppCommand;
	int                         ident, events;
	struct android_poll_source *source;
	while ((ident = ALooper_pollAll(-1, NULL, &events, (void **) &source)) >= 0)
	{
		if (source != NULL)
		{
			source->process(androidapp, source);
		}
		if (androidapp->destroyRequested != 0)
		{
			break;
		}
	}
}
#else
int main()
{
	VulkanExample *vulkanExample = new VulkanExample();
	std::cout << "Finished. Press enter to terminate...";
	getchar();
	delete (vulkanExample);
	return 0;
}
#endif
