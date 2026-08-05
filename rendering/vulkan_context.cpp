#include "vulkan_context.h"

#include <cstring>
#include <exception>
#include <limits>
#include <utility>
#include "../third_party/stb/stb_image.h"

namespace {

Result vkFailure(const std::string& operation, VkResult result) {
    return Result::failure(operation + " failed (VkResult " +
                           std::to_string(static_cast<int>(result)) + ")");
}

Result addContext(const std::string& context, const Result& result) {
    return Result::failure(context + ": " + result.error());
}

bool waitEstablishedCompletion(VkResult result) {
    return result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST;
}

struct ScopedBufferAllocation {
    explicit ScopedBufferAllocation(
        VkDevice owningDevice, VkBuffer* deferredBuffer = nullptr,
        VkDeviceMemory* deferredMemory = nullptr,
        const bool* submissionMayBePending = nullptr)
        : device(owningDevice),
          deferredBuffer(deferredBuffer),
          deferredMemory(deferredMemory),
          submissionMayBePending(submissionMayBePending) {}

    ~ScopedBufferAllocation() noexcept {
        if (submissionMayBePending && *submissionMayBePending) {
            if (deferredBuffer) {
                *deferredBuffer = buffer;
                buffer = VK_NULL_HANDLE;
            }
            if (deferredMemory) {
                *deferredMemory = memory;
                memory = VK_NULL_HANDLE;
            }
        }

        if (buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, buffer, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }

    ScopedBufferAllocation(const ScopedBufferAllocation&) = delete;
    ScopedBufferAllocation& operator=(const ScopedBufferAllocation&) = delete;

    void release() noexcept {
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    VkDevice device = VK_NULL_HANDLE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkBuffer* deferredBuffer = nullptr;
    VkDeviceMemory* deferredMemory = nullptr;
    const bool* submissionMayBePending = nullptr;
};

struct ScopedImageAllocation {
    explicit ScopedImageAllocation(VkDevice owningDevice)
        : device(owningDevice) {}

    ~ScopedImageAllocation() noexcept {
        if (image != VK_NULL_HANDLE) {
            vkDestroyImage(device, image, nullptr);
        }
        if (memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, memory, nullptr);
        }
    }

    ScopedImageAllocation(const ScopedImageAllocation&) = delete;
    ScopedImageAllocation& operator=(const ScopedImageAllocation&) = delete;

    void release() noexcept {
        image = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }

    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
};

struct ScopedShaderModule {
    explicit ScopedShaderModule(VkDevice owningDevice)
        : device(owningDevice) {}

    ~ScopedShaderModule() noexcept {
        if (module != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device, module, nullptr);
        }
    }

    ScopedShaderModule(const ScopedShaderModule&) = delete;
    ScopedShaderModule& operator=(const ScopedShaderModule&) = delete;

    VkDevice device = VK_NULL_HANDLE;
    VkShaderModule module = VK_NULL_HANDLE;
};

}  // namespace

VulkanContext::~VulkanContext() noexcept {
    (void)cleanup();
}

Result VulkanContext::validateSceneRenderStateIsEmpty(
    const Scene* scene) const {
    for (size_t objectIndex = 0;
         objectIndex < scene->gameObjects().size();
         ++objectIndex) {
        const auto& object = scene->gameObjects()[objectIndex];
        if (!object) {
            continue;
        }

        for (size_t instanceIndex = 0;
             instanceIndex < object->meshInstances_.size();
             ++instanceIndex) {
            const auto& instanceData =
                object->meshInstances_[instanceIndex];
            const auto& mesh = instanceData.mesh;
            const auto& material = instanceData.material;
            const auto& renderData = instanceData.renderData;

            const bool hasMeshState =
                mesh.vertexBuffer != VK_NULL_HANDLE ||
                mesh.vertexBufferMemory != VK_NULL_HANDLE ||
                mesh.indexBuffer != VK_NULL_HANDLE ||
                mesh.indexBufferMemory != VK_NULL_HANDLE;
            const bool hasMaterialState =
                material.textureImage != VK_NULL_HANDLE ||
                material.textureImageMemory != VK_NULL_HANDLE ||
                material.textureImageView != VK_NULL_HANDLE ||
                material.textureSampler != VK_NULL_HANDLE;
            const bool hasRenderData =
                !renderData.uniformBuffers.empty() ||
                !renderData.uniformBuffersMemory.empty() ||
                !renderData.uniformBuffersMapped.empty() ||
                !renderData.descriptorSets.empty();

            if (hasMeshState || hasMaterialState || hasRenderData) {
                return Result::failure(
                    "Scene object " + std::to_string(objectIndex) +
                    ", mesh instance " + std::to_string(instanceIndex) +
                    " already contains Vulkan render state");
            }
        }
    }

    return Result::success();
}

Result VulkanContext::prepareSceneResourceTracking(const Scene* scene) {
    size_t meshInstanceCount = 0;
    for (const auto& object : scene->gameObjects()) {
        if (object) {
            meshInstanceCount += object->meshInstances().size();
        }
    }

    try {
        ownedSceneBuffers.reserve(
            meshInstanceCount * (MAX_FRAMES_IN_FLIGHT + 2));
        ownedSceneImages.reserve(meshInstanceCount);
        ownedSceneImageViews.reserve(meshInstanceCount);
        ownedSceneSamplers.reserve(meshInstanceCount);
        ownedSceneDescriptorSets.reserve(meshInstanceCount);
        ownedRenderData.reserve(meshInstanceCount);
        ownedTemporaryBuffers.reserve(meshInstanceCount * 3);
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to reserve Vulkan scene-resource ownership records: " +
            std::string(exception.what()));
    }

    return Result::success();
}

Result VulkanContext::init(SDL_Window* w, Scene* scene) {
    spdlog::info("Initializing Vulkan Context...");
    if (!w) {
        return Result::failure(
            "Cannot initialize Vulkan Context with a null SDL window");
    }
    if (!scene) {
        return Result::failure(
            "Cannot initialize Vulkan Context with a null scene");
    }

    if (!cleanup()) {
        return Result::failure(
            "Cannot initialize Vulkan Context while previous device cleanup "
            "is waiting for submitted work to become idle");
    }

    Result result = validateSceneRenderStateIsEmpty(scene);
    if (!result) {
        return result;
    }
    result = prepareSceneResourceTracking(scene);
    if (!result) {
        return result;
    }

    window = w;
    currentScene = scene;

    const auto initializeStep = [this](Result result,
                                       const char* description) -> Result {
        if (!result) {
            Result failure = addContext(description, result);
            (void)cleanup();
            return failure;
        }
        return Result::success();
    };

    try {
        result = initializeStep(createInstance(), "Instance creation");
        if (!result) return result;
        result = initializeStep(setupDebugMessenger(),
                                "Debug messenger creation");
        if (!result) return result;
        result = initializeStep(createSurface(), "Surface creation");
        if (!result) return result;
        result = initializeStep(pickPhysicalDevice(),
                                "Physical-device selection");
        if (!result) return result;
        result = initializeStep(createLogicalDevice(),
                                "Logical-device creation");
        if (!result) return result;
        result = initializeStep(createSwapchain(), "Swapchain creation");
        if (!result) return result;
        result = initializeStep(createImageViews(),
                                "Swapchain image-view creation");
        if (!result) return result;
        result = initializeStep(createRenderPass(), "Render-pass creation");
        if (!result) return result;
        result = initializeStep(createDescriptorSetLayout(),
                                "Descriptor-set-layout creation");
        if (!result) return result;
        result = initializeStep(createLightsDescriptorSetLayout(),
                                "Lights descriptor-set-layout creation");
        if (!result) return result;
        result = initializeStep(createGraphicsPipeline(),
                                "Graphics-pipeline creation");
        if (!result) return result;
        result = initializeStep(createCommandPool(), "Command-pool creation");
        if (!result) return result;
        result = initializeStep(createColorResources(),
                                "Color-resource creation");
        if (!result) return result;
        result = initializeStep(createDepthResources(),
                                "Depth-resource creation");
        if (!result) return result;
        result = initializeStep(createFramebuffers(), "Framebuffer creation");
        if (!result) return result;
        result = initializeStep(createCommandBuffers(),
                                "Command-buffer creation");
        if (!result) return result;
        result = initializeStep(createSyncObjects(),
                                "Synchronization-object creation");
        if (!result) return result;

        initialized = true;
        spdlog::info("Successfully initialized Vulkan Context");
        return Result::success();
    } catch (const std::exception& exception) {
        (void)cleanup();
        return Result::failure(
            "Vulkan initialization raised an exception: " +
            std::string(exception.what()));
    } catch (...) {
        (void)cleanup();
        return Result::failure(
            "Vulkan initialization raised an unknown exception");
    }
}

Result VulkanContext::createInstance() {

    if (enableValidationLayers) {
        bool validationLayersSupported = false;
        Result result =
            checkValidationLayerSupport(validationLayersSupported);
        if (!result) {
            return result;
        }
        if (!validationLayersSupported) {
            return Result::failure(
                "Validation layers were requested but are not available");
        }
        spdlog::info("Validation layers are enabled and supported");
    }

    VkApplicationInfo appInfo{};
    appInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    appInfo.pApplicationName = "Dunamis Engine";
    appInfo.applicationVersion = VK_MAKE_VERSION(0, 0, 2);
    appInfo.pEngineName = "Dunamis Engine";
    appInfo.engineVersion = VK_MAKE_VERSION(0, 0, 2);
    appInfo.apiVersion = VK_API_VERSION_1_3;

    VkInstanceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    createInfo.pApplicationInfo = &appInfo;

    std::vector<const char*> extensions;
    Result result = getRequiredExtensions(extensions);
    if (!result) {
        return result;
    }
    createInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    createInfo.ppEnabledExtensionNames = extensions.data();

    VkDebugUtilsMessengerCreateInfoEXT debugCreateInfo{};
    if (enableValidationLayers) {

        createInfo.enabledLayerCount = 0;

        populateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext =
            (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
    } else {
        createInfo.enabledLayerCount = 0;
        createInfo.pNext = nullptr;
    }

    const VkResult createResult =
        vkCreateInstance(&createInfo, nullptr, &instance);
    if (createResult != VK_SUCCESS) {
        instance = VK_NULL_HANDLE;
        return vkFailure("vkCreateInstance", createResult);
    }

    spdlog::info("Successfully created Vulkan instance");
    return Result::success();
}

Result VulkanContext::checkValidationLayerSupport(bool& supported) const {
    supported = false;
    uint32_t layerCount = 0;
    VkResult result =
        vkEnumerateInstanceLayerProperties(&layerCount, nullptr);
    if (result != VK_SUCCESS) {
        return vkFailure("vkEnumerateInstanceLayerProperties(count)", result);
    }

    std::vector<VkLayerProperties> availableLayers(layerCount);
    result = vkEnumerateInstanceLayerProperties(
        &layerCount, availableLayers.data());
    if (result != VK_SUCCESS) {
        return vkFailure("vkEnumerateInstanceLayerProperties(data)", result);
    }

    // Compare our available layers to the validation layers we want
    // If we find a match, return true
    for (const char* layerName : validationLayers) {
        bool layerFound = false;

        for (const auto& layerProperties : availableLayers) {
            if (strcmp(layerName, layerProperties.layerName) == 0) {
                layerFound = true;
                break;
            }
        }

        if (!layerFound) {
            return Result::success();
        }
    }
    supported = true;
    return Result::success();
}

Result VulkanContext::getRequiredExtensions(
    std::vector<const char*>& extensions) const {
    uint32_t extensionCount = 0;

    const char* const* sdlExtensions =
        SDL_Vulkan_GetInstanceExtensions(&extensionCount);
    if (!sdlExtensions) {
        return Result::failure(
            std::string("SDL_Vulkan_GetInstanceExtensions failed: ") +
            SDL_GetError());
    }

    extensions.assign(sdlExtensions, sdlExtensions + extensionCount);

    if (enableValidationLayers) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    return Result::success();
}

static VKAPI_ATTR VkBool32 VKAPI_CALL
debugCallback(VkDebugUtilsMessageSeverityFlagBitsEXT messageSeverity,
              VkDebugUtilsMessageTypeFlagsEXT messageType,
              const VkDebugUtilsMessengerCallbackDataEXT* pCallbackData,
              void* pUserData) {
    spdlog::warn("Validation layer: {}", pCallbackData->pMessage);
    return VK_FALSE;
}

void VulkanContext::populateDebugMessengerCreateInfo(
    VkDebugUtilsMessengerCreateInfoEXT& createInfo) const {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
        VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    createInfo.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    createInfo.pfnUserCallback = debugCallback;
}

Result VulkanContext::setupDebugMessenger() {
    if (!enableValidationLayers) return Result::success();

    VkDebugUtilsMessengerCreateInfoEXT createInfo{};
    populateDebugMessengerCreateInfo(createInfo);

    const VkResult result = createDebugUtilsMessengerEXT(
        instance, &createInfo, nullptr, &debugMessenger);
    if (result != VK_SUCCESS) {
        debugMessenger = VK_NULL_HANDLE;
        return vkFailure("vkCreateDebugUtilsMessengerEXT", result);
    }
    return Result::success();
}

VkResult VulkanContext::createDebugUtilsMessengerEXT(
    VkInstance instance, const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDebugUtilsMessengerEXT* pDebugMessenger) {
    auto func = (PFN_vkCreateDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkCreateDebugUtilsMessengerEXT");
    if (func != nullptr) {
        return func(instance, pCreateInfo, pAllocator, pDebugMessenger);
    } else {
        return VK_ERROR_EXTENSION_NOT_PRESENT;
    }
}

Result VulkanContext::createSurface() {
    const bool surfaceResult =
        SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface);

    if (!surfaceResult) {
        surface = VK_NULL_HANDLE;
        return Result::failure(
            std::string("SDL_Vulkan_CreateSurface failed: ") +
            SDL_GetError());
    }
    spdlog::info("Successfully created Vulkan surface");
    return Result::success();
}

void VulkanContext::destroyDebugUtilsMessengerEXT(
    VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
    const VkAllocationCallbacks* pAllocator) {
    auto func = (PFN_vkDestroyDebugUtilsMessengerEXT)vkGetInstanceProcAddr(
        instance, "vkDestroyDebugUtilsMessengerEXT");
    if (func != nullptr) {
        func(instance, debugMessenger, pAllocator);
    }
}

Result VulkanContext::pickPhysicalDevice() {
    spdlog::info("Searching for physical devices...");
    uint32_t deviceCount = 0;
    VkResult result =
        vkEnumeratePhysicalDevices(instance, &deviceCount, nullptr);
    if (result != VK_SUCCESS) {
        return vkFailure("vkEnumeratePhysicalDevices(count)", result);
    }

    if (deviceCount == 0) {
        return Result::failure(
            "Failed to find any GPUs with Vulkan support");
    }
    spdlog::info("Found at least one GPU with Vulkan support");

    std::vector<VkPhysicalDevice> devices(deviceCount);
    result =
        vkEnumeratePhysicalDevices(instance, &deviceCount, devices.data());
    if (result != VK_SUCCESS) {
        return vkFailure("vkEnumeratePhysicalDevices(data)", result);
    }
    spdlog::info("Checking GPU suitability...");
    for (const auto& candidate : devices) {
        bool suitable = false;
        Result suitabilityResult = isDeviceSuitable(candidate, suitable);
        if (!suitabilityResult) {
            return addContext("Failed to query physical-device suitability",
                              suitabilityResult);
        }
        if (suitable) {
            physicalDevice = candidate;
            msaaSamples = getMaxUsableSampleCount();
            break;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        return Result::failure("Failed to find a suitable GPU");
    }
    spdlog::info("Successfully selected physical device");
    return Result::success();
}

Result VulkanContext::isDeviceSuitable(VkPhysicalDevice candidate,
                                       bool& suitable) {
    suitable = false;
    QueueFamilyIndices indices;
    Result result = findQueueFamilies(candidate, indices);
    if (!result) {
        return result;
    }

    bool extensionsSupported = false;
    result =
        checkDeviceExtensionSupport(candidate, extensionsSupported);
    if (!result) {
        return result;
    }

    bool swapchainAdequate = false;
    if (extensionsSupported) {
        SwapchainSupportDetails swapchainSupport;
        result = querySwapchainSupport(candidate, swapchainSupport);
        if (!result) {
            return result;
        }
        swapchainAdequate = !swapchainSupport.formats.empty() &&
                            !swapchainSupport.presentModes.empty();
    }

    VkPhysicalDeviceFeatures supportedFeatures{};
    vkGetPhysicalDeviceFeatures(candidate, &supportedFeatures);

    suitable = indices.isComplete() && extensionsSupported &&
               swapchainAdequate && supportedFeatures.samplerAnisotropy;
    return Result::success();
}

Result VulkanContext::findQueueFamilies(
    VkPhysicalDevice candidate, QueueFamilyIndices& indices) const {
    indices = {};
    uint32_t queueFamilyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount,
                                             nullptr);

    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(candidate, &queueFamilyCount,
                                             queueFamilies.data());

    // Grab the present and graphics queue families
    uint32_t i = 0;
    for (const auto& queueFamily : queueFamilies) {
        if (queueFamily.queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            indices.graphicsFamily = i;
        }

        VkBool32 presentSupport = false;
        const VkResult result = vkGetPhysicalDeviceSurfaceSupportKHR(
            candidate, i, surface, &presentSupport);
        if (result != VK_SUCCESS) {
            return vkFailure("vkGetPhysicalDeviceSurfaceSupportKHR", result);
        }

        if (presentSupport) {
            indices.presentFamily = i;
        }
        if (indices.isComplete()) {
            break;
        }
        i++;
    }

    return Result::success();
}

Result VulkanContext::checkDeviceExtensionSupport(
    VkPhysicalDevice candidate, bool& supported) const {
    supported = false;
    uint32_t extensionCount = 0;
    VkResult result = vkEnumerateDeviceExtensionProperties(
        candidate, nullptr, &extensionCount, nullptr);
    if (result != VK_SUCCESS) {
        return vkFailure(
            "vkEnumerateDeviceExtensionProperties(count)", result);
    }

    std::vector<VkExtensionProperties> availableExtensions(extensionCount);
    result = vkEnumerateDeviceExtensionProperties(
        candidate, nullptr, &extensionCount, availableExtensions.data());
    if (result != VK_SUCCESS) {
        return vkFailure(
            "vkEnumerateDeviceExtensionProperties(data)", result);
    }

    std::set<std::string> requiredExtensions(deviceExtensions.begin(),
                                             deviceExtensions.end());

    for (const auto& extension : availableExtensions) {
        requiredExtensions.erase(extension.extensionName);
    }

    supported = requiredExtensions.empty();
    return Result::success();
}

Result VulkanContext::querySwapchainSupport(
    VkPhysicalDevice candidate, SwapchainSupportDetails& details) const {
    details = {};

    VkResult result = vkGetPhysicalDeviceSurfaceCapabilitiesKHR(
        candidate, surface, &details.capabilities);
    if (result != VK_SUCCESS) {
        return vkFailure(
            "vkGetPhysicalDeviceSurfaceCapabilitiesKHR", result);
    }

    uint32_t formatCount = 0;
    result = vkGetPhysicalDeviceSurfaceFormatsKHR(
        candidate, surface, &formatCount, nullptr);
    if (result != VK_SUCCESS) {
        return vkFailure(
            "vkGetPhysicalDeviceSurfaceFormatsKHR(count)", result);
    }

    if (formatCount != 0) {
        details.formats.resize(formatCount);
        result = vkGetPhysicalDeviceSurfaceFormatsKHR(
            candidate, surface, &formatCount, details.formats.data());
        if (result != VK_SUCCESS) {
            return vkFailure(
                "vkGetPhysicalDeviceSurfaceFormatsKHR(data)", result);
        }
    }

    uint32_t presentModeCount = 0;
    result = vkGetPhysicalDeviceSurfacePresentModesKHR(
        candidate, surface, &presentModeCount, nullptr);
    if (result != VK_SUCCESS) {
        return vkFailure(
            "vkGetPhysicalDeviceSurfacePresentModesKHR(count)", result);
    }

    if (presentModeCount != 0) {
        details.presentModes.resize(presentModeCount);
        result = vkGetPhysicalDeviceSurfacePresentModesKHR(
            candidate, surface, &presentModeCount,
            details.presentModes.data());
        if (result != VK_SUCCESS) {
            return vkFailure(
                "vkGetPhysicalDeviceSurfacePresentModesKHR(data)", result);
        }
    }
    return Result::success();
}

VkSampleCountFlagBits VulkanContext::getMaxUsableSampleCount() const {
    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

    VkSampleCountFlags counts =
        physicalDeviceProperties.limits.framebufferColorSampleCounts &
        physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    if (counts & VK_SAMPLE_COUNT_64_BIT) {
        return VK_SAMPLE_COUNT_64_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_32_BIT) {
        return VK_SAMPLE_COUNT_32_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_16_BIT) {
        return VK_SAMPLE_COUNT_16_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_8_BIT) {
        return VK_SAMPLE_COUNT_8_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_4_BIT) {
        return VK_SAMPLE_COUNT_4_BIT;
    }
    if (counts & VK_SAMPLE_COUNT_2_BIT) {
        return VK_SAMPLE_COUNT_2_BIT;
    }

    return VK_SAMPLE_COUNT_1_BIT;
}

Result VulkanContext::createLogicalDevice() {

    QueueFamilyIndices indices;
    Result result = findQueueFamilies(physicalDevice, indices);
    if (!result) {
        return result;
    }
    if (!indices.isComplete()) {
        return Result::failure(
            "Selected physical device has incomplete queue families");
    }

    std::vector<VkDeviceQueueCreateInfo> queueCreateInfos;
    std::set<uint32_t> uniqueQueueFamilies = {indices.graphicsFamily.value(),
                                              indices.presentFamily.value()};

    float queuePriority = 1.0f;
    for (uint32_t queueFamily : uniqueQueueFamilies) {
        VkDeviceQueueCreateInfo queueCreateInfo{};
        queueCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queueCreateInfo.queueFamilyIndex = queueFamily;
        queueCreateInfo.queueCount = 1;
        queueCreateInfo.pQueuePriorities = &queuePriority;
        queueCreateInfos.push_back(queueCreateInfo);
    }

    VkPhysicalDeviceFeatures deviceFeatures{};
    deviceFeatures.samplerAnisotropy = VK_TRUE;
    deviceFeatures.sampleRateShading = VK_TRUE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;

    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    if (enableValidationLayers) {
        createInfo.enabledLayerCount =
            static_cast<uint32_t>(validationLayers.size());
    } else {
        createInfo.enabledLayerCount = 0;
    }

    const VkResult createResult =
        vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
    if (createResult != VK_SUCCESS) {
        device = VK_NULL_HANDLE;
        return vkFailure("vkCreateDevice", createResult);
    }

    vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
    vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    spdlog::info("Successfully created logical device");
    return Result::success();
}

Result VulkanContext::createSwapchain() {

    SwapchainSupportDetails swapchainSupport;
    Result result =
        querySwapchainSupport(physicalDevice, swapchainSupport);
    if (!result) {
        return result;
    }
    if (swapchainSupport.formats.empty() ||
        swapchainSupport.presentModes.empty()) {
        return Result::failure(
            "Swapchain support became inadequate during creation");
    }

    VkSurfaceFormatKHR surfaceFormat =
        chooseSwapSurfaceFormat(swapchainSupport.formats);

    VkPresentModeKHR presentMode =
        chooseSwapPresentMode(swapchainSupport.presentModes);

    VkExtent2D extent{};
    result = chooseSwapExtent(swapchainSupport.capabilities, extent);
    if (!result) {
        return result;
    }

    uint32_t imageCount = swapchainSupport.capabilities.minImageCount + 1;
    if (swapchainSupport.capabilities.maxImageCount > 0 &&
        imageCount > swapchainSupport.capabilities.maxImageCount) {
        imageCount = swapchainSupport.capabilities.maxImageCount;
    }

    VkSwapchainCreateInfoKHR createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    createInfo.surface = surface;

    createInfo.minImageCount = imageCount;
    createInfo.imageFormat = surfaceFormat.format;
    createInfo.imageColorSpace = surfaceFormat.colorSpace;
    createInfo.imageExtent = extent;
    createInfo.imageArrayLayers = 1;
    createInfo.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;

    QueueFamilyIndices indices;
    result = findQueueFamilies(physicalDevice, indices);
    if (!result) {
        return result;
    }
    if (!indices.isComplete()) {
        return Result::failure(
            "Selected physical device has incomplete queue families");
    }
    uint32_t queueFamilyIndices[] = {indices.graphicsFamily.value(),
                                     indices.presentFamily.value()};

    if (indices.graphicsFamily != indices.presentFamily) {
        createInfo.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        createInfo.queueFamilyIndexCount = 2;
        createInfo.pQueueFamilyIndices = queueFamilyIndices;
    } else {
        createInfo.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }

    createInfo.preTransform = swapchainSupport.capabilities.currentTransform;
    createInfo.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    createInfo.presentMode = presentMode;
    createInfo.clipped = VK_TRUE;

    const VkResult createResult =
        vkCreateSwapchainKHR(device, &createInfo, nullptr, &swapchain);
    if (createResult != VK_SUCCESS) {
        swapchain = VK_NULL_HANDLE;
        return vkFailure("vkCreateSwapchainKHR", createResult);
    }

    VkResult imageResult =
        vkGetSwapchainImagesKHR(device, swapchain, &imageCount, nullptr);
    if (imageResult != VK_SUCCESS) {
        return vkFailure("vkGetSwapchainImagesKHR(count)", imageResult);
    }
    swapchainImages.resize(imageCount);
    imageResult = vkGetSwapchainImagesKHR(
        device, swapchain, &imageCount, swapchainImages.data());
    if (imageResult != VK_SUCCESS) {
        return vkFailure("vkGetSwapchainImagesKHR(data)", imageResult);
    }

    swapchainImageFormat = surfaceFormat.format;
    swapchainExtent = extent;
    spdlog::info("Successfully created swapchain");
    return Result::success();
}

VkSurfaceFormatKHR VulkanContext::chooseSwapSurfaceFormat(
    const std::vector<VkSurfaceFormatKHR>& availableFormats) const {
    for (const auto& availableFormat : availableFormats) {
        if (availableFormat.format == VK_FORMAT_B8G8R8A8_SRGB &&
            availableFormat.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            return availableFormat;
        }
    }
    return availableFormats[0];
}

VkPresentModeKHR VulkanContext::chooseSwapPresentMode(
    const std::vector<VkPresentModeKHR>& availablePresentModes) const {
    for (const auto& availablePresentMode : availablePresentModes) {
        if (availablePresentMode == VK_PRESENT_MODE_MAILBOX_KHR) {
            return availablePresentMode;
        }
    }
    return VK_PRESENT_MODE_FIFO_KHR;
}

Result VulkanContext::chooseSwapExtent(
    const VkSurfaceCapabilitiesKHR& capabilities,
    VkExtent2D& extent) const {
    if (capabilities.currentExtent.width !=
        std::numeric_limits<uint32_t>::max()) {
        extent = capabilities.currentExtent;
        return Result::success();
    } else {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSize(window, &width, &height)) {
            return Result::failure(
                std::string("SDL_GetWindowSize failed: ") +
                SDL_GetError());
        }

        VkExtent2D actualExtent = {static_cast<uint32_t>(width),
                                   static_cast<uint32_t>(height)};

        actualExtent.width =
            std::clamp(actualExtent.width, capabilities.minImageExtent.width,
                       capabilities.maxImageExtent.width);
        actualExtent.height =
            std::clamp(actualExtent.height, capabilities.minImageExtent.height,
                       capabilities.maxImageExtent.height);

        extent = actualExtent;
        return Result::success();
    }
}

Result VulkanContext::createImageViews() {
    swapchainImageViews.resize(swapchainImages.size());

    for (size_t i = 0; i < swapchainImages.size(); i++) {
        Result result = createImageView(
            swapchainImages[i], swapchainImageFormat,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, swapchainImageViews[i]);
        if (!result) {
            return addContext(
                "Failed to create swapchain image view " +
                    std::to_string(i),
                result);
        }
    }
    spdlog::info("Successfully created image views");
    return Result::success();
}

Result VulkanContext::createImageView(VkImage image, VkFormat format,
                                      VkImageAspectFlags aspectFlags,
                                      uint32_t mipLevels,
                                      VkImageView& imageView,
                                      OwnedImageView* ownership) {
    imageView = VK_NULL_HANDLE;
    VkImageViewCreateInfo viewInfo{};
    viewInfo.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    viewInfo.image = image;
    viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
    viewInfo.format = format;
    viewInfo.subresourceRange.aspectMask = aspectFlags;
    viewInfo.subresourceRange.baseMipLevel = 0;
    viewInfo.subresourceRange.levelCount = mipLevels;
    viewInfo.subresourceRange.baseArrayLayer = 0;
    viewInfo.subresourceRange.layerCount = 1;

    const VkResult result =
        vkCreateImageView(device, &viewInfo, nullptr, &imageView);
    if (result != VK_SUCCESS) {
        imageView = VK_NULL_HANDLE;
        return vkFailure("vkCreateImageView", result);
    }
    if (ownership) {
        ownership->view = imageView;
    }
    return Result::success();
}

Result VulkanContext::createRenderPass() {

    VkAttachmentDescription colorAttachment{};
    colorAttachment.format = swapchainImageFormat;
    colorAttachment.samples = msaaSamples;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentDescription colorAttachmentResolve{};
    colorAttachmentResolve.format = swapchainImageFormat;
    colorAttachmentResolve.samples = VK_SAMPLE_COUNT_1_BIT;
    colorAttachmentResolve.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachmentResolve.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachmentResolve.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    colorAttachmentResolve.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    colorAttachmentResolve.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    VkAttachmentReference colorAttachmentResolveRef{};
    colorAttachmentResolveRef.attachment = 2;
    colorAttachmentResolveRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkAttachmentReference colorAttachmentRef{};
    colorAttachmentRef.attachment = 0;
    colorAttachmentRef.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    Result result = findDepthFormat(depthFormat);
    if (!result) {
        return result;
    }

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = depthFormat;
    depthAttachment.samples = msaaSamples;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkAttachmentReference depthAttachmentRef{};
    depthAttachmentRef.attachment = 1;
    depthAttachmentRef.layout =
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.colorAttachmentCount = 1;
    subpass.pResolveAttachments = &colorAttachmentResolveRef;
    subpass.pColorAttachments = &colorAttachmentRef;
    subpass.pDepthStencilAttachment = &depthAttachmentRef;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
    dependency.dstSubpass = 0;
    dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = 0;
    dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                              VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                               VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

    std::array<VkAttachmentDescription, 3> attachments = {
        colorAttachment, depthAttachment, colorAttachmentResolve};
    VkRenderPassCreateInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    renderPassInfo.attachmentCount = static_cast<uint32_t>(attachments.size());
    renderPassInfo.pAttachments = attachments.data();
    renderPassInfo.subpassCount = 1;
    renderPassInfo.pSubpasses = &subpass;
    renderPassInfo.dependencyCount = 1;
    renderPassInfo.pDependencies = &dependency;

    const VkResult createResult =
        vkCreateRenderPass(device, &renderPassInfo, nullptr, &renderPass);
    if (createResult != VK_SUCCESS) {
        renderPass = VK_NULL_HANDLE;
        return vkFailure("vkCreateRenderPass", createResult);
    }

    spdlog::info("Successfully created render pass");
    return Result::success();
}

Result VulkanContext::findDepthFormat(VkFormat& format) const {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D32_SFLOAT_S8_UINT,
         VK_FORMAT_D24_UNORM_S8_UINT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT, format);
}

Result VulkanContext::findSupportedFormat(
    const std::vector<VkFormat>& candidates, VkImageTiling tiling,
    VkFormatFeatureFlags features, VkFormat& format) const {
    format = VK_FORMAT_UNDEFINED;
    for (VkFormat candidate : candidates) {
        VkFormatProperties props{};
        vkGetPhysicalDeviceFormatProperties(physicalDevice, candidate, &props);

        if (tiling == VK_IMAGE_TILING_LINEAR &&
            (props.linearTilingFeatures & features) == features) {
            format = candidate;
            return Result::success();
        } else if (tiling == VK_IMAGE_TILING_OPTIMAL &&
                   (props.optimalTilingFeatures & features) == features) {
            format = candidate;
            return Result::success();
        }
    }

    return Result::failure("Failed to find a supported Vulkan format");
}

Result VulkanContext::createDescriptorSetLayout() {

    VkDescriptorSetLayoutBinding uboLayoutBinding{};
    uboLayoutBinding.binding = 0;
    uboLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    uboLayoutBinding.descriptorCount = 1;
    uboLayoutBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    uboLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutBinding samplerLayoutBinding{};
    samplerLayoutBinding.binding = 1;
    samplerLayoutBinding.descriptorCount = 1;
    samplerLayoutBinding.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    samplerLayoutBinding.pImmutableSamplers = nullptr;
    samplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
        uboLayoutBinding, samplerLayoutBinding};

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
    layoutInfo.pBindings = bindings.data();

    const VkResult result = vkCreateDescriptorSetLayout(
        device, &layoutInfo, nullptr, &descriptorSetLayout);
    if (result != VK_SUCCESS) {
        descriptorSetLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreateDescriptorSetLayout", result);
    }
    spdlog::info("Successfully created descriptor set layout");
    return Result::success();
}

Result VulkanContext::createLightsDescriptorSetLayout() {

    VkDescriptorSetLayoutBinding lightsLayoutBinding{};
    lightsLayoutBinding.binding = 0;
    lightsLayoutBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    lightsLayoutBinding.descriptorCount = 1;
    lightsLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    lightsLayoutBinding.pImmutableSamplers = nullptr;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &lightsLayoutBinding;

    const VkResult result = vkCreateDescriptorSetLayout(
        device, &layoutInfo, nullptr, &lightsDescriptorSetLayout);
    if (result != VK_SUCCESS) {
        lightsDescriptorSetLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreateDescriptorSetLayout(lights)", result);
    }
    spdlog::info("Successfully created lights descriptor set layout");
    return Result::success();
}

Result VulkanContext::createLightsUBO() {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }

    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = 1;
    allocInfo.pSetLayouts = &lightsDescriptorSetLayout;

    const VkResult result = vkAllocateDescriptorSets(
        device, &allocInfo, &lightsDescriptorSet);
    if (result != VK_SUCCESS) {
        lightsDescriptorSet = VK_NULL_HANDLE;
        return vkFailure("vkAllocateDescriptorSets(lights)", result);
    }

    spdlog::info("Successfully allocated lights descriptor set");
    return Result::success();
}

Result VulkanContext::updateLightsDescriptorSet() {
    const VkResult result = vkDeviceWaitIdle(device);
    if (waitEstablishedCompletion(result)) {
        hasSubmittedWork = false;
        singleTimeSubmissionMayBePending = false;
    }
    if (result != VK_SUCCESS) {
        return vkFailure("vkDeviceWaitIdle", result);
    }
    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = lightsBuffer;
    bufferInfo.offset = 0;
    bufferInfo.range = sizeof(LightsUBO);

    VkWriteDescriptorSet descriptorWrite{};
    descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    descriptorWrite.dstSet = lightsDescriptorSet;
    descriptorWrite.dstBinding = 0;
    descriptorWrite.dstArrayElement = 0;
    descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    descriptorWrite.descriptorCount = 1;
    descriptorWrite.pBufferInfo = &bufferInfo;

    vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    return Result::success();
}

Result VulkanContext::createGraphicsPipeline() {

    spdlog::info("Reading vertex shader code {}...", "rendering/shaders/vert.spv");
    std::vector<char> vertShaderCode;
    Result result = readFile("rendering/shaders/vert.spv", vertShaderCode);
    if (!result) {
        return result;
    }
    ScopedShaderModule vertShaderModule(device);
    result = createShaderModule(vertShaderCode, vertShaderModule.module);
    if (!result) {
        return addContext("Failed to create vertex shader module", result);
    }

    spdlog::info("Reading fragment shader code {}...", "rendering/shaders/frag.spv");
    std::vector<char> fragShaderCode;
    result = readFile("rendering/shaders/frag.spv", fragShaderCode);
    if (!result) {
        return result;
    }
    ScopedShaderModule fragShaderModule(device);
    result = createShaderModule(fragShaderCode, fragShaderModule.module);
    if (!result) {
        return addContext("Failed to create fragment shader module", result);
    }

    VkPipelineShaderStageCreateInfo vertShaderStageInfo{};
    vertShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertShaderStageInfo.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertShaderStageInfo.module = vertShaderModule.module;
    vertShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo fragShaderStageInfo{};
    fragShaderStageInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragShaderStageInfo.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragShaderStageInfo.module = fragShaderModule.module;
    fragShaderStageInfo.pName = "main";

    VkPipelineShaderStageCreateInfo shaderStages[] = {vertShaderStageInfo,
                                                      fragShaderStageInfo};

    auto bindingDescription = Vertex::getBindingDescription();
    auto attributeDescriptions = Vertex::getAttributeDescriptions();

    VkPipelineVertexInputStateCreateInfo vertexInputInfo{};
    vertexInputInfo.sType =
        VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInputInfo.vertexBindingDescriptionCount = 1;
    vertexInputInfo.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInputInfo.pVertexBindingDescriptions = &bindingDescription;
    vertexInputInfo.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType =
        VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    inputAssembly.primitiveRestartEnable = VK_FALSE;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType =
        VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType =
        VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable = VK_TRUE;
    multisampling.minSampleShading = .2f;
    multisampling.rasterizationSamples = msaaSamples;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask =
        VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
        VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;

    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType =
        VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.logicOp = VK_LOGIC_OP_COPY;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;
    colorBlending.blendConstants[0] = 0.0f;
    colorBlending.blendConstants[1] = 0.0f;
    colorBlending.blendConstants[2] = 0.0f;
    colorBlending.blendConstants[3] = 0.0f;

    std::vector<VkDynamicState> dynamicStates = {VK_DYNAMIC_STATE_VIEWPORT,
                                                 VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount =
        static_cast<uint32_t>(dynamicStates.size());
    dynamicState.pDynamicStates = dynamicStates.data();

    VkDeviceSize bufferSize = sizeof(LightsUBO);

    result = createBuffer(bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                          lightsBuffer, lightsBufferMemory);
    if (!result) {
        return addContext("Failed to create lights uniform buffer", result);
    }

    VkDescriptorSetLayout setLayouts[] = {descriptorSetLayout,
        lightsDescriptorSetLayout};

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    //pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.setLayoutCount = 2;
    //pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 0;

    VkResult createResult = vkCreatePipelineLayout(
        device, &pipelineLayoutInfo, nullptr, &pipelineLayout);
    if (createResult != VK_SUCCESS) {
        pipelineLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreatePipelineLayout", createResult);
    }
    spdlog::info("Successfully created pipeline layout");

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType =
        VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_TRUE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.minDepthBounds = 0.0f;
    depthStencil.maxDepthBounds = 1.0f;
    depthStencil.stencilTestEnable = VK_FALSE;
    depthStencil.front = {};
    depthStencil.back = {};

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInputInfo;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = pipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;
    pipelineInfo.basePipelineHandle = VK_NULL_HANDLE;

    createResult = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &graphicsPipeline);

    if (createResult != VK_SUCCESS) {
        if (graphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphicsPipeline, nullptr);
        }
        graphicsPipeline = VK_NULL_HANDLE;
        return vkFailure("vkCreateGraphicsPipelines", createResult);
    }

    spdlog::info("Successfully created graphics pipeline");
    return Result::success();
}

Result VulkanContext::readFile(const std::string& filename,
                               std::vector<char>& contents) const {
    contents.clear();
    std::ifstream file(filename, std::ios::ate | std::ios::binary);

    if (!file.is_open()) {
        return Result::failure("Failed to open file: " + filename);
    }

    const std::streampos endPosition = file.tellg();
    if (endPosition <= 0) {
        return Result::failure("Shader file is empty or unreadable: " +
                               filename);
    }

    const size_t fileSize = static_cast<size_t>(endPosition);
    if (fileSize % sizeof(uint32_t) != 0) {
        return Result::failure(
            "Shader bytecode size is not a multiple of four: " + filename);
    }
    contents.resize(fileSize);

    file.seekg(0);
    if (!file.read(contents.data(),
                   static_cast<std::streamsize>(fileSize))) {
        contents.clear();
        return Result::failure("Failed to read file: " + filename);
    }

    return Result::success();
}

Result VulkanContext::createShaderModule(const std::vector<char>& code,
                                         VkShaderModule& shaderModule) {
    shaderModule = VK_NULL_HANDLE;
    if (code.empty() || code.size() % sizeof(uint32_t) != 0) {
        return Result::failure("Invalid shader bytecode");
    }

    VkShaderModuleCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    createInfo.codeSize = code.size();
    createInfo.pCode = reinterpret_cast<const uint32_t*>(code.data());

    const VkResult result =
        vkCreateShaderModule(device, &createInfo, nullptr, &shaderModule);
    if (result != VK_SUCCESS) {
        shaderModule = VK_NULL_HANDLE;
        return vkFailure("vkCreateShaderModule", result);
    }
    spdlog::info("Successfully created shader module");
    return Result::success();
}

Result VulkanContext::createCommandPool() {

    QueueFamilyIndices queueFamilyIndices;
    Result result =
        findQueueFamilies(physicalDevice, queueFamilyIndices);
    if (!result) {
        return result;
    }
    if (!queueFamilyIndices.graphicsFamily.has_value()) {
        return Result::failure("Graphics queue family is unavailable");
    }

    VkCommandPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    poolInfo.queueFamilyIndex = queueFamilyIndices.graphicsFamily.value();

    const VkResult createResult =
        vkCreateCommandPool(device, &poolInfo, nullptr, &commandPool);
    if (createResult != VK_SUCCESS) {
        commandPool = VK_NULL_HANDLE;
        return vkFailure("vkCreateCommandPool", createResult);
    }
    spdlog::info("Successfully created command pool");
    return Result::success();
}

Result VulkanContext::createColorResources() {
    VkFormat colorFormat = swapchainImageFormat;

    Result result = createImage(
        swapchainExtent.width, swapchainExtent.height, 1, msaaSamples,
        colorFormat, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT |
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, colorImage, colorImageMemory);
    if (!result) {
        return result;
    }
    result = createImageView(colorImage, colorFormat,
                             VK_IMAGE_ASPECT_COLOR_BIT, 1,
                             colorImageView);
    if (!result) {
        return result;
    }
    spdlog::info("Successfully created color resources");
    return Result::success();
}

Result VulkanContext::createImage(
    uint32_t width, uint32_t height, uint32_t mipLevels,
    VkSampleCountFlagBits numSamples, VkFormat format,
    VkImageTiling tiling, VkImageUsageFlags usage,
    VkMemoryPropertyFlags properties, VkImage& image,
    VkDeviceMemory& imageMemory, OwnedImageAllocation* ownership) {
    image = VK_NULL_HANDLE;
    imageMemory = VK_NULL_HANDLE;
    ScopedImageAllocation allocation(device);

    VkImageCreateInfo imageInfo{};
    imageInfo.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    imageInfo.imageType = VK_IMAGE_TYPE_2D;
    imageInfo.extent.width = width;
    imageInfo.extent.height = height;
    imageInfo.extent.depth = 1;
    imageInfo.mipLevels = mipLevels;
    imageInfo.arrayLayers = 1;
    imageInfo.format = format;
    imageInfo.tiling = tiling;
    imageInfo.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    imageInfo.usage = usage;
    imageInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    imageInfo.samples = numSamples;
    imageInfo.flags = 0;

    VkResult result =
        vkCreateImage(device, &imageInfo, nullptr, &allocation.image);
    if (result != VK_SUCCESS) {
        allocation.image = VK_NULL_HANDLE;
        return vkFailure("vkCreateImage", result);
    }

    VkMemoryRequirements memRequirements{};
    vkGetImageMemoryRequirements(device, allocation.image, &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    Result memoryTypeResult = findMemoryType(
        memRequirements.memoryTypeBits, properties,
        allocInfo.memoryTypeIndex);
    if (!memoryTypeResult) {
        return memoryTypeResult;
    }

    result = vkAllocateMemory(device, &allocInfo, nullptr,
                              &allocation.memory);
    if (result != VK_SUCCESS) {
        allocation.memory = VK_NULL_HANDLE;
        return vkFailure("vkAllocateMemory(image)", result);
    }

    result = vkBindImageMemory(device, allocation.image,
                               allocation.memory, 0);
    if (result != VK_SUCCESS) {
        return vkFailure("vkBindImageMemory", result);
    }

    Result success = Result::success();
    image = allocation.image;
    imageMemory = allocation.memory;
    if (ownership) {
        ownership->image = allocation.image;
        ownership->memory = allocation.memory;
    }
    allocation.release();
    return success;
}

Result VulkanContext::findMemoryType(
    uint32_t typeFilter, VkMemoryPropertyFlags properties,
    uint32_t& memoryTypeIndex) const {
    memoryTypeIndex = 0;
    VkPhysicalDeviceMemoryProperties memProperties{};
    vkGetPhysicalDeviceMemoryProperties(physicalDevice, &memProperties);

    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) &&
            (memProperties.memoryTypes[i].propertyFlags & properties) ==
                properties) {
            memoryTypeIndex = i;
            return Result::success();
        }
    }

    return Result::failure("Failed to find a suitable memory type");
}

Result VulkanContext::createDepthResources() {
    VkFormat depthFormat = VK_FORMAT_UNDEFINED;
    Result result = findDepthFormat(depthFormat);
    if (!result) {
        return result;
    }
    result = createImage(
        swapchainExtent.width, swapchainExtent.height, 1, msaaSamples,
        depthFormat, VK_IMAGE_TILING_OPTIMAL,
        VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
        VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, depthImage, depthImageMemory);
    if (!result) {
        return result;
    }
    result = createImageView(depthImage, depthFormat,
                             VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                             depthImageView);
    if (!result) {
        return result;
    }
    spdlog::info("Successfully created depth resources");

    return transitionImageLayout(
        depthImage, depthFormat, VK_IMAGE_LAYOUT_UNDEFINED,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, 1);
}

Result VulkanContext::transitionImageLayout(
    VkImage image, VkFormat format, VkImageLayout oldLayout,
    VkImageLayout newLayout, uint32_t mipLevels) {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    Result result = beginSingleTimeCommands(commandBuffer);
    if (!result) {
        return result;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = mipLevels;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.srcAccessMask = 0;
    barrier.dstAccessMask = 0;

    VkPipelineStageFlags sourceStage;
    VkPipelineStageFlags destinationStage;

    if (newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;

        if (hasStencilComponent(format)) {
            barrier.subresourceRange.aspectMask |= VK_IMAGE_ASPECT_STENCIL_BIT;
        }
    } else {
        barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    }

    if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
        newLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL &&
               newLayout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        sourceStage = VK_PIPELINE_STAGE_TRANSFER_BIT;
        destinationStage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    } else if (oldLayout == VK_IMAGE_LAYOUT_UNDEFINED &&
               newLayout == VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = 0;
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT |
                                VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;

        sourceStage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
        destinationStage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        return Result::failure("Unsupported image layout transition");
    }

    vkCmdPipelineBarrier(commandBuffer, sourceStage, destinationStage, 0, 0,
                         nullptr, 0, nullptr, 1, &barrier);

    return endSingleTimeCommands(commandBuffer);
}

bool VulkanContext::hasStencilComponent(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT ||
           format == VK_FORMAT_D24_UNORM_S8_UINT;
}

Result VulkanContext::beginSingleTimeCommands(
    VkCommandBuffer& commandBuffer) {
    commandBuffer = VK_NULL_HANDLE;
    if (singleTimeSubmissionMayBePending) {
        return Result::failure(
            "A previous single-use Vulkan submission may still be pending");
    }

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandPool = commandPool;
    allocInfo.commandBufferCount = 1;

    VkResult result =
        vkAllocateCommandBuffers(device, &allocInfo, &commandBuffer);
    if (result != VK_SUCCESS) {
        commandBuffer = VK_NULL_HANDLE;
        return vkFailure("vkAllocateCommandBuffers(single-use)", result);
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;

    result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        commandBuffer = VK_NULL_HANDLE;
        return vkFailure("vkBeginCommandBuffer(single-use)", result);
    }
    return Result::success();
}

Result VulkanContext::endSingleTimeCommands(
    VkCommandBuffer commandBuffer) {
    VkResult result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        return vkFailure("vkEndCommandBuffer(single-use)", result);
    }

    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffer;

    result =
        vkQueueSubmit(graphicsQueue, 1, &submitInfo, VK_NULL_HANDLE);
    if (result != VK_SUCCESS) {
        if (result == VK_ERROR_DEVICE_LOST) {
            hasSubmittedWork = true;
            singleTimeSubmissionMayBePending = true;
        } else {
            vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
        }
        return vkFailure("vkQueueSubmit(single-use)", result);
    }

    hasSubmittedWork = true;
    singleTimeSubmissionMayBePending = true;
    result = vkQueueWaitIdle(graphicsQueue);
    if (!waitEstablishedCompletion(result)) {
        return vkFailure("vkQueueWaitIdle(single-use)", result);
    }

    singleTimeSubmissionMayBePending = false;
    vkFreeCommandBuffers(device, commandPool, 1, &commandBuffer);
    if (result != VK_SUCCESS) {
        return vkFailure("vkQueueWaitIdle(single-use)", result);
    }
    return Result::success();
}

Result VulkanContext::createFramebuffers() {
    swapchainFramebuffers.resize(swapchainImageViews.size());

    for (size_t i = 0; i < swapchainImageViews.size(); i++) {
        std::array<VkImageView, 3> attachments = {
            colorImageView, depthImageView, swapchainImageViews[i]};

        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = renderPass;
        framebufferInfo.attachmentCount =
            static_cast<uint32_t>(attachments.size());
        framebufferInfo.pAttachments = attachments.data();
        framebufferInfo.width = swapchainExtent.width;
        framebufferInfo.height = swapchainExtent.height;
        framebufferInfo.layers = 1;

        const VkResult result = vkCreateFramebuffer(
            device, &framebufferInfo, nullptr, &swapchainFramebuffers[i]);
        if (result != VK_SUCCESS) {
            swapchainFramebuffers[i] = VK_NULL_HANDLE;
            return addContext(
                "Failed to create framebuffer " + std::to_string(i),
                vkFailure("vkCreateFramebuffer", result));
        }
    }
    spdlog::info("Successfully created framebuffers");
    return Result::success();
}

Result VulkanContext::createTextureImages(
    const std::unique_ptr<GameObject>& gameObject) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!gameObject) {
        return Result::failure("Cannot create textures for a null game object");
    }

    for (auto& instance : gameObject->meshInstances_) {

        VkDeviceSize imageSize = instance.material.texWidth *
                                instance.material.texHeight * 4;

        ownedTemporaryBuffers.emplace_back();
        auto& deferredStaging = ownedTemporaryBuffers.back();
        ScopedBufferAllocation staging(
            device, &deferredStaging.buffer, &deferredStaging.memory,
            &singleTimeSubmissionMayBePending);

        Result result = createBuffer(
            imageSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging.buffer, staging.memory);
        if (!result) {
            return addContext("Failed to create texture staging buffer",
                              result);
        }


        void* data = nullptr;
        VkResult mapResult = vkMapMemory(
            device, staging.memory, 0, imageSize, 0, &data);
        if (mapResult != VK_SUCCESS) {
            return vkFailure("vkMapMemory(texture staging)", mapResult);
        }
        memcpy(data, instance.material.pixels, static_cast<size_t>(imageSize));
        vkUnmapMemory(device, staging.memory);
        stbi_image_free(instance.material.pixels);
        instance.material.pixels = nullptr;

        if (instance.material.textureImage != VK_NULL_HANDLE ||
            instance.material.textureImageMemory != VK_NULL_HANDLE) {
            return Result::failure(
                "Texture image slots already contain Vulkan resources");
        }
        const size_t ownershipIndex = ownedSceneImages.size();
        ownedSceneImages.push_back(
            {&instance.material.textureImage,
             &instance.material.textureImageMemory});

        result = createImage(
            instance.material.texWidth, instance.material.texHeight,
            instance.material.mipLevels, VK_SAMPLE_COUNT_1_BIT,
            VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
            instance.material.textureImage,
            instance.material.textureImageMemory,
            &ownedSceneImages[ownershipIndex]);
        if (!result) {
            return addContext("Failed to create texture image", result);
        }

        result = transitionImageLayout(
            instance.material.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_LAYOUT_UNDEFINED,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            instance.material.mipLevels);
        if (!result) {
            return result;
        }
        result = copyBufferToImage(
            staging.buffer, instance.material.textureImage,
            static_cast<uint32_t>(instance.material.texWidth),
            static_cast<uint32_t>(instance.material.texHeight));
        if (!result) {
            return result;
        }

        result = generateMipmaps(
            instance.material.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
            instance.material.texWidth, instance.material.texHeight,
            instance.material.mipLevels);
        if (!result) {
            return result;
        }

    }
    return Result::success();
}

Result VulkanContext::createTextureImageViews(
    const std::unique_ptr<GameObject>& gameObject) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!gameObject) {
        return Result::failure(
            "Cannot create texture image views for a null game object");
    }
    for (auto& instance : gameObject->meshInstances_) {
        if (instance.material.textureImageView != VK_NULL_HANDLE) {
            return Result::failure(
                "Texture image-view slot already contains a Vulkan resource");
        }
        const size_t ownershipIndex = ownedSceneImageViews.size();
        ownedSceneImageViews.push_back(
            {&instance.material.textureImageView});

        Result result = createImageView(
            instance.material.textureImage, VK_FORMAT_R8G8B8A8_SRGB,
            VK_IMAGE_ASPECT_COLOR_BIT, instance.material.mipLevels,
            instance.material.textureImageView,
            &ownedSceneImageViews[ownershipIndex]);
        if (!result) {
            return addContext("Failed to create texture image view", result);
        }
    }
    return Result::success();
}

Result VulkanContext::createTextureSamplers(
    const std::unique_ptr<GameObject>& gameObject) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!gameObject) {
        return Result::failure(
            "Cannot create texture samplers for a null game object");
    }

    for (auto& instance : gameObject->meshInstances_) {
        if (instance.material.textureSampler != VK_NULL_HANDLE) {
            return Result::failure(
                "Texture sampler slot already contains a Vulkan resource");
        }
        const size_t ownershipIndex = ownedSceneSamplers.size();
        ownedSceneSamplers.push_back(
            {&instance.material.textureSampler});

        VkSamplerCreateInfo samplerInfo{};
        samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
        samplerInfo.magFilter = VK_FILTER_LINEAR;
        samplerInfo.minFilter = VK_FILTER_LINEAR;
        samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        samplerInfo.anisotropyEnable = VK_TRUE;

        VkPhysicalDeviceProperties properties{};
        vkGetPhysicalDeviceProperties(physicalDevice, &properties);

        samplerInfo.maxAnisotropy = properties.limits.maxSamplerAnisotropy;
        samplerInfo.borderColor = VK_BORDER_COLOR_INT_OPAQUE_BLACK;
        samplerInfo.unnormalizedCoordinates = VK_FALSE;
        samplerInfo.compareEnable = VK_FALSE;
        samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
        samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        samplerInfo.mipLodBias = 0.0f;
        samplerInfo.minLod = 0.0f;
        samplerInfo.maxLod = static_cast<float>(instance.material.mipLevels / 2);

        const VkResult result = vkCreateSampler(
            device, &samplerInfo, nullptr,
            &instance.material.textureSampler);
        if (result != VK_SUCCESS) {
            instance.material.textureSampler = VK_NULL_HANDLE;
            return vkFailure("vkCreateSampler", result);
        }
        ownedSceneSamplers[ownershipIndex].sampler =
            instance.material.textureSampler;

    }
    return Result::success();
}

Result VulkanContext::createVertexBuffers(
    const std::unique_ptr<GameObject>& gameObject) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!gameObject) {
        return Result::failure(
            "Cannot create vertex buffers for a null game object");
    }

    for (auto& instance : gameObject->meshInstances_) {
        VkDeviceSize bufferSize = sizeof(instance.mesh.vertices[0]) * instance.mesh.vertices.size();
        ownedTemporaryBuffers.emplace_back();
        auto& deferredStaging = ownedTemporaryBuffers.back();
        ScopedBufferAllocation staging(
            device, &deferredStaging.buffer, &deferredStaging.memory,
            &singleTimeSubmissionMayBePending);
        Result result = createBuffer(
            bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging.buffer, staging.memory);
        if (!result) {
            return addContext("Failed to create vertex staging buffer",
                              result);
        }

        void* data = nullptr;
        VkResult mapResult = vkMapMemory(
            device, staging.memory, 0, bufferSize, 0, &data);
        if (mapResult != VK_SUCCESS) {
            return vkFailure("vkMapMemory(vertex staging)", mapResult);
        }
        memcpy(data, instance.mesh.vertices.data(), (size_t)bufferSize);
        vkUnmapMemory(device, staging.memory);

        if (instance.mesh.vertexBuffer != VK_NULL_HANDLE ||
            instance.mesh.vertexBufferMemory != VK_NULL_HANDLE) {
            return Result::failure(
                "Vertex-buffer slots already contain Vulkan resources");
        }
        const size_t ownershipIndex = ownedSceneBuffers.size();
        ownedSceneBuffers.push_back(
            {&instance.mesh.vertexBuffer,
             &instance.mesh.vertexBufferMemory});
        result = createBuffer(
            bufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, instance.mesh.vertexBuffer,
            instance.mesh.vertexBufferMemory,
            &ownedSceneBuffers[ownershipIndex]);
        if (!result) {
            return addContext("Failed to create vertex buffer", result);
        }

        result =
            copyBuffer(staging.buffer, instance.mesh.vertexBuffer, bufferSize);
        if (!result) {
            return addContext("Failed to upload vertex buffer", result);
        }
    }
    return Result::success();
}

Result VulkanContext::copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer,
                                 VkDeviceSize size) {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    Result result = beginSingleTimeCommands(commandBuffer);
    if (!result) {
        return result;
    }

    VkBufferCopy copyRegion{};
    copyRegion.size = size;
    vkCmdCopyBuffer(commandBuffer, srcBuffer, dstBuffer, 1, &copyRegion);
    return endSingleTimeCommands(commandBuffer);
}

Result VulkanContext::createIndexBuffers(
    const std::unique_ptr<GameObject>& gameObject) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!gameObject) {
        return Result::failure(
            "Cannot create index buffers for a null game object");
    }

    for (auto& instance : gameObject->meshInstances_) {
        VkDeviceSize bufferSize = sizeof(instance.mesh.indices[0]) * instance.mesh.indices.size();

        ownedTemporaryBuffers.emplace_back();
        auto& deferredStaging = ownedTemporaryBuffers.back();
        ScopedBufferAllocation staging(
            device, &deferredStaging.buffer, &deferredStaging.memory,
            &singleTimeSubmissionMayBePending);

        Result result = createBuffer(
            bufferSize, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            staging.buffer, staging.memory);
        if (!result) {
            return addContext("Failed to create index staging buffer",
                              result);
        }

        void* data = nullptr;
        VkResult mapResult = vkMapMemory(
            device, staging.memory, 0, bufferSize, 0, &data);
        if (mapResult != VK_SUCCESS) {
            return vkFailure("vkMapMemory(index staging)", mapResult);
        }
        memcpy(data, instance.mesh.indices.data(), (size_t)bufferSize);
        vkUnmapMemory(device, staging.memory);

        if (instance.mesh.indexBuffer != VK_NULL_HANDLE ||
            instance.mesh.indexBufferMemory != VK_NULL_HANDLE) {
            return Result::failure(
                "Index-buffer slots already contain Vulkan resources");
        }
        const size_t ownershipIndex = ownedSceneBuffers.size();
        ownedSceneBuffers.push_back(
            {&instance.mesh.indexBuffer,
             &instance.mesh.indexBufferMemory});
        result = createBuffer(
            bufferSize,
            VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, instance.mesh.indexBuffer,
            instance.mesh.indexBufferMemory,
            &ownedSceneBuffers[ownershipIndex]);
        if (!result) {
            return addContext("Failed to create index buffer", result);
        }

        result =
            copyBuffer(staging.buffer, instance.mesh.indexBuffer, bufferSize);
        if (!result) {
            return addContext("Failed to upload index buffer", result);
        }
    }

    return Result::success();
}

Result VulkanContext::createUniformBuffers(
    const std::unique_ptr<GameObject>& gameObject) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!gameObject) {
        return Result::failure(
            "Cannot create uniform buffers for a null game object");
    }

    for (auto& instance : gameObject->meshInstances_) {
        VkDeviceSize bufferSize = sizeof(UniformBufferObject);

        if (!instance.renderData.uniformBuffers.empty() ||
            !instance.renderData.uniformBuffersMemory.empty() ||
            !instance.renderData.uniformBuffersMapped.empty()) {
            return Result::failure(
                "Uniform-buffer render data is already initialized");
        }
        ownedRenderData.push_back(&instance.renderData);

        instance.renderData.uniformBuffers.resize(MAX_FRAMES_IN_FLIGHT);
        instance.renderData.uniformBuffersMemory.resize(MAX_FRAMES_IN_FLIGHT);
        instance.renderData.uniformBuffersMapped.resize(MAX_FRAMES_IN_FLIGHT);

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            const size_t ownershipIndex = ownedSceneBuffers.size();
            ownedSceneBuffers.push_back(
                {&instance.renderData.uniformBuffers[i],
                 &instance.renderData.uniformBuffersMemory[i],
                 &instance.renderData.uniformBuffersMapped[i]});
            Result result = createBuffer(
                bufferSize, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                instance.renderData.uniformBuffers[i],
                instance.renderData.uniformBuffersMemory[i],
                &ownedSceneBuffers[ownershipIndex]);
            if (!result) {
                return addContext("Failed to create uniform buffer", result);
            }

            const VkResult mapResult = vkMapMemory(
                device, instance.renderData.uniformBuffersMemory[i], 0,
                bufferSize, 0,
                &instance.renderData.uniformBuffersMapped[i]);
            if (mapResult != VK_SUCCESS) {
                instance.renderData.uniformBuffersMapped[i] = nullptr;
                return vkFailure("vkMapMemory(uniform buffer)", mapResult);
            }
            ownedSceneBuffers[ownershipIndex].mappedAddress =
                instance.renderData.uniformBuffersMapped[i];
        }

    }

    return Result::success();
}

Result VulkanContext::createDescriptorPool(uint32_t numOfObjects) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }

    // Game Object uniform buffer, image sampler, and point light uniform buffer
    std::array<VkDescriptorPoolSize, 3> poolSizes{};

    // Per Game Object
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount =
        static_cast<uint32_t>(numOfObjects * MAX_FRAMES_IN_FLIGHT);

    // Texture samplers 
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount =
        static_cast<uint32_t>(numOfObjects * MAX_FRAMES_IN_FLIGHT);

    // Point light UB
    poolSizes[2].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    // Will need to be the number of lights
    poolSizes[2].descriptorCount = 2;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    // +1 for point light ub
    poolInfo.maxSets = static_cast<uint32_t>(numOfObjects * MAX_FRAMES_IN_FLIGHT) + 2;

    const VkResult result = vkCreateDescriptorPool(
        device, &poolInfo, nullptr, &descriptorPool);
    if (result != VK_SUCCESS) {
        descriptorPool = VK_NULL_HANDLE;
        return vkFailure("vkCreateDescriptorPool", result);
    }
    spdlog::info("Successfully created descriptor pool");
    return Result::success();
}

Result VulkanContext::createDescriptorSets(
    const std::unique_ptr<GameObject>& gameObject) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!gameObject) {
        return Result::failure(
            "Cannot create descriptor sets for a null game object");
    }

    for (auto& instance : gameObject->meshInstances_) {

        std::vector<VkDescriptorSetLayout> layouts(MAX_FRAMES_IN_FLIGHT,
                                                descriptorSetLayout);
        VkDescriptorSetAllocateInfo allocInfo{};
        allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        allocInfo.descriptorPool = descriptorPool;
        allocInfo.descriptorSetCount = static_cast<uint32_t>(MAX_FRAMES_IN_FLIGHT);
        allocInfo.pSetLayouts = layouts.data();

        if (!instance.renderData.descriptorSets.empty()) {
            return Result::failure(
                "Descriptor-set render data is already initialized");
        }
        if (std::find(ownedRenderData.begin(), ownedRenderData.end(),
                      &instance.renderData) == ownedRenderData.end()) {
            ownedRenderData.push_back(&instance.renderData);
        }
        const size_t ownershipIndex = ownedSceneDescriptorSets.size();
        ownedSceneDescriptorSets.push_back(
            {&instance.renderData.descriptorSets});
        instance.renderData.descriptorSets.resize(MAX_FRAMES_IN_FLIGHT);
        const VkResult result = vkAllocateDescriptorSets(
            device, &allocInfo,
            instance.renderData.descriptorSets.data());
        if (result != VK_SUCCESS) {
            instance.renderData.descriptorSets.clear();
            return vkFailure("vkAllocateDescriptorSets", result);
        }
        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
            ownedSceneDescriptorSets[ownershipIndex].sets[i] =
                instance.renderData.descriptorSets[i];
        }

        for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
            VkDescriptorBufferInfo bufferInfo{};
            bufferInfo.buffer = instance.renderData.uniformBuffers[i];
            bufferInfo.offset = 0;
            bufferInfo.range = sizeof(UniformBufferObject);

            VkDescriptorImageInfo imageInfo{};
            imageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            imageInfo.imageView = instance.material.textureImageView;
            imageInfo.sampler = instance.material.textureSampler;

            std::array<VkWriteDescriptorSet, 2> descriptorWrites{};

            descriptorWrites[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[0].dstSet = instance.renderData.descriptorSets[i];
            descriptorWrites[0].dstBinding = 0;
            descriptorWrites[0].dstArrayElement = 0;
            descriptorWrites[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            descriptorWrites[0].descriptorCount = 1;
            descriptorWrites[0].pBufferInfo = &bufferInfo;

            descriptorWrites[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[1].dstSet = instance.renderData.descriptorSets[i];
            descriptorWrites[1].dstBinding = 1;
            descriptorWrites[1].dstArrayElement = 0;
            descriptorWrites[1].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[1].descriptorCount = 1;
            descriptorWrites[1].pImageInfo = &imageInfo;

            vkUpdateDescriptorSets(device,
                                static_cast<uint32_t>(descriptorWrites.size()),
                                descriptorWrites.data(), 0, nullptr);
        }

    }

    return Result::success();
}

Result VulkanContext::createBuffer(VkDeviceSize size,
                                   VkBufferUsageFlags usage,
                                   VkMemoryPropertyFlags properties,
                                   VkBuffer& buffer,
                                   VkDeviceMemory& bufferMemory,
                                   OwnedBufferAllocation* ownership) {
    buffer = VK_NULL_HANDLE;
    bufferMemory = VK_NULL_HANDLE;
    ScopedBufferAllocation allocation(device);

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VkResult result = vkCreateBuffer(
        device, &bufferInfo, nullptr, &allocation.buffer);
    if (result != VK_SUCCESS) {
        allocation.buffer = VK_NULL_HANDLE;
        return vkFailure("vkCreateBuffer", result);
    }

    VkMemoryRequirements memRequirements{};
    vkGetBufferMemoryRequirements(device, allocation.buffer,
                                  &memRequirements);

    VkMemoryAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    allocInfo.allocationSize = memRequirements.size;
    Result memoryTypeResult = findMemoryType(
        memRequirements.memoryTypeBits, properties,
        allocInfo.memoryTypeIndex);
    if (!memoryTypeResult) {
        return memoryTypeResult;
    }

    result = vkAllocateMemory(device, &allocInfo, nullptr,
                              &allocation.memory);
    if (result != VK_SUCCESS) {
        allocation.memory = VK_NULL_HANDLE;
        return vkFailure("vkAllocateMemory(buffer)", result);
    }

    result = vkBindBufferMemory(device, allocation.buffer,
                                allocation.memory, 0);
    if (result != VK_SUCCESS) {
        return vkFailure("vkBindBufferMemory", result);
    }

    Result success = Result::success();
    buffer = allocation.buffer;
    bufferMemory = allocation.memory;
    if (ownership) {
        ownership->buffer = allocation.buffer;
        ownership->memory = allocation.memory;
    }
    allocation.release();
    return success;
}

Result VulkanContext::copyBufferToImage(VkBuffer buffer, VkImage image,
                                        uint32_t width, uint32_t height) {
    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    Result result = beginSingleTimeCommands(commandBuffer);
    if (!result) {
        return result;
    }

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    region.imageSubresource.mipLevel = 0;
    region.imageSubresource.baseArrayLayer = 0;
    region.imageSubresource.layerCount = 1;
    region.imageOffset = {0, 0, 0};
    region.imageExtent = {width, height, 1};

    vkCmdCopyBufferToImage(commandBuffer, buffer, image,
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);

    return endSingleTimeCommands(commandBuffer);
}

Result VulkanContext::generateMipmaps(VkImage image, VkFormat imageFormat,
                                      int32_t texWidth, int32_t texHeight,
                                      uint32_t mipLevels) {
    VkFormatProperties formatProperties{};
    vkGetPhysicalDeviceFormatProperties(physicalDevice, imageFormat,
                                        &formatProperties);

    if (!(formatProperties.optimalTilingFeatures &
          VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)) {
        return Result::failure(
            "Texture image format does not support linear blitting");
    }

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    Result result = beginSingleTimeCommands(commandBuffer);
    if (!result) {
        return result;
    }

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.image = image;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;
    barrier.subresourceRange.levelCount = 1;

    int32_t mipWidth = texWidth;
    int32_t mipHeight = texHeight;

    for (uint32_t i = 1; i < mipLevels; i++) {
        barrier.subresourceRange.baseMipLevel = i - 1;
        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
                             nullptr, 1, &barrier);

        VkImageBlit blit{};
        blit.srcOffsets[0] = {0, 0, 0};
        blit.srcOffsets[1] = {mipWidth, mipHeight, 1};
        blit.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.srcSubresource.mipLevel = i - 1;
        blit.srcSubresource.baseArrayLayer = 0;
        blit.srcSubresource.layerCount = 1;
        blit.dstOffsets[0] = {0, 0, 0};
        blit.dstOffsets[1] = {mipWidth > 1 ? mipWidth / 2 : 1,
                              mipHeight > 1 ? mipHeight / 2 : 1, 1};
        blit.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        blit.dstSubresource.mipLevel = i;
        blit.dstSubresource.baseArrayLayer = 0;
        blit.dstSubresource.layerCount = 1;

        vkCmdBlitImage(
            commandBuffer, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image,
            VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);

        barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

        vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &barrier);

        if (mipWidth > 1) mipWidth /= 2;
        if (mipHeight > 1) mipHeight /= 2;
    }

    barrier.subresourceRange.baseMipLevel = mipLevels - 1;
    barrier.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    vkCmdPipelineBarrier(commandBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &barrier);

    return endSingleTimeCommands(commandBuffer);
}

Result VulkanContext::createCommandBuffers() {
    commandBuffers.resize(MAX_FRAMES_IN_FLIGHT);

    VkCommandBufferAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    allocInfo.commandPool = commandPool;
    allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    allocInfo.commandBufferCount = (uint32_t)commandBuffers.size();

    const VkResult result =
        vkAllocateCommandBuffers(device, &allocInfo, commandBuffers.data());
    if (result != VK_SUCCESS) {
        commandBuffers.clear();
        return vkFailure("vkAllocateCommandBuffers", result);
    }
    spdlog::info("Successfully created command buffers");
    return Result::success();
}

Result VulkanContext::createSyncObjects() {
    imageAvailableSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    renderFinishedSemaphores.resize(MAX_FRAMES_IN_FLIGHT);
    inFlightFences.resize(MAX_FRAMES_IN_FLIGHT);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

    VkFenceCreateInfo fenceInfo{};
    fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; i++) {
        VkResult result = vkCreateSemaphore(
            device, &semaphoreInfo, nullptr,
            &imageAvailableSemaphores[i]);
        if (result != VK_SUCCESS) {
            imageAvailableSemaphores[i] = VK_NULL_HANDLE;
            return addContext(
                "Failed to create image-available semaphore for frame " +
                    std::to_string(i),
                vkFailure("vkCreateSemaphore", result));
        }

        result = vkCreateSemaphore(device, &semaphoreInfo, nullptr,
                                   &renderFinishedSemaphores[i]);
        if (result != VK_SUCCESS) {
            renderFinishedSemaphores[i] = VK_NULL_HANDLE;
            return addContext(
                "Failed to create render-finished semaphore for frame " +
                    std::to_string(i),
                vkFailure("vkCreateSemaphore", result));
        }

        result = vkCreateFence(device, &fenceInfo, nullptr,
                               &inFlightFences[i]);
        if (result != VK_SUCCESS) {
            inFlightFences[i] = VK_NULL_HANDLE;
            return addContext(
                "Failed to create in-flight fence for frame " +
                    std::to_string(i),
                vkFailure("vkCreateFence", result));
        }
    }
    spdlog::info("Successfully created synchronization objects");
    return Result::success();
}

void VulkanContext::cleanupSwapchain() noexcept {
    if (device == VK_NULL_HANDLE) {
        return;
    }

    for (auto framebuffer : swapchainFramebuffers) {
        if (framebuffer != VK_NULL_HANDLE) {
            vkDestroyFramebuffer(device, framebuffer, nullptr);
        }
    }
    swapchainFramebuffers.clear();

    if (depthImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, depthImageView, nullptr);
        depthImageView = VK_NULL_HANDLE;
    }
    if (depthImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, depthImage, nullptr);
        depthImage = VK_NULL_HANDLE;
    }
    if (depthImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, depthImageMemory, nullptr);
        depthImageMemory = VK_NULL_HANDLE;
    }

    if (colorImageView != VK_NULL_HANDLE) {
        vkDestroyImageView(device, colorImageView, nullptr);
        colorImageView = VK_NULL_HANDLE;
    }
    if (colorImage != VK_NULL_HANDLE) {
        vkDestroyImage(device, colorImage, nullptr);
        colorImage = VK_NULL_HANDLE;
    }
    if (colorImageMemory != VK_NULL_HANDLE) {
        vkFreeMemory(device, colorImageMemory, nullptr);
        colorImageMemory = VK_NULL_HANDLE;
    }

    for (auto imageView : swapchainImageViews) {
        if (imageView != VK_NULL_HANDLE) {
            vkDestroyImageView(device, imageView, nullptr);
        }
    }
    swapchainImageViews.clear();

    if (swapchain != VK_NULL_HANDLE) {
        vkDestroySwapchainKHR(device, swapchain, nullptr);
        swapchain = VK_NULL_HANDLE;
    }
    swapchainImages.clear();
    swapchainImageFormat = VK_FORMAT_UNDEFINED;
    swapchainExtent = {};
}

void VulkanContext::updateUniformBuffer(
    uint32_t currentImage,
    const std::unique_ptr<GameObject>& gameObject, glm::vec3 camPos,
    glm::vec3 camFront, glm::vec3 camUp) {
    for (auto& instance : gameObject->meshInstances_) {
        UniformBufferObject ubo{};

        ubo.model = glm::mat4(1.0f);

        ubo.model = glm::translate(ubo.model, gameObject->position);

        ubo.model = glm::rotate(ubo.model, glm::radians(gameObject->rotation.x),
                            glm::vec3(1.0f, 0.0f, 0.0f));
        ubo.model = glm::rotate(ubo.model, glm::radians(gameObject->rotation.y),
                            glm::vec3(0.0f, 1.0f, 0.0f));
        ubo.model = glm::rotate(ubo.model, glm::radians(gameObject->rotation.z),
                            glm::vec3(0.0f, 0.0f, 1.0f));
        
        ubo.model = glm::scale(ubo.model, gameObject->scale);

        ubo.view =
            glm::lookAt(camPos, camPos + camFront, camUp);

        ubo.proj = glm::perspective(
            glm::radians(45.0f),
            swapchainExtent.width / (float)swapchainExtent.height, 0.1f, 10000.0f);

        ubo.proj[1][1] *= -1;

        ubo.cameraPosition = camFront;
        memcpy(instance.renderData.uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));

    }
}

Result VulkanContext::drawFrame(Scene* scene) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!scene) {
        return Result::failure("Cannot draw a null scene");
    }
    if (scene != currentScene) {
        return Result::failure(
            "Cannot draw a scene that was not used to initialize this "
            "Vulkan Context");
    }
    if (!scene->isActive()) {
        return Result::failure("Cannot draw an inactive scene");
    }
    const Camera* camera = scene->activeCamera();
    if (!camera) {
        return Result::failure(
            "Cannot draw a scene without an active camera");
    }
    if (scene->pointLightCount() > scene_limits::maxPointLights) {
        return Result::failure(
            "Cannot draw a scene that exceeds the point-light limit");
    }

    VkResult result = vkWaitForFences(
        device, 1, &inFlightFences[currentFrame], VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        return vkFailure("vkWaitForFences", result);
    }

    uint32_t imageIndex = 0;
    result = vkAcquireNextImageKHR(
        device, swapchain, UINT64_MAX, imageAvailableSemaphores[currentFrame],
        VK_NULL_HANDLE, &imageIndex);

    // If our window has been resized, we need to recreate the swap chain
    if (result == VK_ERROR_OUT_OF_DATE_KHR) {
        spdlog::warn("Swapchain out of date... Recreating swapchain...");
        return recreateSwapchain();
    } else if (result != VK_SUCCESS && result != VK_SUBOPTIMAL_KHR) {
        return vkFailure("vkAcquireNextImageKHR", result);
    }

    result = vkResetFences(device, 1, &inFlightFences[currentFrame]);
    if (result != VK_SUCCESS) {
        return vkFailure("vkResetFences", result);
    }

    result = vkResetCommandBuffer(commandBuffers[currentFrame], 0);
    if (result != VK_SUCCESS) {
        return vkFailure("vkResetCommandBuffer", result);
    }
    Result recordResult = recordCommandBuffer(
        commandBuffers[currentFrame], imageIndex, scene);
    if (!recordResult) {
        return recordResult;
    }


    for (const auto& obj : scene->gameObjects()) {
        updateUniformBuffer(currentFrame, obj, camera->position,
                            camera->front, camera->up);
    }


    VkSubmitInfo submitInfo{};
    submitInfo.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;

    VkSemaphore waitSemaphores[] = {imageAvailableSemaphores[currentFrame]};
    VkPipelineStageFlags waitStages[] = {
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT};
    submitInfo.waitSemaphoreCount = 1;
    submitInfo.pWaitSemaphores = waitSemaphores;
    submitInfo.pWaitDstStageMask = waitStages;

    submitInfo.commandBufferCount = 1;
    submitInfo.pCommandBuffers = &commandBuffers[currentFrame];

    VkSemaphore signalSemaphores[] = {renderFinishedSemaphores[currentFrame]};
    submitInfo.signalSemaphoreCount = 1;
    submitInfo.pSignalSemaphores = signalSemaphores;

    result = vkQueueSubmit(graphicsQueue, 1, &submitInfo,
                           inFlightFences[currentFrame]);
    if (result == VK_SUCCESS || result == VK_ERROR_DEVICE_LOST) {
        hasSubmittedWork = true;
    }
    if (result != VK_SUCCESS) {
        return vkFailure("vkQueueSubmit", result);
    }

    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;

    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = signalSemaphores;

    VkSwapchainKHR swapChains[] = {swapchain};
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = swapChains;

    presentInfo.pImageIndices = &imageIndex;

    result = vkQueuePresentKHR(presentQueue, &presentInfo);

    if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR ||
        framebufferResized) {
        framebufferResized = false;
        Result recreateResult = recreateSwapchain();
        if (!recreateResult) {
            return recreateResult;
        }
    } else if (result != VK_SUCCESS) {
        return vkFailure("vkQueuePresentKHR", result);
    }

    currentFrame = (currentFrame + 1) % MAX_FRAMES_IN_FLIGHT;
    return Result::success();
}

Result VulkanContext::recordCommandBuffer(VkCommandBuffer commandBuffer,
                                          uint32_t imageIndex,
                                          Scene* scene) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        return vkFailure("vkBeginCommandBuffer", result);
    }

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = swapchainFramebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent;

    // VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    std::array<VkClearValue, 2> clearValues{};
    clearValues[0].color = {{0.639f, 0.965f, 1.0f, 1.0f}};
    clearValues[1].depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    LightsUBO lightsUbo{};
    lightsUbo.numLights =
        static_cast<std::int32_t>(scene->pointLightCount());
    for (std::size_t i = 0; i < scene->pointLightCount(); ++i) {
        const PointLight& light = scene->pointLightAt(i);
        lightsUbo.lights[i].position = light.position;
        lightsUbo.lights[i].color = light.color;
        lightsUbo.lights[i].intensity = light.intensity;
    }

    void* data = nullptr;
    result = vkMapMemory(device, lightsBufferMemory, 0, sizeof(LightsUBO), 0,
                         &data);
    if (result != VK_SUCCESS) {
        return vkFailure("vkMapMemory(lights buffer)", result);
    }
    memcpy(data, &lightsUbo, sizeof(LightsUBO));
    vkUnmapMemory(device, lightsBufferMemory);

    Result descriptorResult = updateLightsDescriptorSet();
    if (!descriptorResult) {
        return descriptorResult;
    }

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      graphicsPipeline);
    
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 1, 1,
                                    &lightsDescriptorSet, 0, nullptr);

    VkViewport viewport{};
    viewport.x = 0.0f;
    viewport.y = 0.0f;
    viewport.width = (float)swapchainExtent.width;
    viewport.height = (float)swapchainExtent.height;
    viewport.minDepth = 0.0f;
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.offset = {0, 0};
    scissor.extent = swapchainExtent;
    vkCmdSetScissor(commandBuffer, 0, 1, &scissor);

    for (const auto& obj : scene->gameObjects()) {
        for (auto& instance : obj->meshInstances_) {
            VkBuffer vertexBuffers[] = {instance.mesh.vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer, instance.mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 0, 1, &instance.renderData.descriptorSets[currentFrame],
                                    0, nullptr);

            vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(instance.mesh.indices.size()), 1, 0,
                            0, 0);
        }

    }

    // Draw imgui stuff
    //drawImguiFrame(commandBuffer);

    vkCmdEndRenderPass(commandBuffer);

    result = vkEndCommandBuffer(commandBuffer);
    if (result != VK_SUCCESS) {
        return vkFailure("vkEndCommandBuffer", result);
    }
    return Result::success();
}


Result VulkanContext::recreateSwapchain() {
    int width = 0, height = 0;
    if (!SDL_GetWindowSize(window, &width, &height)) {
        return Result::failure(std::string("SDL_GetWindowSize failed: ") +
                               SDL_GetError());
    }

    while (width == 0 || height == 0) {
        if (!SDL_WaitEvent(nullptr)) {
            return Result::failure(std::string("SDL_WaitEvent failed: ") +
                                   SDL_GetError());
        }
        if (!SDL_GetWindowSize(window, &width, &height)) {
            return Result::failure(
                std::string("SDL_GetWindowSize failed: ") +
                SDL_GetError());
        }
    }

    const VkResult idleResult = vkDeviceWaitIdle(device);
    if (waitEstablishedCompletion(idleResult)) {
        hasSubmittedWork = false;
        singleTimeSubmissionMayBePending = false;
    }
    if (idleResult != VK_SUCCESS) {
        return vkFailure("vkDeviceWaitIdle", idleResult);
    }
    // Clean up the swap chain and recreate it with the image views and frame
    // buffers
    cleanupSwapchain();
    Result result = createSwapchain();
    if (!result) {
        cleanupSwapchain();
        return addContext("Failed to recreate swapchain", result);
    }
    result = createImageViews();
    if (!result) {
        cleanupSwapchain();
        return addContext("Failed to recreate swapchain image views", result);
    }
    result = createColorResources();
    if (!result) {
        cleanupSwapchain();
        return addContext("Failed to recreate color resources", result);
    }
    result = createDepthResources();
    if (!result) {
        cleanupSwapchain();
        return addContext("Failed to recreate depth resources", result);
    }
    result = createFramebuffers();
    if (!result) {
        cleanupSwapchain();
        return addContext("Failed to recreate framebuffers", result);
    }
    return Result::success();
}

void VulkanContext::cleanupTrackedSceneResources() noexcept {
    if (device == VK_NULL_HANDLE) {
        ownedTemporaryBuffers.clear();
        ownedSceneBuffers.clear();
        ownedSceneImages.clear();
        ownedSceneImageViews.clear();
        ownedSceneSamplers.clear();
        ownedSceneDescriptorSets.clear();
        ownedRenderData.clear();
        return;
    }

    for (auto& ownership : ownedSceneDescriptorSets) {
        if (!ownership.slots) {
            continue;
        }
        const size_t count =
            std::min(ownership.slots->size(), ownership.sets.size());
        for (size_t i = 0; i < count; ++i) {
            if ((*ownership.slots)[i] == ownership.sets[i]) {
                (*ownership.slots)[i] = VK_NULL_HANDLE;
            }
        }
    }

    for (auto& ownership : ownedSceneImageViews) {
        if (ownership.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, ownership.view, nullptr);
            if (ownership.slot && *ownership.slot == ownership.view) {
                *ownership.slot = VK_NULL_HANDLE;
            }
        }
    }

    for (auto& ownership : ownedSceneSamplers) {
        if (ownership.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, ownership.sampler, nullptr);
            if (ownership.slot && *ownership.slot == ownership.sampler) {
                *ownership.slot = VK_NULL_HANDLE;
            }
        }
    }

    for (auto& ownership : ownedSceneImages) {
        if (ownership.image != VK_NULL_HANDLE) {
            vkDestroyImage(device, ownership.image, nullptr);
            if (ownership.imageSlot &&
                *ownership.imageSlot == ownership.image) {
                *ownership.imageSlot = VK_NULL_HANDLE;
            }
        }
        if (ownership.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, ownership.memory, nullptr);
            if (ownership.memorySlot &&
                *ownership.memorySlot == ownership.memory) {
                *ownership.memorySlot = VK_NULL_HANDLE;
            }
        }
    }

    for (auto& ownership : ownedTemporaryBuffers) {
        if (ownership.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, ownership.buffer, nullptr);
        }
        if (ownership.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, ownership.memory, nullptr);
        }
    }

    for (auto& ownership : ownedSceneBuffers) {
        if (ownership.mappedAddress != nullptr &&
            ownership.memory != VK_NULL_HANDLE) {
            vkUnmapMemory(device, ownership.memory);
            if (ownership.mappedSlot &&
                *ownership.mappedSlot == ownership.mappedAddress) {
                *ownership.mappedSlot = nullptr;
            }
        }
        if (ownership.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, ownership.buffer, nullptr);
            if (ownership.bufferSlot &&
                *ownership.bufferSlot == ownership.buffer) {
                *ownership.bufferSlot = VK_NULL_HANDLE;
            }
        }
        if (ownership.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, ownership.memory, nullptr);
            if (ownership.memorySlot &&
                *ownership.memorySlot == ownership.memory) {
                *ownership.memorySlot = VK_NULL_HANDLE;
            }
        }
    }

    for (RenderData* renderData : ownedRenderData) {
        if (!renderData) {
            continue;
        }
        if (std::all_of(renderData->uniformBuffers.begin(),
                        renderData->uniformBuffers.end(),
                        [](VkBuffer buffer) {
                            return buffer == VK_NULL_HANDLE;
                        })) {
            renderData->uniformBuffers.clear();
        }
        if (std::all_of(renderData->uniformBuffersMemory.begin(),
                        renderData->uniformBuffersMemory.end(),
                        [](VkDeviceMemory memory) {
                            return memory == VK_NULL_HANDLE;
                        })) {
            renderData->uniformBuffersMemory.clear();
        }
        if (std::all_of(renderData->uniformBuffersMapped.begin(),
                        renderData->uniformBuffersMapped.end(),
                        [](const void* mapping) {
                            return mapping == nullptr;
                        })) {
            renderData->uniformBuffersMapped.clear();
        }
        if (std::all_of(renderData->descriptorSets.begin(),
                        renderData->descriptorSets.end(),
                        [](VkDescriptorSet descriptorSet) {
                            return descriptorSet == VK_NULL_HANDLE;
                        })) {
            renderData->descriptorSets.clear();
        }
    }

    ownedTemporaryBuffers.clear();
    ownedSceneBuffers.clear();
    ownedSceneImages.clear();
    ownedSceneImageViews.clear();
    ownedSceneSamplers.clear();
    ownedSceneDescriptorSets.clear();
    ownedRenderData.clear();
}

bool VulkanContext::cleanup() noexcept {
    const bool hadResources =
        instance != VK_NULL_HANDLE || device != VK_NULL_HANDLE ||
        currentScene != nullptr || !ownedTemporaryBuffers.empty() ||
        !ownedSceneBuffers.empty() ||
        !ownedSceneImages.empty() || !ownedSceneImageViews.empty() ||
        !ownedSceneSamplers.empty() ||
        !ownedSceneDescriptorSets.empty();
    if (hadResources) {
        spdlog::info("Cleaning up Vulkan Context...");
    }

    if (device != VK_NULL_HANDLE) {
        const VkResult idleResult = vkDeviceWaitIdle(device);
        if (waitEstablishedCompletion(idleResult)) {
            hasSubmittedWork = false;
            singleTimeSubmissionMayBePending = false;
        } else if (hasSubmittedWork) {
            spdlog::warn(
                "vkDeviceWaitIdle failed during Vulkan cleanup (VkResult "
                "{}); retaining device resources because submitted work may "
                "still be active",
                static_cast<int>(idleResult));
            return false;
        } else {
            spdlog::warn(
                "vkDeviceWaitIdle failed during Vulkan cleanup (VkResult {}), "
                "but no queue work was submitted",
                static_cast<int>(idleResult));
        }

        cleanupSwapchain();

        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
            lightsDescriptorSet = VK_NULL_HANDLE;
        }

        cleanupTrackedSceneResources();

        if (lightsBuffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, lightsBuffer, nullptr);
            lightsBuffer = VK_NULL_HANDLE;
        }
        if (lightsBufferMemory != VK_NULL_HANDLE) {
            vkFreeMemory(device, lightsBufferMemory, nullptr);
            lightsBufferMemory = VK_NULL_HANDLE;
        }

        if (graphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphicsPipeline, nullptr);
            graphicsPipeline = VK_NULL_HANDLE;
        }
        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
        if (descriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, descriptorSetLayout,
                                         nullptr);
            descriptorSetLayout = VK_NULL_HANDLE;
        }
        if (lightsDescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(
                device, lightsDescriptorSetLayout, nullptr);
            lightsDescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }

        for (VkSemaphore semaphore : renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, semaphore, nullptr);
            }
        }
        for (VkSemaphore semaphore : imageAvailableSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, semaphore, nullptr);
            }
        }
        for (VkFence fence : inFlightFences) {
            if (fence != VK_NULL_HANDLE) {
                vkDestroyFence(device, fence, nullptr);
            }
        }
        renderFinishedSemaphores.clear();
        imageAvailableSemaphores.clear();
        inFlightFences.clear();
        commandBuffers.clear();

        if (commandPool != VK_NULL_HANDLE) {
            vkDestroyCommandPool(device, commandPool, nullptr);
            commandPool = VK_NULL_HANDLE;
        }

        vkDestroyDevice(device, nullptr);
        device = VK_NULL_HANDLE;
    } else {
        swapchainFramebuffers.clear();
        swapchainImageViews.clear();
        swapchainImages.clear();
        commandBuffers.clear();
        renderFinishedSemaphores.clear();
        imageAvailableSemaphores.clear();
        inFlightFences.clear();
        cleanupTrackedSceneResources();
    }

    graphicsQueue = VK_NULL_HANDLE;
    presentQueue = VK_NULL_HANDLE;
    physicalDevice = VK_NULL_HANDLE;

    if (instance != VK_NULL_HANDLE) {
        if (debugMessenger != VK_NULL_HANDLE) {
            destroyDebugUtilsMessengerEXT(instance, debugMessenger, nullptr);
            debugMessenger = VK_NULL_HANDLE;
        }
        if (surface != VK_NULL_HANDLE) {
            SDL_Vulkan_DestroySurface(instance, surface, nullptr);
            surface = VK_NULL_HANDLE;
        }
        vkDestroyInstance(instance, nullptr);
        instance = VK_NULL_HANDLE;
    }

    debugMessenger = VK_NULL_HANDLE;
    surface = VK_NULL_HANDLE;
    descriptorPool = VK_NULL_HANDLE;
    lightsDescriptorSet = VK_NULL_HANDLE;
    lightsBuffer = VK_NULL_HANDLE;
    lightsBufferMemory = VK_NULL_HANDLE;
    descriptorSetLayout = VK_NULL_HANDLE;
    lightsDescriptorSetLayout = VK_NULL_HANDLE;
    graphicsPipeline = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
    commandPool = VK_NULL_HANDLE;
    colorImage = VK_NULL_HANDLE;
    colorImageView = VK_NULL_HANDLE;
    colorImageMemory = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    depthImageView = VK_NULL_HANDLE;
    depthImageMemory = VK_NULL_HANDLE;
    swapchain = VK_NULL_HANDLE;
    swapchainImageFormat = VK_FORMAT_UNDEFINED;
    swapchainExtent = {};
    msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    currentFrame = 0;
    framebufferResized = false;
    hasSubmittedWork = false;
    singleTimeSubmissionMayBePending = false;
    initialized = false;
    currentScene = nullptr;
    window = nullptr;

    if (hadResources) {
        spdlog::info("Successfully cleaned up Vulkan Context");
    }
    return true;
}
