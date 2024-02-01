
#ifndef VULKAN_CONTEXT_H
#define VULKAN_CONTEXT_H

#include <iostream>

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>
#include <vulkan/vulkan.h>
#include <vulkan/vk_enum_string_helper.h>

#include <algorithm>
#include <array>
#include <assimp/Importer.hpp>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "../../core/debugger/debugger.h"
#include "../../editor/editor.h"
#include "../../scene/scene.h"

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>

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
    VkSurfaceCapabilitiesKHR capabilities;
    std::vector<VkSurfaceFormatKHR> formats;
    std::vector<VkPresentModeKHR> presentModes;
};

/*struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    bool operator==(const Vertex& other) const {
        return pos == other.pos && color == other.color &&
               texCoord == other.texCoord;
    }

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3>
    getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3>
            attributeDescriptions{};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

        return attributeDescriptions;
    }
};*/

const int MAX_FRAMES_IN_FLIGHT = 2;

/*namespace std {
template <>
struct hash<Vertex> {
    size_t operator()(Vertex const& vertex) const {
        return ((hash<glm::vec3>()(vertex.pos) ^
                 (hash<glm::vec3>()(vertex.color) << 1)) >>
                1) ^
               (hash<glm::vec2>()(vertex.texCoord) << 1);
    }
};
}  // namespace std*/

/*struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
};*/

class VulkanContext {
   public:
    void setWindow(SDL_Window* sdlWindow);
    void setEditor(Editor* editor);
    void setScene(Scene* scene);
    void initVulkan();
    void initImgui();
    void drawFrame();
    void cleanup();
    VkDevice device;

   private:
    SDL_Window* window = nullptr;
    Scene* scene = nullptr;

    Editor* editor = nullptr;

    void createInstance();
    bool checkValidationLayerSupport();
    const std::vector<const char*> validationLayers = {
        "VK_LAYER_KHRONOS_validation"};
    std::vector<const char*> getRequiredExtensions();
    void populateDebugMessengerCreateInfo(
        VkDebugUtilsMessengerCreateInfoEXT& createInfo);
    VkInstance instance;

    void setupDebugMessenger();
    VkDebugUtilsMessengerEXT debugMessenger;
    VkResult createDebugUtilsMessengerEXT(
        VkInstance instance,
        const VkDebugUtilsMessengerCreateInfoEXT* pCreateInfo,
        const VkAllocationCallbacks* pAllocator,
        VkDebugUtilsMessengerEXT* pDebugMessenger);

    void createSurface();
    VkSurfaceKHR surface;

    void pickPhysicalDevice();
    bool isDeviceSuitable(VkPhysicalDevice device);
    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice device);
    bool checkDeviceExtensionSupport(VkPhysicalDevice device);
    const std::vector<const char*> deviceExtensions = {
        VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    SwapchainSupportDetails querySwapchainSupport(VkPhysicalDevice device);
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkSampleCountFlagBits getMaxUsableSampleCount();
    VkSampleCountFlagBits msaaSamples = VK_SAMPLE_COUNT_1_BIT;

    void createLogicalDevice();
    VkQueue graphicsQueue;
    VkQueue presentQueue;

    void createSwapchain();
    VkSurfaceFormatKHR chooseSwapSurfaceFormat(
        const std::vector<VkSurfaceFormatKHR>& availableFormats);
    VkPresentModeKHR chooseSwapPresentMode(
        const std::vector<VkPresentModeKHR>& availablePresentModes);
    VkExtent2D chooseSwapExtent(const VkSurfaceCapabilitiesKHR& capabilities);
    VkSwapchainKHR swapchain;
    std::vector<VkImage> swapchainImages;
    VkFormat swapchainImageFormat;
    VkExtent2D swapchainExtent;

    void createImageViews();
    std::vector<VkImageView> swapchainImageViews;
    VkImageView createImageView(VkImage image, VkFormat format,
                                VkImageAspectFlags aspectFlags,
                                uint32_t mipLevels);

    void createRenderPass();
    VkFormat findDepthFormat();
    VkRenderPass renderPass;
    VkFormat findSupportedFormat(const std::vector<VkFormat>& candidates,
                                 VkImageTiling tiling,
                                 VkFormatFeatureFlags features);

    bool my_tool_active = true;

    void createDescriptorSetLayout();
    VkDescriptorSetLayout descriptorSetLayout;

    void createGraphicsPipeline();
    std::vector<char> readFile(const std::string& filename);
    VkShaderModule createShaderModule(const std::vector<char>& code);
    VkPipelineLayout pipelineLayout;
    VkPipeline graphicsPipeline;

    VkPipelineCache pipelineCache = VK_NULL_HANDLE;

    void createCommandPool();
    VkCommandPool commandPool;

    void createColorResources();
    VkDeviceMemory colorImageMemory;
    VkImage colorImage;
    void createImage(uint32_t width, uint32_t height, uint32_t mipLevels,
                     VkSampleCountFlagBits numSamples, VkFormat format,
                     VkImageTiling tiling, VkImageUsageFlags usage,
                     VkMemoryPropertyFlags properties, VkImage& image,
                     VkDeviceMemory& imageMemory);
    uint32_t findMemoryType(uint32_t typeFilter,
                            VkMemoryPropertyFlags properties);

    void createDepthResources();
    VkImage depthImage;
    VkDeviceMemory depthImageMemory;
    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels);
    bool hasStencilComponent(VkFormat format);

    void createFramebuffers();
    std::vector<VkFramebuffer> swapchainFramebuffers;
    VkImageView colorImageView;
    VkImageView depthImageView;

    void createTextureImage();
    uint32_t mipLevels;
    VkImage textureImage;
    VkDeviceMemory textureImageMemory;
    void createTextureImage2();
    uint32_t mipLevels2;
    VkImage textureImage2;
    VkDeviceMemory textureImageMemory2;
    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                           uint32_t height);
    void generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth,
                         int32_t texHeight, uint32_t mipLevels);
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties, VkBuffer& buffer,
                      VkDeviceMemory& bufferMemory);

    void createTextureImageView();
    VkImageView textureImageView;
    void createTextureImageView2();
    VkImageView textureImageView2;

    void createTextureSampler();
    VkSampler textureSampler;
    void createTextureSampler2();
    VkSampler textureSampler2;

    Assimp::Importer importer;
    void loadModel();
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    void loadModel2();
    std::vector<uint32_t> indices2;

    void createVertexBuffer();
    VkBuffer vertexBuffer;
    VkDeviceMemory vertexBufferMemory;
    void createVertexBuffer2();
    VkBuffer vertexBuffer2;
    VkDeviceMemory vertexBufferMemory2;
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);

    void createIndexBuffer();
    VkBuffer indexBuffer;
    VkDeviceMemory indexBufferMemory;
    void createIndexBuffer2();
    VkBuffer indexBuffer2;
    VkDeviceMemory indexBufferMemory2;

    //void createUniformBuffers();
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;
    void createUniformBuffers2();
    std::vector<VkBuffer> uniformBuffers2;
    std::vector<VkDeviceMemory> uniformBuffersMemory2;
    std::vector<void*> uniformBuffersMapped2;

    void createDescriptorPool();
    VkDescriptorPool descriptorPool;

    //void createDescriptorSets();
    std::vector<VkDescriptorSet> descriptorSets;
    void createDescriptorSets2();
    std::vector<VkDescriptorSet> descriptorSets2;

    void createCommandBuffers();
    std::vector<VkCommandBuffer> commandBuffers;

    void createSyncObjects();
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;

    void recordCommandBuffer(VkCommandBuffer commandBuffer,
                             uint32_t imageIndex);
    void updateUniformBuffer(uint32_t currentImage);
    void updateUniformBuffer2(uint32_t currentImage);

    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);
    void recreateSwapchain();
    void cleanupSwapchain();
    bool framebufferResized = false;

    void destroyDebugUtilsMessengerEXT(VkInstance instance,
                                       VkDebugUtilsMessengerEXT debugMessenger,
                                       const VkAllocationCallbacks* pAllocator);
    
    static void checkImguiVkResult(VkResult err);

    void drawImguiFrame(VkCommandBuffer commandBuffer);

    void createTextureImages();
    void createTextureImageViews();
    void createTextureSamplers();
    void createVertexBuffers();
    void createIndexBuffers();
    void createUniformBuffers();
    void createDescriptorSets();
};

#endif  // VULKAN_CONTEXT_H
