#include "vulkan_context.h"

#include "editor_picking.h"
#include "renderer_configuration.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <exception>
#include <limits>
#include <utility>
#include "../third_party/stb/stb_image.h"

namespace {

constexpr float minDirectionalDirectionLengthSquared = 1.0e-8f;

bool isFiniteVector(const glm::vec3& vector) {
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
           std::isfinite(vector.z);
}

Result normalizeDirectionalLightDirection(const DirectionalLight& light,
                                          glm::vec3& normalizedDirection) {
    if (!isFiniteVector(light.direction)) {
        return Result::failure("Directional light direction must be finite");
    }

    const float directionLengthSquared = glm::dot(
        light.direction, light.direction);
    if (!std::isfinite(directionLengthSquared)) {
        return Result::failure(
            "Directional light direction must have finite length");
    }
    if (directionLengthSquared <= minDirectionalDirectionLengthSquared) {
        return Result::failure(
            "Directional light direction must have nonzero length");
    }

    if (!isFiniteVector(light.color) || light.color.r < 0.0f ||
        light.color.g < 0.0f || light.color.b < 0.0f) {
        return Result::failure(
            "Directional light color must be finite and nonnegative");
    }
    if (!std::isfinite(light.intensity) || light.intensity < 0.0f) {
        return Result::failure(
            "Directional light intensity must be finite and nonnegative");
    }

    normalizedDirection = light.direction /
                          std::sqrt(directionLengthSquared);
    if (!isFiniteVector(normalizedDirection)) {
        return Result::failure(
            "Directional light direction could not be normalized");
    }

    return Result::success();
}

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
    if (!cleanup()) {
        // Catastrophic cleanup intentionally retains live Vulkan resources.
        // Prevent ImGuiLayer's defensive destructor from destroying objects
        // which submitted work may still reference.
        imguiLayer.abandon();
    }
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
        if (!object->renderTopologyMutable()) {
            return Result::failure(
                "Scene object " + std::to_string(objectIndex) +
                " has render resources attached");
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
                material.textureSampler != VK_NULL_HANDLE ||
                material.normalMapImage != VK_NULL_HANDLE ||
                material.normalMapImageMemory != VK_NULL_HANDLE ||
                material.normalMapImageView != VK_NULL_HANDLE ||
                material.normalMapSampler != VK_NULL_HANDLE ||
                material.metallicRoughnessMapImage != VK_NULL_HANDLE ||
                material.metallicRoughnessMapImageMemory != VK_NULL_HANDLE ||
                material.metallicRoughnessMapImageView != VK_NULL_HANDLE ||
                material.metallicRoughnessMapSampler != VK_NULL_HANDLE;
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
    if (!ownedTemporaryBuffers.empty() || !ownedSceneBuffers.empty() ||
        !ownedSceneImages.empty() || !ownedSceneImageViews.empty() ||
        !ownedSceneSamplers.empty() || !ownedSceneDescriptorSets.empty() ||
        !ownedRenderData.empty()) {
        return Result::failure(
            "Another scene resource load is already in progress");
    }

    size_t meshInstanceCount = 0;
    for (const auto& object : scene->gameObjects()) {
        if (object) {
            meshInstanceCount += object->meshInstances().size();
        }
    }

    try {
        ownedSceneBuffers.reserve(
            meshInstanceCount * (MAX_FRAMES_IN_FLIGHT + 2));
        ownedSceneImages.reserve(meshInstanceCount * 3);
        ownedSceneImageViews.reserve(meshInstanceCount * 3);
        ownedSceneSamplers.reserve(meshInstanceCount * 3);
        ownedSceneDescriptorSets.reserve(meshInstanceCount);
        ownedRenderData.reserve(meshInstanceCount);
        ownedTemporaryBuffers.reserve(meshInstanceCount * 5);
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to reserve Vulkan scene-resource ownership records: " +
            std::string(exception.what()));
    }

    return Result::success();
}

Result VulkanContext::beginSceneResourceLoad(Scene* scene) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!scene) {
        return Result::failure("Cannot load resources for a null scene");
    }
    if (sceneResourceLoadTarget_) {
        return Result::failure(
            "Another scene resource load is already in progress");
    }
    if (hasSceneResources(scene)) {
        return Result::failure("Scene rendering resources are already loaded");
    }

    Result result = validateSceneRenderStateIsEmpty(scene);
    if (!result) {
        return result;
    }
    try {
        auto [ownership, inserted] = sceneResourceOwnership_.try_emplace(scene);
        (void)inserted;
        ownership->second.attachedGameObjects.reserve(
            scene->gameObjects().size());
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to allocate scene resource ownership: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to allocate scene resource ownership with an unknown "
            "error");
    }
    sceneResourceLoadTarget_ = scene;
    result = prepareSceneResourceTracking(scene);
    if (!result) {
        sceneResourceOwnership_.erase(scene);
        sceneResourceLoadTarget_ = nullptr;
    }
    return result;
}

Result VulkanContext::commitSceneResourceLoad(Scene* scene) {
    if (!scene) {
        return Result::failure("Cannot commit resources for a null scene");
    }
    if (sceneResourceLoadTarget_ != scene) {
        return Result::failure(
            "Scene is not the current resource-load target");
    }

    auto ownership = sceneResourceOwnership_.find(scene);
    if (ownership == sceneResourceOwnership_.end()) {
        return Result::failure("Scene resource ownership is unavailable");
    }
    SceneResourceOwnership& resources = ownership->second;
    resources.temporaryBuffers = std::move(ownedTemporaryBuffers);
    resources.buffers = std::move(ownedSceneBuffers);
    resources.images = std::move(ownedSceneImages);
    resources.imageViews = std::move(ownedSceneImageViews);
    resources.samplers = std::move(ownedSceneSamplers);
    resources.descriptorSets = std::move(ownedSceneDescriptorSets);
    resources.renderData = std::move(ownedRenderData);
    for (const auto& object : scene->gameObjects()) {
        if (!object) {
            continue;
        }
        Result result = object->markRenderResourcesAttached();
        if (!result) {
            cleanupSceneResources(resources, false);
            sceneResourceOwnership_.erase(ownership);
            sceneResourceLoadTarget_ = nullptr;
            return Result::failure(
                "Failed to attach GameObject render topology: " +
                result.error());
        }
        resources.attachedGameObjects.push_back(object.get());
    }
    sceneResourceLoadTarget_ = nullptr;
    return Result::success();
}

Result VulkanContext::cancelSceneResourceLoad() {
    if (device != VK_NULL_HANDLE) {
        const VkResult result = vkDeviceWaitIdle(device);
        if (!waitEstablishedCompletion(result)) {
            return vkFailure(
                "vkDeviceWaitIdle(cancel scene resource load)", result);
        }
        hasSubmittedWork = false;
        singleTimeSubmissionMayBePending = false;
    }
    cleanupTrackedSceneResources();
    if (sceneResourceLoadTarget_) {
        sceneResourceOwnership_.erase(sceneResourceLoadTarget_);
        sceneResourceLoadTarget_ = nullptr;
    }
    return Result::success();
}

Result VulkanContext::unloadSceneResources(Scene* scene) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!scene) {
        return Result::failure("Cannot unload resources for a null scene");
    }
    if (scene == currentScene) {
        return Result::failure(
            "Cannot unload resources for the current render scene");
    }

    const auto ownership = sceneResourceOwnership_.find(scene);
    if (ownership == sceneResourceOwnership_.end()) {
        return Result::success();
    }
    const VkResult result = vkDeviceWaitIdle(device);
    if (!waitEstablishedCompletion(result)) {
        return vkFailure("vkDeviceWaitIdle(unload scene resources)", result);
    }
    hasSubmittedWork = false;
    singleTimeSubmissionMayBePending = false;

    for (const auto& descriptorOwnership :
         ownership->second.descriptorSets) {
        if (!descriptorOwnership.slots) {
            continue;
        }
        const size_t count = std::min(
            descriptorOwnership.slots->size(),
            descriptorOwnership.sets.size());
        if (count == 0) {
            continue;
        }
        const VkResult freeResult = vkFreeDescriptorSets(
            device, descriptorPool, static_cast<uint32_t>(count),
            descriptorOwnership.sets.data());
        if (freeResult != VK_SUCCESS) {
            return vkFailure("vkFreeDescriptorSets(unload scene)",
                             freeResult);
        }
    }
    cleanupSceneResources(ownership->second, false);
    sceneResourceOwnership_.erase(ownership);
    return Result::success();
}

Result VulkanContext::switchScene(Scene* scene) {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }
    if (!scene) {
        return Result::failure("Cannot switch to a null scene");
    }
    if (!hasSceneResources(scene)) {
        return Result::failure(
            "Incoming scene rendering resources are not loaded");
    }
    currentScene = scene;
    return Result::success();
}

bool VulkanContext::hasSceneResources(const Scene* scene) const noexcept {
    return scene && sceneResourceOwnership_.find(scene) !=
                        sceneResourceOwnership_.end() &&
           scene != sceneResourceLoadTarget_;
}

Result VulkanContext::validateTextureData(
    const std::unique_ptr<GameObject>& gameObject) const {
    if (!gameObject) {
        return Result::failure("Cannot validate textures for a null game object");
    }

    for (std::size_t index = 0; index < gameObject->meshInstances_.size();
         ++index) {
        const Material& material =
            gameObject->meshInstances_[index].material;
        if (!material.pixels) {
            return Result::failure("Game object mesh instance " +
                                   std::to_string(index) +
                                   " has missing texture pixels");
        }
        if (material.normalMapEnabled && !material.normalMapPixels) {
            return Result::failure("Game object mesh instance " +
                                   std::to_string(index) +
                                   " has missing normal-map pixels");
        }
        if (material.hasMetallicRoughnessMap &&
            !material.metallicRoughnessMapPixels) {
            return Result::failure("Game object mesh instance " +
                                   std::to_string(index) +
                                   " has missing metallic-roughness pixels");
        }
        if (material.texWidth <= 0 || material.texHeight <= 0) {
            return Result::failure("Game object mesh instance " +
                                   std::to_string(index) +
                                   " has invalid texture dimensions");
        }
        const auto width = static_cast<std::size_t>(material.texWidth);
        const auto height = static_cast<std::size_t>(material.texHeight);
        if (width > std::numeric_limits<std::size_t>::max() / height ||
            width * height > std::numeric_limits<std::size_t>::max() / 4 ||
            width > std::numeric_limits<VkDeviceSize>::max() / height ||
            width * height > std::numeric_limits<VkDeviceSize>::max() / 4 ||
            width * height * 4 == 0 || material.mipLevels == 0) {
            return Result::failure("Game object mesh instance " +
                                   std::to_string(index) +
                                   " has invalid texture image size");
        }
        if (material.normalMapEnabled &&
            (material.normalMapWidth <= 0 || material.normalMapHeight <= 0 ||
             material.normalMapMipLevels == 0)) {
            return Result::failure("Game object mesh instance " +
                                   std::to_string(index) +
                                   " has invalid normal-map image size");
        }
        if (material.hasMetallicRoughnessMap &&
            (material.metallicRoughnessMapWidth <= 0 ||
             material.metallicRoughnessMapHeight <= 0 ||
             material.metallicRoughnessMapMipLevels == 0)) {
            return Result::failure(
                "Game object mesh instance " + std::to_string(index) +
                " has invalid metallic-roughness image size");
        }
        if (material.hasMetallicRoughnessMap) {
            const auto metallicRoughnessWidth = static_cast<std::size_t>(
                material.metallicRoughnessMapWidth);
            const auto metallicRoughnessHeight = static_cast<std::size_t>(
                material.metallicRoughnessMapHeight);
            if (metallicRoughnessWidth >
                    std::numeric_limits<std::size_t>::max() /
                        metallicRoughnessHeight ||
                metallicRoughnessWidth * metallicRoughnessHeight >
                    std::numeric_limits<std::size_t>::max() / 4 ||
                metallicRoughnessWidth >
                    std::numeric_limits<VkDeviceSize>::max() /
                        metallicRoughnessHeight ||
                metallicRoughnessWidth * metallicRoughnessHeight >
                    std::numeric_limits<VkDeviceSize>::max() / 4) {
                return Result::failure(
                    "Game object mesh instance " + std::to_string(index) +
                    " has invalid metallic-roughness image size");
            }
        }
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
        result = initializeStep(createDirectionalShadowRenderPass(),
                                "Directional-shadow render-pass creation");
        if (!result) return result;
        result = initializeStep(createAmbientOcclusionRenderPasses(),
                                "Ambient-occlusion render-pass creation");
        if (!result) return result;
        result = initializeStep(createDescriptorSetLayout(),
                                "Descriptor-set-layout creation");
        if (!result) return result;
        result = initializeStep(createLightsDescriptorSetLayout(),
                                "Lights descriptor-set-layout creation");
        if (!result) return result;
        result = initializeStep(createDirectionalShadowDescriptorSetLayout(),
                                "Directional-shadow descriptor-set-layout creation");
        if (!result) return result;
        result = initializeStep(createAmbientOcclusionDescriptorSetLayout(),
                                "Ambient-occlusion descriptor-set-layout creation");
        if (!result) return result;
        result = initializeStep(createGraphicsPipeline(),
                                "Graphics-pipeline creation");
        if (!result) return result;
        result = initializeStep(createDirectionalShadowPipelines(),
                                "Directional-shadow pipeline creation");
        if (!result) return result;
        result = initializeStep(createSelectionOutlinePipeline(),
                                "Selection-outline-pipeline creation");
        if (!result) return result;
        result = initializeStep(createAmbientOcclusionPipelines(),
                                "Ambient-occlusion pipeline creation");
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
        result = initializeStep(initializeImGui(),
                                "Dear ImGui initialization");
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
        createInfo.enabledLayerCount =
            static_cast<uint32_t>(validationLayers.size());
        createInfo.ppEnabledLayerNames = validationLayers.data();

        populateDebugMessengerCreateInfo(debugCreateInfo);
        createInfo.pNext =
            (VkDebugUtilsMessengerCreateInfoEXT*)&debugCreateInfo;
    } else {
        createInfo.enabledLayerCount = 0;
        createInfo.ppEnabledLayerNames = nullptr;
        createInfo.pNext = nullptr;
    }

    const VkResult createResult =
        vkCreateInstance(&createInfo, nullptr, &instance);
    if (createResult != VK_SUCCESS) {
        instance = VK_NULL_HANDLE;
        return vkFailure("vkCreateInstance", createResult);
    }

    if (enableValidationLayers) {
        spdlog::info("Vulkan validation layers are enabled");
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
              void* pUserData) noexcept {
    (void)messageType;
    (void)pUserData;
    const char* message = pCallbackData && pCallbackData->pMessage
                              ? pCallbackData->pMessage
                              : "Validation callback supplied no message";
    try {
        if (messageSeverity &
            VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
            spdlog::error("Validation layer: {}", message);
        } else if (messageSeverity &
                   VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
            spdlog::warn("Validation layer: {}", message);
        } else {
            spdlog::debug("Validation layer: {}", message);
        }
    } catch (...) {
        std::fputs("Vulkan validation callback logging failed\n", stderr);
    }
    return VK_FALSE;
}

void VulkanContext::populateDebugMessengerCreateInfo(
    VkDebugUtilsMessengerCreateInfoEXT& createInfo) const {
    createInfo = {};
    createInfo.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    createInfo.messageSeverity =
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
    sampleRateShadingSupported = false;
    sampleRateShadingEnabled = false;
    msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    for (const auto& candidate : devices) {
        bool suitable = false;
        Result suitabilityResult = isDeviceSuitable(candidate, suitable);
        if (!suitabilityResult) {
            return addContext("Failed to query physical-device suitability",
                              suitabilityResult);
        }
        if (suitable) {
            physicalDevice = candidate;
            VkPhysicalDeviceFeatures supportedFeatures{};
            vkGetPhysicalDeviceFeatures(physicalDevice, &supportedFeatures);
            sampleRateShadingSupported =
                supportedFeatures.sampleRateShading == VK_TRUE;
            msaaSamples = getCappedUsableSampleCount();
            sampleRateShadingEnabled =
                renderer_configuration::shouldEnableSampleRateShading(
                    sampleRateShadingSupported, msaaSamples);
            break;
        }
    }

    if (physicalDevice == VK_NULL_HANDLE) {
        return Result::failure("Failed to find a suitable GPU");
    }
    spdlog::info("Successfully selected physical device");
    spdlog::info("Sample-rate shading supported: {}",
                 sampleRateShadingSupported ? "yes" : "no");
    spdlog::info("Sample-rate shading enabled: {}",
                 sampleRateShadingEnabled ? "yes" : "no");
    spdlog::info("Selected MSAA sample count: {}",
                 renderer_configuration::sampleCountName(msaaSamples));
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
    if (supportedFeatures.samplerAnisotropy != VK_TRUE) {
        spdlog::warn(
            "Rejecting physical device because sampler anisotropy is not "
            "supported");
    }

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

VkSampleCountFlagBits VulkanContext::getCappedUsableSampleCount() const {
    VkPhysicalDeviceProperties physicalDeviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &physicalDeviceProperties);

    VkSampleCountFlags counts =
        physicalDeviceProperties.limits.framebufferColorSampleCounts &
        physicalDeviceProperties.limits.framebufferDepthSampleCounts;
    return renderer_configuration::selectMsaaSampleCount(counts);
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
    deviceFeatures.sampleRateShading =
        sampleRateShadingEnabled ? VK_TRUE : VK_FALSE;

    VkDeviceCreateInfo createInfo{};
    createInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;

    createInfo.queueCreateInfoCount =
        static_cast<uint32_t>(queueCreateInfos.size());
    createInfo.pQueueCreateInfos = queueCreateInfos.data();

    createInfo.pEnabledFeatures = &deviceFeatures;

    createInfo.enabledExtensionCount =
        static_cast<uint32_t>(deviceExtensions.size());
    createInfo.ppEnabledExtensionNames = deviceExtensions.data();

    createInfo.enabledLayerCount = 0;
    createInfo.ppEnabledLayerNames = nullptr;

    const VkResult createResult =
        vkCreateDevice(physicalDevice, &createInfo, nullptr, &device);
    if (createResult != VK_SUCCESS) {
        device = VK_NULL_HANDLE;
        return vkFailure("vkCreateDevice", createResult);
    }

    vkGetDeviceQueue(device, indices.graphicsFamily.value(), 0, &graphicsQueue);
    vkGetDeviceQueue(device, indices.presentFamily.value(), 0, &presentQueue);
    graphicsQueueFamily = indices.graphicsFamily;
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
    swapchainMinimumImageCount = createInfo.minImageCount;

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

Result VulkanContext::findDirectionalShadowDepthFormat(VkFormat& format) const {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM,
         VK_FORMAT_D32_SFLOAT_S8_UINT}, VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        format);
}

Result VulkanContext::findAmbientOcclusionDepthFormat(VkFormat& format) const {
    return findSupportedFormat(
        {VK_FORMAT_D32_SFLOAT, VK_FORMAT_D24_UNORM_S8_UINT,
         VK_FORMAT_D16_UNORM}, VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        format);
}

Result VulkanContext::findAmbientOcclusionNormalFormat(VkFormat& format) const {
    return findSupportedFormat(
        {VK_FORMAT_R16G16B16A16_SFLOAT, VK_FORMAT_R32G32B32A32_SFLOAT},
        VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        format);
}

Result VulkanContext::findAmbientOcclusionOutputFormat(VkFormat& format) const {
    return findSupportedFormat(
        {VK_FORMAT_R8_UNORM, VK_FORMAT_R16_SFLOAT}, VK_IMAGE_TILING_OPTIMAL,
        VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT |
            VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT,
        format);
}

Result VulkanContext::createDirectionalShadowRenderPass() {
    Result formatResult = findDirectionalShadowDepthFormat(
        directionalShadowDepthFormat);
    if (!formatResult) {
        return addContext("Failed to find a sampleable directional-shadow depth format",
                          formatResult);
    }

    VkAttachmentDescription depthAttachment{};
    depthAttachment.format = directionalShadowDepthFormat;
    depthAttachment.samples = VK_SAMPLE_COUNT_1_BIT;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depthAttachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depthAttachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depthAttachment.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;

    VkAttachmentReference depthReference{};
    depthReference.attachment = 0;
    depthReference.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkSubpassDescription subpass{};
    subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subpass.pDepthStencilAttachment = &depthReference;

    VkSubpassDependency dependency{};
    dependency.srcSubpass = 0;
    dependency.dstSubpass = VK_SUBPASS_EXTERNAL;
    dependency.srcStageMask = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                              VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dependency.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    dependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;

    VkRenderPassCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    info.attachmentCount = 1;
    info.pAttachments = &depthAttachment;
    info.subpassCount = 1;
    info.pSubpasses = &subpass;
    info.dependencyCount = 1;
    info.pDependencies = &dependency;
    const VkResult result = vkCreateRenderPass(device, &info, nullptr,
                                                &directionalShadowRenderPass);
    if (result != VK_SUCCESS) {
        directionalShadowRenderPass = VK_NULL_HANDLE;
        directionalShadowDepthFormat = VK_FORMAT_UNDEFINED;
        return vkFailure("vkCreateRenderPass(directional shadow)", result);
    }
    return Result::success();
}

Result VulkanContext::createAmbientOcclusionRenderPasses() {
    Result result = findAmbientOcclusionDepthFormat(ambientOcclusionDepthFormat);
    if (!result) return addContext("Failed to find sampleable screen-space depth format", result);
    result = findAmbientOcclusionNormalFormat(ambientOcclusionNormalFormat);
    if (!result) return addContext("Failed to find sampleable screen-space normal format", result);
    result = findAmbientOcclusionOutputFormat(ambientOcclusionOutputFormat);
    if (!result) return addContext("Failed to find sampleable ambient-occlusion output format", result);

    VkAttachmentDescription normal{};
    normal.format = ambientOcclusionNormalFormat;
    normal.samples = VK_SAMPLE_COUNT_1_BIT;
    normal.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    normal.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    normal.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    normal.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    normal.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    normal.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentDescription depth{};
    depth.format = ambientOcclusionDepthFormat;
    depth.samples = VK_SAMPLE_COUNT_1_BIT;
    depth.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    depth.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depth.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    depth.finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    VkAttachmentReference normalReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthReference{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription geometrySubpass{};
    geometrySubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    geometrySubpass.colorAttachmentCount = 1;
    geometrySubpass.pColorAttachments = &normalReference;
    geometrySubpass.pDepthStencilAttachment = &depthReference;
    VkSubpassDependency geometryDependency{};
    geometryDependency.srcSubpass = 0;
    geometryDependency.dstSubpass = VK_SUBPASS_EXTERNAL;
    geometryDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                                      VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                                      VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    geometryDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT |
                                      VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    geometryDependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    geometryDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    const std::array<VkAttachmentDescription, 2> geometryAttachments{normal, depth};
    VkRenderPassCreateInfo geometryInfo{};
    geometryInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    geometryInfo.attachmentCount = static_cast<uint32_t>(geometryAttachments.size());
    geometryInfo.pAttachments = geometryAttachments.data();
    geometryInfo.subpassCount = 1;
    geometryInfo.pSubpasses = &geometrySubpass;
    geometryInfo.dependencyCount = 1;
    geometryInfo.pDependencies = &geometryDependency;
    VkResult createResult = vkCreateRenderPass(device, &geometryInfo, nullptr,
                                               &ambientOcclusionGeometryRenderPass);
    if (createResult != VK_SUCCESS) {
        ambientOcclusionGeometryRenderPass = VK_NULL_HANDLE;
        return vkFailure("vkCreateRenderPass(ambient-occlusion geometry)", createResult);
    }

    VkAttachmentDescription output{};
    output.format = ambientOcclusionOutputFormat;
    output.samples = VK_SAMPLE_COUNT_1_BIT;
    output.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    output.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    output.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    output.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    output.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    output.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference outputReference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription outputSubpass{};
    outputSubpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    outputSubpass.colorAttachmentCount = 1;
    outputSubpass.pColorAttachments = &outputReference;
    VkSubpassDependency outputDependency{};
    outputDependency.srcSubpass = 0;
    outputDependency.dstSubpass = VK_SUBPASS_EXTERNAL;
    outputDependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    outputDependency.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    outputDependency.dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    outputDependency.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo outputInfo{};
    outputInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    outputInfo.attachmentCount = 1;
    outputInfo.pAttachments = &output;
    outputInfo.subpassCount = 1;
    outputInfo.pSubpasses = &outputSubpass;
    outputInfo.dependencyCount = 1;
    outputInfo.pDependencies = &outputDependency;
    createResult = vkCreateRenderPass(device, &outputInfo, nullptr,
                                      &ambientOcclusionRenderPass);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreateRenderPass(ambient occlusion)", createResult);
    createResult = vkCreateRenderPass(device, &outputInfo, nullptr,
                                      &ambientOcclusionBlurRenderPass);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreateRenderPass(ambient-occlusion blur)", createResult);
    return Result::success();
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

    VkDescriptorSetLayoutBinding normalSamplerLayoutBinding{};
    normalSamplerLayoutBinding.binding = 2;
    normalSamplerLayoutBinding.descriptorCount = 1;
    normalSamplerLayoutBinding.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    normalSamplerLayoutBinding.pImmutableSamplers = nullptr;
    normalSamplerLayoutBinding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutBinding metallicRoughnessSamplerLayoutBinding{};
    metallicRoughnessSamplerLayoutBinding.binding = 3;
    metallicRoughnessSamplerLayoutBinding.descriptorCount = 1;
    metallicRoughnessSamplerLayoutBinding.descriptorType =
        VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    metallicRoughnessSamplerLayoutBinding.pImmutableSamplers = nullptr;
    metallicRoughnessSamplerLayoutBinding.stageFlags =
        VK_SHADER_STAGE_FRAGMENT_BIT;

    std::array<VkDescriptorSetLayoutBinding, 4> bindings = {
        uboLayoutBinding, samplerLayoutBinding, normalSamplerLayoutBinding,
        metallicRoughnessSamplerLayoutBinding};

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

Result VulkanContext::createDirectionalShadowDescriptorSetLayout() {
    VkDescriptorSetLayoutBinding transform{};
    transform.binding = 0;
    transform.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    transform.descriptorCount = 1;
    transform.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutBinding depth{};
    depth.binding = 1;
    depth.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    depth.descriptorCount = 1;
    depth.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    const std::array<VkDescriptorSetLayoutBinding, 2> bindings = {
        transform, depth};
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    const VkResult result = vkCreateDescriptorSetLayout(
        device, &info, nullptr, &directionalShadowDescriptorSetLayout);
    if (result != VK_SUCCESS) {
        directionalShadowDescriptorSetLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreateDescriptorSetLayout(directional shadow)", result);
    }
    return Result::success();
}

Result VulkanContext::createAmbientOcclusionDescriptorSetLayout() {
    std::array<VkDescriptorSetLayoutBinding, 5> bindings{};
    for (uint32_t binding = 0; binding < 4; ++binding) {
        bindings[binding].binding = binding;
        bindings[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[binding].descriptorCount = 1;
        bindings[binding].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bindings[4].binding = 4;
    bindings[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bindings[4].descriptorCount = 1;
    bindings[4].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    info.bindingCount = static_cast<uint32_t>(bindings.size());
    info.pBindings = bindings.data();
    const VkResult result = vkCreateDescriptorSetLayout(device, &info, nullptr,
                                                         &ambientOcclusionDescriptorSetLayout);
    if (result != VK_SUCCESS) {
        ambientOcclusionDescriptorSetLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreateDescriptorSetLayout(ambient occlusion)", result);
    }
    return Result::success();
}

Result VulkanContext::createLightsUBO() {
    if (!initialized) {
        return Result::failure("Vulkan Context is not initialized");
    }

    if (std::any_of(lightsDescriptorSets.begin(),
                    lightsDescriptorSets.end(),
                    [](VkDescriptorSet descriptorSet) {
                        return descriptorSet != VK_NULL_HANDLE;
                    })) {
        return Result::failure("Lights resources are already initialized");
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        Result result = createBuffer(
            sizeof(LightsUBO), VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            lightsBuffers[i], lightsBufferMemory[i]);
        if (!result) {
            destroyLightsUBOs();
            return addContext(
                "Failed to create lights uniform buffer for frame " +
                    std::to_string(i),
                result);
        }

        const VkResult mapResult = vkMapMemory(
            device, lightsBufferMemory[i], 0, sizeof(LightsUBO), 0,
            &lightsBufferMapped[i]);
        if (mapResult != VK_SUCCESS) {
            lightsBufferMapped[i] = nullptr;
            destroyLightsUBOs();
            return addContext(
                "Failed to map lights uniform buffer for frame " +
                    std::to_string(i),
                vkFailure("vkMapMemory(lights buffer)", mapResult));
        }
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(lightsDescriptorSetLayout);
    VkDescriptorSetAllocateInfo allocInfo{};
    allocInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocInfo.descriptorPool = descriptorPool;
    allocInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocInfo.pSetLayouts = layouts.data();

    const VkResult result = vkAllocateDescriptorSets(
        device, &allocInfo, lightsDescriptorSets.data());
    if (result != VK_SUCCESS) {
        lightsDescriptorSets.fill(VK_NULL_HANDLE);
        destroyLightsUBOs();
        return vkFailure("vkAllocateDescriptorSets(lights)", result);
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = lightsBuffers[i];
        bufferInfo.offset = 0;
        bufferInfo.range = sizeof(LightsUBO);

        VkWriteDescriptorSet descriptorWrite{};
        descriptorWrite.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        descriptorWrite.dstSet = lightsDescriptorSets[i];
        descriptorWrite.dstBinding = 0;
        descriptorWrite.dstArrayElement = 0;
        descriptorWrite.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        descriptorWrite.descriptorCount = 1;
        descriptorWrite.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(device, 1, &descriptorWrite, 0, nullptr);
    }

    spdlog::info("Successfully created per-frame lights resources");
    return Result::success();
}

void VulkanContext::destroyLightsUBOs() noexcept {
    if (device == VK_NULL_HANDLE) {
        lightsDescriptorSets.fill(VK_NULL_HANDLE);
        lightsBuffers.fill(VK_NULL_HANDLE);
        lightsBufferMemory.fill(VK_NULL_HANDLE);
        lightsBufferMapped.fill(nullptr);
        return;
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        if (lightsBufferMapped[i] != nullptr &&
            lightsBufferMemory[i] != VK_NULL_HANDLE) {
            vkUnmapMemory(device, lightsBufferMemory[i]);
        }
        lightsBufferMapped[i] = nullptr;
        if (lightsBuffers[i] != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, lightsBuffers[i], nullptr);
            lightsBuffers[i] = VK_NULL_HANDLE;
        }
        if (lightsBufferMemory[i] != VK_NULL_HANDLE) {
            vkFreeMemory(device, lightsBufferMemory[i], nullptr);
            lightsBufferMemory[i] = VK_NULL_HANDLE;
        }
    }
    lightsDescriptorSets.fill(VK_NULL_HANDLE);
}

Result VulkanContext::createDirectionalShadowResources() {
    if (!initialized || descriptorPool == VK_NULL_HANDLE) {
        return Result::failure(
            "Directional-shadow resources require an initialized descriptor pool");
    }
    if (directionalShadowSampler != VK_NULL_HANDLE) {
        return Result::failure("Directional-shadow resources are already initialized");
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    samplerInfo.mipLodBias = 0.0f;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.compareOp = VK_COMPARE_OP_ALWAYS;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    samplerInfo.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    samplerInfo.unnormalizedCoordinates = VK_FALSE;
    VkResult result = vkCreateSampler(device, &samplerInfo, nullptr,
                                      &directionalShadowSampler);
    if (result != VK_SUCCESS) {
        directionalShadowSampler = VK_NULL_HANDLE;
        return vkFailure("vkCreateSampler(directional shadow)", result);
    }

    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        auto& frame = directionalShadowFrames[i];
        Result createResult = createImage(
            directional_shadow::mapResolution, directional_shadow::mapResolution,
            1, VK_SAMPLE_COUNT_1_BIT, directionalShadowDepthFormat,
            VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
                VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame.depthImage,
            frame.depthImageMemory);
        if (!createResult) {
            destroyDirectionalShadowResources();
            return addContext("Failed to create directional-shadow depth image for frame " +
                                  std::to_string(i), createResult);
        }
        createResult = createImageView(frame.depthImage, directionalShadowDepthFormat,
                                       VK_IMAGE_ASPECT_DEPTH_BIT, 1,
                                       frame.depthImageView);
        if (!createResult) {
            destroyDirectionalShadowResources();
            return addContext("Failed to create directional-shadow depth view for frame " +
                                  std::to_string(i), createResult);
        }
        VkFramebufferCreateInfo framebufferInfo{};
        framebufferInfo.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebufferInfo.renderPass = directionalShadowRenderPass;
        framebufferInfo.attachmentCount = 1;
        framebufferInfo.pAttachments = &frame.depthImageView;
        framebufferInfo.width = directional_shadow::mapResolution;
        framebufferInfo.height = directional_shadow::mapResolution;
        framebufferInfo.layers = 1;
        result = vkCreateFramebuffer(device, &framebufferInfo, nullptr,
                                     &frame.framebuffer);
        if (result != VK_SUCCESS) {
            destroyDirectionalShadowResources();
            return vkFailure("vkCreateFramebuffer(directional shadow)", result);
        }
        createResult = createBuffer(sizeof(DirectionalShadowUBO),
                                    VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    frame.transformBuffer,
                                    frame.transformBufferMemory);
        if (!createResult) {
            destroyDirectionalShadowResources();
            return addContext("Failed to create directional-shadow UBO for frame " +
                                  std::to_string(i), createResult);
        }
        result = vkMapMemory(device, frame.transformBufferMemory, 0,
                             sizeof(DirectionalShadowUBO), 0,
                             &frame.transformBufferMapped);
        if (result != VK_SUCCESS) {
            frame.transformBufferMapped = nullptr;
            destroyDirectionalShadowResources();
            return vkFailure("vkMapMemory(directional shadow UBO)", result);
        }
        const DirectionalShadowUBO initialUbo{glm::mat4(1.0f)};
        memcpy(frame.transformBufferMapped, &initialUbo, sizeof(initialUbo));
    }

    Result initializeResult = initializeDirectionalShadowImages();
    if (!initializeResult) {
        return addContext("Failed to initialize directional-shadow images",
                          initializeResult);
    }

    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(directionalShadowDescriptorSetLayout);
    VkDescriptorSetAllocateInfo allocateInfo{};
    allocateInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocateInfo.descriptorPool = descriptorPool;
    allocateInfo.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocateInfo.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets{};
    result = vkAllocateDescriptorSets(device, &allocateInfo, sets.data());
    if (result != VK_SUCCESS) {
        destroyDirectionalShadowResources();
        return vkFailure("vkAllocateDescriptorSets(directional shadow)", result);
    }
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        auto& frame = directionalShadowFrames[i];
        frame.descriptorSet = sets[i];
        VkDescriptorBufferInfo bufferInfo{frame.transformBuffer, 0,
                                          sizeof(DirectionalShadowUBO)};
        VkDescriptorImageInfo imageInfo{};
        imageInfo.sampler = directionalShadowSampler;
        imageInfo.imageView = frame.depthImageView;
        imageInfo.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        std::array<VkWriteDescriptorSet, 2> writes{};
        writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[0].dstSet = frame.descriptorSet;
        writes[0].dstBinding = 0;
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].descriptorCount = 1;
        writes[0].pBufferInfo = &bufferInfo;
        writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[1].dstSet = frame.descriptorSet;
        writes[1].dstBinding = 1;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[1].descriptorCount = 1;
        writes[1].pImageInfo = &imageInfo;
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()),
                               writes.data(), 0, nullptr);
    }
    return Result::success();
}

Result VulkanContext::initializeDirectionalShadowImages() {
    for (const auto& frame : directionalShadowFrames) {
        if (frame.framebuffer == VK_NULL_HANDLE) {
            return Result::failure(
                "Directional-shadow framebuffer is unavailable during image initialization");
        }
    }

    VkCommandBuffer commandBuffer = VK_NULL_HANDLE;
    Result result = beginSingleTimeCommands(commandBuffer);
    if (!result) {
        return result;
    }

    for (const auto& frame : directionalShadowFrames) {
        VkRenderPassBeginInfo renderPassInfo{};
        renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        renderPassInfo.renderPass = directionalShadowRenderPass;
        renderPassInfo.framebuffer = frame.framebuffer;
        renderPassInfo.renderArea.offset = {0, 0};
        renderPassInfo.renderArea.extent = {
            directional_shadow::mapResolution,
            directional_shadow::mapResolution};

        VkClearValue clearValue{};
        clearValue.depthStencil = {1.0f, 0};
        renderPassInfo.clearValueCount = 1;
        renderPassInfo.pClearValues = &clearValue;

        vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                             VK_SUBPASS_CONTENTS_INLINE);
        vkCmdEndRenderPass(commandBuffer);
    }

    return endSingleTimeCommands(commandBuffer);
}

void VulkanContext::destroyDirectionalShadowResources() noexcept {
    if (device != VK_NULL_HANDLE) {
        for (auto& frame : directionalShadowFrames) {
            if (frame.framebuffer != VK_NULL_HANDLE) {
                vkDestroyFramebuffer(device, frame.framebuffer, nullptr);
            }
            if (frame.transformBufferMapped != nullptr &&
                frame.transformBufferMemory != VK_NULL_HANDLE) {
                vkUnmapMemory(device, frame.transformBufferMemory);
            }
            if (frame.transformBuffer != VK_NULL_HANDLE) {
                vkDestroyBuffer(device, frame.transformBuffer, nullptr);
            }
            if (frame.transformBufferMemory != VK_NULL_HANDLE) {
                vkFreeMemory(device, frame.transformBufferMemory, nullptr);
            }
            if (frame.depthImageView != VK_NULL_HANDLE) {
                vkDestroyImageView(device, frame.depthImageView, nullptr);
            }
            if (frame.depthImage != VK_NULL_HANDLE) {
                vkDestroyImage(device, frame.depthImage, nullptr);
            }
            if (frame.depthImageMemory != VK_NULL_HANDLE) {
                vkFreeMemory(device, frame.depthImageMemory, nullptr);
            }
            frame = {};
        }
        if (directionalShadowSampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, directionalShadowSampler, nullptr);
        }
    } else {
        directionalShadowFrames = {};
    }
    directionalShadowSampler = VK_NULL_HANDLE;
    directionalShadowDepthFormat = VK_FORMAT_UNDEFINED;
}

Result VulkanContext::createAmbientOcclusionResources() {
    if (!initialized || descriptorPool == VK_NULL_HANDLE) {
        return Result::failure("Ambient-occlusion resources require an initialized descriptor pool");
    }
    if (ambientOcclusionSampler != VK_NULL_HANDLE) {
        return Result::failure("Ambient-occlusion resources are already initialized");
    }
    Result settingsResult = ambient_occlusion::validate(ambientOcclusionSettings);
    if (!settingsResult) return settingsResult;

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.anisotropyEnable = VK_FALSE;
    samplerInfo.compareEnable = VK_FALSE;
    samplerInfo.minLod = 0.0f;
    samplerInfo.maxLod = 0.0f;
    VkResult result = vkCreateSampler(device, &samplerInfo, nullptr,
                                      &ambientOcclusionSampler);
    if (result != VK_SUCCESS) {
        ambientOcclusionSampler = VK_NULL_HANDLE;
        return vkFailure("vkCreateSampler(ambient occlusion)", result);
    }
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        auto& frame = ambientOcclusionFrames[i];
        Result createResult = createBuffer(sizeof(AmbientOcclusionUBO),
            VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
            VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
            frame.uniformBuffer, frame.uniformBufferMemory);
        if (!createResult) {
            destroyAmbientOcclusionResources();
            return addContext("Failed to create ambient-occlusion UBO for frame " +
                              std::to_string(i), createResult);
        }
        result = vkMapMemory(device, frame.uniformBufferMemory, 0,
                             sizeof(AmbientOcclusionUBO), 0,
                             &frame.uniformBufferMapped);
        if (result != VK_SUCCESS) {
            frame.uniformBufferMapped = nullptr;
            destroyAmbientOcclusionResources();
            return vkFailure("vkMapMemory(ambient-occlusion UBO)", result);
        }
        const AmbientOcclusionUBO initial{};
        memcpy(frame.uniformBufferMapped, &initial, sizeof(initial));
    }
    Result createResult = createAmbientOcclusionExtentResources();
    if (!createResult) {
        destroyAmbientOcclusionResources();
        return createResult;
    }
    std::array<VkDescriptorSetLayout, MAX_FRAMES_IN_FLIGHT> layouts{};
    layouts.fill(ambientOcclusionDescriptorSetLayout);
    VkDescriptorSetAllocateInfo allocation{};
    allocation.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    allocation.descriptorPool = descriptorPool;
    allocation.descriptorSetCount = MAX_FRAMES_IN_FLIGHT;
    allocation.pSetLayouts = layouts.data();
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets{};
    result = vkAllocateDescriptorSets(device, &allocation, sets.data());
    if (result != VK_SUCCESS) {
        destroyAmbientOcclusionResources();
        return vkFailure("vkAllocateDescriptorSets(ambient occlusion)", result);
    }
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        ambientOcclusionFrames[i].descriptorSet = sets[i];
    }
    createResult = rewriteAmbientOcclusionDescriptors();
    if (!createResult) {
        destroyAmbientOcclusionResources();
        return createResult;
    }
    return Result::success();
}

Result VulkanContext::createAmbientOcclusionExtentResources() {
    if (!ambient_occlusion::validExtent(swapchainExtent.width, swapchainExtent.height)) {
        return Result::failure("Ambient-occlusion images require a nonzero swapchain extent");
    }
    for (size_t i = 0; i < MAX_FRAMES_IN_FLIGHT; ++i) {
        auto& frame = ambientOcclusionFrames[i];
        Result result = createImage(swapchainExtent.width, swapchainExtent.height, 1,
            VK_SAMPLE_COUNT_1_BIT, ambientOcclusionDepthFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame.depthImage, frame.depthImageMemory);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create AO depth image", result); }
        result = createImageView(frame.depthImage, ambientOcclusionDepthFormat,
            VK_IMAGE_ASPECT_DEPTH_BIT, 1, frame.depthImageView);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create AO depth view", result); }
        result = createImage(swapchainExtent.width, swapchainExtent.height, 1,
            VK_SAMPLE_COUNT_1_BIT, ambientOcclusionNormalFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame.normalImage, frame.normalImageMemory);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create AO normal image", result); }
        result = createImageView(frame.normalImage, ambientOcclusionNormalFormat,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, frame.normalImageView);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create AO normal view", result); }
        result = createImage(swapchainExtent.width, swapchainExtent.height, 1,
            VK_SAMPLE_COUNT_1_BIT, ambientOcclusionOutputFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame.rawImage, frame.rawImageMemory);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create raw AO image", result); }
        result = createImageView(frame.rawImage, ambientOcclusionOutputFormat,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, frame.rawImageView);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create raw AO view", result); }
        result = createImage(swapchainExtent.width, swapchainExtent.height, 1,
            VK_SAMPLE_COUNT_1_BIT, ambientOcclusionOutputFormat, VK_IMAGE_TILING_OPTIMAL,
            VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, frame.blurredImage, frame.blurredImageMemory);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create blurred AO image", result); }
        result = createImageView(frame.blurredImage, ambientOcclusionOutputFormat,
            VK_IMAGE_ASPECT_COLOR_BIT, 1, frame.blurredImageView);
        if (!result) { destroyAmbientOcclusionExtentResources(); return addContext("Failed to create blurred AO view", result); }
        const std::array<VkImageView, 2> geometryAttachments{frame.normalImageView, frame.depthImageView};
        VkFramebufferCreateInfo framebuffer{};
        framebuffer.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        framebuffer.renderPass = ambientOcclusionGeometryRenderPass;
        framebuffer.attachmentCount = static_cast<uint32_t>(geometryAttachments.size());
        framebuffer.pAttachments = geometryAttachments.data();
        framebuffer.width = swapchainExtent.width;
        framebuffer.height = swapchainExtent.height;
        framebuffer.layers = 1;
        VkResult createResult = vkCreateFramebuffer(device, &framebuffer, nullptr, &frame.geometryFramebuffer);
        if (createResult != VK_SUCCESS) { destroyAmbientOcclusionExtentResources(); return vkFailure("vkCreateFramebuffer(AO geometry)", createResult); }
        framebuffer.renderPass = ambientOcclusionRenderPass;
        framebuffer.attachmentCount = 1;
        framebuffer.pAttachments = &frame.rawImageView;
        createResult = vkCreateFramebuffer(device, &framebuffer, nullptr, &frame.rawFramebuffer);
        if (createResult != VK_SUCCESS) { destroyAmbientOcclusionExtentResources(); return vkFailure("vkCreateFramebuffer(raw AO)", createResult); }
        framebuffer.renderPass = ambientOcclusionBlurRenderPass;
        framebuffer.pAttachments = &frame.blurredImageView;
        createResult = vkCreateFramebuffer(device, &framebuffer, nullptr, &frame.blurFramebuffer);
        if (createResult != VK_SUCCESS) { destroyAmbientOcclusionExtentResources(); return vkFailure("vkCreateFramebuffer(blurred AO)", createResult); }
    }
    return Result::success();
}

Result VulkanContext::rewriteAmbientOcclusionDescriptors() {
    for (const auto& frame : ambientOcclusionFrames) {
        if (frame.descriptorSet == VK_NULL_HANDLE || frame.depthImageView == VK_NULL_HANDLE ||
            frame.normalImageView == VK_NULL_HANDLE || frame.rawImageView == VK_NULL_HANDLE ||
            frame.blurredImageView == VK_NULL_HANDLE || frame.uniformBuffer == VK_NULL_HANDLE) {
            return Result::failure("Ambient-occlusion descriptor inputs are unavailable");
        }
        std::array<VkDescriptorImageInfo, 4> images{};
        images[0] = {ambientOcclusionSampler, frame.depthImageView, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
        images[1] = {ambientOcclusionSampler, frame.normalImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[2] = {ambientOcclusionSampler, frame.rawImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        images[3] = {ambientOcclusionSampler, frame.blurredImageView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorBufferInfo buffer{frame.uniformBuffer, 0, sizeof(AmbientOcclusionUBO)};
        std::array<VkWriteDescriptorSet, 5> writes{};
        for (uint32_t binding = 0; binding < 4; ++binding) {
            writes[binding].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[binding].dstSet = frame.descriptorSet;
            writes[binding].dstBinding = binding;
            writes[binding].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            writes[binding].descriptorCount = 1;
            writes[binding].pImageInfo = &images[binding];
        }
        writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[4].dstSet = frame.descriptorSet;
        writes[4].dstBinding = 4;
        writes[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[4].descriptorCount = 1;
        writes[4].pBufferInfo = &buffer;
        vkUpdateDescriptorSets(device, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    }
    return Result::success();
}

void VulkanContext::destroyAmbientOcclusionExtentResources() noexcept {
    if (device != VK_NULL_HANDLE) {
        for (auto& frame : ambientOcclusionFrames) {
            if (frame.geometryFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, frame.geometryFramebuffer, nullptr);
            if (frame.rawFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, frame.rawFramebuffer, nullptr);
            if (frame.blurFramebuffer != VK_NULL_HANDLE) vkDestroyFramebuffer(device, frame.blurFramebuffer, nullptr);
            frame.geometryFramebuffer = frame.rawFramebuffer = frame.blurFramebuffer = VK_NULL_HANDLE;
            const std::array<VkImageView*, 4> views{&frame.depthImageView, &frame.normalImageView, &frame.rawImageView, &frame.blurredImageView};
            for (VkImageView* view : views) { if (*view != VK_NULL_HANDLE) { vkDestroyImageView(device, *view, nullptr); *view = VK_NULL_HANDLE; } }
            const std::array<VkImage*, 4> images{&frame.depthImage, &frame.normalImage, &frame.rawImage, &frame.blurredImage};
            for (VkImage* image : images) { if (*image != VK_NULL_HANDLE) { vkDestroyImage(device, *image, nullptr); *image = VK_NULL_HANDLE; } }
            const std::array<VkDeviceMemory*, 4> memories{&frame.depthImageMemory, &frame.normalImageMemory, &frame.rawImageMemory, &frame.blurredImageMemory};
            for (VkDeviceMemory* memory : memories) { if (*memory != VK_NULL_HANDLE) { vkFreeMemory(device, *memory, nullptr); *memory = VK_NULL_HANDLE; } }
        }
    }
}

void VulkanContext::destroyAmbientOcclusionResources() noexcept {
    destroyAmbientOcclusionExtentResources();
    if (device != VK_NULL_HANDLE) {
        for (auto& frame : ambientOcclusionFrames) {
            if (frame.uniformBufferMapped != nullptr && frame.uniformBufferMemory != VK_NULL_HANDLE) vkUnmapMemory(device, frame.uniformBufferMemory);
            frame.uniformBufferMapped = nullptr;
            if (frame.uniformBuffer != VK_NULL_HANDLE) vkDestroyBuffer(device, frame.uniformBuffer, nullptr);
            if (frame.uniformBufferMemory != VK_NULL_HANDLE) vkFreeMemory(device, frame.uniformBufferMemory, nullptr);
            frame.uniformBuffer = VK_NULL_HANDLE;
            frame.uniformBufferMemory = VK_NULL_HANDLE;
            frame.descriptorSet = VK_NULL_HANDLE;
        }
        if (ambientOcclusionSampler != VK_NULL_HANDLE) vkDestroySampler(device, ambientOcclusionSampler, nullptr);
    }
    ambientOcclusionFrames = {};
    ambientOcclusionSampler = VK_NULL_HANDLE;
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
    multisampling.sampleShadingEnable =
        sampleRateShadingEnabled ? VK_TRUE : VK_FALSE;
    multisampling.minSampleShading =
        sampleRateShadingEnabled ? 0.2f : 0.0f;
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

    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
    if (sizeof(MaterialPushConstants) >
        deviceProperties.limits.maxPushConstantsSize) {
        return Result::failure(
            "Material push constants exceed the device push-constant limit");
    }

    VkDescriptorSetLayout setLayouts[] = {descriptorSetLayout,
        lightsDescriptorSetLayout, directionalShadowDescriptorSetLayout,
        ambientOcclusionDescriptorSetLayout};
    VkPushConstantRange materialPushConstantRange{};
    materialPushConstantRange.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialPushConstantRange.offset = 0;
    materialPushConstantRange.size = sizeof(MaterialPushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 4;
    pipelineLayoutInfo.pSetLayouts = setLayouts;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &materialPushConstantRange;

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

    rasterizer.cullMode = VK_CULL_MODE_NONE;
    createResult = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &doubleSidedGraphicsPipeline);
    if (createResult != VK_SUCCESS) {
        if (doubleSidedGraphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, doubleSidedGraphicsPipeline, nullptr);
        }
        doubleSidedGraphicsPipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(device, graphicsPipeline, nullptr);
        graphicsPipeline = VK_NULL_HANDLE;
        return vkFailure("vkCreateGraphicsPipelines (double-sided)", createResult);
    }

    spdlog::info("Successfully created graphics pipelines");
    return Result::success();
}

Result VulkanContext::createDirectionalShadowPipelines() {
    std::vector<char> vertexShaderCode;
    Result result = readFile("rendering/shaders/directional_shadow.vert.spv",
                             vertexShaderCode);
    if (!result) return addContext("Failed to read directional-shadow vertex shader", result);
    ScopedShaderModule vertexShader(device);
    result = createShaderModule(vertexShaderCode, vertexShader.module);
    if (!result) return addContext("Failed to create directional-shadow vertex shader module", result);
    std::vector<char> fragmentShaderCode;
    result = readFile("rendering/shaders/directional_shadow.frag.spv",
                      fragmentShaderCode);
    if (!result) return addContext("Failed to read directional-shadow fragment shader", result);
    ScopedShaderModule fragmentShader(device);
    result = createShaderModule(fragmentShaderCode, fragmentShader.module);
    if (!result) return addContext("Failed to create directional-shadow fragment shader module", result);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertexShader.module;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragmentShader.module;
    stages[1].pName = "main";

    const VkVertexInputBindingDescription binding = Vertex::getBindingDescription();
    std::array<VkVertexInputAttributeDescription, 2> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)};
    attributes[1] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)};
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_TRUE;
    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.logicOpEnable = VK_FALSE;
    blend.attachmentCount = 0;
    std::array<VkDynamicState, 3> dynamicStates = {
        VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();

    VkDescriptorSetLayout layouts[] = {descriptorSetLayout,
                                        directionalShadowDescriptorSetLayout};
    VkPushConstantRange push{};
    push.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    push.size = sizeof(MaterialPushConstants);
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 2;
    layoutInfo.pSetLayouts = layouts;
    layoutInfo.pushConstantRangeCount = 1;
    layoutInfo.pPushConstantRanges = &push;
    VkResult createResult = vkCreatePipelineLayout(device, &layoutInfo, nullptr,
                                                    &directionalShadowPipelineLayout);
    if (createResult != VK_SUCCESS) {
        directionalShadowPipelineLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreatePipelineLayout(directional shadow)", createResult);
    }

    VkGraphicsPipelineCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vertexInput;
    info.pInputAssemblyState = &assembly;
    info.pViewportState = &viewport;
    info.pRasterizationState = &rasterizer;
    info.pMultisampleState = &multisample;
    info.pDepthStencilState = &depth;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = directionalShadowPipelineLayout;
    info.renderPass = directionalShadowRenderPass;
    createResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &info,
                                             nullptr, &directionalShadowPipeline);
    if (createResult != VK_SUCCESS) {
        directionalShadowPipeline = VK_NULL_HANDLE;
        return vkFailure("vkCreateGraphicsPipelines(directional shadow)", createResult);
    }
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    createResult = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &info, nullptr,
        &directionalShadowDoubleSidedPipeline);
    if (createResult != VK_SUCCESS) {
        directionalShadowDoubleSidedPipeline = VK_NULL_HANDLE;
        vkDestroyPipeline(device, directionalShadowPipeline, nullptr);
        directionalShadowPipeline = VK_NULL_HANDLE;
        return vkFailure("vkCreateGraphicsPipelines(directional shadow double-sided)", createResult);
    }
    return Result::success();
}

Result VulkanContext::createAmbientOcclusionPipelines() {
    auto readModule = [this](const char* path, VkShaderModule& module) -> Result {
        std::vector<char> code;
        Result result = readFile(path, code);
        if (!result) return result;
        return createShaderModule(code, module);
    };
    ScopedShaderModule geometryVertex(device), geometryFragment(device);
    Result result = readModule("rendering/shaders/ambient_occlusion_geometry.vert.spv", geometryVertex.module);
    if (!result) return addContext("Failed to create AO geometry vertex shader", result);
    result = readModule("rendering/shaders/ambient_occlusion_geometry.frag.spv", geometryFragment.module);
    if (!result) return addContext("Failed to create AO geometry fragment shader", result);
    VkPipelineShaderStageCreateInfo geometryStages[2]{};
    geometryStages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                         VK_SHADER_STAGE_VERTEX_BIT, geometryVertex.module, "main"};
    geometryStages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                         VK_SHADER_STAGE_FRAGMENT_BIT, geometryFragment.module, "main"};
    const VkVertexInputBindingDescription binding = Vertex::getBindingDescription();
    std::array<VkVertexInputAttributeDescription, 3> attributes{};
    attributes[0] = {0, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, pos)};
    attributes[1] = {2, 0, VK_FORMAT_R32G32_SFLOAT, offsetof(Vertex, texCoord)};
    attributes[2] = {3, 0, VK_FORMAT_R32G32B32_SFLOAT, offsetof(Vertex, normal)};
    VkPipelineVertexInputStateCreateInfo vertexInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &binding;
    vertexInput.vertexAttributeDescriptionCount = static_cast<uint32_t>(attributes.size());
    vertexInput.pVertexAttributeDescriptions = attributes.data();
    VkPipelineInputAssemblyStateCreateInfo assembly{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo viewport{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rasterizer{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.lineWidth = 1.0f;
    rasterizer.cullMode = VK_CULL_MODE_BACK_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo multisample{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo depth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState colorAttachment{};
    colorAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &colorAttachment;
    const std::array<VkDynamicState, 2> dynamicStates{VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
    dynamic.pDynamicStates = dynamicStates.data();
    VkPushConstantRange materialPush{};
    materialPush.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    materialPush.size = sizeof(MaterialPushConstants);
    VkPipelineLayoutCreateInfo geometryLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    geometryLayoutInfo.setLayoutCount = 1;
    geometryLayoutInfo.pSetLayouts = &descriptorSetLayout;
    geometryLayoutInfo.pushConstantRangeCount = 1;
    geometryLayoutInfo.pPushConstantRanges = &materialPush;
    VkResult createResult = vkCreatePipelineLayout(device, &geometryLayoutInfo, nullptr,
                                                    &ambientOcclusionGeometryPipelineLayout);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreatePipelineLayout(AO geometry)", createResult);
    VkGraphicsPipelineCreateInfo geometryInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    geometryInfo.stageCount = 2;
    geometryInfo.pStages = geometryStages;
    geometryInfo.pVertexInputState = &vertexInput;
    geometryInfo.pInputAssemblyState = &assembly;
    geometryInfo.pViewportState = &viewport;
    geometryInfo.pRasterizationState = &rasterizer;
    geometryInfo.pMultisampleState = &multisample;
    geometryInfo.pDepthStencilState = &depth;
    geometryInfo.pColorBlendState = &blend;
    geometryInfo.pDynamicState = &dynamic;
    geometryInfo.layout = ambientOcclusionGeometryPipelineLayout;
    geometryInfo.renderPass = ambientOcclusionGeometryRenderPass;
    createResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &geometryInfo, nullptr,
                                             &ambientOcclusionGeometryPipeline);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreateGraphicsPipelines(AO geometry)", createResult);
    rasterizer.cullMode = VK_CULL_MODE_NONE;
    createResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &geometryInfo, nullptr,
                                             &ambientOcclusionGeometryDoubleSidedPipeline);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreateGraphicsPipelines(AO geometry double-sided)", createResult);

    ScopedShaderModule fullscreenVertex(device), occlusionFragment(device), blurFragment(device);
    result = readModule("rendering/shaders/fullscreen_triangle.vert.spv", fullscreenVertex.module);
    if (!result) return addContext("Failed to create fullscreen AO vertex shader", result);
    result = readModule("rendering/shaders/ambient_occlusion.frag.spv", occlusionFragment.module);
    if (!result) return addContext("Failed to create AO evaluation fragment shader", result);
    result = readModule("rendering/shaders/ambient_occlusion_blur.frag.spv", blurFragment.module);
    if (!result) return addContext("Failed to create AO blur fragment shader", result);
    VkPipelineShaderStageCreateInfo fullscreenStages[2]{};
    fullscreenStages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                           VK_SHADER_STAGE_VERTEX_BIT, fullscreenVertex.module, "main"};
    fullscreenStages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                           VK_SHADER_STAGE_FRAGMENT_BIT, occlusionFragment.module, "main"};
    VkPipelineVertexInputStateCreateInfo fullscreenInput{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineRasterizationStateCreateInfo fullscreenRaster{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    fullscreenRaster.polygonMode = VK_POLYGON_MODE_FILL;
    fullscreenRaster.lineWidth = 1.0f;
    fullscreenRaster.cullMode = VK_CULL_MODE_NONE;
    fullscreenRaster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    VkPipelineMultisampleStateCreateInfo fullscreenSamples{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    fullscreenSamples.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo fullscreenDepth{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    fullscreenDepth.depthTestEnable = VK_FALSE;
    fullscreenDepth.depthWriteEnable = VK_FALSE;
    VkPipelineLayoutCreateInfo fullscreenLayoutInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    fullscreenLayoutInfo.setLayoutCount = 1;
    fullscreenLayoutInfo.pSetLayouts = &ambientOcclusionDescriptorSetLayout;
    createResult = vkCreatePipelineLayout(device, &fullscreenLayoutInfo, nullptr,
                                          &ambientOcclusionPipelineLayout);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreatePipelineLayout(AO)", createResult);
    createResult = vkCreatePipelineLayout(device, &fullscreenLayoutInfo, nullptr,
                                          &ambientOcclusionBlurPipelineLayout);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreatePipelineLayout(AO blur)", createResult);
    VkGraphicsPipelineCreateInfo fullscreenInfo{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    fullscreenInfo.stageCount = 2;
    fullscreenInfo.pStages = fullscreenStages;
    fullscreenInfo.pVertexInputState = &fullscreenInput;
    fullscreenInfo.pInputAssemblyState = &assembly;
    fullscreenInfo.pViewportState = &viewport;
    fullscreenInfo.pRasterizationState = &fullscreenRaster;
    fullscreenInfo.pMultisampleState = &fullscreenSamples;
    fullscreenInfo.pDepthStencilState = &fullscreenDepth;
    fullscreenInfo.pColorBlendState = &blend;
    fullscreenInfo.pDynamicState = &dynamic;
    fullscreenInfo.layout = ambientOcclusionPipelineLayout;
    fullscreenInfo.renderPass = ambientOcclusionRenderPass;
    createResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &fullscreenInfo, nullptr,
                                             &ambientOcclusionPipeline);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreateGraphicsPipelines(AO)", createResult);
    fullscreenStages[1].module = blurFragment.module;
    fullscreenInfo.pStages = fullscreenStages;
    fullscreenInfo.layout = ambientOcclusionBlurPipelineLayout;
    fullscreenInfo.renderPass = ambientOcclusionBlurRenderPass;
    createResult = vkCreateGraphicsPipelines(device, VK_NULL_HANDLE, 1, &fullscreenInfo, nullptr,
                                             &ambientOcclusionBlurPipeline);
    if (createResult != VK_SUCCESS) return vkFailure("vkCreateGraphicsPipelines(AO blur)", createResult);
    return Result::success();
}

Result VulkanContext::createSelectionOutlinePipeline() {
    constexpr const char* vertexShaderPath =
        "rendering/shaders/selection_outline.vert.spv";
    constexpr const char* fragmentShaderPath =
        "rendering/shaders/selection_outline.frag.spv";

    std::vector<char> vertexShaderCode;
    Result result = readFile(vertexShaderPath, vertexShaderCode);
    if (!result) {
        return addContext("Failed to read selection outline vertex shader", result);
    }
    ScopedShaderModule vertexShaderModule(device);
    result = createShaderModule(vertexShaderCode, vertexShaderModule.module);
    if (!result) {
        return addContext("Failed to create selection outline vertex shader module",
                          result);
    }

    std::vector<char> fragmentShaderCode;
    result = readFile(fragmentShaderPath, fragmentShaderCode);
    if (!result) {
        return addContext("Failed to read selection outline fragment shader", result);
    }
    ScopedShaderModule fragmentShaderModule(device);
    result = createShaderModule(fragmentShaderCode, fragmentShaderModule.module);
    if (!result) {
        return addContext("Failed to create selection outline fragment shader module",
                          result);
    }

    VkPipelineShaderStageCreateInfo vertexStage{};
    vertexStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    vertexStage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    vertexStage.module = vertexShaderModule.module;
    vertexStage.pName = "main";

    VkPipelineShaderStageCreateInfo fragmentStage{};
    fragmentStage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    fragmentStage.stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    fragmentStage.module = fragmentShaderModule.module;
    fragmentStage.pName = "main";
    const VkPipelineShaderStageCreateInfo shaderStages[] = {vertexStage,
                                                             fragmentStage};

    const auto bindingDescription = Vertex::getBindingDescription();
    const auto attributeDescriptions = Vertex::getAttributeDescriptions();
    VkPipelineVertexInputStateCreateInfo vertexInput{};
    vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    vertexInput.vertexBindingDescriptionCount = 1;
    vertexInput.pVertexBindingDescriptions = &bindingDescription;
    vertexInput.vertexAttributeDescriptionCount =
        static_cast<uint32_t>(attributeDescriptions.size());
    vertexInput.pVertexAttributeDescriptions = attributeDescriptions.data();

    VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
    inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewportState{};
    viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewportState.viewportCount = 1;
    viewportState.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo rasterizer{};
    rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rasterizer.depthClampEnable = VK_FALSE;
    rasterizer.rasterizerDiscardEnable = VK_FALSE;
    rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
    rasterizer.cullMode = VK_CULL_MODE_FRONT_BIT;
    rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rasterizer.depthBiasEnable = VK_FALSE;
    rasterizer.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisampling{};
    multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisampling.sampleShadingEnable =
        sampleRateShadingEnabled ? VK_TRUE : VK_FALSE;
    multisampling.minSampleShading =
        sampleRateShadingEnabled ? 0.2f : 0.0f;
    multisampling.rasterizationSamples = msaaSamples;

    VkPipelineDepthStencilStateCreateInfo depthStencil{};
    depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depthStencil.depthTestEnable = VK_TRUE;
    depthStencil.depthWriteEnable = VK_FALSE;
    depthStencil.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
    depthStencil.depthBoundsTestEnable = VK_FALSE;
    depthStencil.stencilTestEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState colorBlendAttachment{};
    colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
        VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
        VK_COLOR_COMPONENT_A_BIT;
    colorBlendAttachment.blendEnable = VK_FALSE;
    VkPipelineColorBlendStateCreateInfo colorBlending{};
    colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    colorBlending.logicOpEnable = VK_FALSE;
    colorBlending.attachmentCount = 1;
    colorBlending.pAttachments = &colorBlendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT,
                                              VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamicState{};
    dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamicState.dynamicStateCount = 2;
    dynamicState.pDynamicStates = dynamicStates;

    VkPhysicalDeviceProperties deviceProperties{};
    vkGetPhysicalDeviceProperties(physicalDevice, &deviceProperties);
    if (sizeof(OutlinePushConstants) >
        deviceProperties.limits.maxPushConstantsSize) {
        return Result::failure(
            "Selection outline push constants exceed the device limit");
    }

    VkPushConstantRange pushConstantRange{};
    pushConstantRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
        VK_SHADER_STAGE_FRAGMENT_BIT;
    pushConstantRange.size = sizeof(OutlinePushConstants);
    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &descriptorSetLayout;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushConstantRange;

    VkResult createResult = vkCreatePipelineLayout(
        device, &pipelineLayoutInfo, nullptr, &selectionOutlinePipelineLayout);
    if (createResult != VK_SUCCESS) {
        selectionOutlinePipelineLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreatePipelineLayout(selection outline)", createResult);
    }

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = shaderStages;
    pipelineInfo.pVertexInputState = &vertexInput;
    pipelineInfo.pInputAssemblyState = &inputAssembly;
    pipelineInfo.pViewportState = &viewportState;
    pipelineInfo.pRasterizationState = &rasterizer;
    pipelineInfo.pMultisampleState = &multisampling;
    pipelineInfo.pDepthStencilState = &depthStencil;
    pipelineInfo.pColorBlendState = &colorBlending;
    pipelineInfo.pDynamicState = &dynamicState;
    pipelineInfo.layout = selectionOutlinePipelineLayout;
    pipelineInfo.renderPass = renderPass;
    pipelineInfo.subpass = 0;

    createResult = vkCreateGraphicsPipelines(
        device, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
        &selectionOutlinePipeline);
    if (createResult != VK_SUCCESS) {
        selectionOutlinePipeline = VK_NULL_HANDLE;
        vkDestroyPipelineLayout(device, selectionOutlinePipelineLayout, nullptr);
        selectionOutlinePipelineLayout = VK_NULL_HANDLE;
        return vkFailure("vkCreateGraphicsPipelines(selection outline)",
                         createResult);
    }

    spdlog::info("Successfully created selection outline pipeline");
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
    Result validationResult = validateTextureData(gameObject);
    if (!validationResult) {
        return validationResult;
    }

    for (auto& instance : gameObject->meshInstances_) {
        Material& material = instance.material;
        if (!material.normalMapPixels) {
            material.normalMapPixels = static_cast<stbi_uc*>(std::malloc(4));
            if (!material.normalMapPixels) {
                return Result::failure("Failed to allocate neutral normal map");
            }
            try {
                material.normalMapPixelsOwner =
                    makeStbiPixelOwner(material.normalMapPixels);
            } catch (...) {
                stbi_image_free(material.normalMapPixels);
                material.normalMapPixels = nullptr;
                return Result::failure(
                    "Failed to create neutral normal-map ownership");
            }
            material.normalMapPixels[0] = 128;
            material.normalMapPixels[1] = 128;
            material.normalMapPixels[2] = 255;
            material.normalMapPixels[3] = 255;
            material.normalMapWidth = 1;
            material.normalMapHeight = 1;
            material.normalMapChannels = 4;
            material.normalMapMipLevels = 1;
        }
        if (!material.metallicRoughnessMapPixels) {
            material.metallicRoughnessMapPixels =
                static_cast<stbi_uc*>(std::malloc(4));
            if (!material.metallicRoughnessMapPixels) {
                return Result::failure(
                    "Failed to allocate neutral metallic-roughness map");
            }
            try {
                material.metallicRoughnessMapPixelsOwner =
                    makeStbiPixelOwner(material.metallicRoughnessMapPixels);
            } catch (...) {
                stbi_image_free(material.metallicRoughnessMapPixels);
                material.metallicRoughnessMapPixels = nullptr;
                return Result::failure(
                    "Failed to create neutral metallic-roughness ownership");
            }
            material.metallicRoughnessMapPixels[0] = 255;
            material.metallicRoughnessMapPixels[1] = 255;
            material.metallicRoughnessMapPixels[2] = 255;
            material.metallicRoughnessMapPixels[3] = 255;
            material.metallicRoughnessMapWidth = 1;
            material.metallicRoughnessMapHeight = 1;
            material.metallicRoughnessMapChannels = 4;
            material.metallicRoughnessMapMipLevels = 1;
        }

        auto uploadTexture = [&](StbiPixelOwner& pixelsOwner,
                                 stbi_uc*& pixels, int width, int height,
                                 uint32_t mipLevels, VkFormat format,
                                 VkImage& image, VkDeviceMemory& memory,
                                 const char* label) -> Result {
            const VkDeviceSize imageSize =
                static_cast<VkDeviceSize>(width) *
                static_cast<VkDeviceSize>(height) * 4;
            auto releasePixels = [&pixelsOwner, &pixels]() {
                releaseStbiPixel(pixelsOwner, pixels);
            };
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
                releasePixels();
                return addContext(std::string("Failed to create ") + label +
                                      " staging buffer", result);
            }
            void* data = nullptr;
            const VkResult mapResult = vkMapMemory(
                device, staging.memory, 0, imageSize, 0, &data);
            if (mapResult != VK_SUCCESS) {
                releasePixels();
                return vkFailure(std::string("vkMapMemory(") + label + ")",
                                 mapResult);
            }
            memcpy(data, pixels, static_cast<size_t>(imageSize));
            vkUnmapMemory(device, staging.memory);
            releasePixels();

            if (image != VK_NULL_HANDLE || memory != VK_NULL_HANDLE) {
                return Result::failure(std::string(label) +
                                       " image slots already contain Vulkan resources");
            }
            const size_t ownershipIndex = ownedSceneImages.size();
            ownedSceneImages.push_back({&image, &memory});
            result = createImage(
                width, height, mipLevels, VK_SAMPLE_COUNT_1_BIT, format,
                VK_IMAGE_TILING_OPTIMAL, VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                    VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, image, memory,
                &ownedSceneImages[ownershipIndex]);
            if (!result) return addContext(std::string("Failed to create ") + label, result);
            result = transitionImageLayout(image, format, VK_IMAGE_LAYOUT_UNDEFINED,
                                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                                           mipLevels);
            if (!result) return result;
            result = copyBufferToImage(staging.buffer, image,
                                       static_cast<uint32_t>(width),
                                       static_cast<uint32_t>(height));
            if (!result) return result;
            return generateMipmaps(image, format, width, height, mipLevels);
        };

        Result result = uploadTexture(
            material.pixelsOwner, material.pixels, material.texWidth,
            material.texHeight, material.mipLevels, VK_FORMAT_R8G8B8A8_SRGB,
            material.textureImage, material.textureImageMemory, "texture image");
        if (!result) return result;
        result = uploadTexture(
            material.normalMapPixelsOwner, material.normalMapPixels,
            material.normalMapWidth, material.normalMapHeight,
            material.normalMapMipLevels, VK_FORMAT_R8G8B8A8_UNORM,
            material.normalMapImage, material.normalMapImageMemory,
            "normal-map image");
        if (!result) return result;
        result = uploadTexture(
            material.metallicRoughnessMapPixelsOwner,
            material.metallicRoughnessMapPixels,
            material.metallicRoughnessMapWidth,
            material.metallicRoughnessMapHeight,
            material.metallicRoughnessMapMipLevels, VK_FORMAT_R8G8B8A8_UNORM,
            material.metallicRoughnessMapImage,
            material.metallicRoughnessMapImageMemory,
            "metallic-roughness image");
        if (!result) return result;
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
        if (instance.material.textureImageView != VK_NULL_HANDLE ||
            instance.material.normalMapImageView != VK_NULL_HANDLE ||
            instance.material.metallicRoughnessMapImageView !=
                VK_NULL_HANDLE) {
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
        const size_t normalOwnershipIndex = ownedSceneImageViews.size();
        ownedSceneImageViews.push_back({&instance.material.normalMapImageView});
        result = createImageView(
            instance.material.normalMapImage, VK_FORMAT_R8G8B8A8_UNORM,
            VK_IMAGE_ASPECT_COLOR_BIT, instance.material.normalMapMipLevels,
            instance.material.normalMapImageView,
            &ownedSceneImageViews[normalOwnershipIndex]);
        if (!result) {
            return addContext("Failed to create normal-map image view", result);
        }
        const size_t metallicRoughnessOwnershipIndex =
            ownedSceneImageViews.size();
        ownedSceneImageViews.push_back(
            {&instance.material.metallicRoughnessMapImageView});
        result = createImageView(
            instance.material.metallicRoughnessMapImage,
            VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_ASPECT_COLOR_BIT,
            instance.material.metallicRoughnessMapMipLevels,
            instance.material.metallicRoughnessMapImageView,
            &ownedSceneImageViews[metallicRoughnessOwnershipIndex]);
        if (!result) {
            return addContext(
                "Failed to create metallic-roughness image view", result);
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
        if (instance.material.textureSampler != VK_NULL_HANDLE ||
            instance.material.normalMapSampler != VK_NULL_HANDLE ||
            instance.material.metallicRoughnessMapSampler != VK_NULL_HANDLE) {
            return Result::failure(
                "Texture sampler slot already contains a Vulkan resource");
        }
        auto createSampler = [&](VkSampler& sampler, uint32_t mipLevels,
                                 const char* label) -> Result {
            float maxLod = 0.0f;
            Result mipResult =
                renderer_configuration::samplerMaxLod(mipLevels, maxLod);
            if (!mipResult) {
                return addContext(std::string("Invalid mip count for ") +
                                      label + " sampler",
                                  mipResult);
            }
            const size_t ownershipIndex = ownedSceneSamplers.size();
            ownedSceneSamplers.push_back({&sampler});
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
            samplerInfo.maxLod = maxLod;

            const VkResult result = vkCreateSampler(
                device, &samplerInfo, nullptr, &sampler);
            if (result != VK_SUCCESS) {
                sampler = VK_NULL_HANDLE;
                return vkFailure(
                    std::string("vkCreateSampler(") + label + ")", result);
            }
            ownedSceneSamplers[ownershipIndex].sampler = sampler;
            return Result::success();
        };

        Result result = createSampler(instance.material.textureSampler,
                                      instance.material.mipLevels, "texture");
        if (!result) return result;
        result = createSampler(instance.material.normalMapSampler,
                               instance.material.normalMapMipLevels,
                               "normal map");
        if (!result) return result;
        result = createSampler(
            instance.material.metallicRoughnessMapSampler,
            instance.material.metallicRoughnessMapMipLevels,
            "metallic-roughness map");
        if (!result) return result;
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

    std::array<VkDescriptorPoolSize, 2> poolSizes{};

    const directional_shadow::DescriptorPoolRequirements shadowPool =
        directional_shadow::descriptorPoolRequirements(MAX_FRAMES_IN_FLIGHT);
    const ambient_occlusion::DescriptorPoolRequirements aoPool =
        ambient_occlusion::descriptorPoolRequirements(MAX_FRAMES_IN_FLIGHT);

    // Per-object UBOs plus lighting and directional-shadow UBOs per frame.
    poolSizes[0].type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    poolSizes[0].descriptorCount =
        static_cast<uint32_t>(numOfObjects * MAX_FRAMES_IN_FLIGHT) +
        MAX_FRAMES_IN_FLIGHT + shadowPool.uniformBuffers + aoPool.uniformBuffers;

    // Texture samplers 
    poolSizes[1].type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    poolSizes[1].descriptorCount =
        static_cast<uint32_t>(numOfObjects * MAX_FRAMES_IN_FLIGHT * 3) +
        shadowPool.combinedImageSamplers + aoPool.combinedImageSamplers;

    VkDescriptorPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    poolInfo.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
    poolInfo.poolSizeCount = static_cast<uint32_t>(poolSizes.size());
    poolInfo.pPoolSizes = poolSizes.data();
    poolInfo.maxSets =
        static_cast<uint32_t>(numOfObjects * MAX_FRAMES_IN_FLIGHT) +
        MAX_FRAMES_IN_FLIGHT + shadowPool.descriptorSets + aoPool.descriptorSets;

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

            VkDescriptorImageInfo normalImageInfo{};
            normalImageInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            normalImageInfo.imageView = instance.material.normalMapImageView;
            normalImageInfo.sampler = instance.material.normalMapSampler;

            VkDescriptorImageInfo metallicRoughnessImageInfo{};
            metallicRoughnessImageInfo.imageLayout =
                VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            metallicRoughnessImageInfo.imageView =
                instance.material.metallicRoughnessMapImageView;
            metallicRoughnessImageInfo.sampler =
                instance.material.metallicRoughnessMapSampler;

            std::array<VkWriteDescriptorSet, 4> descriptorWrites{};

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

            descriptorWrites[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[2].dstSet = instance.renderData.descriptorSets[i];
            descriptorWrites[2].dstBinding = 2;
            descriptorWrites[2].dstArrayElement = 0;
            descriptorWrites[2].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[2].descriptorCount = 1;
            descriptorWrites[2].pImageInfo = &normalImageInfo;

            descriptorWrites[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            descriptorWrites[3].dstSet = instance.renderData.descriptorSets[i];
            descriptorWrites[3].dstBinding = 3;
            descriptorWrites[3].dstArrayElement = 0;
            descriptorWrites[3].descriptorType =
                VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            descriptorWrites[3].descriptorCount = 1;
            descriptorWrites[3].pImageInfo = &metallicRoughnessImageInfo;

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
    Result result = createRenderFinishedSemaphores();
    if (!result) {
        return result;
    }
    spdlog::info("Successfully created synchronization objects");
    return Result::success();
}

Result VulkanContext::createRenderFinishedSemaphores() {
    destroyRenderFinishedSemaphores();
    renderFinishedSemaphores.resize(swapchainImages.size(), VK_NULL_HANDLE);

    VkSemaphoreCreateInfo semaphoreInfo{};
    semaphoreInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
    for (size_t i = 0; i < renderFinishedSemaphores.size(); ++i) {
        const VkResult result = vkCreateSemaphore(
            device, &semaphoreInfo, nullptr,
            &renderFinishedSemaphores[i]);
        if (result != VK_SUCCESS) {
            renderFinishedSemaphores[i] = VK_NULL_HANDLE;
            destroyRenderFinishedSemaphores();
            return addContext(
                "Failed to create render-finished semaphore for swapchain "
                "image " + std::to_string(i),
                vkFailure("vkCreateSemaphore", result));
        }
    }
    return Result::success();
}

void VulkanContext::destroyRenderFinishedSemaphores() noexcept {
    if (device != VK_NULL_HANDLE) {
        for (VkSemaphore& semaphore : renderFinishedSemaphores) {
            if (semaphore != VK_NULL_HANDLE) {
                vkDestroySemaphore(device, semaphore, nullptr);
                semaphore = VK_NULL_HANDLE;
            }
        }
    }
    renderFinishedSemaphores.clear();
}

void VulkanContext::cleanupSwapchain() noexcept {
    if (device == VK_NULL_HANDLE) {
        return;
    }

    // These per-frame targets are extent dependent; descriptors are rewritten
    // after recreation before another frame can use them.
    destroyAmbientOcclusionExtentResources();

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
    const std::unique_ptr<GameObject>& gameObject, const glm::mat4& view,
    const glm::mat4& projection, const glm::vec3& cameraPosition) {
    for (auto& instance : gameObject->meshInstances_) {
        const UniformBufferObject ubo = makeUniformBufferObject(
            editor_picking::makeModelMatrix(gameObject->position,
                                             gameObject->rotation,
                                             gameObject->scale),
            view, projection, cameraPosition);
        memcpy(instance.renderData.uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));

    }
}

Result VulkanContext::updateLightsUniformBuffer(Scene* scene) {
    LightsUBO lightsUbo{};
    const glm::vec3& ambientColor = scene->ambientColor();
    lightsUbo.ambientColorIntensity =
        glm::vec4(ambientColor, scene->ambientIntensity());
    lightsUbo.numLights =
        static_cast<std::int32_t>(scene->pointLightCount());
    for (std::size_t i = 0; i < scene->pointLightCount(); ++i) {
        const PointLight& light = scene->pointLightAt(i);
        lightsUbo.lights[i].position = light.position;
        lightsUbo.lights[i].color = light.color;
        lightsUbo.lights[i].intensity = light.intensity;
    }
    lightsUbo.directionalLight.directionEnabled = glm::vec4(0.0f);
    lightsUbo.directionalLight.colorIntensity = glm::vec4(0.0f);

    const DirectionalLight* directionalLight = scene->directionalLight();
    if (directionalLight != nullptr) {
        glm::vec3 normalizedDirection;
        Result directionalResult = normalizeDirectionalLightDirection(
            *directionalLight, normalizedDirection);
        if (!directionalResult) {
            return directionalResult;
        }

        lightsUbo.directionalLight.directionEnabled = glm::vec4(
            normalizedDirection, 1.0f);
        lightsUbo.directionalLight.colorIntensity = glm::vec4(
            directionalLight->color, directionalLight->intensity);
    }

    if (lightsBufferMapped[currentFrame] == nullptr) {
        return Result::failure(
            "Current-frame lights uniform buffer is not mapped");
    }
    memcpy(lightsBufferMapped[currentFrame], &lightsUbo,
           sizeof(LightsUBO));
    return Result::success();
}

Result VulkanContext::updateDirectionalShadowUniformBuffer(Scene* scene) {
    if (!directional_shadow::shouldRender(scene->directionalLight())) {
        return Result::success();
    }
    auto& frame = directionalShadowFrames[currentFrame];
    if (frame.transformBufferMapped == nullptr) {
        return Result::failure(
            "Current-frame directional-shadow uniform buffer is not mapped");
    }
    directional_shadow::LightMatrices matrices;
    Result result = directional_shadow::calculateLightMatrices(
        *scene->directionalLight(), matrices);
    if (!result) return result;
    const DirectionalShadowUBO ubo{matrices.viewProjection};
    memcpy(frame.transformBufferMapped, &ubo, sizeof(ubo));
    return Result::success();
}

Result VulkanContext::updateAmbientOcclusionUniformBuffer(
    const glm::mat4& projection) {
    Result settingsResult = ambient_occlusion::validate(ambientOcclusionSettings);
    if (!settingsResult) return settingsResult;
    const glm::mat4 inverseProjection = glm::inverse(projection);
    if (!ambient_occlusion::isFiniteMatrix(projection) ||
        !ambient_occlusion::isFiniteMatrix(inverseProjection)) {
        return Result::failure("Ambient-occlusion projection matrices are not finite");
    }
    auto& frame = ambientOcclusionFrames[currentFrame];
    if (frame.uniformBufferMapped == nullptr) {
        return Result::failure("Current-frame ambient-occlusion uniform buffer is not mapped");
    }
    AmbientOcclusionUBO ubo{};
    ubo.projection = projection;
    ubo.inverseProjection = inverseProjection;
    for (size_t i = 0; i < ambientOcclusionKernel.size(); ++i) {
        ubo.samples[i] = glm::vec4(ambientOcclusionKernel[i], 0.0f);
    }
    ubo.parameters = glm::vec4(ambientOcclusionSettings.radius,
                               ambientOcclusionSettings.bias,
                               ambientOcclusionSettings.power,
                               ambientOcclusionSettings.enabled ? 1.0f : 0.0f);
    const float width = static_cast<float>(swapchainExtent.width);
    const float height = static_cast<float>(swapchainExtent.height);
    ubo.viewport = glm::vec4(width, height, 1.0f / width, 1.0f / height);
    memcpy(frame.uniformBufferMapped, &ubo, sizeof(ubo));
    return Result::success();
}

Result VulkanContext::initializeImGui() {
    if (!graphicsQueueFamily.has_value()) {
        return Result::failure(
            "Graphics queue-family index is unavailable for Dear ImGui");
    }

    return imguiLayer.initialize(
        window, instance, physicalDevice, device,
        graphicsQueueFamily.value(), graphicsQueue, renderPass, msaaSamples,
        swapchainMinimumImageCount,
        static_cast<uint32_t>(swapchainImages.size()));
}

void VulkanContext::processEvent(const SDL_Event& event) noexcept {
    imguiLayer.processEvent(event);
    if (event.type == SDL_EVENT_WINDOW_RESIZED ||
        event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED) {
        framebufferResized = true;
    }
}

void VulkanContext::setImGuiInputEnabled(bool enabled) noexcept {
    imguiLayer.setInputEnabled(enabled);
}

void VulkanContext::clearEditorSelection() noexcept {
    imguiLayer.clearSelection();
}

EditorCommand VulkanContext::consumeEditorCommand() noexcept {
    return imguiLayer.consumeEditorCommand();
}

bool VulkanContext::sceneInteractionAreaHovered() const noexcept {
    return imguiLayer.sceneInteractionAreaHovered();
}

Result VulkanContext::drawFrame(Scene* scene, const Camera& renderCamera,
                                SceneRunState runState) {
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
    if (imageIndex >= renderFinishedSemaphores.size()) {
        return Result::failure(
            "Acquired swapchain image has no render-finished semaphore");
    }
    VkSemaphore renderFinished = renderFinishedSemaphores[imageIndex];

    result = vkResetFences(device, 1, &inFlightFences[currentFrame]);
    if (result != VK_SUCCESS) {
        return vkFailure("vkResetFences", result);
    }

    result = vkResetCommandBuffer(commandBuffers[currentFrame], 0);
    if (result != VK_SUCCESS) {
        return vkFailure("vkResetCommandBuffer", result);
    }

    Result imguiResult = imguiLayer.beginFrame(runState);
    if (!imguiResult) {
        return Result::failure("Failed to begin Dear ImGui frame: " +
                               imguiResult.error());
    }
    const glm::mat4 view = glm::lookAt(
        renderCamera.position, renderCamera.position + renderCamera.front,
        renderCamera.up);
    glm::mat4 projection = glm::perspective(
        glm::radians(45.0f),
        swapchainExtent.width / static_cast<float>(swapchainExtent.height),
        0.1f, 10000.0f);
    projection[1][1] *= -1.0f;
    imguiLayer.drawEditor(scene, view, projection, runState);
    imguiLayer.finishFrame();

    for (const auto& obj : scene->gameObjects()) {
        updateUniformBuffer(currentFrame, obj, view, projection,
                            renderCamera.position);
    }

    Result lightsResult = updateLightsUniformBuffer(scene);
    if (!lightsResult) {
        return lightsResult;
    }

    Result shadowResult = updateDirectionalShadowUniformBuffer(scene);
    if (!shadowResult) {
        return shadowResult;
    }

    Result aoResult = updateAmbientOcclusionUniformBuffer(projection);
    if (!aoResult) {
        return aoResult;
    }

    Result recordResult = recordCommandBuffer(
        commandBuffers[currentFrame], imageIndex, scene, runState);
    if (!recordResult) {
        return recordResult;
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

    VkSemaphore signalSemaphores[] = {renderFinished};
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
                                          Scene* scene,
                                          SceneRunState runState) {
    if (!scene) {
        return Result::failure(
            "Cannot record a command buffer for a null scene");
    }

    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;

    VkResult result = vkBeginCommandBuffer(commandBuffer, &beginInfo);
    if (result != VK_SUCCESS) {
        return vkFailure("vkBeginCommandBuffer", result);
    }

    if (directional_shadow::shouldRender(scene->directionalLight())) {
        const auto& shadowFrame = directionalShadowFrames[currentFrame];
        if (shadowFrame.framebuffer == VK_NULL_HANDLE ||
            shadowFrame.descriptorSet == VK_NULL_HANDLE) {
            return Result::failure("Current-frame directional-shadow resources are unavailable");
        }
        VkRenderPassBeginInfo shadowPassInfo{};
        shadowPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        shadowPassInfo.renderPass = directionalShadowRenderPass;
        shadowPassInfo.framebuffer = shadowFrame.framebuffer;
        shadowPassInfo.renderArea.offset = {0, 0};
        shadowPassInfo.renderArea.extent = {directional_shadow::mapResolution,
                                           directional_shadow::mapResolution};
        VkClearValue shadowClear{};
        shadowClear.depthStencil = {1.0f, 0};
        shadowPassInfo.clearValueCount = 1;
        shadowPassInfo.pClearValues = &shadowClear;
        vkCmdBeginRenderPass(commandBuffer, &shadowPassInfo,
                             VK_SUBPASS_CONTENTS_INLINE);
        VkViewport shadowViewport{};
        shadowViewport.width = static_cast<float>(directional_shadow::mapResolution);
        shadowViewport.height = static_cast<float>(directional_shadow::mapResolution);
        shadowViewport.minDepth = 0.0f;
        shadowViewport.maxDepth = 1.0f;
        vkCmdSetViewport(commandBuffer, 0, 1, &shadowViewport);
        VkRect2D shadowScissor{};
        shadowScissor.extent = {directional_shadow::mapResolution,
                                directional_shadow::mapResolution};
        vkCmdSetScissor(commandBuffer, 0, 1, &shadowScissor);
        vkCmdSetDepthBias(commandBuffer, directional_shadow::depthBiasConstantFactor,
                          directional_shadow::depthBiasClamp,
                          directional_shadow::depthBiasSlopeFactor);
        vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                directionalShadowPipelineLayout, 1, 1,
                                &shadowFrame.descriptorSet, 0, nullptr);
        VkPipeline boundShadowPipeline = VK_NULL_HANDLE;
        for (const auto& obj : scene->gameObjects()) {
            for (const auto& instance : obj->meshInstances_) {
                if (instance.material.alphaMode == MaterialAlphaMode::Blend) {
                    continue;
                }
                const VkPipeline pipeline = instance.material.doubleSided
                    ? directionalShadowDoubleSidedPipeline
                    : directionalShadowPipeline;
                if (pipeline != boundShadowPipeline) {
                    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                      pipeline);
                    boundShadowPipeline = pipeline;
                }
                VkBuffer vertexBuffer[] = {instance.mesh.vertexBuffer};
                VkDeviceSize offset[] = {0};
                vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffer, offset);
                vkCmdBindIndexBuffer(commandBuffer, instance.mesh.indexBuffer, 0,
                                     VK_INDEX_TYPE_UINT32);
                vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                        directionalShadowPipelineLayout, 0, 1,
                                        &instance.renderData.descriptorSets[currentFrame],
                                        0, nullptr);
                const MaterialPushConstants materialPushConstants{
                    instance.material.baseColorFactor, instance.material.metallicFactor,
                    instance.material.roughnessFactor,
                    static_cast<std::int32_t>(instance.material.alphaMode),
                    instance.material.alphaCutoff,
                    instance.material.normalMapEnabled ? 1 : 0,
                    instance.material.hasMetallicRoughnessMap ? 1 : 0};
                vkCmdPushConstants(commandBuffer, directionalShadowPipelineLayout,
                                   VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                                   sizeof(MaterialPushConstants),
                                   &materialPushConstants);
                vkCmdDrawIndexed(commandBuffer,
                                 static_cast<uint32_t>(instance.mesh.indices.size()),
                                 1, 0, 0, 0);
            }
        }
        vkCmdEndRenderPass(commandBuffer);
    }

    const auto& aoFrame = ambientOcclusionFrames[currentFrame];
    if (aoFrame.geometryFramebuffer == VK_NULL_HANDLE ||
        aoFrame.rawFramebuffer == VK_NULL_HANDLE ||
        aoFrame.blurFramebuffer == VK_NULL_HANDLE ||
        aoFrame.descriptorSet == VK_NULL_HANDLE) {
        return Result::failure("Current-frame ambient-occlusion resources are unavailable");
    }
    VkViewport aoViewport{};
    aoViewport.width = static_cast<float>(swapchainExtent.width);
    aoViewport.height = static_cast<float>(swapchainExtent.height);
    aoViewport.minDepth = 0.0f;
    aoViewport.maxDepth = 1.0f;
    VkRect2D aoScissor{};
    aoScissor.extent = swapchainExtent;

    VkRenderPassBeginInfo geometryPass{};
    geometryPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    geometryPass.renderPass = ambientOcclusionGeometryRenderPass;
    geometryPass.framebuffer = aoFrame.geometryFramebuffer;
    geometryPass.renderArea.extent = swapchainExtent;
    std::array<VkClearValue, 2> geometryClears{};
    geometryClears[0].color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    geometryClears[1].depthStencil = {1.0f, 0};
    geometryPass.clearValueCount = static_cast<uint32_t>(geometryClears.size());
    geometryPass.pClearValues = geometryClears.data();
    vkCmdBeginRenderPass(commandBuffer, &geometryPass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(commandBuffer, 0, 1, &aoViewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &aoScissor);
    VkPipeline boundGeometryPipeline = VK_NULL_HANDLE;
    for (const auto& obj : scene->gameObjects()) {
        for (const auto& instance : obj->meshInstances_) {
            if (instance.material.alphaMode == MaterialAlphaMode::Blend) continue;
            const VkPipeline pipeline = instance.material.doubleSided
                ? ambientOcclusionGeometryDoubleSidedPipeline
                : ambientOcclusionGeometryPipeline;
            if (pipeline != boundGeometryPipeline) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                boundGeometryPipeline = pipeline;
            }
            const VkBuffer vertexBuffer[] = {instance.mesh.vertexBuffer};
            const VkDeviceSize offset[] = {0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffer, offset);
            vkCmdBindIndexBuffer(commandBuffer, instance.mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ambientOcclusionGeometryPipelineLayout, 0, 1,
                                    &instance.renderData.descriptorSets[currentFrame], 0, nullptr);
            const MaterialPushConstants materialPushConstants{
                instance.material.baseColorFactor, instance.material.metallicFactor,
                instance.material.roughnessFactor,
                static_cast<std::int32_t>(instance.material.alphaMode),
                instance.material.alphaCutoff,
                instance.material.normalMapEnabled ? 1 : 0,
                instance.material.hasMetallicRoughnessMap ? 1 : 0};
            vkCmdPushConstants(commandBuffer, ambientOcclusionGeometryPipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(MaterialPushConstants), &materialPushConstants);
            vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(instance.mesh.indices.size()),
                             1, 0, 0, 0);
        }
    }
    vkCmdEndRenderPass(commandBuffer);

    VkClearValue aoClear{};
    aoClear.color = {{1.0f, 1.0f, 1.0f, 1.0f}};
    VkRenderPassBeginInfo aoPass{};
    aoPass.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    aoPass.renderPass = ambientOcclusionRenderPass;
    aoPass.framebuffer = aoFrame.rawFramebuffer;
    aoPass.renderArea.extent = swapchainExtent;
    aoPass.clearValueCount = 1;
    aoPass.pClearValues = &aoClear;
    vkCmdBeginRenderPass(commandBuffer, &aoPass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(commandBuffer, 0, 1, &aoViewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &aoScissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ambientOcclusionPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ambientOcclusionPipelineLayout, 0, 1,
                            &aoFrame.descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);

    aoPass.renderPass = ambientOcclusionBlurRenderPass;
    aoPass.framebuffer = aoFrame.blurFramebuffer;
    vkCmdBeginRenderPass(commandBuffer, &aoPass, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdSetViewport(commandBuffer, 0, 1, &aoViewport);
    vkCmdSetScissor(commandBuffer, 0, 1, &aoScissor);
    vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, ambientOcclusionBlurPipeline);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            ambientOcclusionBlurPipelineLayout, 0, 1,
                            &aoFrame.descriptorSet, 0, nullptr);
    vkCmdDraw(commandBuffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(commandBuffer);

    VkRenderPassBeginInfo renderPassInfo{};
    renderPassInfo.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    renderPassInfo.renderPass = renderPass;
    renderPassInfo.framebuffer = swapchainFramebuffers[imageIndex];
    renderPassInfo.renderArea.offset = {0, 0};
    renderPassInfo.renderArea.extent = swapchainExtent;

    // VkClearValue clearColor = {{{0.0f, 0.0f, 0.0f, 1.0f}}};
    std::array<VkClearValue, 2> clearValues{};
    const glm::vec4& backgroundColor = scene->backgroundColor();
    clearValues[0].color = {{backgroundColor.r, backgroundColor.g,
                             backgroundColor.b, backgroundColor.a}};

    clearValues[1].depthStencil = {1.0f, 0};
    renderPassInfo.clearValueCount = static_cast<uint32_t>(clearValues.size());
    renderPassInfo.pClearValues = clearValues.data();

    vkCmdBeginRenderPass(commandBuffer, &renderPassInfo,
                         VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 1, 1,
                            &lightsDescriptorSets[currentFrame], 0, nullptr);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 2, 1,
                            &directionalShadowFrames[currentFrame].descriptorSet,
                            0, nullptr);
    vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            pipelineLayout, 3, 1, &aoFrame.descriptorSet,
                            0, nullptr);

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

    VkPipeline boundPipeline = VK_NULL_HANDLE;
    for (const auto& obj : scene->gameObjects()) {
        for (auto& instance : obj->meshInstances_) {
            const VkPipeline pipeline = instance.material.doubleSided
                ? doubleSidedGraphicsPipeline
                : graphicsPipeline;
            if (pipeline != boundPipeline) {
                vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                  pipeline);
                boundPipeline = pipeline;
            }

            VkBuffer vertexBuffers[] = {instance.mesh.vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers, offsets);
            vkCmdBindIndexBuffer(commandBuffer, instance.mesh.indexBuffer, 0, VK_INDEX_TYPE_UINT32);

            vkCmdBindDescriptorSets(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    pipelineLayout, 0, 1, &instance.renderData.descriptorSets[currentFrame],
                                    0, nullptr);

            const MaterialPushConstants materialPushConstants{
                instance.material.baseColorFactor,
                instance.material.metallicFactor,
                instance.material.roughnessFactor,
                static_cast<std::int32_t>(instance.material.alphaMode),
                instance.material.alphaCutoff,
                instance.material.normalMapEnabled ? 1 : 0,
                instance.material.hasMetallicRoughnessMap ? 1 : 0};
            vkCmdPushConstants(commandBuffer, pipelineLayout,
                               VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                               sizeof(MaterialPushConstants),
                               &materialPushConstants);

            vkCmdDrawIndexed(commandBuffer, static_cast<uint32_t>(instance.mesh.indices.size()), 1, 0,
                            0, 0);
        }

    }

    const GameObject* selectedObject =
        imguiLayer.selectedGameObjectForScene(scene);
    if (runState == SceneRunState::Editing && selectedObject != nullptr &&
        !selectedObject->meshInstances_.empty() &&
        swapchainExtent.width > 0 && swapchainExtent.height > 0) {
        const OutlinePushConstants outlinePushConstants{
            glm::vec4(1.0f, 0.55f, 0.05f, 1.0f),
            glm::vec4(3.0f, static_cast<float>(swapchainExtent.width),
                      static_cast<float>(swapchainExtent.height), 0.0f)};

        vkCmdBindPipeline(commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          selectionOutlinePipeline);
        vkCmdPushConstants(commandBuffer, selectionOutlinePipelineLayout,
                           VK_SHADER_STAGE_VERTEX_BIT |
                               VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(OutlinePushConstants),
                           &outlinePushConstants);

        for (const auto& instance : selectedObject->meshInstances_) {
            VkBuffer vertexBuffers[] = {instance.mesh.vertexBuffer};
            VkDeviceSize offsets[] = {0};
            vkCmdBindVertexBuffers(commandBuffer, 0, 1, vertexBuffers,
                                   offsets);
            vkCmdBindIndexBuffer(commandBuffer, instance.mesh.indexBuffer, 0,
                                 VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(
                commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                selectionOutlinePipelineLayout, 0, 1,
                &instance.renderData.descriptorSets[currentFrame], 0, nullptr);
            vkCmdDrawIndexed(commandBuffer,
                             static_cast<uint32_t>(instance.mesh.indices.size()),
                             1, 0, 0, 0);
        }
    }

    imguiLayer.recordDrawData(commandBuffer);

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
    destroyRenderFinishedSemaphores();
    cleanupSwapchain();
    Result result = createSwapchain();
    if (!result) {
        cleanupSwapchain();
        return addContext("Failed to recreate swapchain", result);
    }
    result = createRenderFinishedSemaphores();
    if (!result) {
        cleanupSwapchain();
        return addContext(
            "Failed to recreate render-finished semaphores", result);
    }
    result = createImageViews();
    if (!result) {
        destroyRenderFinishedSemaphores();
        cleanupSwapchain();
        return addContext("Failed to recreate swapchain image views", result);
    }
    result = createColorResources();
    if (!result) {
        destroyRenderFinishedSemaphores();
        cleanupSwapchain();
        return addContext("Failed to recreate color resources", result);
    }
    result = createDepthResources();
    if (!result) {
        destroyRenderFinishedSemaphores();
        cleanupSwapchain();
        return addContext("Failed to recreate depth resources", result);
    }
    result = createAmbientOcclusionExtentResources();
    if (!result) {
        destroyRenderFinishedSemaphores();
        cleanupSwapchain();
        return addContext("Failed to recreate ambient-occlusion images", result);
    }
    result = rewriteAmbientOcclusionDescriptors();
    if (!result) {
        destroyRenderFinishedSemaphores();
        cleanupSwapchain();
        return addContext("Failed to rewrite ambient-occlusion descriptors", result);
    }
    result = createFramebuffers();
    if (!result) {
        destroyRenderFinishedSemaphores();
        cleanupSwapchain();
        return addContext("Failed to recreate framebuffers", result);
    }
    result = imguiLayer.onSwapchainRecreated(
        renderPass, msaaSamples, swapchainMinimumImageCount,
        static_cast<uint32_t>(swapchainImages.size()));
    if (!result) {
        return addContext(
            "Failed to update Dear ImGui after swapchain recreation", result);
    }
    return Result::success();
}

void VulkanContext::cleanupTrackedSceneResources() noexcept {
    SceneResourceOwnership resources;
    resources.temporaryBuffers = std::move(ownedTemporaryBuffers);
    resources.buffers = std::move(ownedSceneBuffers);
    resources.images = std::move(ownedSceneImages);
    resources.imageViews = std::move(ownedSceneImageViews);
    resources.samplers = std::move(ownedSceneSamplers);
    resources.descriptorSets = std::move(ownedSceneDescriptorSets);
    resources.renderData = std::move(ownedRenderData);
    cleanupSceneResources(resources, false);
}

void VulkanContext::cleanupSceneResources(
    SceneResourceOwnership& resources,
    bool freeDescriptorSets) noexcept {
    if (device == VK_NULL_HANDLE) {
        resources.temporaryBuffers.clear();
        resources.buffers.clear();
        resources.images.clear();
        resources.imageViews.clear();
        resources.samplers.clear();
        resources.descriptorSets.clear();
        resources.renderData.clear();
        for (GameObject* object : resources.attachedGameObjects) {
            if (object) {
                object->markRenderResourcesDetached();
            }
        }
        resources.attachedGameObjects.clear();
        return;
    }

    for (auto& ownership : resources.descriptorSets) {
        if (!ownership.slots) {
            continue;
        }
        const size_t count =
            std::min(ownership.slots->size(), ownership.sets.size());
        if (freeDescriptorSets && descriptorPool != VK_NULL_HANDLE &&
            count > 0) {
            const VkResult result = vkFreeDescriptorSets(
                device, descriptorPool, static_cast<uint32_t>(count),
                ownership.sets.data());
            if (result != VK_SUCCESS) {
                spdlog::error(
                    "Failed to free runtime descriptor sets (VkResult {})",
                    static_cast<int>(result));
            }
        }
        for (size_t i = 0; i < count; ++i) {
            if ((*ownership.slots)[i] == ownership.sets[i]) {
                (*ownership.slots)[i] = VK_NULL_HANDLE;
            }
        }
    }

    for (auto& ownership : resources.imageViews) {
        if (ownership.view != VK_NULL_HANDLE) {
            vkDestroyImageView(device, ownership.view, nullptr);
            if (ownership.slot && *ownership.slot == ownership.view) {
                *ownership.slot = VK_NULL_HANDLE;
            }
        }
    }

    for (auto& ownership : resources.samplers) {
        if (ownership.sampler != VK_NULL_HANDLE) {
            vkDestroySampler(device, ownership.sampler, nullptr);
            if (ownership.slot && *ownership.slot == ownership.sampler) {
                *ownership.slot = VK_NULL_HANDLE;
            }
        }
    }

    for (auto& ownership : resources.images) {
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

    for (auto& ownership : resources.temporaryBuffers) {
        if (ownership.buffer != VK_NULL_HANDLE) {
            vkDestroyBuffer(device, ownership.buffer, nullptr);
        }
        if (ownership.memory != VK_NULL_HANDLE) {
            vkFreeMemory(device, ownership.memory, nullptr);
        }
    }

    for (auto& ownership : resources.buffers) {
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

    for (RenderData* renderData : resources.renderData) {
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

    resources.temporaryBuffers.clear();
    resources.buffers.clear();
    resources.images.clear();
    resources.imageViews.clear();
    resources.samplers.clear();
    resources.descriptorSets.clear();
    resources.renderData.clear();
    for (GameObject* object : resources.attachedGameObjects) {
        if (object) {
            object->markRenderResourcesDetached();
        }
    }
    resources.attachedGameObjects.clear();
}

bool VulkanContext::cleanup() noexcept {
    const bool hadResources =
        instance != VK_NULL_HANDLE || device != VK_NULL_HANDLE ||
        currentScene != nullptr || !ownedTemporaryBuffers.empty() ||
        !ownedSceneBuffers.empty() ||
        !ownedSceneImages.empty() || !ownedSceneImageViews.empty() ||
        !ownedSceneSamplers.empty() ||
        !ownedSceneDescriptorSets.empty() ||
        !sceneResourceOwnership_.empty();
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

        imguiLayer.shutdown();

        cleanupSwapchain();
        destroyRenderFinishedSemaphores();

        destroyDirectionalShadowResources();
        destroyAmbientOcclusionResources();

        if (descriptorPool != VK_NULL_HANDLE) {
            vkDestroyDescriptorPool(device, descriptorPool, nullptr);
            descriptorPool = VK_NULL_HANDLE;
            lightsDescriptorSets.fill(VK_NULL_HANDLE);
        }

        cleanupTrackedSceneResources();
        for (auto& [scene, resources] : sceneResourceOwnership_) {
            (void)scene;
            cleanupSceneResources(resources, false);
        }
        sceneResourceOwnership_.clear();
        sceneResourceLoadTarget_ = nullptr;

        destroyLightsUBOs();

        if (graphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, graphicsPipeline, nullptr);
            graphicsPipeline = VK_NULL_HANDLE;
        }
        if (doubleSidedGraphicsPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, doubleSidedGraphicsPipeline, nullptr);
            doubleSidedGraphicsPipeline = VK_NULL_HANDLE;
        }
        if (directionalShadowPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, directionalShadowPipeline, nullptr);
            directionalShadowPipeline = VK_NULL_HANDLE;
        }
        if (directionalShadowDoubleSidedPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, directionalShadowDoubleSidedPipeline,
                              nullptr);
            directionalShadowDoubleSidedPipeline = VK_NULL_HANDLE;
        }
        if (selectionOutlinePipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, selectionOutlinePipeline, nullptr);
            selectionOutlinePipeline = VK_NULL_HANDLE;
        }
        if (ambientOcclusionGeometryPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, ambientOcclusionGeometryPipeline, nullptr);
            ambientOcclusionGeometryPipeline = VK_NULL_HANDLE;
        }
        if (ambientOcclusionGeometryDoubleSidedPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, ambientOcclusionGeometryDoubleSidedPipeline, nullptr);
            ambientOcclusionGeometryDoubleSidedPipeline = VK_NULL_HANDLE;
        }
        if (ambientOcclusionPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, ambientOcclusionPipeline, nullptr);
            ambientOcclusionPipeline = VK_NULL_HANDLE;
        }
        if (ambientOcclusionBlurPipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(device, ambientOcclusionBlurPipeline, nullptr);
            ambientOcclusionBlurPipeline = VK_NULL_HANDLE;
        }
        if (selectionOutlinePipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, selectionOutlinePipelineLayout,
                                    nullptr);
            selectionOutlinePipelineLayout = VK_NULL_HANDLE;
        }
        if (pipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, pipelineLayout, nullptr);
            pipelineLayout = VK_NULL_HANDLE;
        }
        if (directionalShadowPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, directionalShadowPipelineLayout,
                                    nullptr);
            directionalShadowPipelineLayout = VK_NULL_HANDLE;
        }
        if (ambientOcclusionGeometryPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, ambientOcclusionGeometryPipelineLayout, nullptr);
            ambientOcclusionGeometryPipelineLayout = VK_NULL_HANDLE;
        }
        if (ambientOcclusionPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, ambientOcclusionPipelineLayout, nullptr);
            ambientOcclusionPipelineLayout = VK_NULL_HANDLE;
        }
        if (ambientOcclusionBlurPipelineLayout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(device, ambientOcclusionBlurPipelineLayout, nullptr);
            ambientOcclusionBlurPipelineLayout = VK_NULL_HANDLE;
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
        if (directionalShadowDescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device,
                                         directionalShadowDescriptorSetLayout,
                                         nullptr);
            directionalShadowDescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (ambientOcclusionDescriptorSetLayout != VK_NULL_HANDLE) {
            vkDestroyDescriptorSetLayout(device, ambientOcclusionDescriptorSetLayout,
                                         nullptr);
            ambientOcclusionDescriptorSetLayout = VK_NULL_HANDLE;
        }
        if (renderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, renderPass, nullptr);
            renderPass = VK_NULL_HANDLE;
        }
        if (directionalShadowRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, directionalShadowRenderPass, nullptr);
            directionalShadowRenderPass = VK_NULL_HANDLE;
        }
        if (ambientOcclusionGeometryRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, ambientOcclusionGeometryRenderPass, nullptr);
            ambientOcclusionGeometryRenderPass = VK_NULL_HANDLE;
        }
        if (ambientOcclusionRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, ambientOcclusionRenderPass, nullptr);
            ambientOcclusionRenderPass = VK_NULL_HANDLE;
        }
        if (ambientOcclusionBlurRenderPass != VK_NULL_HANDLE) {
            vkDestroyRenderPass(device, ambientOcclusionBlurRenderPass, nullptr);
            ambientOcclusionBlurRenderPass = VK_NULL_HANDLE;
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
        for (auto& [scene, resources] : sceneResourceOwnership_) {
            (void)scene;
            cleanupSceneResources(resources, false);
        }
        sceneResourceOwnership_.clear();
        sceneResourceLoadTarget_ = nullptr;
        destroyDirectionalShadowResources();
        destroyAmbientOcclusionResources();
    }

    graphicsQueue = VK_NULL_HANDLE;
    graphicsQueueFamily.reset();
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
    lightsDescriptorSets.fill(VK_NULL_HANDLE);
    lightsBuffers.fill(VK_NULL_HANDLE);
    lightsBufferMemory.fill(VK_NULL_HANDLE);
    lightsBufferMapped.fill(nullptr);
    directionalShadowFrames = {};
    directionalShadowSampler = VK_NULL_HANDLE;
    directionalShadowDepthFormat = VK_FORMAT_UNDEFINED;
    ambientOcclusionFrames = {};
    ambientOcclusionSampler = VK_NULL_HANDLE;
    ambientOcclusionDepthFormat = VK_FORMAT_UNDEFINED;
    ambientOcclusionNormalFormat = VK_FORMAT_UNDEFINED;
    ambientOcclusionOutputFormat = VK_FORMAT_UNDEFINED;
    descriptorSetLayout = VK_NULL_HANDLE;
    lightsDescriptorSetLayout = VK_NULL_HANDLE;
    directionalShadowDescriptorSetLayout = VK_NULL_HANDLE;
    ambientOcclusionDescriptorSetLayout = VK_NULL_HANDLE;
    graphicsPipeline = VK_NULL_HANDLE;
    doubleSidedGraphicsPipeline = VK_NULL_HANDLE;
    directionalShadowPipeline = VK_NULL_HANDLE;
    directionalShadowDoubleSidedPipeline = VK_NULL_HANDLE;
    selectionOutlinePipeline = VK_NULL_HANDLE;
    ambientOcclusionGeometryPipeline = VK_NULL_HANDLE;
    ambientOcclusionGeometryDoubleSidedPipeline = VK_NULL_HANDLE;
    ambientOcclusionPipeline = VK_NULL_HANDLE;
    ambientOcclusionBlurPipeline = VK_NULL_HANDLE;
    selectionOutlinePipelineLayout = VK_NULL_HANDLE;
    pipelineLayout = VK_NULL_HANDLE;
    directionalShadowPipelineLayout = VK_NULL_HANDLE;
    ambientOcclusionGeometryPipelineLayout = VK_NULL_HANDLE;
    ambientOcclusionPipelineLayout = VK_NULL_HANDLE;
    ambientOcclusionBlurPipelineLayout = VK_NULL_HANDLE;
    renderPass = VK_NULL_HANDLE;
    directionalShadowRenderPass = VK_NULL_HANDLE;
    ambientOcclusionGeometryRenderPass = VK_NULL_HANDLE;
    ambientOcclusionRenderPass = VK_NULL_HANDLE;
    ambientOcclusionBlurRenderPass = VK_NULL_HANDLE;
    commandPool = VK_NULL_HANDLE;
    colorImage = VK_NULL_HANDLE;
    colorImageView = VK_NULL_HANDLE;
    colorImageMemory = VK_NULL_HANDLE;
    depthImage = VK_NULL_HANDLE;
    depthImageView = VK_NULL_HANDLE;
    depthImageMemory = VK_NULL_HANDLE;
    swapchain = VK_NULL_HANDLE;
    swapchainMinimumImageCount = 0;
    swapchainImageFormat = VK_FORMAT_UNDEFINED;
    swapchainExtent = {};
    msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    currentFrame = 0;
    framebufferResized = false;
    hasSubmittedWork = false;
    singleTimeSubmissionMayBePending = false;
    initialized = false;
    currentScene = nullptr;
    sceneResourceLoadTarget_ = nullptr;
    window = nullptr;

    if (hadResources) {
        spdlog::info("Successfully cleaned up Vulkan Context");
    }
    return true;
}
