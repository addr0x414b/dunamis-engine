#include "imgui_layer.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>
#include <spdlog/spdlog.h>

#include <cmath>
#include <limits>
#include <string>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

#include "editor_picking.h"
#include "../scene/directional_light.h"
#include "../scene/game_object.h"
#include "../scene/point_light.h"
#include "../scene/scene.h"

namespace {

constexpr const char* dunamisDockspaceName =
    "DunamisEditorDockspace_v1";
constexpr float minDirectionalDirectionLengthSquared = 1.0e-8f;
constexpr float minCameraBasisLengthSquared = 1.0e-8f;
constexpr float cameraVisualizationDistance = 8.0f;

void applyDunamisEditorStyle() {
    ImGuiStyle& style = ImGui::GetStyle();

    const ImVec4 darkAccent(0.4196f, 0.1725f, 0.0549f, 1.0f);
    const ImVec4 normalAccent(0.5608f, 0.2392f, 0.0706f, 1.0f);
    const ImVec4 hoverAccent(0.7255f, 0.3333f, 0.0863f, 1.0f);
    const ImVec4 activeAccent(0.8667f, 0.4510f, 0.1216f, 1.0f);
    const ImVec4 darkNeutral(0.10f, 0.08f, 0.07f, 0.94f);
    const auto withAlpha = [](const ImVec4& color, float alpha) {
        return ImVec4(color.x, color.y, color.z, alpha);
    };

    style.Colors[ImGuiCol_FrameBg] = darkNeutral;
    style.Colors[ImGuiCol_FrameBgHovered] = withAlpha(darkAccent, 0.45f);
    style.Colors[ImGuiCol_FrameBgActive] = withAlpha(normalAccent, 0.60f);
    style.Colors[ImGuiCol_TitleBgActive] = darkNeutral;

    style.Colors[ImGuiCol_Button] = darkAccent;
    style.Colors[ImGuiCol_ButtonHovered] = hoverAccent;
    style.Colors[ImGuiCol_ButtonActive] = activeAccent;

    style.Colors[ImGuiCol_Header] = withAlpha(darkAccent, 0.50f);
    style.Colors[ImGuiCol_HeaderHovered] = withAlpha(hoverAccent, 0.70f);
    style.Colors[ImGuiCol_HeaderActive] = withAlpha(activeAccent, 0.90f);

    style.Colors[ImGuiCol_Separator] = withAlpha(darkAccent, 0.45f);
    style.Colors[ImGuiCol_SeparatorHovered] = withAlpha(hoverAccent, 0.80f);
    style.Colors[ImGuiCol_SeparatorActive] = activeAccent;

    style.Colors[ImGuiCol_CheckMark] = activeAccent;
    style.Colors[ImGuiCol_CheckboxSelectedBg] =
        withAlpha(normalAccent, 0.65f);
    style.Colors[ImGuiCol_SliderGrab] = normalAccent;
    style.Colors[ImGuiCol_SliderGrabActive] = activeAccent;

    style.Colors[ImGuiCol_ResizeGrip] = withAlpha(darkAccent, 0.35f);
    style.Colors[ImGuiCol_ResizeGripHovered] =
        withAlpha(hoverAccent, 0.65f);
    style.Colors[ImGuiCol_ResizeGripActive] = withAlpha(activeAccent, 0.90f);

    style.Colors[ImGuiCol_Tab] = withAlpha(darkAccent, 0.45f);
    style.Colors[ImGuiCol_TabHovered] = withAlpha(hoverAccent, 0.80f);
    style.Colors[ImGuiCol_TabSelected] = withAlpha(normalAccent, 0.85f);
    style.Colors[ImGuiCol_TabSelectedOverline] = activeAccent;
    style.Colors[ImGuiCol_TabDimmed] = withAlpha(darkAccent, 0.25f);
    style.Colors[ImGuiCol_TabDimmedSelected] =
        withAlpha(normalAccent, 0.55f);
    style.Colors[ImGuiCol_TabDimmedSelectedOverline] =
        withAlpha(activeAccent, 0.55f);

    style.Colors[ImGuiCol_DockingPreview] = withAlpha(hoverAccent, 0.45f);
    style.Colors[ImGuiCol_TextSelectedBg] = withAlpha(normalAccent, 0.45f);
    style.Colors[ImGuiCol_DragDropTarget] = withAlpha(activeAccent, 0.95f);
    style.Colors[ImGuiCol_NavCursor] = withAlpha(activeAccent, 0.85f);
    style.Colors[ImGuiCol_TextLink] = activeAccent;
}

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

bool isFiniteVector(const glm::vec4& vector) noexcept {
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
           std::isfinite(vector.z) && std::isfinite(vector.w);
}

bool isFiniteMatrix(const glm::mat4& matrix) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool normalizeFinite(const glm::vec3& value, glm::vec3& normalized) noexcept {
    normalized = {};
    if (!isFiniteVector(value)) {
        return false;
    }

    const float lengthSquared = glm::dot(value, value);
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= minCameraBasisLengthSquared) {
        return false;
    }
    const float length = std::sqrt(lengthSquared);
    if (!std::isfinite(length) || length <= 0.0f) {
        return false;
    }

    normalized = value / length;
    return isFiniteVector(normalized);
}

bool makeCameraBasis(const Camera& camera, glm::vec3& forward,
                     glm::vec3& right, glm::vec3& up) noexcept {
    if (!isFiniteVector(camera.position) ||
        !normalizeFinite(camera.front, forward)) {
        return false;
    }

    glm::vec3 normalizedUp;
    if (!normalizeFinite(camera.up, normalizedUp)) {
        return false;
    }

    if (!normalizeFinite(glm::cross(forward, normalizedUp), right)) {
        return false;
    }
    if (!normalizeFinite(glm::cross(right, forward), up)) {
        return false;
    }

    return isFiniteVector(forward) && isFiniteVector(right) &&
           isFiniteVector(up);
}

bool projectWorldToImGui(const glm::vec3& worldPoint,
                         const glm::mat4& editorView,
                         const glm::mat4& projection,
                         const ImGuiViewport& viewport,
                         ImVec2& screenPoint) noexcept {
    screenPoint = {};
    if (!isFiniteVector(worldPoint) || !isFiniteMatrix(editorView) ||
        !isFiniteMatrix(projection) || !std::isfinite(viewport.Pos.x) ||
        !std::isfinite(viewport.Pos.y) || !std::isfinite(viewport.Size.x) ||
        !std::isfinite(viewport.Size.y) || viewport.Size.x <= 0.0f ||
        viewport.Size.y <= 0.0f) {
        return false;
    }

    const glm::vec4 clip =
        projection * editorView * glm::vec4(worldPoint, 1.0f);
    if (!isFiniteVector(clip) || clip.w <= 1.0e-6f) {
        return false;
    }

    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (!isFiniteVector(ndc)) {
        return false;
    }

    // The renderer already flips projection Y for Vulkan. Mapping NDC Y
    // directly therefore converts world-up to the smaller ImGui screen Y.
    screenPoint = ImVec2(
        viewport.Pos.x + (ndc.x * 0.5f + 0.5f) * viewport.Size.x,
        viewport.Pos.y + (ndc.y * 0.5f + 0.5f) * viewport.Size.y);
    return std::isfinite(screenPoint.x) && std::isfinite(screenPoint.y);
}

bool applyObjectPosition(GameObject& object,
                         const glm::vec3& newPosition) noexcept {
    if (!isFiniteVector(object.position) || !isFiniteVector(newPosition)) {
        return false;
    }

    const glm::vec3 delta = newPosition - object.position;
    if (!isFiniteVector(delta)) {
        return false;
    }

    const bool isStandaloneCamera =
        dynamic_cast<Camera*>(&object) != nullptr;
    Camera* attachedCamera =
        isStandaloneCamera ? nullptr : object.attachedCamera();
    glm::vec3 newCameraPosition;
    if (attachedCamera != nullptr) {
        if (!isFiniteVector(attachedCamera->position)) {
            return false;
        }
        newCameraPosition = attachedCamera->position + delta;
        if (!isFiniteVector(newCameraPosition)) {
            return false;
        }
    }

    object.position = newPosition;
    if (attachedCamera != nullptr) {
        attachedCamera->position = newCameraPosition;
    }
    return true;
}

bool applyObjectRotation(GameObject& object,
                         const glm::vec3& newRotation) noexcept {
    if (!isFiniteVector(object.rotation) || !isFiniteVector(newRotation)) {
        return false;
    }

    const glm::mat4 oldRotation =
        editor_picking::makeRotationMatrix(object.rotation);
    const glm::mat4 updatedRotation =
        editor_picking::makeRotationMatrix(newRotation);
    if (!isFiniteMatrix(oldRotation) || !isFiniteMatrix(updatedRotation)) {
        return false;
    }

    const glm::mat4 deltaRotation =
        updatedRotation * glm::inverse(oldRotation);
    if (!isFiniteMatrix(deltaRotation)) {
        return false;
    }

    Camera* standaloneCamera = dynamic_cast<Camera*>(&object);
    Camera* attachedCamera =
        standaloneCamera == nullptr ? object.attachedCamera() : nullptr;
    Camera* orientationCamera = standaloneCamera != nullptr
                                    ? standaloneCamera
                                    : attachedCamera;
    glm::vec3 newCameraPosition;
    if (orientationCamera != nullptr) {
        if (!isFiniteVector(orientationCamera->position)) {
            return false;
        }

        if (attachedCamera != nullptr) {
            if (!isFiniteVector(object.position)) {
                return false;
            }
            const glm::vec3 offset =
                attachedCamera->position - object.position;
            const glm::vec3 rotatedOffset = glm::vec3(
                deltaRotation * glm::vec4(offset, 0.0f));
            if (!isFiniteVector(offset) || !isFiniteVector(rotatedOffset)) {
                return false;
            }
            newCameraPosition = object.position + rotatedOffset;
            if (!isFiniteVector(newCameraPosition)) {
                return false;
            }
        }

        if (!orientationCamera->applyOrientationDelta(glm::mat3(deltaRotation))) {
            return false;
        }
    }

    object.rotation = newRotation;
    if (attachedCamera != nullptr) {
        attachedCamera->position = newCameraPosition;
    }
    return true;
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
    applyDunamisEditorStyle();

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
    inputEnabled_ = enabled;
    gizmoDragActive_ = false;
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
    ImGuizmo::BeginFrame();
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

void ImGuiLayer::drawEditor(Scene* scene, const glm::mat4& view,
                            const glm::mat4& projection,
                            SceneRunState runState) {
    synchronizeSelection(scene);
    if (!frameStarted_) {
        return;
    }

    if (scene != nullptr) {
        const bool disabled = runState == SceneRunState::Playing;
        drawSceneHierarchy(scene, disabled);
        updateSceneInteractionAreaHovered();
        drawCameraVisualizations(scene, view, projection, runState);
        drawTranslationGizmo(scene, view, projection, runState);
        processWorldSelection(scene, view, projection, runState);
        drawInspector(scene, disabled);
    }
}

void ImGuiLayer::drawToolbar(SceneRunState runState) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const float toolbarCursorX = ImGui::GetCursorPosX();
    const float availableWidth = ImGui::GetContentRegionAvail().x;
    const float playButtonWidth =
        ImGui::CalcTextSize("Play").x + 2.0f * style.FramePadding.x;
    const float stopButtonWidth =
        ImGui::CalcTextSize("Stop").x + 2.0f * style.FramePadding.x;
    const float buttonGroupWidth =
        playButtonWidth + style.ItemSpacing.x + stopButtonWidth;
    const float remainingWidth = availableWidth - buttonGroupWidth;
    const float groupOffset =
        remainingWidth > 0.0f ? remainingWidth * 0.5f : 0.0f;
    ImGui::SetCursorPosX(toolbarCursorX + groupOffset);

    const bool editing = runState == SceneRunState::Editing;
    ImGui::BeginDisabled(!editing);
    if (ImGui::Button("Play", ImVec2(playButtonWidth, 0.0f)) &&
        pendingEditorCommand_ == EditorCommand::None) {
        pendingEditorCommand_ = EditorCommand::Play;
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(editing);
    if (ImGui::Button("Stop", ImVec2(stopButtonWidth, 0.0f)) &&
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

void ImGuiLayer::selectGameObject(Scene* scene, GameObject* object) noexcept {
    if (scene == nullptr) {
        selectionScene_ = nullptr;
        clearSelection();
        return;
    }

    selectionScene_ = scene;
    if (object == nullptr) {
        clearSelection();
        return;
    }

    for (const auto& owner : scene->gameObjects()) {
        if (owner.get() == object) {
            selectedGameObject_ = object;
            inspectorError_.clear();
            return;
        }
    }
    clearSelection();
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
            selectGameObject(scene, object);
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

void ImGuiLayer::drawTranslationGizmo(Scene* scene, const glm::mat4& view,
                                      const glm::mat4& projection,
                                      SceneRunState runState) {
    if (runState != SceneRunState::Editing ||
        !sceneInteractionRect_.valid) {
        gizmoDragActive_ = false;
        return;
    }
    GameObject* selected = const_cast<GameObject*>(
        selectedGameObjectForScene(scene));
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (selected == nullptr || viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f) {
        gizmoDragActive_ = false;
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(sceneInteractionRect_.x + sceneInteractionRect_.width,
                         sceneInteractionRect_.y + sceneInteractionRect_.height);
    drawList->PushClipRect(clipMin, clipMax, true);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(viewport->Pos.x, viewport->Pos.y, viewport->Size.x,
                      viewport->Size.y);
    const glm::mat4 model = editor_picking::makeModelMatrix(
        selected->position, selected->rotation, selected->scale);
    const editor_picking::AggregateBounds aggregate =
        editor_picking::aggregateBounds(selected->meshInstances());
    const glm::vec3 worldCenter = editor_picking::worldBoundsCenter(
        aggregate, model, selected->position);
    glm::mat4 gizmoMatrix = editor_picking::makeTranslationMatrix(worldCenter);
    const glm::vec3 originalGizmoCenter(gizmoMatrix[3]);
    glm::mat4 imguizmoProjection = projection;
    imguizmoProjection[1][1] *= -1.0f;
    const bool manipulated = ImGuizmo::Manipulate(
        glm::value_ptr(view), glm::value_ptr(imguizmoProjection),
        ImGuizmo::TRANSLATE, ImGuizmo::WORLD, glm::value_ptr(gizmoMatrix));
    drawList->PopClipRect();

    if (manipulated) {
        const glm::vec3 manipulatedGizmoCenter(gizmoMatrix[3]);
        const glm::vec3 worldDelta =
            manipulatedGizmoCenter - originalGizmoCenter;
        if (isFiniteVector(manipulatedGizmoCenter) &&
            isFiniteVector(worldDelta) &&
            applyObjectPosition(*selected, selected->position + worldDelta)) {
            inspectorError_.clear();
        } else {
            inspectorError_ = "Transform values must be finite.";
        }
    }
    gizmoDragActive_ = ImGuizmo::IsUsing() &&
                       ImGui::IsMouseDown(ImGuiMouseButton_Left);
}

void ImGuiLayer::drawCameraVisualizations(
    Scene* scene, const glm::mat4& editorView, const glm::mat4& projection,
    SceneRunState runState) {
    if (runState != SceneRunState::Editing || scene == nullptr ||
        !sceneInteractionRect_.valid ||
        !std::isfinite(sceneInteractionRect_.x) ||
        !std::isfinite(sceneInteractionRect_.y) ||
        !std::isfinite(sceneInteractionRect_.width) ||
        !std::isfinite(sceneInteractionRect_.height) ||
        sceneInteractionRect_.width <= 0.0f ||
        sceneInteractionRect_.height <= 0.0f) {
        return;
    }

    std::vector<const GameObject*> objects;
    objects.reserve(scene->gameObjects().size());
    for (const auto& owner : scene->gameObjects()) {
        if (owner != nullptr) {
            objects.push_back(owner.get());
        }
    }

    const Camera* activeCamera = scene->activeCamera();
    const std::vector<const Camera*> cameras =
        editor_picking::collectCameraPointers(objects, activeCamera);
    for (const Camera* camera : cameras) {
        if (camera != nullptr) {
            drawCameraVisualization(*camera, editorView, projection,
                                    camera == activeCamera);
        }
    }
}

void ImGuiLayer::drawCameraVisualization(
    const Camera& camera, const glm::mat4& editorView,
    const glm::mat4& projection, bool active) {

    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f || !isFiniteMatrix(projection)) {
        return;
    }

    glm::vec3 forward;
    glm::vec3 right;
    glm::vec3 up;
    if (!makeCameraBasis(camera, forward, right, up)) {
        return;
    }

    const float horizontalProjectionScale = std::abs(projection[0][0]);
    const float verticalProjectionScale = std::abs(projection[1][1]);
    if (!std::isfinite(horizontalProjectionScale) ||
        !std::isfinite(verticalProjectionScale) ||
        horizontalProjectionScale <= minCameraBasisLengthSquared ||
        verticalProjectionScale <= minCameraBasisLengthSquared) {
        return;
    }

    const float halfWidth =
        cameraVisualizationDistance / horizontalProjectionScale;
    const float halfHeight =
        cameraVisualizationDistance / verticalProjectionScale;
    if (!std::isfinite(halfWidth) || !std::isfinite(halfHeight) ||
        halfWidth <= 0.0f || halfHeight <= 0.0f) {
        return;
    }

    const glm::vec3 apex = camera.position;
    const glm::vec3 planeCenter =
        apex + forward * cameraVisualizationDistance;
    const glm::vec3 topLeft =
        planeCenter + up * halfHeight - right * halfWidth;
    const glm::vec3 topRight =
        planeCenter + up * halfHeight + right * halfWidth;
    const glm::vec3 bottomRight =
        planeCenter - up * halfHeight + right * halfWidth;
    const glm::vec3 bottomLeft =
        planeCenter - up * halfHeight - right * halfWidth;
    if (!isFiniteVector(planeCenter) || !isFiniteVector(topLeft) ||
        !isFiniteVector(topRight) || !isFiniteVector(bottomRight) ||
        !isFiniteVector(bottomLeft)) {
        return;
    }

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    if (drawList == nullptr) {
        return;
    }

    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(
        sceneInteractionRect_.x + sceneInteractionRect_.width,
        sceneInteractionRect_.y + sceneInteractionRect_.height);
    const ImGuiCol accent = active ? ImGuiCol_ButtonActive
                                   : ImGuiCol_ButtonHovered;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[accent]);
    drawList->PushClipRect(clipMin, clipMax, true);

    const float lineThickness = active ? 2.5f : 2.0f;
    const auto drawSegment = [&](const glm::vec3& first,
                                 const glm::vec3& second) {
        ImVec2 firstScreen;
        ImVec2 secondScreen;
        if (projectWorldToImGui(first, editorView, projection, *viewport,
                                firstScreen) &&
            projectWorldToImGui(second, editorView, projection, *viewport,
                                secondScreen)) {
            drawList->AddLine(firstScreen, secondScreen, color,
                              lineThickness);
        }
    };

    drawSegment(apex, topLeft);
    drawSegment(apex, topRight);
    drawSegment(apex, bottomRight);
    drawSegment(apex, bottomLeft);
    drawSegment(topLeft, topRight);
    drawSegment(topRight, bottomRight);
    drawSegment(bottomRight, bottomLeft);
    drawSegment(bottomLeft, topLeft);
    drawSegment(apex, planeCenter);

    ImVec2 apexScreen;
    if (projectWorldToImGui(apex, editorView, projection, *viewport,
                            apexScreen)) {
        drawList->AddCircleFilled(apexScreen, 4.0f, color);
    }

    drawList->PopClipRect();
}

void ImGuiLayer::processWorldSelection(Scene* scene, const glm::mat4& view,
                                       const glm::mat4& projection,
                                       SceneRunState runState) {
    if (runState != SceneRunState::Editing || !inputEnabled_ ||
        !sceneInteractionRect_.valid || !sceneInteractionAreaHovered_ ||
        gizmoDragActive_ || ImGuizmo::IsOver() || ImGuizmo::IsUsing() ||
        ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
        !ImGui::IsMouseClicked(ImGuiMouseButton_Left, false)) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f) {
        return;
    }

    const ImVec2 mouse = ImGui::GetIO().MousePos;
    const float localX = mouse.x - viewport->Pos.x;
    const float localY = mouse.y - viewport->Pos.y;
    const float ndcX = 2.0f * localX / viewport->Size.x - 1.0f;
    const float ndcY = 2.0f * localY / viewport->Size.y - 1.0f;
    const glm::mat4 inverseViewProjection = glm::inverse(projection * view);
    const glm::vec4 nearPoint = inverseViewProjection *
        glm::vec4(ndcX, ndcY, 0.0f, 1.0f);
    const glm::vec4 farPoint = inverseViewProjection *
        glm::vec4(ndcX, ndcY, 1.0f, 1.0f);
    if (!std::isfinite(nearPoint.w) || !std::isfinite(farPoint.w) ||
        std::abs(nearPoint.w) < 1.0e-6f || std::abs(farPoint.w) < 1.0e-6f) {
        return;
    }
    const glm::vec3 origin = glm::vec3(nearPoint) / nearPoint.w;
    const glm::vec3 farWorld = glm::vec3(farPoint) / farPoint.w;
    const glm::vec3 direction = farWorld - origin;
    const float directionLength = glm::length(direction);
    if (!isFiniteVector(origin) || !isFiniteVector(farWorld) ||
        !std::isfinite(directionLength) || directionLength < 1.0e-6f) {
        return;
    }
    const editor_picking::Ray ray{origin, direction / directionLength};

    GameObject* closestObject = nullptr;
    float closestDistance = std::numeric_limits<float>::infinity();
    for (const auto& owner : scene->gameObjects()) {
        GameObject* object = owner.get();
        if (object == nullptr) {
            continue;
        }
        const glm::mat4 model = editor_picking::makeModelMatrix(
            object->position, object->rotation, object->scale);
        std::size_t meshIndex = 0;
        for (const MeshInstance& instance : object->meshInstances()) {
            float distance = 0.0f;
            editor_picking::MeshPickDiagnostics diagnostics;
            const bool hit = editor_picking::intersectMeshWorld(
                ray, instance.mesh, model, distance, &diagnostics);
#ifndef NDEBUG
            const Mesh::Bounds& bounds = instance.mesh.bounds;
            spdlog::debug(
                "Editor pick object='{}' meshes={} mesh={} vertices={} "
                "indices={} boundsValid={} boundsMin=({}, {}, {}) "
                "boundsMax=({}, {}, {}) invertible={} broadPhase={} "
                "triangles={} hitDistance={}",
                object->name, object->meshInstances().size(), meshIndex,
                instance.mesh.vertices.size(), instance.mesh.indices.size(),
                bounds.valid, bounds.minimum.x, bounds.minimum.y,
                bounds.minimum.z, bounds.maximum.x, bounds.maximum.y,
                bounds.maximum.z, diagnostics.transformInvertible,
                diagnostics.broadPhasePassed, diagnostics.triangleTestingReached,
                hit ? distance : -1.0f);
#endif
            if (hit &&
                distance < closestDistance) {
                closestDistance = distance;
                closestObject = object;
            }
            ++meshIndex;
        }
    }
    selectGameObject(scene, closestObject);
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
    Camera* camera = dynamic_cast<Camera*>(selectedGameObject_);
    const char* objectType = pointLight != nullptr
                                 ? "Point Light"
                                 : directionalLight != nullptr
                                       ? "Directional Light"
                                       : camera != nullptr ? "Camera"
                                                           : "GameObject";

    ImGui::Text("Name: %s", objectName);
    ImGui::Text("Type: %s", objectType);

    ImGui::SeparatorText("Transform");
    glm::vec3 position = selectedGameObject_->position;
    if (ImGui::DragFloat3("Position", &position.x, 0.1f)) {
        if (applyObjectPosition(*selectedGameObject_, position)) {
            inspectorError_.clear();
        } else {
            inspectorError_ = "Transform values must be finite.";
        }
    }

    glm::vec3 rotation = selectedGameObject_->rotation;
    if (ImGui::DragFloat3("Rotation (degrees)", &rotation.x, 0.5f)) {
        if (applyObjectRotation(*selectedGameObject_, rotation)) {
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
    sceneInteractionRect_ = {};
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
    if (centralRect.GetWidth() <= 0.0f || centralRect.GetHeight() <= 0.0f ||
        centralRect.Min.x < viewportRect.Min.x ||
        centralRect.Min.y < viewportRect.Min.y ||
        centralRect.Max.x > viewportRect.Max.x ||
        centralRect.Max.y > viewportRect.Max.y) {
        return;
    }
    sceneInteractionRect_.x = centralRect.Min.x;
    sceneInteractionRect_.y = centralRect.Min.y;
    sceneInteractionRect_.width = centralRect.GetWidth();
    sceneInteractionRect_.height = centralRect.GetHeight();
    sceneInteractionRect_.valid = true;
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
    sceneInteractionRect_ = {};
    inputEnabled_ = true;
    gizmoDragActive_ = false;
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
    sceneInteractionRect_ = {};
    inputEnabled_ = true;
    gizmoDragActive_ = false;
    inspectorError_.clear();
}
