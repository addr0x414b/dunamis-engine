#ifndef VULKAN_CONTEXT_H
#define VULKAN_CONTEXT_H

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <functional>
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
#include "../editor/editor_state.h"
#include "../scene/game_object.h"
#include "../scene/loading_cache_key.h"
#include "../scene/scene.h"
#include "imgui_layer.h"
#include "directional_shadow.h"
#include "ambient_occlusion.h"
#include "physics_debug_renderer.h"
#include "../physics/collision_shapes.h"
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
class PhysicsServer;
class Character;
class EditorSession;
class VulkanContextTestAccess;

class VulkanContext {
public:
    VulkanContext() = default;
    ~VulkanContext() noexcept;

    VulkanContext(const VulkanContext&) = delete;
    VulkanContext& operator=(const VulkanContext&) = delete;

    [[nodiscard]] Result init(SDL_Window* window, Scene* scene,
                              EditorSession& editorSession,
                              PhysicsServer* physicsServer = nullptr);
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
    [[nodiscard]] Result createDirectionalShadowResources();
    [[nodiscard]] Result createAmbientOcclusionResources();
    [[nodiscard]] Result initializeDirectionalShadowImages();
    [[nodiscard]] Result beginSceneResourceLoad(Scene* scene);
    [[nodiscard]] Result beginSceneUploadBatch();
    [[nodiscard]] Result finishSceneUploadBatch();
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
    [[nodiscard]] bool sceneInteractionAreaHovered() const noexcept;
    void setCurrentScenePath(const std::string& path);
    void requestLoadConfirmation();
    void requestSaveAsOverwriteConfirmation(const std::string& path);
    void requestQuitConfirmation();

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

    struct GpuModelAsset {
        std::vector<std::shared_ptr<GpuMeshAsset>> meshes;
        bool complete = false;
    };

    struct MeshRenderState {
        std::shared_ptr<GpuMeshAsset> gpuAsset;
        RenderData renderData;
    };

    struct SceneResourceOwnership {
        std::vector<OwnedBufferAllocation> temporaryBuffers;
        std::vector<OwnedBufferAllocation> buffers;
        std::vector<OwnedImageAllocation> images;
        std::vector<OwnedImageView> imageViews;
        std::vector<OwnedSampler> samplers;
        std::vector<OwnedDescriptorSets> descriptorSets;
        std::vector<RenderData*> renderData;
        std::unordered_map<GameObject*, std::vector<MeshRenderState>>
            meshRenderStates;
        std::vector<GameObject*> attachedGameObjects;
        std::vector<std::shared_ptr<GpuModelAsset>> uncachedGpuModels;
    };

    struct ResourceLoadStats {
        std::size_t meshInstances = 0;
        std::size_t modelCacheHits = 0;
        std::size_t modelCacheMisses = 0;
        std::size_t textureUploads = 0;
        std::size_t vertexBufferUploads = 0;
        std::size_t indexBufferUploads = 0;
        std::size_t singleUseSubmissions = 0;
        std::size_t queueIdleWaits = 0;
        std::size_t fenceWaits = 0;
    };

    [[nodiscard]] Result validateSceneRenderStateIsEmpty(
        const Scene* scene) const;
    [[nodiscard]] Result validateTextureData(
        const std::unique_ptr<GameObject>& gameObject) const;
    [[nodiscard]] Result prepareSceneResourceTracking(const Scene* scene);
    void cleanupTrackedSceneResources() noexcept;
    void cleanupSceneResources(SceneResourceOwnership& resources,
                               bool freeDescriptorSets) noexcept;
    void cancelSceneUploadBatch() noexcept;
    void cleanupCompletedUploadStaging() noexcept;
    void destroyGpuModelAsset(GpuModelAsset& asset) noexcept;
    void destroyGpuAssetCache() noexcept;
    [[nodiscard]] std::vector<MeshRenderState>* loadMeshRenderStates(
        const GameObject* object) noexcept;

    [[nodiscard]] Result updateUniformBuffer(
        SceneResourceOwnership& resources, uint32_t currentImage,
        const std::unique_ptr<GameObject>& gameObject, const glm::mat4& view,
        const glm::mat4& projection, const glm::vec3& cameraPosition);
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
    [[nodiscard]] Result createDirectionalShadowRenderPass();
    [[nodiscard]] Result createAmbientOcclusionRenderPasses();
    [[nodiscard]] Result findDepthFormat(VkFormat& format) const;
    [[nodiscard]] Result findSupportedFormat(
        const std::vector<VkFormat>& candidates, VkImageTiling tiling,
        VkFormatFeatureFlags features, VkFormat& format) const;
    [[nodiscard]] Result findDirectionalShadowDepthFormat(VkFormat& format) const;
    [[nodiscard]] Result findAmbientOcclusionDepthFormat(VkFormat& format) const;
    [[nodiscard]] Result findAmbientOcclusionNormalFormat(VkFormat& format) const;
    [[nodiscard]] Result findAmbientOcclusionOutputFormat(VkFormat& format) const;

    [[nodiscard]] Result createDescriptorSetLayout();
    [[nodiscard]] Result createLightsDescriptorSetLayout();
    [[nodiscard]] Result createDirectionalShadowDescriptorSetLayout();
    [[nodiscard]] Result createAmbientOcclusionDescriptorSetLayout();
    [[nodiscard]] Result updateLightsUniformBuffer(Scene* scene);
    [[nodiscard]] Result updateDirectionalShadowUniformBuffer(Scene* scene);
    [[nodiscard]] Result updateAmbientOcclusionUniformBuffer(
        const glm::mat4& projection);
    void destroyLightsUBOs() noexcept;
    void destroyDirectionalShadowResources() noexcept;
    void destroyAmbientOcclusionResources() noexcept;
    void destroyAmbientOcclusionExtentResources() noexcept;
    [[nodiscard]] Result createAmbientOcclusionExtentResources();
    [[nodiscard]] Result rewriteAmbientOcclusionDescriptors();

    [[nodiscard]] Result createGraphicsPipeline();
    [[nodiscard]] Result createPhysicsDebugPipeline();
    [[nodiscard]] Result createDirectionalShadowPipelines();
    [[nodiscard]] Result createSelectionOutlinePipeline();
    [[nodiscard]] Result createAmbientOcclusionPipelines();
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
    void prepareSelectedPhysicsDiagnostics(Scene* scene,
                                           SceneRunState runState);
    [[nodiscard]] Result preparePhysicsDebugDraws(Scene* scene, SceneRunState runState);
    [[nodiscard]] Result ensurePhysicsDebugBatch(const PhysicsDebugRenderer::BatchData& batch);

    struct PhysicsDebugGpuBatch {
        VkBuffer vertices = VK_NULL_HANDLE;
        VkDeviceMemory vertexMemory = VK_NULL_HANDLE;
        VkBuffer indices = VK_NULL_HANDLE;
        VkDeviceMemory indexMemory = VK_NULL_HANDLE;
    };

    struct PhysicsDebugShapeKey {
        std::string objectId;
        bool character = false;

        [[nodiscard]] bool operator==(const PhysicsDebugShapeKey& other) const
            noexcept {
            return objectId == other.objectId && character == other.character;
        }
    };

    struct PhysicsDebugShapeKeyHash {
        [[nodiscard]] std::size_t operator()(
            const PhysicsDebugShapeKey& key) const noexcept {
            std::size_t result = std::hash<std::string>{}(key.objectId);
            result ^= std::hash<bool>{}(key.character) +
                      static_cast<std::size_t>(0x9e3779b9) + (result << 6) +
                      (result >> 2);
            return result;
        }
    };

    struct PhysicsDebugShapeSignature {
        enum class Type : std::uint8_t {
            Mesh,
            ConvexHull,
            Sphere,
            CharacterCapsule,
        };

        Type type = Type::Mesh;
        std::string modelIdentity;
        std::array<std::uint32_t, 3> scaleBits{};
        std::uint32_t radiusBits = 0;
        std::uint32_t heightBits = 0;

        [[nodiscard]] bool operator==(
            const PhysicsDebugShapeSignature& other) const noexcept {
            return type == other.type &&
                   modelIdentity == other.modelIdentity &&
                   scaleBits == other.scaleBits &&
                   radiusBits == other.radiusBits &&
                   heightBits == other.heightBits;
        }

        [[nodiscard]] bool operator!=(
            const PhysicsDebugShapeSignature& other) const noexcept {
            return !(*this == other);
        }
    };

    [[nodiscard]] static PhysicsDebugShapeSignature
    makePhysicsDebugShapeSignature(const GameObject& object);
    [[nodiscard]] static PhysicsDebugShapeSignature
    makeCharacterDebugShapeSignature(const Character& character);

    struct PhysicsDebugShapeCacheEntry {
        PhysicsDebugShapeSignature signature;
        std::optional<physics::CookedShape> cooked;
        std::string error;
    };

    [[nodiscard]] const physics::CookedShape* ensurePhysicsDebugShape(
        Scene* scene, const GameObject& object, bool character,
        const PhysicsDebugShapeSignature& signature,
        std::chrono::nanoseconds& preparationDuration, bool& rebuilt,
        std::string& error);
    void retirePhysicsDebugGpuBatches() noexcept;
    void collectPhysicsDebugGpuBatches() noexcept;
    void destroyPhysicsDebugGpuBatch(PhysicsDebugGpuBatch& batch) noexcept;
    void destroyAllPhysicsDebugGpuBatches() noexcept;

    SDL_Window* window = nullptr;
    Scene* currentScene = nullptr;
    EditorSession* editorSession_ = nullptr;
    // Non-owning. Dunamis owns PhysicsServer and shuts down VisualServer
    // before PhysicsServer; cleanup resets this pointer before returning.
    PhysicsServer* physicsServer_ = nullptr;
    bool initialized = false;
    bool framebufferResized = false;
    bool hasSubmittedWork = false;
    bool singleTimeSubmissionMayBePending = false;
    bool sceneUploadBatchActive_ = false;
    bool sceneUploadBatchHasCommands_ = false;
    VkCommandBuffer sceneUploadCommandBuffer_ = VK_NULL_HANDLE;

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
    VkDescriptorSetLayout directionalShadowDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout ambientOcclusionDescriptorSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool descriptorPool = VK_NULL_HANDLE;
    std::array<VkDescriptorSet, MAX_FRAMES_IN_FLIGHT>
        lightsDescriptorSets{};
    std::array<VkBuffer, MAX_FRAMES_IN_FLIGHT> lightsBuffers{};
    std::array<VkDeviceMemory, MAX_FRAMES_IN_FLIGHT>
        lightsBufferMemory{};
    std::array<void*, MAX_FRAMES_IN_FLIGHT> lightsBufferMapped{};

    struct DirectionalShadowFrameResources {
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
        VkImageView depthImageView = VK_NULL_HANDLE;
        VkFramebuffer framebuffer = VK_NULL_HANDLE;
        VkBuffer transformBuffer = VK_NULL_HANDLE;
        VkDeviceMemory transformBufferMemory = VK_NULL_HANDLE;
        void* transformBufferMapped = nullptr;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };
    std::array<DirectionalShadowFrameResources, MAX_FRAMES_IN_FLIGHT>
        directionalShadowFrames{};
    VkFormat directionalShadowDepthFormat = VK_FORMAT_UNDEFINED;
    VkSampler directionalShadowSampler = VK_NULL_HANDLE;

    struct alignas(16) AmbientOcclusionUBO {
        alignas(16) glm::mat4 projection{1.0f};
        alignas(16) glm::mat4 inverseProjection{1.0f};
        alignas(16) std::array<glm::vec4, ambient_occlusion::sampleCount> samples{};
        alignas(16) glm::vec4 parameters{};
        alignas(16) glm::vec4 viewport{};
    };
    static_assert(offsetof(AmbientOcclusionUBO, projection) == 0);
    static_assert(offsetof(AmbientOcclusionUBO, inverseProjection) == 64);
    static_assert(offsetof(AmbientOcclusionUBO, samples) == 128);
    static_assert(sizeof(AmbientOcclusionUBO) == 672);

    struct AmbientOcclusionFrameResources {
        VkImage depthImage = VK_NULL_HANDLE;
        VkDeviceMemory depthImageMemory = VK_NULL_HANDLE;
        VkImageView depthImageView = VK_NULL_HANDLE;
        VkImage normalImage = VK_NULL_HANDLE;
        VkDeviceMemory normalImageMemory = VK_NULL_HANDLE;
        VkImageView normalImageView = VK_NULL_HANDLE;
        VkImage rawImage = VK_NULL_HANDLE;
        VkDeviceMemory rawImageMemory = VK_NULL_HANDLE;
        VkImageView rawImageView = VK_NULL_HANDLE;
        VkImage blurredImage = VK_NULL_HANDLE;
        VkDeviceMemory blurredImageMemory = VK_NULL_HANDLE;
        VkImageView blurredImageView = VK_NULL_HANDLE;
        VkFramebuffer geometryFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer rawFramebuffer = VK_NULL_HANDLE;
        VkFramebuffer blurFramebuffer = VK_NULL_HANDLE;
        VkBuffer uniformBuffer = VK_NULL_HANDLE;
        VkDeviceMemory uniformBufferMemory = VK_NULL_HANDLE;
        void* uniformBufferMapped = nullptr;
        VkDescriptorSet descriptorSet = VK_NULL_HANDLE;
    };
    std::array<AmbientOcclusionFrameResources, MAX_FRAMES_IN_FLIGHT>
        ambientOcclusionFrames{};
    ambient_occlusion::AmbientOcclusionSettings ambientOcclusionSettings{};
    ambient_occlusion::Kernel ambientOcclusionKernel = ambient_occlusion::makeKernel();
    VkFormat ambientOcclusionDepthFormat = VK_FORMAT_UNDEFINED;
    VkFormat ambientOcclusionNormalFormat = VK_FORMAT_UNDEFINED;
    VkFormat ambientOcclusionOutputFormat = VK_FORMAT_UNDEFINED;
    VkSampler ambientOcclusionSampler = VK_NULL_HANDLE;

    VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
    VkPipeline graphicsPipeline = VK_NULL_HANDLE;
    VkPipeline doubleSidedGraphicsPipeline = VK_NULL_HANDLE;
    VkPipelineLayout physicsDebugPipelineLayout = VK_NULL_HANDLE;
    VkPipeline physicsDebugPipeline = VK_NULL_HANDLE;
    VkRenderPass directionalShadowRenderPass = VK_NULL_HANDLE;
    VkPipelineLayout directionalShadowPipelineLayout = VK_NULL_HANDLE;
    VkPipeline directionalShadowPipeline = VK_NULL_HANDLE;
    VkPipeline directionalShadowDoubleSidedPipeline = VK_NULL_HANDLE;
    VkPipelineLayout selectionOutlinePipelineLayout = VK_NULL_HANDLE;
    VkPipeline selectionOutlinePipeline = VK_NULL_HANDLE;
    VkRenderPass ambientOcclusionGeometryRenderPass = VK_NULL_HANDLE;
    VkRenderPass ambientOcclusionRenderPass = VK_NULL_HANDLE;
    VkRenderPass ambientOcclusionBlurRenderPass = VK_NULL_HANDLE;
    VkPipelineLayout ambientOcclusionGeometryPipelineLayout = VK_NULL_HANDLE;
    VkPipeline ambientOcclusionGeometryPipeline = VK_NULL_HANDLE;
    VkPipeline ambientOcclusionGeometryDoubleSidedPipeline = VK_NULL_HANDLE;
    VkPipelineLayout ambientOcclusionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline ambientOcclusionPipeline = VK_NULL_HANDLE;
    VkPipelineLayout ambientOcclusionBlurPipelineLayout = VK_NULL_HANDLE;
    VkPipeline ambientOcclusionBlurPipeline = VK_NULL_HANDLE;
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
    std::unordered_map<const PhysicsDebugRenderer::BatchData*, PhysicsDebugGpuBatch> physicsDebugGpuBatches_;
    std::array<std::vector<PhysicsDebugGpuBatch>, MAX_FRAMES_IN_FLIGHT>
        deferredPhysicsDebugGpuBatches_;
    std::unordered_map<PhysicsDebugShapeKey, PhysicsDebugShapeCacheEntry,
                       PhysicsDebugShapeKeyHash>
        physicsDebugShapes_;
    std::unique_ptr<PhysicsDebugRenderer> physicsDebugRenderer_;
    std::unordered_map<const Scene*, SceneResourceOwnership>
        sceneResourceOwnership_;
    std::unordered_map<model_loading::ModelAssetCacheKey,
                       std::shared_ptr<GpuModelAsset>,
                       model_loading::ModelAssetCacheKeyHash>
        gpuModelAssetCache_;
    std::vector<model_loading::ModelAssetCacheKey> pendingGpuModelKeys_;
    std::vector<std::shared_ptr<GpuModelAsset>> pendingUncachedGpuModels_;
    ResourceLoadStats resourceLoadStats_{};
    const Scene* sceneResourceLoadTarget_ = nullptr;

    ImGuiLayer imguiLayer;

    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"};
    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};
};

#endif
