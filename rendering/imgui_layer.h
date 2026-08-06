#ifndef IMGUI_LAYER_H
#define IMGUI_LAYER_H

#include <cstdint>

#include <SDL3/SDL.h>
#include <vulkan/vulkan.h>

#include "../core/result.h"

class ImGuiLayer final {
public:
    ImGuiLayer() = default;
    ~ImGuiLayer() noexcept;

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    [[nodiscard]] Result initialize(
        SDL_Window* window, VkInstance instance,
        VkPhysicalDevice physicalDevice, VkDevice device,
        std::uint32_t graphicsQueueFamily, VkQueue graphicsQueue,
        VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
        std::uint32_t minimumImageCount, std::uint32_t imageCount);

    void processEvent(const SDL_Event& event) noexcept;
    void setInputEnabled(bool enabled) noexcept;

    [[nodiscard]] Result beginFrame();
    void drawTestWindow();
    void finishFrame();
    void recordDrawData(VkCommandBuffer commandBuffer);

    [[nodiscard]] Result onSwapchainRecreated(
        VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
        std::uint32_t minimumImageCount, std::uint32_t imageCount);

    [[nodiscard]] bool initialized() const noexcept;

    void shutdown() noexcept;
    void abandon() noexcept;

private:
    [[nodiscard]] Result initializeVulkanBackend();

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t graphicsQueueFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    std::uint32_t minimumImageCount_ = 0;
    std::uint32_t imageCount_ = 0;

    bool contextCreated_ = false;
    bool sdlBackendInitialized_ = false;
    bool vulkanBackendInitialized_ = false;
    bool frameStarted_ = false;
    bool drawDataReady_ = false;
    bool showDemoWindow_ = false;
};

#endif
