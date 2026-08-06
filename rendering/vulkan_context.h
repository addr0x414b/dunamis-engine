#ifndef VULKAN_CONTEXT_H
#define VULKAN_CONTEXT_H

#include <algorithm>
#include <array>
#include <fstream>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <glm/glm.hpp>
#include <spdlog/spdlog.h>
#include <vulkan/vulkan.h>

#include "../core/result.h"
#include "../scene/game_object.h"
#include "../scene/scene.h"
#include "imgui_layer.h"
#include "utils/vulkan_utils.h"

#ifdef NDEBUG
const bool enableValidationLayers = false;
#else
const bool enableValidationLayers = true;
#endif

struct QueueFamilyIndices {
    std::optional<uint32_t> graphicsFamily;
    std::optional<uint32_t> presentFamily;

    [[nodiscard]] bool isComplete() const {
        return graphicsFamily.has_value() && presentFamily.has_value();
    }
};

struct SwapchainSupportDetails {
    VkSurfaceCapabilitiesKHR capabilities{};
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

const int MAX_FRAMES_IN_FLIGHT = 2;

class VisualServer;
class VulkanContextTestAccess;

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() noexcept;

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    [[nodiscard]] Result init(SDL_Window* window, Scene* scene);
    bool cleanup() noexcept;

private:
    friend class VisualServer;
    friend class VulkanContextTestAccess;

    [[nodiscard]] Result createTextureImages(
        const std::unique_ptr<GameObject>& gameObject);
    [[nodiscard]] Result createTextureImageViews(
        const std::unique_ptr<GameObject>& gameObject);
    [[nodiscard]] Result createTextureSamplers(
        const std::unique_ptr<GameObject>& gameObject);
    [[nodiscard]] Result createVertexBuffers(
        const std::unique_ptr<GameObject>& gameObject);
    [[nodiscard]] Result createIndexBuffers(
        const std::unique_ptr<GameObject>& gameObject);
    [[nodiscard]] Result createUniformBuffers(
        const std::unique_ptr<GameObject>& gameObject);
    [[nodiscard]] Result createDescriptorPool(uint32_t numOfObjects);
    [[nodiscard]] Result createDescriptorSets(
        const std::unique_ptr<GameObject>& gameObject);
    [[nodiscard]] Result createLightsUBO();
    [[nodiscard]] Result beginSceneResourceLoad(Scene* scene);
    [[nodiscard]] Result commitSceneResourceLoad(Scene* scene);
    [[nodiscard]] Result cancelSceneResourceLoad();
    [[nodiscard]] Result unloadSceneResources(Scene* scene);
    [[nodiscard]] Result switchScene(Scene* scene);
    [[nodiscard]] bool hasSceneResources(const Scene* scene) const noexcept;

    [[nodiscard]] Result drawFrame(Scene* scene, const Camera& renderCamera,
                                   SceneRunState runState);
    void processEvent(const SDL_Event& event) noexcept;
    void setImGuiInputEnabled(bool enabled) noexcept;
    void clearEditorSelection() noexcept;
    [[nodiscard]] EditorCommand consumeEditorCommand() noexcept;
    [[nodiscard]] bool sceneInteractionAreaHovered() const noexcept;

    struct OwnedBufferAllocation {
        VkBuffer* bufferSlot = nullptr;
        VkDeviceMemory* memorySlot = nullptr;
        void** mappedSlot = nullptr;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* mappedAddress = nullptr;
    };

    struct OwnedImageAllocation {
        VkImage* imageSlot = nullptr;
        VkDeviceMemory* memorySlot = nullptr;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };

    struct OwnedImageView {
        VkImageView* slot = nullptr;
        VkImageView view = VK_NULL_HANDLE;
    };

    struct OwnedSampler {
        VkSampler* slot = nullptr;
        VkSampler sampler = VK_NULL_HANDLE;
    };

    struct OwnedDescriptorSets {
        std::vector<VkDescriptorSet>* slots = nullptr;
        std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT> sets{};
    };

    struct SceneResourceOwnership {
        std::vector<OwnedBufferAllocation> temporaryBuffers;
        std::vector<OwnedBufferAllocation> buffers;
        std::vector<OwnedImageAllocation> images;
        std::vector<OwnedImageView> imageViews;
        std::vector<OwnedSampler> samplers;
        std::vector<OwnedDescriptorSets> descriptorSets;
        std::vector<RenderData*> renderData;
        std::vector<GameObject*> attachedGameObjects;
    };

    [[nodiscard]] Result validateSceneRenderStateIsEmpty(
        const Scene* scene) const;
    [[nodiscard]] Result validateTextureData(
        const std::unique_ptr<GameObject>& gameObject) const;
    [[nodiscard]] Result prepareSceneResourceTracking(const Scene* scene);
    void cleanupTrackedSceneResources() noexcept;
    void cleanupSceneResources(SceneResourceOwnership& resources,
                               bool freeDescriptorSets) noexcept;

    void updateUniformBuffer(uint32_t currentImage,
                             const std::unique_ptr<GameObject>& gameObject,
                             const glm::mat4& view,
                             const glm::mat4& projection,
                             const glm::vec3& cameraPosition);
    [[nodiscard]] Result recreateSwapchain();
    [[nodiscard]] Result recordCommandBuffer(VkCommandBuffer commandBuffer,
                                             uint32_t imageIndex,
                                             Scene* scene,
                                             SceneRunState runState);

    [[nodiscard]] Result createInstance();
    [[nodiscard]] Result checkValidationLayerSupport(bool& supported) const;
    [[nodiscard]] Result getRequiredExtensions(
        std::vector<const char*>& extensions) const;
    void populateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT& createInfo) const;
    [[nodiscard]] Result setupDebugMessenger();
    static VkResult createDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* createInfo,
        const VkAllocationCallbacks* allocator,
        VkDebugUtilsMessengerEXT* debugMessenger);
    static void destroyDebugUtilsMessengerEXT(
        VkInstance instance, VkDebugUtilsMessengerEXT debugMessenger,
        const VkAllocationCallbacks* allocator);

    [[nodiscard]] Result createSurface();
    [[nodiscard]] Result pickPhysicalDevice();
    [[nodiscard]] Result isDeviceSuitable(VkPhysicalDevice candidate,
                                          bool& suitable);
    [[nodiscard]] Result findQueueFamilies(VkPhysicalDevice candidate,
                                           QueueFamilyIndices& indices) const;
    [[nodiscard]] Result checkDeviceExtensionSupport(
        VkPhysicalDevice candidate, bool& supported) const;
    [[nodiscard]] Result querySwapchainSupport(
        VkPhysicalDevice candidate, SwapchainSupportDetails& details) const;
    [[nodiscard]] VkSampleCountFlagBits getCappedUsableSampleCount() const;

    [[nodiscard]] Result createLogicalDevice();
    [[nodiscard]] Result createSwapchain();
    [[nodiscard]] VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats) const;
    [[nodiscard]] VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes) const;
    [[nodiscard]] Result chooseSwapExtent(
        const VkSurfaceCapabilitiesKHR& capabilities,
        VkExtent2D& extent) const;
    void cleanupSwapchain() noexcept;

    [[nodiscard]] Result createImageViews();
    [[nodiscard]] Result createImageView(VkImage image, VkFormat format,
                                         VkImageAspectFlags aspectFlags,
                                         uint32_t mipLevels,
                                         VkImageView& imageView,
                                         OwnedImageView* ownership = nullptr);

    [[nodiscard]] Result createRenderPass();
    [[nodiscard]] Result findDepthFormat(VkFormat& format) const;
    [[nodiscard]] Result findSupportedFormat(
        const std::vector<VkFormat>& candidates, VkImageTiling tiling,
        VkFormatFeatureFlags features, VkFormat& format) const;

    [[nodiscard]] Result createDescriptorSetLayout();
    [[nodiscard]] Result createLightsDescriptorSetLayout();
    [[nodiscard]] Result updateLightsUniformBuffer(Scene* scene);
    void destroyLightsUBOs() noexcept;

    [[nodiscard]] Result createGraphicsPipeline();
    [[nodiscard]] Result createSelectionOutlinePipeline();
    [[nodiscard]] Result readFile(const std::string& filename,
                                  std::vector<char>& contents) const;
    [[nodiscard]] Result createShaderModule(const std::vector<char>& code,
                                            VkShaderModule& shaderModule);
    [[nodiscard]] Result createCommandPool();

    [[nodiscard]] Result createColorResources();
    [[nodiscard]] Result createImage(
        uint32_t width, uint32_t height, uint32_t mipLevels,
        VkSampleCountFlagBits numSamples, VkFormat format,
        VkImageTiling tiling, VkImageUsageFlags usage,
        VkMemoryPropertyFlags properties, VkImage& image,
        VkDeviceMemory& imageMemory,
        OwnedImageAllocation* ownership = nullptr);
    [[nodiscard]] Result findMemoryType(
        uint32_t typeFilter, VkMemoryPropertyFlags properties,
        uint32_t& memoryTypeIndex) const;

    [[nodiscard]] Result createDepthResources();
    [[nodiscard]] Result transitionImageLayout(
        VkImage image, VkFormat format, VkImageLayout oldLayout,
        VkImageLayout newLayout, uint32_t mipLevels);
    [[nodiscard]] static bool hasStencilComponent(VkFormat format);

    [[nodiscard]] Result beginSingleTimeCommands(
        VkCommandBuffer& commandBuffer);
    [[nodiscard]] Result endSingleTimeCommands(
        VkCommandBuffer commandBuffer);
    [[nodiscard]] Result createFramebuffers();
    [[nodiscard]] Result copyBufferToImage(VkBuffer buffer, VkImage image,
                                           uint32_t width, uint32_t height);
    [[nodiscard]] Result generateMipmaps(
        VkImage image, VkFormat imageFormat, int32_t texWidth,
        int32_t texHeight, uint32_t mipLevels);
    [[nodiscard]] Result createBuffer(
        VkDeviceSize size, VkBufferUsageFlags usage,
        VkMemoryPropertyFlags properties, VkBuffer& buffer,
        VkDeviceMemory& bufferMemory,
        OwnedBufferAllocation* ownership = nullptr);
    [[nodiscard]] Result copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer,
                                    VkDeviceSize size);

    [[nodiscard]] Result createCommandBuffers();
    [[nodiscard]] Result createSyncObjects();
    [[nodiscard]] Result createRenderFinishedSemaphores();
    void destroyRenderFinishedSemaphores() noexcept;
    [[nodiscard]] Result initializeImGui();

    SDL_Window* window = nullptr;
    Scene* currentScene = nullptr;
    bool initialized = false;
    bool framebufferResized = false;
    bool hasSubmittedWork = false;
    bool singleTimeSubmissionMayBePending = false;

    VkInstance instance = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;
    bool sampleRateShadingSupported = false;
    bool sampleRateShadingEnabled = false;

    VkDevice device = VK_NULL_HANDLE;
    std::optional<uint32_t> graphicsQueueFamily;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    VkQueue presentQueue = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    std::vector<VkImage> swapchainImages;
    uint32_t swapchainMinimumImageCount = 0;
    VkFormat swapchainImageFormat = VK_FORMAT_UNDEFINED;
    VkExtent2D swapchainExtent{};
    std::vector<VkImageView> swapchainImageViews;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout lightsDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>
        lightsDescriptorSets{};
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> lightsBuffers{};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT>
        lightsBufferMemory{};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> lightsBufferMapped{};

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VkPipeline doubleSidedGraphicsPipeline = VK_NULL_HANDLE;
    VkPipelineLayout selectionOutlinePipelineLayout = VK_NULL_HANDLE;
    VkPipeline selectionOutlinePipeline = VK_NULL_HANDLE;
    VkPipelineCache pipelineCache = VK_NULL_HANDLE;
    VkCommandPool commandPool = VK_NULL_HANDLE;

    VkImage colorImage = VK_NULL_HANDLE;
    VkImageView colorImageView = VK_NULL_HANDLE;
    VkDeviceMemory colorImageMemory = VK_NULL_HANDLE;
    VkImage depthImage = VK_NULL_HANDLE;
    VkImageView depthImageView = VK_NULL_HANDLE;
    VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
    std::vector<VkFramebuffer> swapchainFramebuffers;
    std::vector<VkCommandBuffer> commandBuffers;

    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;

    std::vector<OwnedBufferAllocation> ownedTemporaryBuffers;
    std::vector<OwnedBufferAllocation> ownedSceneBuffers;
    std::vector<OwnedImageAllocation> ownedSceneImages;
    std::vector<OwnedImageView> ownedSceneImageViews;
    std::vector<OwnedSampler> ownedSceneSamplers;
    std::vector<OwnedDescriptorSets> ownedSceneDescriptorSets;
    std::vector<RenderData*> ownedRenderData;
    std::unordered_map<const Scene*, SceneResourceOwnership>
        sceneResourceOwnership_;
    const Scene* sceneResourceLoadTarget_ = nullptr;

    ImGuiLayer imguiLayer;

    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"};
    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};
};

#endif
