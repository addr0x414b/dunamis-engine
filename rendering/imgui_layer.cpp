#include "imgui_layer.h"

#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>

#include <string>

namespace {

void checkImGuiVulkanResult(VkResult result) noexcept {
    if (result == VK_SUCCESS) {
        return;
    }

    try {
        if (result < VK_SUCCESS) {
            spdlog::error("Dear ImGui Vulkan backend reported VkResult {}",
                          static_cast<int>(result));
        } else {
            spdlog::warn("Dear ImGui Vulkan backend reported VkResult {}",
                         static_cast<int>(result));
        }
    } catch (...) {
        // Never allow logging failures to escape through a C callback.
    }
}

Result invalidInitializationValue(const char* value) {
    return Result::failure(std::string("Cannot initialize Dear ImGui with ") +
                           value);
}

}  // namespace

ImGuiLayer::~ImGuiLayer() noexcept {
    shutdown();
}

Result ImGuiLayer::initialize(
    SDL_Window* window, VkInstance instance,
    VkPhysicalDevice physicalDevice, VkDevice device,
    std::uint32_t graphicsQueueFamily, VkQueue graphicsQueue,
    VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
    std::uint32_t minimumImageCount, std::uint32_t imageCount) {
    if (initialized() || contextCreated_) {
        return Result::failure("Dear ImGui is already initialized");
    }
    if (!window) return invalidInitializationValue("a null SDL window");
    if (instance == VK_NULL_HANDLE) {
        return invalidInitializationValue("a null Vulkan instance");
    }
    if (physicalDevice == VK_NULL_HANDLE) {
        return invalidInitializationValue("a null Vulkan physical device");
    }
    if (device == VK_NULL_HANDLE) {
        return invalidInitializationValue("a null Vulkan device");
    }
    if (graphicsQueue == VK_NULL_HANDLE) {
        return invalidInitializationValue("a null Vulkan graphics queue");
    }
    if (renderPass == VK_NULL_HANDLE) {
        return invalidInitializationValue("a null Vulkan render pass");
    }
    if (minimumImageCount < 2 || imageCount < minimumImageCount) {
        return Result::failure(
            "Dear ImGui requires at least two swapchain images and an actual "
            "image count no smaller than the minimum");
    }

    instance_ = instance;
    physicalDevice_ = physicalDevice;
    device_ = device;
    graphicsQueueFamily_ = graphicsQueueFamily;
    graphicsQueue_ = graphicsQueue;
    renderPass_ = renderPass;
    msaaSamples_ = msaaSamples;
    minimumImageCount_ = minimumImageCount;
    imageCount_ = imageCount;

    IMGUI_CHECKVERSION();
    if (!ImGui::CreateContext()) {
        shutdown();
        return Result::failure("Failed to create Dear ImGui context");
    }
    contextCreated_ = true;

    ImGuiIO& io = ImGui::GetIO();
    // Docking is enabled inside the existing SDL window.
    // Native multi-viewport windows remain disabled.
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    ImGui::StyleColorsDark();

    if (!ImGui_ImplSDL3_InitForVulkan(window)) {
        shutdown();
        return Result::failure(
            "Failed to initialize Dear ImGui SDL3 backend");
    }
    sdlBackendInitialized_ = true;

    Result result = initializeVulkanBackend();
    if (!result) {
        shutdown();
        return result;
    }

    return Result::success();
}

Result ImGuiLayer::initializeVulkanBackend() {
    ImGui_ImplVulkan_InitInfo initInfo{};
    initInfo.ApiVersion = VK_API_VERSION_1_3;
    initInfo.Instance = instance_;
    initInfo.PhysicalDevice = physicalDevice_;
    initInfo.Device = device_;
    initInfo.QueueFamily = graphicsQueueFamily_;
    initInfo.Queue = graphicsQueue_;
    initInfo.DescriptorPool = VK_NULL_HANDLE;
    initInfo.DescriptorPoolSize = 64;
    initInfo.MinImageCount = minimumImageCount_;
    initInfo.ImageCount = imageCount_;
    initInfo.PipelineInfoMain.RenderPass = renderPass_;
    initInfo.PipelineInfoMain.Subpass = 0;
    initInfo.PipelineInfoMain.MSAASamples = msaaSamples_;
    initInfo.UseDynamicRendering = false;
    initInfo.Allocator = nullptr;
    initInfo.CheckVkResultFn = checkImGuiVulkanResult;

    if (!ImGui_ImplVulkan_Init(&initInfo)) {
        return Result::failure(
            "Failed to initialize Dear ImGui Vulkan backend");
    }
    vulkanBackendInitialized_ = true;
    return Result::success();
}

void ImGuiLayer::processEvent(const SDL_Event& event) noexcept {
    if (!sdlBackendInitialized_) {
        return;
    }
    (void)ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::setInputEnabled(bool enabled) noexcept {
    if (!contextCreated_) {
        return;
    }

    ImGuiIO& io = ImGui::GetIO();
    if (enabled) {
        io.ClearEventsQueue();
        io.ClearInputKeys();
        io.ClearInputMouse();
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoKeyboard;
        io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        io.SetAppAcceptingEvents(true);
        return;
    }

    io.SetAppAcceptingEvents(false);
    io.ClearEventsQueue();
    io.ClearInputKeys();
    io.ClearInputMouse();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
    io.ConfigFlags |= ImGuiConfigFlags_NoKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::SetWindowFocus(nullptr);
}

Result ImGuiLayer::beginFrame() {
    if (!initialized()) {
        return Result::failure("Cannot begin an uninitialized Dear ImGui frame");
    }
    if (frameStarted_) {
        return Result::failure("Dear ImGui frame has already begun");
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    ImGui::DockSpaceOverViewport(
        0, ImGui::GetMainViewport(),
        ImGuiDockNodeFlags_PassthruCentralNode);
    frameStarted_ = true;
    drawDataReady_ = false;
    return Result::success();
}

void ImGuiLayer::drawTestWindow() {
    if (!frameStarted_) {
        return;
    }

    ImGui::Begin("Dunamis Debug UI");
    ImGui::TextUnformatted("Dear ImGui is active.");
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::Text("Framerate: %.1f FPS", io.Framerate);
    ImGui::Text("Frame time: %.3f ms",
                io.Framerate > 0.0f ? 1000.0f / io.Framerate : 0.0f);
    ImGui::Checkbox("Show Dear ImGui demo window", &showDemoWindow_);
    ImGui::End();

    if (showDemoWindow_) {
        ImGui::ShowDemoWindow(&showDemoWindow_);
    }
}

void ImGuiLayer::finishFrame() {
    if (!frameStarted_) {
        return;
    }
    ImGui::Render();
    frameStarted_ = false;
    drawDataReady_ = true;
}

void ImGuiLayer::recordDrawData(VkCommandBuffer commandBuffer) {
    if (!initialized() || !drawDataReady_ ||
        commandBuffer == VK_NULL_HANDLE) {
        return;
    }
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), commandBuffer);
}

Result ImGuiLayer::onSwapchainRecreated(
    VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
    std::uint32_t minimumImageCount, std::uint32_t imageCount) {
    if (!initialized()) {
        return Result::failure(
            "Cannot update an uninitialized Dear ImGui Vulkan backend");
    }
    if (renderPass == VK_NULL_HANDLE || minimumImageCount < 2 ||
        imageCount < minimumImageCount) {
        return Result::failure(
            "Invalid swapchain state for Dear ImGui Vulkan backend");
    }

    const bool requiresBackendReinitialization =
        imageCount != imageCount_ || renderPass != renderPass_ ||
        msaaSamples != msaaSamples_;

    renderPass_ = renderPass;
    msaaSamples_ = msaaSamples;
    minimumImageCount_ = minimumImageCount;
    imageCount_ = imageCount;

    if (requiresBackendReinitialization) {
        ImGui_ImplVulkan_Shutdown();
        vulkanBackendInitialized_ = false;
        Result result = initializeVulkanBackend();
        if (!result) {
            return Result::failure(
                "Failed to reinitialize Dear ImGui Vulkan backend after "
                "swapchain recreation: " + result.error());
        }
    } else {
        ImGui_ImplVulkan_SetMinImageCount(minimumImageCount_);
    }

    return Result::success();
}

bool ImGuiLayer::initialized() const noexcept {
    return contextCreated_ && sdlBackendInitialized_ &&
           vulkanBackendInitialized_;
}

void ImGuiLayer::shutdown() noexcept {
    frameStarted_ = false;
    drawDataReady_ = false;

    if (vulkanBackendInitialized_) {
        ImGui_ImplVulkan_Shutdown();
        vulkanBackendInitialized_ = false;
    }
    if (sdlBackendInitialized_) {
        ImGui_ImplSDL3_Shutdown();
        sdlBackendInitialized_ = false;
    }
    if (contextCreated_) {
        ImGui::DestroyContext();
        contextCreated_ = false;
    }

    abandon();
}

void ImGuiLayer::abandon() noexcept {
    contextCreated_ = false;
    sdlBackendInitialized_ = false;
    vulkanBackendInitialized_ = false;
    frameStarted_ = false;
    drawDataReady_ = false;
    instance_ = VK_NULL_HANDLE;
    physicalDevice_ = VK_NULL_HANDLE;
    device_ = VK_NULL_HANDLE;
    graphicsQueueFamily_ = 0;
    graphicsQueue_ = VK_NULL_HANDLE;
    renderPass_ = VK_NULL_HANDLE;
    msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    minimumImageCount_ = 0;
    imageCount_ = 0;
    showDemoWindow_ = false;
}
