#ifndef VULKAN_CONTEXT_H
#define VULKAN_CONTEXT_H

#include <algorithm>
#include <fstream>
#include <optional>
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <set>
#include "spdlog/spdlog.h"
#include <vulkan/vulkan.h>
#include "utils/vulkan_utils.h"
#include "../scene/game_object.h"
#include "../scene/scene.h"
#include <glm/glm.hpp>

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

const int MAX_FRAMES_IN_FLIGHT = 2;

class VulkanContext {
public:
    void init(SDL_Window* w);
    void cleanup(Scene* scene);

    void createTextureImages(std::unique_ptr<GameObject>& gameObject);
    void createTextureImageViews(std::unique_ptr<GameObject>& gameObject);
    void createTextureSamplers(std::unique_ptr<GameObject>& gameObject);
    void createVertexBuffers(std::unique_ptr<GameObject>& gameObject);
    void createIndexBuffers(std::unique_ptr<GameObject>& gameObject);
    void createUniformBuffers(std::unique_ptr<GameObject>& gameObject);
    void createDescriptorPool(uint32_t numOfObjects);
    void createDescriptorSets(std::unique_ptr<GameObject>& gameObject);

    void drawFrame(Scene* scene);

private:
    VkDescriptorPool descriptorPool;
    SDL_Window* window;

    bool framebufferResized = false;

    void updateUniformBuffer(uint32_t currentImage, std::unique_ptr<GameObject>& gameObject, glm::vec3 camPos, glm::vec3 camFront, glm::vec3 camUp);

    void recreateSwapchain();
    void recordCommandBuffer(VkCommandBuffer commandBuffer,
                            uint32_t imageIndex, Scene* scene);

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
    void destroyDebugUtilsMessengerEXT(VkInstance instance,
                                       VkDebugUtilsMessengerEXT debugMessenger,
                                       const VkAllocationCallbacks* pAllocator);

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
    VkDevice device;

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
    void cleanupSwapchain();

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
    VkImageView colorImageView;
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
    VkImageView depthImageView;
    VkDeviceMemory depthImageMemory;
    void transitionImageLayout(VkImage image, VkFormat format,
                               VkImageLayout oldLayout, VkImageLayout newLayout,
                               uint32_t mipLevels);
    bool hasStencilComponent(VkFormat format);

    VkCommandBuffer beginSingleTimeCommands();
    void endSingleTimeCommands(VkCommandBuffer commandBuffer);

    void createFramebuffers();
    std::vector<VkFramebuffer> swapchainFramebuffers;

    void copyBufferToImage(VkBuffer buffer, VkImage image, uint32_t width,
                           uint32_t height);
    void generateMipmaps(VkImage image, VkFormat imageFormat, int32_t texWidth,
                         int32_t texHeight, uint32_t mipLevels);
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags properties, VkBuffer& buffer,
                      VkDeviceMemory& bufferMemory);
    void copyBuffer(VkBuffer srcBuffer, VkBuffer dstBuffer, VkDeviceSize size);
    
    void createCommandBuffers();
    std::vector<VkCommandBuffer> commandBuffers;

    void createSyncObjects();
    std::vector<VkSemaphore> imageAvailableSemaphores;
    std::vector<VkSemaphore> renderFinishedSemaphores;
    std::vector<VkFence> inFlightFences;
    uint32_t currentFrame = 0;

};

#endif