#include "imgui_layer.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <misc/cpp/imgui_stdlib.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <ImGuizmo.h>
#include <spdlog/spdlog.h>
#include <SDL3/SDL_dialog.h>

#include <array>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <iterator>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_set>
#include <utility>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

#include "editor_picking.h"
#include "../editor/editor_mutation.h"
#include "../editor/editor_picking.h"
#include "../editor/editor_transform.h"
#include "../editor/editor_session.h"
#include "../physics/physics_transforms.h"
#include "../scene/character.h"
#include "../scene/directional_light.h"
#include "../scene/game_object.h"
#include "../scene/model_renderable.h"
#include "../scene/point_light.h"
#include "../scene/scene.h"

struct NativeFileDialogState {
    enum class Kind {
        Load,
        SaveAs,
    };

    struct Result {
        Kind kind = Kind::Load;
        bool cancelled = false;
        std::string path;
        std::string error;
    };

    std::mutex mutex;
    bool outstanding = false;
    bool shuttingDown = false;
    std::string defaultLocation;
    std::optional<Result> pendingResult;
};

namespace {

constexpr SDL_DialogFileFilter sceneFileFilters[] = {
    {"Dunamis Scene", "scene.json"},
    {"All Files", "*"},
};

constexpr char hierarchyDragPayloadType[] = "DUNAMIS_HIERARCHY_OBJECT";

struct HierarchyDragPayload {
    Scene* scene = nullptr;
    GameObject* object = nullptr;
};

Result validateRuntimeScaleCandidate(GameObject& changedObject,
                                     const glm::vec3& candidateScale) {
    struct RestoreScale {
        GameObject& object;
        glm::vec3 original;
        ~RestoreScale() { object.scale = original; }
    } restore{changedObject, changedObject.scale};

    changedObject.scale = candidateScale;
    std::vector<const GameObject*> pending{&changedObject};
    std::unordered_set<const GameObject*> visited;
    while (!pending.empty()) {
        const GameObject* current = pending.back();
        pending.pop_back();
        if (current == nullptr || !visited.insert(current).second) continue;
        if (const auto* character = dynamic_cast<const Character*>(current)) {
            const Result result =
                physics::validateCharacterPhysicsHierarchy(*character);
            if (!result) {
                return Result::failure(
                    "Runtime scale edit would invalidate Character physics: " +
                    result.error());
            }
        } else if (current->physics.enabled) {
            const Result result =
                physics::validatePhysicsHierarchy(*current);
            if (!result) {
                return Result::failure(
                    "Runtime scale edit would invalidate physics for '" +
                    current->name + "': " + result.error());
            }
        }
        for (const GameObject* child : current->children()) {
            pending.push_back(child);
        }
    }
    return Result::success();
}

Result applyEditorScale(GameObject& object, const glm::vec3& scale,
                        SceneRunState runState) {
    if (runState == SceneRunState::Simulating && scale != object.scale) {
        const Result validation =
            validateRuntimeScaleCandidate(object, scale);
        if (!validation) return validation;
    }
    return editor_mutation::applyScale(object, scale);
}

std::string formatMemoryBytes(std::size_t bytes) {
    constexpr double kibibyte = 1024.0;
    constexpr double mebibyte = kibibyte * 1024.0;
    constexpr double gibibyte = mebibyte * 1024.0;
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(2);
    if (bytes >= static_cast<std::size_t>(gibibyte)) {
        stream << static_cast<double>(bytes) / gibibyte << " GiB";
    } else if (bytes >= static_cast<std::size_t>(mebibyte)) {
        stream << static_cast<double>(bytes) / mebibyte << " MiB";
    } else if (bytes >= static_cast<std::size_t>(kibibyte)) {
        stream << static_cast<double>(bytes) / kibibyte << " KiB";
    } else {
        stream << bytes << " B";
    }
    return stream.str();
}

struct NativeFileDialogCallbackContext {
    std::shared_ptr<NativeFileDialogState> state;
    NativeFileDialogState::Kind kind = NativeFileDialogState::Kind::Load;
};

void SDLCALL nativeFileDialogCallback(
    void* userdata, const char* const* filelist, int /*filter*/) noexcept {
    auto* context = static_cast<NativeFileDialogCallbackContext*>(userdata);
    if (!context) {
        return;
    }

    std::shared_ptr<NativeFileDialogState> state = std::move(context->state);
    const NativeFileDialogState::Kind kind = context->kind;
    delete context;
    if (!state) {
        return;
    }

    NativeFileDialogState::Result result;
    result.kind = kind;
    try {
        if (filelist == nullptr) {
            const char* error = SDL_GetError();
            if (error && error[0] != '\0') {
                result.error = error;
            } else {
                result.error = "SDL file dialog failed";
            }
        } else if (filelist[0] == nullptr) {
            result.cancelled = true;
        } else {
            result.path = filelist[0];
        }
    } catch (...) {
        result.cancelled = false;
        result.path.clear();
        result.error.clear();
        try {
            result.error = "Failed to capture SDL file dialog result";
        } catch (...) {
            // Keep the callback exception-safe even if result capture cannot
            // allocate memory.
        }
    }

    std::lock_guard<std::mutex> lock(state->mutex);
    state->outstanding = false;
    if (state->shuttingDown) {
        state->pendingResult.reset();
        return;
    }
    try {
        state->pendingResult = std::move(result);
    } catch (...) {
        state->pendingResult.reset();
    }
}

constexpr const char* dunamisDockspaceName =
    "DunamisEditorDockspace_v1";
constexpr float minCameraBasisLengthSquared = 1.0e-8f;
constexpr std::size_t characterVisualizationRadialSegments = 12;
constexpr float characterVisualizationTwoPi = 6.28318530717958647692f;
constexpr float characterVisualizationDiagonalRatio = 0.70710678118654752440f;

bool hasOpenImGuiPopup() noexcept {
    return ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
}

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

bool makeCameraBasis(const glm::vec3& cameraFront,
                     const glm::vec3& cameraUp, glm::vec3& forward,
                     glm::vec3& right, glm::vec3& up) noexcept {
    if (!normalizeFinite(cameraFront, forward)) {
        return false;
    }

    glm::vec3 normalizedUp;
    if (!normalizeFinite(cameraUp, normalizedUp)) {
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
                         glm::vec2& screenPoint) noexcept {
    return editor_picking::projectVulkanWorldToImGui(
        worldPoint, editorView, projection,
        glm::vec2(viewport.Pos.x, viewport.Pos.y),
        glm::vec2(viewport.Size.x, viewport.Size.y), screenPoint);
}

float viewDepthForWorldPoint(const glm::vec3& worldPoint,
                             const glm::mat4& view) noexcept {
    if (!isFiniteVector(worldPoint) || !isFiniteMatrix(view)) {
        return std::numeric_limits<float>::infinity();
    }
    const glm::vec4 viewPoint = view * glm::vec4(worldPoint, 1.0f);
    if (!isFiniteVector(viewPoint) || !std::isfinite(viewPoint.z)) {
        return std::numeric_limits<float>::infinity();
    }
    const float depth = -viewPoint.z;
    return std::isfinite(depth) ? depth
                                : std::numeric_limits<float>::infinity();
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

ImGuiLayer::ImGuiLayer()
    : nativeFileDialogState_(std::make_shared<NativeFileDialogState>()) {}

ImGuiLayer::~ImGuiLayer() noexcept {
    shutdown();
}

Result ImGuiLayer::initialize(
    SDL_Window* window, VkInstance instance,
    VkPhysicalDevice physicalDevice, VkDevice device,
    std::uint32_t graphicsQueueFamily, VkQueue graphicsQueue,
    VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
    std::uint32_t minimumImageCount, std::uint32_t imageCount,
    EditorSession& editorSession) {
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

    editorSession_ = &editorSession;
    nativeFileDialogState_ = std::make_shared<NativeFileDialogState>();
    window_ = window;
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
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        cancelEditorTransformDrag();
        if (editorSession_ != nullptr) {
            finishInspectorTransformEdit(
                editorSession_->selectionScene(), true);
        }
    }
    if (!sdlBackendInitialized_) {
        return;
    }
    (void)ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::setInputEnabled(bool enabled) noexcept {
    inputEnabled_ = enabled;
    if (!enabled && editorSession_ != nullptr) {
        finishInspectorTransformEdit(
            editorSession_->selectionScene(), true);
    }
    cancelEditorTransformDrag();
    clearInspectorScaleDrag();
    gizmoDragActive_ = false;
    runtimeTransformDragActive_ = false;
    runtimeTransformObject_ = nullptr;
    if (editorSession_ != nullptr) {
        (void)editorSession_->consumeRuntimeTransformEdit();
    }
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
    consumeNativeFileDialogResult();
    if (suppressSceneInputUntilMouseRelease_ &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        !ImGui::IsMouseDown(ImGuiMouseButton_Right)) {
        suppressSceneInputUntilMouseRelease_ = false;
    }
    popupBlocksSceneInteractionThisFrame_ =
        suppressSceneInputUntilMouseRelease_ || hasOpenImGuiPopup() ||
        openSaveAsOverwritePopup_ ||
        openLoadConfirmationPopup_ || openQuitConfirmationPopup_;
    drawPersistenceDialogs();
    drawToolbar(runState);
    if (hasOpenImGuiPopup()) {
        suppressSceneInputUntilMouseRelease_ = true;
    }
    popupBlocksSceneInteractionThisFrame_ =
        popupBlocksSceneInteractionThisFrame_ || hasOpenImGuiPopup() ||
        suppressSceneInputUntilMouseRelease_;
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
    inspectorScaleWidgetSeen_ = false;
    synchronizeSelection(scene);
    if (!frameStarted_) {
        finishInspectorTransformEdit(scene, true);
        clearInspectorScaleDrag();
        return;
    }

    if (scene != nullptr) {
        const bool disabled = !editorToolsEnabled(runState);
        drawSceneHierarchy(scene, disabled);
        updateSceneInteractionAreaHovered();
        processGizmoShortcuts(scene, runState);
        drawCameraVisualizations(scene, view, projection, runState);
        drawTransformGizmo(scene, view, projection, runState);
        processWorldSelection(scene, view, projection, runState);
        drawInspector(scene, disabled, runState);
    }
    if (!inspectorScaleWidgetSeen_) {
        clearInspectorScaleDrag();
    }
}

void ImGuiLayer::drawToolbar(SceneRunState runState) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }

    const ImGuiStyle& style = ImGui::GetStyle();
    const bool editing = runState == SceneRunState::Editing;
    if (ImGui::BeginMenu("File")) {
        ImGui::BeginDisabled(!editing);
        if (ImGui::MenuItem("Save", "Ctrl+S") &&
            !editorActionPending()) {
            submitEditorAction(EditorCommand::SaveScene);
        }
        ImGui::BeginDisabled(nativeFileDialogBusy() ||
                             editorActionPending());
        if (ImGui::MenuItem("Save As...")) {
            (void)requestNativeFileDialog(true);
        }
        if (ImGui::MenuItem("Load...")) {
            (void)requestNativeFileDialog(false);
        }
        ImGui::EndDisabled();
        ImGui::EndDisabled();
        ImGui::EndMenu();
    }
    const bool editorShortcutAvailable =
        editing && inputEnabled_ && !ImGui::GetIO().WantTextInput &&
        !ImGui::IsAnyItemActive() && !nativeFileDialogBusy() &&
        !popupBlocksSceneInteractionThisFrame_ && !hasOpenImGuiPopup();
    if (editorShortcutAvailable && !gizmoDragActive_ &&
        !editorTransformDragActive_ &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_Z,
                        ImGuiInputFlags_RouteGlobal) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::UndoTransform);
    }
    if (editorShortcutAvailable &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_S,
                        ImGuiInputFlags_RouteGlobal) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::SaveScene);
    }
    if (editorShortcutAvailable &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_D,
                        ImGuiInputFlags_RouteGlobal) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::DuplicateGameObject);
    }
    if (editorShortcutAvailable &&
        ImGui::Shortcut(ImGuiKey_Delete, ImGuiInputFlags_RouteGlobal) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::DeleteSelection);
    }
    if (editorShortcutAvailable &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_P,
                        ImGuiInputFlags_RouteGlobal) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::ParentSelectionToActive);
    }
    if (editorShortcutAvailable &&
        ImGui::Shortcut(ImGuiMod_Ctrl | ImGuiKey_G,
                        ImGuiInputFlags_RouteGlobal) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::GroupSelection);
    }
    const float playButtonWidth =
        ImGui::CalcTextSize("Play").x + 2.0f * style.FramePadding.x;
    const float simulateButtonWidth =
        ImGui::CalcTextSize("Simulate").x + 2.0f * style.FramePadding.x;
    const float stopButtonWidth =
        ImGui::CalcTextSize("Stop").x + 2.0f * style.FramePadding.x;
    const float buttonGroupWidth =
        playButtonWidth + simulateButtonWidth + stopButtonWidth +
        2.0f * style.ItemSpacing.x;
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float centeredX = (viewport->Size.x - buttonGroupWidth) * 0.5f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), centeredX));

    ImGui::BeginDisabled(!editing);
    if (ImGui::Button("Play", ImVec2(playButtonWidth, 0.0f)) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::Play);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!editing);
    if (ImGui::Button("Simulate", ImVec2(simulateButtonWidth, 0.0f)) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::Simulate);
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!runtimeSceneRunning(runState));
    if (ImGui::Button("Stop", ImVec2(stopButtonWidth, 0.0f)) &&
        !editorActionPending()) {
        submitEditorAction(EditorCommand::Stop);
    }
    ImGui::EndDisabled();
    ImGui::EndMainMenuBar();
}

void ImGuiLayer::drawPersistenceDialogs() {
    if (hasOpenImGuiPopup()) {
        suppressSceneInputUntilMouseRelease_ = true;
    }
    popupBlocksSceneInteractionThisFrame_ =
        popupBlocksSceneInteractionThisFrame_ || hasOpenImGuiPopup() ||
        suppressSceneInputUntilMouseRelease_;
    if (openSaveAsOverwritePopup_) {
        ImGui::OpenPopup("Overwrite Scene##SaveAs");
        openSaveAsOverwritePopup_ = false;
        popupBlocksSceneInteractionThisFrame_ = true;
        suppressSceneInputUntilMouseRelease_ = true;
    }
    if (ImGui::BeginPopupModal("Overwrite Scene##SaveAs", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("File already exists. Overwrite it?");
        ImGui::Text("%s", saveAsOverwritePath_.c_str());
        if (ImGui::Button("Overwrite") &&
            !editorActionPending()) {
            submitEditorAction(EditorCommand::ConfirmSaveSceneAsOverwrite);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel") &&
            !editorActionPending()) {
            submitEditorAction(EditorCommand::CancelSaveSceneAs);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openLoadConfirmationPopup_) {
        ImGui::OpenPopup("Unsaved Changes##Load");
        openLoadConfirmationPopup_ = false;
        popupBlocksSceneInteractionThisFrame_ = true;
        suppressSceneInputUntilMouseRelease_ = true;
    }
    if (ImGui::BeginPopupModal("Unsaved Changes##Load", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Unsaved changes detected.");
        if (ImGui::Button("Save and Load")) {
            submitEditorAction(EditorCommand::SaveAndLoad);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save and Load")) {
            submitEditorAction(EditorCommand::DiscardAndLoad);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            submitEditorAction(EditorCommand::Cancel);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    if (openQuitConfirmationPopup_) {
        ImGui::OpenPopup("Unsaved Changes##Quit");
        openQuitConfirmationPopup_ = false;
        popupBlocksSceneInteractionThisFrame_ = true;
        suppressSceneInputUntilMouseRelease_ = true;
    }
    if (ImGui::BeginPopupModal("Unsaved Changes##Quit", nullptr,
                               ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::TextUnformatted("Unsaved changes detected. Save before quitting?");
        if (ImGui::Button("Save and Quit")) {
            submitEditorAction(EditorCommand::SaveAndQuit);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Don't Save and Quit")) {
            submitEditorAction(EditorCommand::DiscardAndQuit);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) {
            submitEditorAction(EditorCommand::Cancel);
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

}

void ImGuiLayer::setCurrentScenePath(const std::string& path) {
    currentScenePath_ = path;
}

void ImGuiLayer::setEditorError(std::string error) {
    inspectorError_ = std::move(error);
}

bool ImGuiLayer::requestNativeFileDialog(bool saveAs) {
    if (!window_ || !nativeFileDialogState_) {
        return false;
    }

    const auto kind = saveAs ? NativeFileDialogState::Kind::SaveAs
                             : NativeFileDialogState::Kind::Load;
    const std::shared_ptr<NativeFileDialogState> state =
        nativeFileDialogState_;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->shuttingDown || state->outstanding ||
            state->pendingResult.has_value()) {
            return false;
        }
    }

    auto* callbackContext =
        new NativeFileDialogCallbackContext{state, kind};
    const char* defaultLocation = nullptr;
    {
        std::lock_guard<std::mutex> lock(state->mutex);
        if (state->shuttingDown || state->outstanding ||
            state->pendingResult.has_value()) {
            delete callbackContext;
            return false;
        }
        state->defaultLocation.clear();
        if (saveAs) {
            state->defaultLocation = currentScenePath_;
        } else {
            const std::filesystem::path gameDirectory =
                std::filesystem::path(DUNAMIS_SOURCE_DIR) / "game";
            std::error_code directoryQueryError;
            const bool directoryExists = std::filesystem::exists(
                gameDirectory, directoryQueryError);
            if (directoryQueryError) {
                spdlog::warn(
                    "Failed to query Load dialog default directory '{}': "
                    "{}; using SDL's default location",
                    gameDirectory.string(), directoryQueryError.message());
            } else if (!directoryExists) {
                spdlog::warn(
                    "Load dialog default directory '{}' does not exist; "
                    "using SDL's default location",
                    gameDirectory.string());
            } else if (!std::filesystem::is_directory(
                           gameDirectory, directoryQueryError)) {
                if (directoryQueryError) {
                    spdlog::warn(
                        "Failed to query Load dialog default directory '{}': "
                        "{}; using SDL's default location",
                        gameDirectory.string(),
                        directoryQueryError.message());
                } else {
                    spdlog::warn(
                        "Load dialog default path '{}' is not a directory; "
                        "using SDL's default location",
                        gameDirectory.string());
                }
            } else {
                state->defaultLocation = gameDirectory.string();
            }
        }
        state->outstanding = true;
        if (!state->defaultLocation.empty()) {
            defaultLocation = state->defaultLocation.c_str();
        }
    }

    if (saveAs) {
        SDL_ShowSaveFileDialog(
            nativeFileDialogCallback, callbackContext, window_,
            sceneFileFilters,
            static_cast<int>(std::size(sceneFileFilters)), defaultLocation);
    } else {
        SDL_ShowOpenFileDialog(
            nativeFileDialogCallback, callbackContext, window_,
            sceneFileFilters,
            static_cast<int>(std::size(sceneFileFilters)), defaultLocation,
            false);
    }
    return true;
}

void ImGuiLayer::consumeNativeFileDialogResult() {
    if (!nativeFileDialogState_) {
        return;
    }

    std::optional<NativeFileDialogState::Result> result;
    {
        std::lock_guard<std::mutex> lock(nativeFileDialogState_->mutex);
        if (!nativeFileDialogState_->pendingResult.has_value()) {
            return;
        }
        result = std::move(nativeFileDialogState_->pendingResult);
        nativeFileDialogState_->pendingResult.reset();
    }

    if (!result || result->cancelled) {
        return;
    }
    if (!result->error.empty()) {
        spdlog::error("Native file dialog failed: {}", result->error);
        return;
    }
    if (result->path.empty()) {
        spdlog::error("Native file dialog returned an empty path");
        return;
    }

    if (result->kind == NativeFileDialogState::Kind::SaveAs) {
        submitEditorAction(EditorAction{
            EditorCommand::SaveSceneAs,
            std::filesystem::path(std::move(result->path))});
    } else {
        submitEditorAction(EditorAction{
            EditorCommand::LoadScene,
            std::filesystem::path(std::move(result->path))});
    }
}

bool ImGuiLayer::nativeFileDialogBusy() const {
    if (!nativeFileDialogState_) {
        return false;
    }
    std::lock_guard<std::mutex> lock(nativeFileDialogState_->mutex);
    return nativeFileDialogState_->outstanding ||
           nativeFileDialogState_->pendingResult.has_value();
}

void ImGuiLayer::stopNativeFileDialog() noexcept {
    if (!nativeFileDialogState_) {
        return;
    }
    std::lock_guard<std::mutex> lock(nativeFileDialogState_->mutex);
    nativeFileDialogState_->shuttingDown = true;
    nativeFileDialogState_->outstanding = false;
    nativeFileDialogState_->pendingResult.reset();
}

void ImGuiLayer::requestLoadConfirmation() { openLoadConfirmationPopup_ = true; }
void ImGuiLayer::requestSaveAsOverwriteConfirmation(const std::string& path) {
    saveAsOverwritePath_ = path;
    openSaveAsOverwritePopup_ = true;
}
void ImGuiLayer::requestQuitConfirmation() { openQuitConfirmationPopup_ = true; }

void ImGuiLayer::setPhysicsDiagnostics(
    const GameObject* object, std::optional<physics::ShapeDiagnostics> diagnostics,
    std::string error) {
    physicsDiagnosticsObject_ = object;
    physicsDiagnostics_ = std::move(diagnostics);
    physicsDiagnosticsError_ = std::move(error);
}

bool ImGuiLayer::editorActionPending() const noexcept {
    return editorSession_ != nullptr &&
           editorSession_->pendingEditorAction().command != EditorCommand::None;
}

void ImGuiLayer::submitEditorAction(EditorCommand command) {
    submitEditorAction(EditorAction{command, {}});
}

void ImGuiLayer::submitEditorAction(EditorAction action) {
    if (action.command == EditorCommand::DuplicateGameObject ||
        action.command == EditorCommand::DeleteSelection ||
        action.command == EditorCommand::ParentSelectionToActive ||
        action.command == EditorCommand::GroupSelection) {
        cancelStructuralAction();
    }
    if (editorSession_ != nullptr) {
        editorSession_->submitEditorAction(std::move(action));
    }
}

void ImGuiLayer::submitRuntimeTransformEdit(
    const RuntimeTransformEdit& edit) noexcept {
    if (editorSession_ != nullptr) {
        editorSession_->submitRuntimeTransformEdit(edit);
    }
}

const GameObject* ImGuiLayer::selectedGameObjectForScene(
    const Scene* scene) const noexcept {
    return editorSession_ == nullptr
               ? nullptr
               : editorSession_->selectedGameObjectForScene(scene);
}

bool ImGuiLayer::isPhysicsDebugScalePreviewActive(
    const Scene* scene, const GameObject* object) const noexcept {
    if (scene == nullptr || object == nullptr) {
        return false;
    }
    if (editorTransformDragActive_ && !editorTransformDragFailed_ &&
        editor_transform::isScalePreviewParticipant(
            editorTransformSnapshot_, scene, object)) {
        return true;
    }
    if (!inspectorScaleDragActive_ || inspectorScaleDragScene_ != scene ||
        inspectorScaleDragObject_ == nullptr) {
        return false;
    }
    for (const GameObject* current = object; current != nullptr;
         current = current->parent()) {
        if (current == inspectorScaleDragObject_) return true;
    }
    return false;
}

void ImGuiLayer::finishRuntimeTransformDrag() noexcept {
    if (runtimeTransformDragActive_ && runtimeTransformObject_ != nullptr) {
        const Scene* selectionScene =
            editorSession_ == nullptr ? nullptr : editorSession_->selectionScene();
        if (editorSession_ != nullptr &&
            editorSession_->isSelectedForScene(selectionScene,
                                               runtimeTransformObject_)) {
            submitRuntimeTransformEdit(RuntimeTransformEdit{
                runtimeTransformObject_, runtimeTransformObject_->position,
                runtimeTransformObject_->rotation, false});
        }
    }
    runtimeTransformDragActive_ = false;
    runtimeTransformObject_ = nullptr;
}

void ImGuiLayer::clearInspectorScaleDrag() noexcept {
    inspectorScaleDragActive_ = false;
    inspectorScaleDragScene_ = nullptr;
    inspectorScaleDragObject_ = nullptr;
}

void ImGuiLayer::beginInspectorTransformEdit(
    Scene* scene, GameObject* object, InspectorTransformWidget widget) {
    if (scene == nullptr || object == nullptr || editorSession_ == nullptr) {
        return;
    }
    const std::uint64_t sceneId = scene->lifetimeId();
    if (inspectorTransformEditActive_ &&
        inspectorTransformEditSceneId_ == sceneId &&
        inspectorTransformEditObjectId_ == object->persistentId &&
        inspectorTransformEditObjectInstanceId_ == object->instanceId() &&
        inspectorTransformEditWidget_ == widget) {
        return;
    }
    if (inspectorTransformEditActive_) {
        finishInspectorTransformEdit(
            inspectorTransformEditSceneId_ == sceneId ? scene : nullptr,
            inspectorTransformEditSceneId_ == sceneId);
    }

    inspectorTransformEditSceneId_ = sceneId;
    inspectorTransformEditObjectId_ = object->persistentId;
    inspectorTransformEditObjectInstanceId_ = object->instanceId();
    inspectorTransformEditWidget_ = widget;
    inspectorTransformEditActive_ = true;
    try {
        editor_undo::TransformUndoSnapshot snapshot;
        const Result result = editorSession_->transformUndoHistory().capture(
            *scene, std::vector<GameObject*>{object}, snapshot);
        if (!result) {
            inspectorTransformUndoCaptureFailed_ = true;
            inspectorError_ = "Inspector transform changed, but undo could not "
                              "be started: " + result.error();
            return;
        }
        inspectorTransformUndoSnapshot_ = std::move(snapshot);
    } catch (const std::exception& exception) {
        inspectorTransformUndoCaptureFailed_ = true;
        inspectorError_ = "Inspector transform changed, but undo could not "
                          "be started: " + std::string(exception.what());
    } catch (...) {
        inspectorTransformUndoCaptureFailed_ = true;
        inspectorError_ =
            "Inspector transform changed, but undo could not be started.";
    }
}

void ImGuiLayer::finishInspectorTransformEdit(Scene* scene,
                                               bool commit) noexcept {
    if (!inspectorTransformEditActive_) {
        return;
    }
    if (commit && scene != nullptr &&
        scene->lifetimeId() == inspectorTransformEditSceneId_ &&
        editorSession_ != nullptr &&
        !inspectorTransformUndoCaptureFailed_) {
        try {
            const Result result = editorSession_->transformUndoHistory().commit(
                *scene, inspectorTransformUndoSnapshot_);
            if (!result) {
                try {
                    inspectorError_ =
                        "Inspector transform completed, but undo could not be "
                        "recorded: " + result.error();
                } catch (...) {
                }
            }
        } catch (...) {
            try {
                inspectorError_ =
                    "Inspector transform completed, but undo could not be recorded.";
            } catch (...) {
            }
        }
    }
    clearInspectorTransformEdit();
}

void ImGuiLayer::clearInspectorTransformEdit() noexcept {
    inspectorTransformEditActive_ = false;
    inspectorTransformUndoCaptureFailed_ = false;
    inspectorTransformEditWidget_ = InspectorTransformWidget::Position;
    inspectorTransformEditSceneId_ = 0;
    inspectorTransformEditObjectId_.clear();
    inspectorTransformEditObjectInstanceId_ = 0;
    inspectorTransformUndoSnapshot_ = {};
}

void ImGuiLayer::clearEditorTransformDrag() noexcept {
    editorTransformDragActive_ = false;
    editorTransformDragFailed_ = false;
    editorTransformUndoCaptureFailed_ = false;
    editorTransformSnapshot_ = {};
    editorTransformUndoSnapshot_ = {};
    editorTransformGizmoMatrix_ = glm::mat4(1.0f);
}

void ImGuiLayer::cancelEditorTransformDrag() noexcept {
    if (editorTransformDragActive_) {
        (void)editor_transform::restoreTransformDragSnapshot(
            editorTransformSnapshot_);
    }
    clearEditorTransformDrag();
}

void ImGuiLayer::cancelStructuralAction() noexcept {
    if (editorSession_ != nullptr) {
        finishInspectorTransformEdit(
            editorSession_->selectionScene(), true);
    }
    cancelEditorTransformDrag();
    finishRuntimeTransformDrag();
    clearInspectorScaleDrag();
    gizmoDragActive_ = false;
    runtimeTransformDragActive_ = false;
    runtimeTransformObject_ = nullptr;
    if (editorSession_ != nullptr) {
        (void)editorSession_->consumeRuntimeTransformEdit();
    }
}

void ImGuiLayer::clearSelection() noexcept {
    if (editorSession_ != nullptr) {
        finishInspectorTransformEdit(
            editorSession_->selectionScene(), true);
    }
    cancelEditorTransformDrag();
    finishRuntimeTransformDrag();
    clearInspectorScaleDrag();
    if (editorSession_ != nullptr) {
        editorSession_->clearSelection();
    }
    inspectorError_.clear();
}

bool ImGuiLayer::sceneInteractionAreaHovered() const noexcept {
    return sceneInteractionAreaHovered_;
}

void ImGuiLayer::synchronizeSelection(Scene* scene) {
    if (editorSession_ == nullptr) {
        return;
    }

    if (scene != editorSession_->selectionScene()) {
        // The old scene may already have been retired. Do not submit a final
        // runtime edit through a pointer whose owner is no longer known.
        runtimeTransformDragActive_ = false;
        runtimeTransformObject_ = nullptr;
        clearInspectorScaleDrag();
        clearInspectorTransformEdit();
        if (editorTransformDragActive_) {
            clearEditorTransformDrag();
        }
    } else if (editorTransformDragActive_) {
        const std::vector<GameObject*>& selected =
            editorSession_->selectedGameObjects();
        const bool selectionChanged =
            editorSession_->activeGameObject() !=
                editorTransformSnapshot_.active ||
            selected.size() != editorTransformSnapshot_.selectedObjects.size() ||
            !std::equal(selected.begin(), selected.end(),
                        editorTransformSnapshot_.selectedObjects.begin());
        if (selectionChanged) {
            cancelEditorTransformDrag();
        }
    }
    editorSession_->synchronizeSelection(scene);
    if (inspectorTransformEditActive_) {
        bool stillSelected = false;
        if (scene != nullptr &&
            scene->lifetimeId() == inspectorTransformEditSceneId_) {
            GameObject* target =
                scene->findGameObject(inspectorTransformEditObjectId_);
            stillSelected = target != nullptr &&
                target->instanceId() == inspectorTransformEditObjectInstanceId_ &&
                editorSession_->isSelectedForScene(scene, target);
        }
        if (!stillSelected) {
            finishInspectorTransformEdit(
                scene != nullptr &&
                        scene->lifetimeId() == inspectorTransformEditSceneId_
                    ? scene
                    : nullptr,
                scene != nullptr &&
                    scene->lifetimeId() == inspectorTransformEditSceneId_);
        }
    }
    if (inspectorScaleDragActive_ &&
        (inspectorScaleDragScene_ != scene ||
         !editorSession_->isSelectedForScene(scene,
                                             inspectorScaleDragObject_))) {
        clearInspectorScaleDrag();
    }
    if (runtimeTransformObject_ != nullptr &&
        !editorSession_->isSelectedForScene(scene, runtimeTransformObject_)) {
        runtimeTransformDragActive_ = false;
        runtimeTransformObject_ = nullptr;
    }
}

void ImGuiLayer::selectGameObject(Scene* scene, GameObject* object,
                                  SelectionOperation operation) {
    if (editorSession_ == nullptr) {
        return;
    }

    if (object == nullptr && operation != SelectionOperation::ReplaceExact) {
        return;
    }

    finishInspectorTransformEdit(scene, true);
    cancelEditorTransformDrag();
    finishRuntimeTransformDrag();
    clearInspectorScaleDrag();
    editorSession_->applySelection(scene, object, operation);
    inspectorError_.clear();
}

void ImGuiLayer::drawSceneHierarchy(Scene* scene, bool disabled) {
    pendingHierarchyReorder_.reset();
    ImGui::SetNextWindowSize(ImVec2(300.0f, 400.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Scene Hierarchy")) {
        ImGui::End();
        return;
    }

    ImGui::BeginDisabled(disabled);

    bool hasObject = false;
    for (GameObject* object : scene->rootObjects()) {
        if (object == nullptr) {
            continue;
        }

        hasObject = true;
        drawSceneHierarchyNode(scene, object);
    }

    if (!hasObject) {
        ImGui::TextUnformatted("No GameObjects in the active scene.");
    }

    const ImVec2 available = ImGui::GetContentRegionAvail();
    if (available.x > 0.0f && available.y > 0.0f &&
        ImGui::InvisibleButton("##SceneHierarchyBlankSpace", available)) {
        const ImGuiIO& io = ImGui::GetIO();
        if (emptyWorldSelectionOperationForModifiers(io.KeyCtrl, io.KeyShift) ==
            EmptyWorldSelectionOperation::Clear) {
            selectGameObject(scene, nullptr,
                             SelectionOperation::ReplaceExact);
        }
    }

    ImGui::EndDisabled();

    if (pendingHierarchyReorder_.has_value()) {
        const HierarchyReorderRequest request = *pendingHierarchyReorder_;
        pendingHierarchyReorder_.reset();
        if (request.scene == scene && request.object != nullptr) {
            finishInspectorTransformEdit(scene, true);
            cancelEditorTransformDrag();
            const Result result = request.scene->reorderGameObject(
                *request.object, request.expectedParent,
                request.siblingIndex);
            if (result) {
                if (editorSession_ != nullptr) {
                    editorSession_->transformUndoHistory().clear();
                }
                inspectorError_.clear();
            } else {
                inspectorError_ = result.error();
            }
        }
    }
    ImGui::End();
}

void ImGuiLayer::drawSceneHierarchyNode(Scene* scene, GameObject* object) {
    if (scene == nullptr || object == nullptr) {
        return;
    }

    ImGui::PushID(static_cast<const void*>(object));
    const char* label = object->name.empty() ? "<Unnamed GameObject>"
                                             : object->name.c_str();
    const bool hasChildren = !object->children().empty();
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_SpanAvailWidth;
    if (hasChildren) {
        flags |= ImGuiTreeNodeFlags_OpenOnArrow |
                 ImGuiTreeNodeFlags_DefaultOpen;
    } else {
        flags |= ImGuiTreeNodeFlags_Leaf |
                 ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }
    if (editorSession_ != nullptr &&
        editorSession_->isSelectedForScene(scene, object)) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const bool open = ImGui::TreeNodeEx(label, flags);
    const ImVec2 rowMin = ImGui::GetItemRectMin();
    const ImVec2 rowMax = ImGui::GetItemRectMax();
    const bool clicked = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool toggledOpen = ImGui::IsItemToggledOpen();

    bool dragDropInteraction = false;
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceNoHoldToOpenOthers)) {
        const HierarchyDragPayload dragPayload{scene, object};
        ImGui::SetDragDropPayload(hierarchyDragPayloadType, &dragPayload,
                                  sizeof(dragPayload));
        ImGui::TextUnformatted(label);
        ImGui::EndDragDropSource();
        dragDropInteraction = true;
    }

    bool reorderDelivered = false;
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* payload =
                ImGui::AcceptDragDropPayload(hierarchyDragPayloadType)) {
            if (payload->DataSize == sizeof(HierarchyDragPayload)) {
                const auto* dragPayload =
                    static_cast<const HierarchyDragPayload*>(payload->Data);
                GameObject* draggedObject = dragPayload->object;
                if (dragPayload->scene == scene && draggedObject != nullptr &&
                    draggedObject != object &&
                    draggedObject->parent() == object->parent()) {
                    const auto& siblings = object->parent() == nullptr
                                               ? scene->rootObjects()
                                               : object->parent()->children();
                    const auto draggedIt =
                        std::find(siblings.begin(), siblings.end(),
                                  draggedObject);
                    const auto targetIt =
                        std::find(siblings.begin(), siblings.end(), object);
                    if (draggedIt != siblings.end() &&
                        targetIt != siblings.end()) {
                        const std::size_t draggedIndex = static_cast<std::size_t>(
                            std::distance(siblings.begin(), draggedIt));
                        const std::size_t targetIndex = static_cast<std::size_t>(
                            std::distance(siblings.begin(), targetIt));
                        const bool dropAfter =
                            ImGui::GetIO().MousePos.y >
                            (rowMin.y + rowMax.y) * 0.5f;
                        std::size_t destinationIndex =
                            targetIndex + (dropAfter ? 1U : 0U);
                        if (draggedIndex < destinationIndex) {
                            --destinationIndex;
                        }

                        if (payload->IsDelivery()) {
                            pendingHierarchyReorder_ = HierarchyReorderRequest{
                                scene, draggedObject, object->parent(),
                                destinationIndex};
                            reorderDelivered = true;
                        }
                    }
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (clicked && !toggledOpen && !dragDropInteraction &&
        !reorderDelivered && !ImGui::IsDragDropActive() &&
        !ImGui::IsMouseDragging(ImGuiMouseButton_Left)) {
        const ImGuiIO& io = ImGui::GetIO();
        selectGameObject(
            scene, object,
            selectionOperationForModifiers(io.KeyCtrl, io.KeyShift));
    }

    if (open && hasChildren) {
        for (GameObject* child : object->children()) {
            drawSceneHierarchyNode(scene, child);
        }
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void ImGuiLayer::processGizmoShortcuts(Scene* scene,
                                       SceneRunState runState) noexcept {
    if (!editorToolsEnabled(runState) || !inputEnabled_ ||
        selectedGameObjectForScene(scene) == nullptr ||
        ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
        popupBlocksSceneInteractionThisFrame_ || hasOpenImGuiPopup() ||
        ImGuizmo::IsUsing()) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_W, false)) {
        editorSession_->setTransformTool(TransformTool::Translate);
    } else if (ImGui::IsKeyPressed(ImGuiKey_E, false)) {
        editorSession_->setTransformTool(TransformTool::Scale);
    } else if (ImGui::IsKeyPressed(ImGuiKey_R, false)) {
        editorSession_->setTransformTool(TransformTool::Rotate);
    }
}

void ImGuiLayer::drawTransformGizmo(Scene* scene, const glm::mat4& view,
                                    const glm::mat4& projection,
                                    SceneRunState runState) {
    if (popupBlocksSceneInteractionThisFrame_ || hasOpenImGuiPopup()) {
        ImGuizmo::Enable(false);
        cancelEditorTransformDrag();
        finishRuntimeTransformDrag();
        gizmoDragActive_ = false;
        return;
    }
    ImGuizmo::Enable(true);
    if (!editorToolsEnabled(runState) ||
        !sceneInteractionRect_.valid) {
        cancelEditorTransformDrag();
        gizmoDragActive_ = false;
        finishRuntimeTransformDrag();
        return;
    }
    GameObject* selected = const_cast<GameObject*>(
        selectedGameObjectForScene(scene));
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (selected == nullptr || viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f) {
        cancelEditorTransformDrag();
        gizmoDragActive_ = false;
        finishRuntimeTransformDrag();
        return;
    }

    ImDrawList* drawList = ImGui::GetBackgroundDrawList(viewport);
    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(sceneInteractionRect_.x + sceneInteractionRect_.width,
                         sceneInteractionRect_.y + sceneInteractionRect_.height);
    drawList->PushClipRect(clipMin, clipMax, true);
    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist(drawList);
    ImGuizmo::SetRect(viewport->Pos.x, viewport->Pos.y, viewport->Size.x,
                      viewport->Size.y);
    const TransformTool transformTool = editorSession_->transformTool();
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    const ImGuizmo::MODE mode =
        editorSession_->transformSpace() == TransformSpace::Local
            ? ImGuizmo::LOCAL
            : ImGuizmo::WORLD;
    switch (transformTool) {
    case TransformTool::Translate:
        operation = ImGuizmo::TRANSLATE;
        break;
    case TransformTool::Scale:
        if (runState == SceneRunState::Simulating &&
            selected->physics.enabled &&
            selected->physics.motionType ==
                GameObject::PhysicsMotionType::Dynamic) {
            cancelEditorTransformDrag();
            gizmoDragActive_ = false;
            inspectorError_ =
                "Runtime Scale is unavailable for dynamic physics bodies.";
            drawList->PopClipRect();
            return;
        }
        operation = ImGuizmo::SCALE;
        break;
    case TransformTool::Rotate:
        operation = ImGuizmo::ROTATE;
        break;
    }

    if (runState == SceneRunState::Editing) {
        if (editorTransformDragActive_ &&
            (editorTransformSnapshot_.scene != scene ||
             editorTransformSnapshot_.active != selected ||
             editorTransformSnapshot_.space != editorSession_->transformSpace() ||
             editorTransformSnapshot_.tool != transformTool)) {
            cancelEditorTransformDrag();
        }

        glm::mat4 baseGizmoMatrix;
        if (editorTransformDragActive_) {
            baseGizmoMatrix = editorTransformGizmoMatrix_;
        } else {
            const Result frameResult =
                editor_transform::calculateSelectionGizmoFrame(
                    *editorSession_, editorSession_->transformSpace(),
                    baseGizmoMatrix);
            if (!frameResult) {
                gizmoDragActive_ = false;
                inspectorError_ = frameResult.error();
                drawList->PopClipRect();
                return;
            }

            if (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                ImGuizmo::IsUsing()) {
                const Result snapshotResult =
                    editor_transform::captureTransformDragSnapshot(
                        *scene, *editorSession_, editorSession_->transformSpace(),
                        editorTransformSnapshot_);
                if (!snapshotResult) {
                    gizmoDragActive_ = false;
                    inspectorError_ = snapshotResult.error();
                    drawList->PopClipRect();
                    return;
                }
                editorTransformDragActive_ = true;
                editorTransformDragFailed_ = false;
                editorTransformUndoCaptureFailed_ = false;
                editorTransformGizmoMatrix_ =
                    editorTransformSnapshot_.originalGizmoWorld;
                try {
                    std::vector<GameObject*> affected;
                    affected.reserve(editorTransformSnapshot_.objects.size());
                    for (const auto& objectSnapshot :
                         editorTransformSnapshot_.objects) {
                        affected.push_back(objectSnapshot.object);
                    }
                    const Result undoSnapshotResult =
                        editorSession_->transformUndoHistory().capture(
                            *scene, affected,
                            editorTransformUndoSnapshot_);
                    if (!undoSnapshotResult) {
                        editorTransformUndoCaptureFailed_ = true;
                        inspectorError_ =
                            "Transform is active, but undo could not be "
                            "captured: " + undoSnapshotResult.error();
                    }
                } catch (const std::exception& exception) {
                    editorTransformUndoCaptureFailed_ = true;
                    inspectorError_ =
                        "Transform is active, but undo could not be "
                        "captured: " + std::string(exception.what());
                } catch (...) {
                    editorTransformUndoCaptureFailed_ = true;
                    inspectorError_ =
                        "Transform is active, but undo could not be captured.";
                }
                baseGizmoMatrix = editorTransformGizmoMatrix_;
            }
        }

        glm::mat4 gizmoMatrix = baseGizmoMatrix;
        glm::mat4 imguizmoProjection = projection;
        imguizmoProjection[1][1] *= -1.0f;
        const bool manipulated = ImGuizmo::Manipulate(
            glm::value_ptr(view), glm::value_ptr(imguizmoProjection),
            operation, mode, glm::value_ptr(gizmoMatrix));
        const bool usingNow = ImGuizmo::IsUsing();
        drawList->PopClipRect();

        if (editorTransformDragActive_ && !editorTransformDragFailed_) {
            // ImGuizmo's matrix is mutable interaction state. Keep its latest
            // result for the next frame; the solver still uses the immutable
            // editorTransformSnapshot_ as its drag origin.
            editorTransformGizmoMatrix_ = gizmoMatrix;
        }

        if (manipulated && editorTransformDragActive_ &&
            !editorTransformDragFailed_) {
            glm::mat4 currentGizmoWorld = editorTransformGizmoMatrix_;
            // ImGuizmo's scale and rotation output can carry the frame origin
            // through its internal matrix convention. The shared transaction
            // frame is explicitly pivot-centered before deriving D.
            if (transformTool == TransformTool::Rotate ||
                transformTool == TransformTool::Scale) {
                currentGizmoWorld[3] = glm::vec4(
                    editorTransformSnapshot_.pivot, 1.0f);
                editorTransformGizmoMatrix_ = currentGizmoWorld;
            }

            std::vector<editor_transform::TransformCandidate> candidates;
            Result result = editor_transform::solveTransformDrag(
                editorTransformSnapshot_, currentGizmoWorld, candidates);
            if (result) {
                result = editor_transform::commitTransformCandidates(
                    editorTransformSnapshot_, candidates);
            }
            if (result) {
                if (transformTool == TransformTool::Scale) {
                    // The pre-ImGui diagnostics pass described the scale
                    // before this transaction step. Do not display those
                    // cooked statistics for the newly authored scale.
                    setPhysicsDiagnostics(nullptr, std::nullopt, {});
                }
                if (!editorTransformUndoCaptureFailed_) {
                    inspectorError_.clear();
                }
            } else {
                const bool restored =
                    editor_transform::restoreTransformDragSnapshot(
                        editorTransformSnapshot_);
                editorTransformDragFailed_ = true;
                inspectorError_ = result.error();
                if (!restored) {
                    inspectorError_ +=
                        " The original transform snapshot could not be restored.";
                }
                editorTransformGizmoMatrix_ =
                    editorTransformSnapshot_.originalGizmoWorld;
            }
        }

        const bool mouseDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        gizmoDragActive_ = (usingNow || editorTransformDragFailed_) &&
                           mouseDown;
        if (!usingNow && (!editorTransformDragFailed_ || !mouseDown)) {
            if (!editorTransformDragFailed_ &&
                !editorTransformUndoCaptureFailed_ &&
                editorTransformUndoSnapshot_.valid) {
                const Result undoResult =
                    editorSession_->transformUndoHistory().commit(
                        *scene, editorTransformUndoSnapshot_);
                if (!undoResult) {
                    inspectorError_ =
                        "Transform completed, but undo could not be "
                        "recorded: " + undoResult.error();
                }
            }
            clearEditorTransformDrag();
        }
        return;
    }

    cancelEditorTransformDrag();
    const glm::mat4 worldTransform = selected->worldTransformMatrix();
    if (!isFiniteMatrix(worldTransform)) {
        gizmoDragActive_ = false;
        runtimeTransformDragActive_ = false;
        runtimeTransformObject_ = nullptr;
        inspectorError_ = "Selected GameObject world transform is not finite.";
        drawList->PopClipRect();
        return;
    }

    glm::mat4 gizmoMatrix = worldTransform;
    glm::mat4 imguizmoProjection = projection;
    imguizmoProjection[1][1] *= -1.0f;
    const bool manipulated = ImGuizmo::Manipulate(
        glm::value_ptr(view), glm::value_ptr(imguizmoProjection),
        operation, mode, glm::value_ptr(gizmoMatrix));
    drawList->PopClipRect();

    if (manipulated) {
        transform_math::DecomposedTransform localTransform;
        Result result = editor_transform::deriveLocalTransformFromWorld(
            *selected, gizmoMatrix, localTransform);
        if (result) {
            switch (transformTool) {
            case TransformTool::Translate:
                result = editor_mutation::applyPosition(
                    *selected, localTransform.position);
                break;
            case TransformTool::Rotate:
                result = editor_mutation::applyRotation(
                    *selected, localTransform.rotation);
                break;
            case TransformTool::Scale:
                result = applyEditorScale(*selected, localTransform.scale,
                                          runState);
                break;
            }
        }
        if (result) {
            inspectorError_.clear();
        } else {
            inspectorError_ = result.error();
        }
    }
    gizmoDragActive_ = ImGuizmo::IsUsing() &&
                       ImGui::IsMouseDown(ImGuiMouseButton_Left);
    const bool runtimePhysicsBacked =
        runState == SceneRunState::Simulating && selected->physics.enabled;
    const bool runtimeTransformMode =
        transformTool == TransformTool::Translate ||
        transformTool == TransformTool::Rotate;
    const bool activeNow = runtimePhysicsBacked && runtimeTransformMode && gizmoDragActive_;
    if (activeNow) {
        runtimeTransformDragActive_ = true;
        runtimeTransformObject_ = selected;
    }
    if (runtimeTransformDragActive_ && runtimeTransformObject_ != nullptr) {
        submitRuntimeTransformEdit(RuntimeTransformEdit{
            runtimeTransformObject_, runtimeTransformObject_->position,
            runtimeTransformObject_->rotation, activeNow});
        if (!activeNow) {
            runtimeTransformDragActive_ = false;
            runtimeTransformObject_ = nullptr;
        }
    }
}

void ImGuiLayer::drawCameraVisualizations(
    Scene* scene, const glm::mat4& editorView, const glm::mat4& projection,
    SceneRunState runState) {
    editorHelperGeometry_.clear();
    if (!editorToolsEnabled(runState) || scene == nullptr ||
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
    const std::vector<editor_picking::CameraVisualizationEntry> cameras =
        editor_picking::collectCameraVisualizationEntries(objects,
                                                          activeCamera);
    for (const auto& entry : cameras) {
        if (entry.camera != nullptr) {
            drawCameraVisualization(*entry.camera, editorView, projection,
                                    entry.selectionTarget, entry.active);
        }
    }

    for (const GameObject* object : objects) {
        if (const auto* character = dynamic_cast<const Character*>(object)) {
            drawCharacterVisualization(*character, object, editorView,
                                        projection);
        }
        if (const auto* pointLight = dynamic_cast<const PointLight*>(object)) {
            drawPointLightVisualization(*pointLight, object, editorView,
                                        projection);
        }
        if (const auto* directionalLight =
                dynamic_cast<const DirectionalLight*>(object)) {
            drawDirectionalLightVisualization(*directionalLight, object,
                                              editorView, projection);
        }
    }
}

void ImGuiLayer::drawCameraVisualization(
    const Camera& camera, const glm::mat4& editorView,
    const glm::mat4& projection, const GameObject* selectionTarget,
    bool active) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f || !isFiniteMatrix(projection)) {
        return;
    }

    CameraWorldPose pose;
    if (!editor_picking::calculateCameraVisualizationPose(
            camera, selectionTarget, pose)) {
        return;
    }

    glm::vec3 forward;
    glm::vec3 right;
    glm::vec3 up;
    if (!makeCameraBasis(pose.front, pose.up, forward, right, up)) {
        return;
    }

    glm::vec2 apexScreen;
    if (!projectWorldToImGui(pose.position, editorView, projection, *viewport,
                             apexScreen)) {
        return;
    }

    const float viewDepth = viewDepthForWorldPoint(pose.position, editorView);
    if (!std::isfinite(viewDepth) || viewDepth <= 0.0f) {
        return;
    }

    glm::vec2 projectedForward;
    glm::vec2 projectedRight;
    glm::vec2 projectedUp;
    if (!editor_picking::projectVulkanDirectionToImGui(
            pose.position, forward, editorView, projection,
            glm::vec2(viewport->Size.x, viewport->Size.y),
            projectedForward) ||
        !editor_picking::projectVulkanDirectionToImGui(
            pose.position, right, editorView, projection,
            glm::vec2(viewport->Size.x, viewport->Size.y), projectedRight) ||
        !editor_picking::projectVulkanDirectionToImGui(
            pose.position, up, editorView, projection,
            glm::vec2(viewport->Size.x, viewport->Size.y), projectedUp)) {
        return;
    }

    editor_picking::EditorHelperGeometry geometry;
    geometry.kind = editor_picking::EditorHelperKind::Camera;
    geometry.selectionTarget = selectionTarget;
    geometry.viewDepth = viewDepth;
    if (!editor_picking::makeCameraHelperGeometry(
            apexScreen, projectedForward, projectedRight, projectedUp,
            camera.fov(), viewport->Size.x / viewport->Size.y, viewDepth,
            geometry.screen)) {
        return;
    }
    editorHelperGeometry_.push_back(geometry);

    ImDrawList* drawList = ImGui::GetBackgroundDrawList(viewport);
    if (drawList == nullptr) {
        return;
    }

    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(
        sceneInteractionRect_.x + sceneInteractionRect_.width,
        sceneInteractionRect_.y + sceneInteractionRect_.height);
    const bool selected = editorSession_ != nullptr &&
                          editorSession_->isSelected(selectionTarget);
    const bool highlighted = active || selected;
    const ImGuiCol accent = highlighted ? ImGuiCol_ButtonActive
                                        : ImGuiCol_ButtonHovered;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[accent]);
    drawList->PushClipRect(clipMin, clipMax, true);

    const float lineThickness = highlighted ? 2.5f : 2.0f;
    for (std::size_t index = 0;
         index < geometry.screen.segmentCount; ++index) {
        const editor_picking::ScreenSpaceSegment& segment =
            geometry.screen.segments[index];
        drawList->AddLine(ImVec2(segment.start.x, segment.start.y),
                          ImVec2(segment.end.x, segment.end.y), color,
                          lineThickness);
    }
    for (std::size_t index = 0; index < geometry.screen.circleCount; ++index) {
        const editor_picking::ScreenSpaceCircle& circle =
            geometry.screen.circles[index];
        drawList->AddCircleFilled(
            ImVec2(circle.center.x, circle.center.y), circle.radius, color);
    }

    drawList->PopClipRect();
}

void ImGuiLayer::drawPointLightVisualization(
    const PointLight& light, const GameObject* selectionTarget,
    const glm::mat4& editorView, const glm::mat4& projection) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f || !isFiniteMatrix(projection)) {
        return;
    }

    glm::vec3 worldPosition;
    if (!editor_picking::deriveWorldPosition(light, worldPosition)) {
        return;
    }

    glm::vec2 center;
    if (!projectWorldToImGui(worldPosition, editorView, projection, *viewport,
                             center)) {
        return;
    }

    editor_picking::EditorHelperGeometry geometry;
    geometry.kind = editor_picking::EditorHelperKind::PointLight;
    geometry.selectionTarget = selectionTarget;
    geometry.viewDepth = viewDepthForWorldPoint(worldPosition, editorView);
    if (!std::isfinite(geometry.viewDepth) || geometry.viewDepth <= 0.0f) {
        return;
    }
    geometry.screen = editor_picking::makePointLightHelperGeometry(
        center, editor_picking::pointLightOuterRadiusPixels,
        geometry.viewDepth);
    if (!geometry.screen.valid) {
        return;
    }
    editorHelperGeometry_.push_back(geometry);

    ImDrawList* drawList = ImGui::GetBackgroundDrawList(viewport);
    if (drawList == nullptr) {
        return;
    }
    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(
        sceneInteractionRect_.x + sceneInteractionRect_.width,
        sceneInteractionRect_.y + sceneInteractionRect_.height);
    const bool selected = editorSession_ != nullptr &&
                          editorSession_->isSelected(selectionTarget);
    const ImGuiCol accent = selected ? ImGuiCol_ButtonActive
                                     : ImGuiCol_ButtonHovered;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[accent]);
    drawList->PushClipRect(clipMin, clipMax, true);
    const float lineThickness = selected ? 2.5f : 2.0f;
    for (std::size_t index = 0;
         index < geometry.screen.segmentCount; ++index) {
        const editor_picking::ScreenSpaceSegment& segment =
            geometry.screen.segments[index];
        drawList->AddLine(ImVec2(segment.start.x, segment.start.y),
                          ImVec2(segment.end.x, segment.end.y), color,
                          lineThickness);
    }
    for (std::size_t index = 0; index < geometry.screen.circleCount; ++index) {
        const editor_picking::ScreenSpaceCircle& circle =
            geometry.screen.circles[index];
        drawList->AddCircleFilled(
            ImVec2(circle.center.x, circle.center.y), circle.radius, color);
    }
    drawList->PopClipRect();
}

void ImGuiLayer::drawDirectionalLightVisualization(
    const DirectionalLight& light, const GameObject* selectionTarget,
    const glm::mat4& editorView, const glm::mat4& projection) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f || !isFiniteMatrix(projection)) {
        return;
    }

    glm::vec3 worldPosition;
    if (!editor_picking::deriveWorldPosition(light, worldPosition)) {
        return;
    }

    glm::vec2 center;
    if (!projectWorldToImGui(worldPosition, editorView, projection, *viewport,
                             center)) {
        return;
    }

    editor_picking::EditorHelperGeometry geometry;
    geometry.kind = editor_picking::EditorHelperKind::DirectionalLight;
    geometry.selectionTarget = selectionTarget;
    geometry.viewDepth = viewDepthForWorldPoint(worldPosition, editorView);
    if (!std::isfinite(geometry.viewDepth) || geometry.viewDepth <= 0.0f) {
        return;
    }

    glm::vec2 projectedDirection(0.0f);
    bool projectedDirectionValid = false;
    glm::vec3 normalizedDirection;
    if (light.calculateWorldDirection(normalizedDirection)) {
        projectedDirectionValid =
            editor_picking::projectVulkanDirectionToImGui(
                worldPosition, normalizedDirection, editorView, projection,
                glm::vec2(viewport->Size.x, viewport->Size.y),
                projectedDirection) &&
            glm::length(projectedDirection) > 1.0e-4f;
    }
    geometry.screen =
        editor_picking::makeDirectionalLightHelperGeometry(
            center, projectedDirection, projectedDirectionValid,
            geometry.viewDepth);
    if (!geometry.screen.valid) {
        return;
    }
    editorHelperGeometry_.push_back(geometry);

    ImDrawList* drawList = ImGui::GetBackgroundDrawList(viewport);
    if (drawList == nullptr) {
        return;
    }
    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(
        sceneInteractionRect_.x + sceneInteractionRect_.width,
        sceneInteractionRect_.y + sceneInteractionRect_.height);
    const bool selected = editorSession_ != nullptr &&
                          editorSession_->isSelected(selectionTarget);
    const ImGuiCol accent = selected ? ImGuiCol_ButtonActive
                                     : ImGuiCol_ButtonHovered;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[accent]);
    drawList->PushClipRect(clipMin, clipMax, true);
    const float lineThickness = selected ? 2.5f : 2.0f;
    for (std::size_t index = 0;
         index < geometry.screen.segmentCount; ++index) {
        const editor_picking::ScreenSpaceSegment& segment =
            geometry.screen.segments[index];
        drawList->AddLine(ImVec2(segment.start.x, segment.start.y),
                          ImVec2(segment.end.x, segment.end.y), color,
                          lineThickness);
    }
    for (std::size_t index = 0; index < geometry.screen.circleCount; ++index) {
        const editor_picking::ScreenSpaceCircle& circle =
            geometry.screen.circles[index];
        drawList->AddCircleFilled(
            ImVec2(circle.center.x, circle.center.y), circle.radius, color);
    }
    drawList->PopClipRect();
}

void ImGuiLayer::drawCharacterVisualization(
    const Character& character, const GameObject* selectionTarget,
    const glm::mat4& editorView, const glm::mat4& projection) {
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f || !isFiniteMatrix(projection)) {
        return;
    }

    const float height = character.capsuleHeight;
    const float radius = character.capsuleRadius;
    if (!std::isfinite(height) || !std::isfinite(radius) || radius <= 0.0f ||
        height <= 2.0f * radius) {
        return;
    }

    glm::vec3 characterWorldPosition;
    const Result worldPositionResult =
        physics::deriveCharacterWorldPosition(character, characterWorldPosition);
    if (!worldPositionResult || !isFiniteVector(characterWorldPosition)) {
        return;
    }

    const float lowerHemisphereCenterY = radius;
    const float upperHemisphereCenterY = height - radius;
    const float diagonalRadius =
        radius * characterVisualizationDiagonalRatio;
    const glm::vec3 capsuleCenter =
        characterWorldPosition + glm::vec3(0.0f, 0.5f * height, 0.0f);
    if (!isFiniteVector(capsuleCenter) ||
        !std::isfinite(lowerHemisphereCenterY) ||
        !std::isfinite(upperHemisphereCenterY) ||
        !std::isfinite(diagonalRadius)) {
        return;
    }

    editor_picking::EditorHelperGeometry geometry;
    geometry.kind = editor_picking::EditorHelperKind::Character;
    geometry.selectionTarget = selectionTarget;
    geometry.viewDepth = viewDepthForWorldPoint(capsuleCenter, editorView);
    float nearestVisibleSegmentDepth =
        std::numeric_limits<float>::infinity();

    const auto projectPoint = [&](const glm::vec3& worldPoint,
                                  glm::vec2& screenPoint) {
        return projectWorldToImGui(worldPoint, editorView, projection,
                                   *viewport, screenPoint);
    };
    const auto appendSegment = [&](const glm::vec3& first,
                                   const glm::vec3& second) {
        if (geometry.screen.segmentCount >=
            geometry.screen.segments.size()) {
            return;
        }

        glm::vec2 firstScreen;
        glm::vec2 secondScreen;
        if (!projectPoint(first, firstScreen) ||
            !projectPoint(second, secondScreen)) {
            return;
        }
        const float firstDepth = viewDepthForWorldPoint(first, editorView);
        const float secondDepth = viewDepthForWorldPoint(second, editorView);
        if (std::isfinite(firstDepth) && firstDepth > 0.0f) {
            nearestVisibleSegmentDepth =
                std::min(nearestVisibleSegmentDepth, firstDepth);
        }
        if (std::isfinite(secondDepth) && secondDepth > 0.0f) {
            nearestVisibleSegmentDepth =
                std::min(nearestVisibleSegmentDepth, secondDepth);
        }
        geometry.screen.segments[geometry.screen.segmentCount++] = {
            firstScreen, secondScreen, 7.0f};
    };
    const auto ringPoint = [&](float y, float ringRadius, float angle) {
        return characterWorldPosition + glm::vec3(
            std::cos(angle) * ringRadius, y,
            std::sin(angle) * ringRadius);
    };
    const auto appendRing = [&](float y, float ringRadius) {
        const float angleStep = characterVisualizationTwoPi /
            static_cast<float>(characterVisualizationRadialSegments);
        for (std::size_t index = 0;
             index < characterVisualizationRadialSegments; ++index) {
            const float angle = angleStep * static_cast<float>(index);
            const float nextAngle = angleStep * static_cast<float>(
                (index + 1) % characterVisualizationRadialSegments);
            appendSegment(ringPoint(y, ringRadius, angle),
                          ringPoint(y, ringRadius, nextAngle));
        }
    };

    appendRing(lowerHemisphereCenterY - diagonalRadius, diagonalRadius);
    appendRing(lowerHemisphereCenterY, radius);
    appendRing(0.5f * height, radius);
    appendRing(upperHemisphereCenterY, radius);
    appendRing(upperHemisphereCenterY + diagonalRadius, diagonalRadius);

    const std::array<glm::vec2, 6> meridianProfile{
        glm::vec2(0.0f, 0.0f),
        glm::vec2(diagonalRadius,
                  lowerHemisphereCenterY - diagonalRadius),
        glm::vec2(radius, lowerHemisphereCenterY),
        glm::vec2(radius, upperHemisphereCenterY),
        glm::vec2(diagonalRadius,
                  upperHemisphereCenterY + diagonalRadius),
        glm::vec2(0.0f, height)};
    const float angleStep = characterVisualizationTwoPi /
        static_cast<float>(characterVisualizationRadialSegments);
    for (std::size_t index = 0;
         index < characterVisualizationRadialSegments; ++index) {
        const float angle = angleStep * static_cast<float>(index);
        const float axisX = std::cos(angle);
        const float axisZ = std::sin(angle);
        std::array<glm::vec3, 6> meridianPoints{};
        for (std::size_t profileIndex = 0;
             profileIndex < meridianProfile.size(); ++profileIndex) {
            const glm::vec2& profilePoint = meridianProfile[profileIndex];
            meridianPoints[profileIndex] = characterWorldPosition + glm::vec3(
                profilePoint.x * axisX, profilePoint.y,
                profilePoint.x * axisZ);
        }
        for (std::size_t profileIndex = 1;
             profileIndex < meridianPoints.size(); ++profileIndex) {
            appendSegment(meridianPoints[profileIndex - 1],
                          meridianPoints[profileIndex]);
        }
    }

    if (!std::isfinite(geometry.viewDepth) || geometry.viewDepth <= 0.0f) {
        geometry.viewDepth = nearestVisibleSegmentDepth;
    }
    geometry.screen.valid = geometry.screen.segmentCount > 0 &&
                            std::isfinite(geometry.viewDepth) &&
                            geometry.viewDepth > 0.0f;
    if (geometry.screen.valid) {
        editorHelperGeometry_.push_back(geometry);
    }

    // The projected segments above remain only as editor picking metadata.
    // Character drawing itself is performed by the Jolt/Vulkan debug pass.
}

void ImGuiLayer::processWorldSelection(Scene* scene, const glm::mat4& view,
                                       const glm::mat4& projection,
                                       SceneRunState runState) {
    if (!editorToolsEnabled(runState) || !inputEnabled_ ||
        !sceneInteractionRect_.valid || !sceneInteractionAreaHovered_ ||
        popupBlocksSceneInteractionThisFrame_ || hasOpenImGuiPopup() ||
        gizmoDragActive_ || ImGuizmo::IsOver() || ImGuizmo::IsUsing() ||
        ImGui::IsDragDropActive() ||
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
    const glm::vec2 mousePoint(mouse.x, mouse.y);
    if (!editor_picking::isScreenPointInsideRect(
            mousePoint,
            glm::vec2(sceneInteractionRect_.x, sceneInteractionRect_.y),
            glm::vec2(sceneInteractionRect_.width,
                      sceneInteractionRect_.height))) {
        return;
    }
    const std::size_t closestHelperIndex =
        editor_picking::closestHelperGeometryIndex(
            editorHelperGeometry_, mousePoint);
    if (closestHelperIndex < editorHelperGeometry_.size()) {
        const GameObject* closestHelperTarget =
            editorHelperGeometry_[closestHelperIndex].selectionTarget;
        const ImGuiIO& io = ImGui::GetIO();
        selectGameObject(
            scene, const_cast<GameObject*>(closestHelperTarget),
            selectionOperationForModifiers(io.KeyCtrl, io.KeyShift));
        return;
    }

    const float localX = mouse.x - viewport->Pos.x;
    const float localY = mouse.y - viewport->Pos.y;
    const float ndcX = 2.0f * localX / viewport->Size.x - 1.0f;
    const float ndcY = 2.0f * localY / viewport->Size.y - 1.0f;
    const glm::mat4 inverseViewProjection = glm::inverse(projection * view);
    if (!isFiniteMatrix(inverseViewProjection)) {
        return;
    }
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
    GameObject* closestObject = editor_picking::pickClosestObject(*scene, ray);
    const ImGuiIO& io = ImGui::GetIO();
    if (closestObject == nullptr) {
        if (emptyWorldSelectionOperationForModifiers(io.KeyCtrl, io.KeyShift) ==
            EmptyWorldSelectionOperation::Clear) {
            selectGameObject(scene, nullptr,
                             SelectionOperation::ReplaceExact);
        }
        return;
    }
    selectGameObject(scene, closestObject,
                     selectionOperationForModifiers(io.KeyCtrl, io.KeyShift));
}

void ImGuiLayer::drawInspector(Scene* scene, bool disabled,
                               SceneRunState runState) {
    ImGui::SetNextWindowSize(ImVec2(340.0f, 500.0f),
                             ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Inspector")) {
        finishInspectorTransformEdit(scene, true);
        ImGui::End();
        return;
    }

    ImGui::BeginDisabled(disabled);

    GameObject* selectedGameObject = const_cast<GameObject*>(
        selectedGameObjectForScene(scene));
    if (selectedGameObject == nullptr) {
        finishInspectorTransformEdit(scene, true);
        ImGui::TextUnformatted(
            "Select a GameObject from the Scene Hierarchy.");
        if (!inspectorError_.empty()) {
            ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.3f, 1.0f), "%s",
                               inspectorError_.c_str());
        }
        ImGui::EndDisabled();
        ImGui::End();
        return;
    }

    PointLight* pointLight = dynamic_cast<PointLight*>(selectedGameObject);
    DirectionalLight* directionalLight =
        dynamic_cast<DirectionalLight*>(selectedGameObject);
    Camera* camera = dynamic_cast<Camera*>(selectedGameObject);
    const Character* character = dynamic_cast<const Character*>(selectedGameObject);
    const char* objectType = pointLight != nullptr
                                 ? "Point Light"
                                 : directionalLight != nullptr
                                       ? "Directional Light"
                                       : camera != nullptr ? "Camera"
                                                           : "GameObject";

    std::string name = selectedGameObject->name;
    if (ImGui::InputText("Name", &name)) {
        const Result result = editor_mutation::applyName(*selectedGameObject,
                                                         name);
        if (result) {
            inspectorError_.clear();
        } else {
            inspectorError_ = result.error();
        }
    }
    ImGui::Text("Type: %s", objectType);

    ImGui::SeparatorText("Transform");
    bool localTransform =
        editorSession_->transformSpace() == TransformSpace::Local;
    if (ImGui::Checkbox("Local Transform", &localTransform)) {
        editorSession_->setTransformSpace(
            localTransform ? TransformSpace::Local : TransformSpace::World);
    }
    glm::vec3 position = selectedGameObject->position;
    const bool positionChanged =
        ImGui::DragFloat3("Position", &position.x, 0.1f);
    const bool positionActivated = ImGui::IsItemActivated();
    const bool positionDeactivated = ImGui::IsItemDeactivated();
    if (runState == SceneRunState::Editing &&
        (positionActivated || positionChanged)) {
        beginInspectorTransformEdit(
            scene, selectedGameObject, InspectorTransformWidget::Position);
    }
    if (positionChanged) {
        const Result result =
            editor_mutation::applyPosition(*selectedGameObject, position);
        if (result) {
            if (!inspectorTransformUndoCaptureFailed_) {
                inspectorError_.clear();
            }
        } else {
            inspectorError_ = result.error();
        }
    }
    if (positionDeactivated) {
        finishInspectorTransformEdit(scene, true);
    }

    glm::vec3 rotation = selectedGameObject->rotation;
    const bool rotationChanged = ImGui::DragFloat3(
        "Rotation (degrees)", &rotation.x, 0.5f);
    const bool rotationActivated = ImGui::IsItemActivated();
    const bool rotationDeactivated = ImGui::IsItemDeactivated();
    if (runState == SceneRunState::Editing &&
        (rotationActivated || rotationChanged)) {
        beginInspectorTransformEdit(
            scene, selectedGameObject, InspectorTransformWidget::Rotation);
    }
    if (rotationChanged) {
        const Result result =
            editor_mutation::applyRotation(*selectedGameObject, rotation);
        if (result) {
            if (!inspectorTransformUndoCaptureFailed_) {
                inspectorError_.clear();
            }
        } else {
            inspectorError_ = result.error();
        }
    }
    if (rotationDeactivated) {
        finishInspectorTransformEdit(scene, true);
    }

    glm::vec3 scale = selectedGameObject->scale;
    const bool scaleChanged = ImGui::DragFloat3("Scale", &scale.x, 0.01f);
    const bool scaleActivated = ImGui::IsItemActivated();
    const bool scaleDeactivated = ImGui::IsItemDeactivated();
    const bool scaleItemActive = ImGui::IsItemActive();
    inspectorScaleWidgetSeen_ = true;
    if (scaleItemActive) {
        inspectorScaleDragActive_ = true;
        inspectorScaleDragScene_ = scene;
        inspectorScaleDragObject_ = selectedGameObject;
    } else {
        clearInspectorScaleDrag();
    }
    if (runState == SceneRunState::Editing &&
        (scaleActivated || scaleChanged)) {
        beginInspectorTransformEdit(
            scene, selectedGameObject, InspectorTransformWidget::Scale);
    }
    if (scaleChanged) {
        const Result result =
            applyEditorScale(*selectedGameObject, scale, runState);
        if (result) {
            // Diagnostics were prepared before this widget was evaluated and
            // therefore may describe the previous exact scale.
            setPhysicsDiagnostics(nullptr, std::nullopt, {});
            if (!inspectorTransformUndoCaptureFailed_) {
                inspectorError_.clear();
            }
        } else {
            inspectorError_ = result.error();
        }
    }
    if (scaleDeactivated) {
        finishInspectorTransformEdit(scene, true);
    }

    if (character != nullptr || selectedGameObject->physics.enabled) {
        ImGui::SeparatorText("Physics");
        const bool diagnosticsMatch =
            physicsDiagnosticsObject_ == selectedGameObject &&
            physicsDiagnostics_.has_value();
        if (character != nullptr) {
            ImGui::TextUnformatted("Collider Type: Capsule");
            ImGui::TextUnformatted("Collision Representation: Analytic");
            ImGui::Text("Height: %.3f", character->capsuleHeight);
            ImGui::Text("Radius: %.3f", character->capsuleRadius);
            if (diagnosticsMatch) {
                ImGui::Text("Jolt Shape Memory: %s",
                            formatMemoryBytes(physicsDiagnostics_->joltBytes).c_str());
            }
        } else {
            const char* motion = selectedGameObject->physics.motionType ==
                                         GameObject::PhysicsMotionType::Static
                                     ? "Static"
                                     : "Dynamic";
            const auto collider = selectedGameObject->physics.colliderType;
            const char* type = collider == GameObject::PhysicsColliderType::Mesh
                                   ? "Mesh"
                                   : collider == GameObject::PhysicsColliderType::Sphere
                                         ? "Sphere"
                                         : "ConvexHull";
            ImGui::Text("Motion Type: %s", motion);
            ImGui::Text("Collider Type: %s", type);
            if (diagnosticsMatch) {
                switch (collider) {
                case GameObject::PhysicsColliderType::Mesh:
                    ImGui::Text("Input Vertices: %zu",
                                physicsDiagnostics_->inputVertices);
                    ImGui::Text("Input Triangles: %zu",
                                physicsDiagnostics_->inputTriangles);
                    ImGui::Text("Jolt Triangles: %zu",
                                physicsDiagnostics_->joltTriangles);
                    break;
                case GameObject::PhysicsColliderType::ConvexHull:
                    ImGui::Text("Input Points: %zu",
                                physicsDiagnostics_->inputPoints);
                    ImGui::Text("Cooked Hull Vertices: %zu",
                                physicsDiagnostics_->cookedHullVertices);
                    ImGui::Text("Jolt Triangles: %zu",
                                physicsDiagnostics_->joltTriangles);
                    break;
                case GameObject::PhysicsColliderType::Sphere:
                    ImGui::TextUnformatted("Collision Representation: Analytic");
                    ImGui::Text("Radius: %.3f",
                                selectedGameObject->physics.sphereRadius);
                    break;
                }
                ImGui::Text("Jolt Shape Memory: %s",
                            formatMemoryBytes(physicsDiagnostics_->joltBytes).c_str());
            } else if (collider == GameObject::PhysicsColliderType::Sphere) {
                ImGui::TextUnformatted("Collision Representation: Analytic");
                ImGui::Text("Radius: %.3f",
                            selectedGameObject->physics.sphereRadius);
            }

            bool enabled = editorSession_->renderColliderEnabled(
                *selectedGameObject);
            if (ImGui::Checkbox("Render Collider", &enabled)) {
                editorSession_->setRenderColliderEnabled(*selectedGameObject,
                                                         enabled);
            }
        }
        if (physicsDiagnosticsObject_ == selectedGameObject &&
            !physicsDiagnosticsError_.empty()) {
            ImGui::TextWrapped("Collision diagnostics unavailable: %s",
                               physicsDiagnosticsError_.c_str());
        }
    }

    if (pointLight != nullptr) {
        ImGui::SeparatorText("Point Light");

        glm::vec3 color = pointLight->color;
        if (ImGui::ColorEdit3("Color", &color.x,
                              ImGuiColorEditFlags_HDR |
                                  ImGuiColorEditFlags_Float)) {
            const Result result =
                editor_mutation::applyPointLightColor(*pointLight, color);
            if (result) {
                inspectorError_.clear();
            } else {
                inspectorError_ = result.error();
            }
        }

        float intensity = pointLight->intensity;
        if (ImGui::DragFloat("Intensity", &intensity, 0.1f)) {
            const Result result = editor_mutation::applyPointLightIntensity(
                *pointLight, intensity);
            if (result) {
                inspectorError_.clear();
            } else {
                inspectorError_ = result.error();
            }
        }
    }

    if (directionalLight != nullptr) {
        ImGui::SeparatorText("Directional Light");

        glm::vec3 color = directionalLight->color;
        if (ImGui::ColorEdit3("Color", &color.x,
                              ImGuiColorEditFlags_HDR |
                                  ImGuiColorEditFlags_Float)) {
            const Result result = editor_mutation::applyDirectionalLightColor(
                *directionalLight, color);
            if (result) {
                inspectorError_.clear();
            } else {
                inspectorError_ = result.error();
            }
        }

        float intensity = directionalLight->intensity;
        if (ImGui::DragFloat("Intensity", &intensity, 0.1f)) {
            const Result result =
                editor_mutation::applyDirectionalLightIntensity(
                    *directionalLight, intensity);
            if (result) {
                inspectorError_.clear();
            } else {
                inspectorError_ = result.error();
            }
        }

        ImGui::TextWrapped(
            "Directional-light position does not affect lighting; use "
            "Rotation to aim the light.");
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
    const bool popupOpen = popupBlocksSceneInteractionThisFrame_ ||
                           hasOpenImGuiPopup();
    const bool imguiObstructed = popupOpen ||
                                 context->HoveredWindow != nullptr ||
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
    stopNativeFileDialog();
    cancelEditorTransformDrag();
    clearInspectorScaleDrag();
    if (editorSession_ != nullptr) {
        finishInspectorTransformEdit(
            editorSession_->selectionScene(), true);
    }
    frameStarted_ = false;
    drawDataReady_ = false;
    physicsDiagnosticsObject_ = nullptr;
    physicsDiagnostics_.reset();
    physicsDiagnosticsError_.clear();
    sceneInteractionAreaHovered_ = false;
    sceneInteractionRect_ = {};
    popupBlocksSceneInteractionThisFrame_ = false;
    suppressSceneInputUntilMouseRelease_ = false;
    pendingHierarchyReorder_.reset();
    inputEnabled_ = true;
    gizmoDragActive_ = false;
    runtimeTransformDragActive_ = false;
    runtimeTransformObject_ = nullptr;
    if (editorSession_ != nullptr) {
        editorSession_->select(nullptr, nullptr);
        (void)editorSession_->consumeEditorAction();
        (void)editorSession_->consumeRuntimeTransformEdit();
    }
    inspectorError_.clear();
    saveAsOverwritePath_.clear();
    openSaveAsOverwritePopup_ = false;
    openLoadConfirmationPopup_ = false;
    openQuitConfirmationPopup_ = false;

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
    stopNativeFileDialog();
    cancelEditorTransformDrag();
    clearInspectorScaleDrag();
    clearInspectorTransformEdit();
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
    physicsDiagnosticsObject_ = nullptr;
    physicsDiagnostics_.reset();
    physicsDiagnosticsError_.clear();
    sceneInteractionAreaHovered_ = false;
    sceneInteractionRect_ = {};
    popupBlocksSceneInteractionThisFrame_ = false;
    suppressSceneInputUntilMouseRelease_ = false;
    pendingHierarchyReorder_.reset();
    inputEnabled_ = true;
    gizmoDragActive_ = false;
    runtimeTransformDragActive_ = false;
    runtimeTransformObject_ = nullptr;
    editorSession_ = nullptr;
    inspectorError_.clear();
    saveAsOverwritePath_.clear();
    openSaveAsOverwritePopup_ = false;
    openLoadConfirmationPopup_ = false;
    openQuitConfirmationPopup_ = false;
    window_ = nullptr;
}
