#include "imgui_layer.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <string>

#include "../scene/directional_light.h"
#include "../scene/game_object.h"
#include "../scene/point_light.h"
#include "../scene/scene.h"

namespace {

constexpr const char* dunamisDockspaceName =
    "DunamisEditorDockspace_v1";
constexpr float minDirectionalDirectionLengthSquared = 1.0e-8f;

void buildDefaultDockLayout(ImGuiID dockspaceId,
                            const ImVec2& viewportSize) {
    if (ImGui::DockBuilderGetNode(dockspaceId) != nullptr) {
        return;
    }

    ImGui::DockBuilderAddNode(dockspaceId, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(dockspaceId, viewportSize);

    ImGuiID centralDockId = dockspaceId;
    ImGuiID leftDockId = 0;
    ImGuiID rightDockId = 0;

    ImGui::DockBuilderSplitNode(centralDockId, ImGuiDir_Left, 0.20f,
                                &leftDockId, &centralDockId);
    ImGui::DockBuilderSplitNode(centralDockId, ImGuiDir_Right, 0.25f,
                                &rightDockId, &centralDockId);

    ImGui::DockBuilderDockWindow("Scene Hierarchy", leftDockId);
    ImGui::DockBuilderDockWindow("Inspector", rightDockId);
    ImGui::DockBuilderFinish(dockspaceId);
}

bool isFiniteVector(const glm::vec3& vector) noexcept {
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
           std::isfinite(vector.z);
}

bool isFiniteNonnegativeVector(const glm::vec3& vector) noexcept {
    return isFiniteVector(vector) && vector.x >= 0.0f &&
           vector.y >= 0.0f && vector.z >= 0.0f;
}

bool isFiniteNonnegative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f;
}

bool isValidDirectionalDirection(const glm::vec3& direction) noexcept {
    if (!isFiniteVector(direction)) {
        return false;
    }

    const float lengthSquared = glm::dot(direction, direction);
    return std::isfinite(lengthSquared) &&
           lengthSquared > minDirectionalDirectionLengthSquared;
}

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

Result ImGuiLayer::beginFrame(SceneRunState runState) {
    if (!initialized()) {
        return Result::failure("Cannot begin an uninitialized Dear ImGui frame");
    }
    if (frameStarted_) {
        return Result::failure("Dear ImGui frame has already begun");
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    drawToolbar(runState);
    const ImGuiID dockspaceId = ImGui::GetID(dunamisDockspaceName);
    const ImGuiViewport* mainViewport = ImGui::GetMainViewport();
    buildDefaultDockLayout(dockspaceId, mainViewport->WorkSize);
    ImGui::DockSpaceOverViewport(
        dockspaceId, mainViewport, ImGuiDockNodeFlags_PassthruCentralNode);
    frameStarted_ = true;
    drawDataReady_ = false;
    return Result::success();
}

void ImGuiLayer::drawEditor(Scene* scene, SceneRunState runState) {
    synchronizeSelection(scene);
    if (!frameStarted_) {
        return;
    }

    if (scene != nullptr) {
        const bool disabled = runState == SceneRunState::Playing;
        drawSceneHierarchy(scene, disabled);
        drawInspector(scene, disabled);
    }
}

void ImGuiLayer::drawToolbar(SceneRunState runState) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    const bool editing = runState == SceneRunState::Editing;
    ImGui::BeginDisabled(!editing);
    if (ImGui::Button("Play") &&
        pendingEditorCommand_ == EditorCommand::None) {
        pendingEditorCommand_ = EditorCommand::Play;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(editing);
    if (ImGui::Button("Stop") &&
        pendingEditorCommand_ == EditorCommand::None) {
        pendingEditorCommand_ = EditorCommand::Stop;
    }
    ImGui::EndDisabled();
    ImGui::EndMainMenuBar();
}

const GameObject* ImGuiLayer::selectedGameObjectForScene(
    const Scene* scene) const noexcept {
    if (scene == nullptr || scene != selectionScene_ ||
        selectedGameObject_ == nullptr) {
        return nullptr;
    }

    for (const auto& object : scene->gameObjects()) {
        if (object.get() == selectedGameObject_) {
            return selectedGameObject_;
        }
    }
    return nullptr;
}

void ImGuiLayer::clearSelection() noexcept {
    selectedGameObject_ = nullptr;
    inspectorError_.clear();
}

EditorCommand ImGuiLayer::consumeEditorCommand() noexcept {
    const EditorCommand command = pendingEditorCommand_;
    pendingEditorCommand_ = EditorCommand::None;
    return command;
}

bool ImGuiLayer::sceneInteractionAreaHovered() const noexcept {
    return sceneInteractionAreaHovered_;
}

void ImGuiLayer::synchronizeSelection(Scene* scene) noexcept {
    if (scene != selectionScene_) {
        selectionScene_ = scene;
        clearSelection();
    }

    if (scene == nullptr) {
        selectionScene_ = nullptr;
        clearSelection();
        return;
    }

    if (selectedGameObject_ == nullptr) {
        return;
    }

    bool selectedObjectIsPresent = false;
    for (const auto& object : scene->gameObjects()) {
        if (object.get() == selectedGameObject_) {
            selectedObjectIsPresent = true;
            break;
        }
    }

    if (!selectedObjectIsPresent) {
        clearSelection();
    }
}

void ImGuiLayer::drawSceneHierarchy(Scene* scene, bool disabled) {
    ImGui::SetNextWindowSize(ImVec2(300.0f, 400.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Hierarchy")) {
        ImGui::End();
        return;
    }

    ImGui::BeginDisabled(disabled);

    bool hasObject = false;
    for (const auto& objectOwner : scene->gameObjects()) {
        GameObject* object = objectOwner.get();
        if (object == nullptr) {
            continue;
        }

        hasObject = true;
        ImGui::PushID(static_cast<const void*>(object));
        const char* label = object->name.empty()
                                ? "<Unnamed GameObject>"
                                : object->name.c_str();
        const bool selected = selectedGameObject_ == object;
        if (ImGui::Selectable(label, selected)) {
            selectedGameObject_ = object;
            inspectorError_.clear();
        }
        ImGui::PopID();
    }

    if (!hasObject) {
        ImGui::TextUnformatted("No GameObjects in the active scene.");
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x > 0.0f && available.y > 0.0f &&
        ImGui::InvisibleButton("##SceneHierarchyBlankSpace", available)) {
        clearSelection();
    }

    ImGui::EndDisabled();
    ImGui::End();
}

void ImGuiLayer::drawInspector(Scene* scene, bool disabled) {
    ImGui::SetNextWindowSize(ImVec2(340.0f, 500.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector")) {
        ImGui::End();
        return;
    }

    ImGui::BeginDisabled(disabled);

    if (scene == nullptr || selectedGameObject_ == nullptr) {
        ImGui::TextUnformatted(
            "Select a GameObject from the Scene Hierarchy.");
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }

    const char* objectName = selectedGameObject_->name.empty()
                                 ? "<Unnamed GameObject>"
                                 : selectedGameObject_->name.c_str();
    PointLight* pointLight = dynamic_cast<PointLight*>(selectedGameObject_);
    DirectionalLight* directionalLight =
        dynamic_cast<DirectionalLight*>(selectedGameObject_);
    const char* objectType = pointLight != nullptr
                                 ? "Point Light"
                                 : directionalLight != nullptr
                                       ? "Directional Light"
                                       : "GameObject";

    ImGui::Text("Name: %s", objectName);
    ImGui::Text("Type: %s", objectType);

    ImGui::SeparatorText("Transform");
    glm::vec3 position = selectedGameObject_->position;
    if (ImGui::DragFloat3("Position", &position.x, 0.1f)) {
        if (isFiniteVector(position)) {
            selectedGameObject_->position = position;
            inspectorError_.clear();
        } else {
            inspectorError_ = "Transform values must be finite.";
        }
    }

    glm::vec3 rotation = selectedGameObject_->rotation;
    if (ImGui::DragFloat3("Rotation (degrees)", &rotation.x, 0.5f)) {
        if (isFiniteVector(rotation)) {
            selectedGameObject_->rotation = rotation;
            inspectorError_.clear();
        } else {
            inspectorError_ = "Transform values must be finite.";
        }
    }

    glm::vec3 scale = selectedGameObject_->scale;
    if (ImGui::DragFloat3("Scale", &scale.x, 0.01f)) {
        if (isFiniteVector(scale)) {
            selectedGameObject_->scale = scale;
            inspectorError_.clear();
        } else {
            inspectorError_ = "Transform values must be finite.";
        }
    }

    if (pointLight != nullptr) {
        ImGui::SeparatorText("Point Light");

        glm::vec3 color = pointLight->color;
        if (ImGui::ColorEdit3("Color", &color.x,
                              ImGuiColorEditFlags_HDR |
                                  ImGuiColorEditFlags_Float)) {
            if (isFiniteNonnegativeVector(color)) {
                pointLight->color = color;
                inspectorError_.clear();
            } else {
                inspectorError_ =
                    "Color components must be finite and nonnegative.";
            }
        }

        float intensity = pointLight->intensity;
        if (ImGui::DragFloat("Intensity", &intensity, 0.1f)) {
            if (isFiniteNonnegative(intensity)) {
                pointLight->intensity = intensity;
                inspectorError_.clear();
            } else {
                inspectorError_ =
                    "Intensity must be finite and nonnegative.";
            }
        }
    }

    if (directionalLight != nullptr) {
        ImGui::SeparatorText("Directional Light");

        glm::vec3 direction = directionalLight->direction;
        if (ImGui::DragFloat3("Direction", &direction.x, 0.01f)) {
            if (isValidDirectionalDirection(direction)) {
                directionalLight->direction = direction;
                inspectorError_.clear();
            } else {
                inspectorError_ =
                    "Direction must be finite and nonzero.";
            }
        }

        glm::vec3 color = directionalLight->color;
        if (ImGui::ColorEdit3("Color", &color.x,
                              ImGuiColorEditFlags_HDR |
                                  ImGuiColorEditFlags_Float)) {
            if (isFiniteNonnegativeVector(color)) {
                directionalLight->color = color;
                inspectorError_.clear();
            } else {
                inspectorError_ =
                    "Color components must be finite and nonnegative.";
            }
        }

        float intensity = directionalLight->intensity;
        if (ImGui::DragFloat("Intensity", &intensity, 0.1f)) {
            if (isFiniteNonnegative(intensity)) {
                directionalLight->intensity = intensity;
                inspectorError_.clear();
            } else {
                inspectorError_ =
                    "Intensity must be finite and nonnegative.";
            }
        }

        ImGui::TextWrapped(
            "Directional-light position does not affect lighting; use "
            "Direction.");
    }

    if (!inspectorError_.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s",
                           inspectorError_.c_str());
    }

    ImGui::EndDisabled();
    ImGui::End();
}

void ImGuiLayer::updateSceneInteractionAreaHovered() noexcept {
    sceneInteractionAreaHovered_ = false;
    ImGuiContext* context = ImGui::GetCurrentContext();
    if (context == nullptr) {
        return;
    }

    const ImGuiID dockspaceId = ImGui::GetID(dunamisDockspaceName);
    const ImGuiDockNode* centralNode =
        ImGui::DockBuilderGetCentralNode(dockspaceId);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (centralNode == nullptr || viewport == nullptr) {
        return;
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const ImRect viewportRect(
        viewport->Pos,
        ImVec2(viewport->Pos.x + viewport->Size.x,
               viewport->Pos.y + viewport->Size.y));
    const ImRect centralRect(
        centralNode->Pos,
        ImVec2(centralNode->Pos.x + centralNode->Size.x,
               centralNode->Pos.y + centralNode->Size.y));
    const bool imguiObstructed = context->HoveredWindow != nullptr ||
                                 context->HoveredId != 0 ||
                                 context->ActiveId != 0;
    sceneInteractionAreaHovered_ = viewportRect.Contains(mouse) &&
                                   centralRect.Contains(mouse) &&
                                   !imguiObstructed;
}

void ImGuiLayer::finishFrame() {
    if (!frameStarted_) {
        return;
    }
    updateSceneInteractionAreaHovered();
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
    selectionScene_ = nullptr;
    selectedGameObject_ = nullptr;
    pendingEditorCommand_ = EditorCommand::None;
    sceneInteractionAreaHovered_ = false;
    inspectorError_.clear();

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
    selectionScene_ = nullptr;
    selectedGameObject_ = nullptr;
    pendingEditorCommand_ = EditorCommand::None;
    sceneInteractionAreaHovered_ = false;
    inspectorError_.clear();
}
