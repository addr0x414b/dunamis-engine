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
#include <utility>
#include <vector>

#include <glm/gtc/type_ptr.hpp>

#include "editor_picking.h"
#include "../editor/editor_mutation.h"
#include "../editor/editor_picking.h"
#include "../editor/editor_transform.h"
#include "../editor/editor_session.h"
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
constexpr float cameraVisualizationDistance = 30.0f;
constexpr float directionalVisualizationDistance = 30.0f;
constexpr float pointLightVisualizationRadius = 7.0f;
constexpr float pointLightVisualizationInnerRadiusRatio = 0.45f;
constexpr float pointLightVisualizationCenterRadiusRatio = 1.0f / 3.0f;
constexpr float directionalLightVisualizationRadius = 5.0f;
constexpr float directionalLightArrowheadLength = 9.0f;
constexpr float directionalLightArrowheadHalfWidth = 4.0f;
constexpr float directionalLightCenterRadius = 2.5f;
constexpr std::size_t characterVisualizationRadialSegments = 12;
constexpr float characterVisualizationTwoPi = 6.28318530717958647692f;
constexpr float characterVisualizationDiagonalRatio = 0.70710678118654752440f;
constexpr float minimumLightVisualizationPixelRadius = 1.0f;
constexpr float editorHelperLineHitTolerance = 7.0f;
constexpr float editorHelperPointHitRadius = 8.0f;
constexpr float editorHelperHitTieEpsilonSquared = 1.0e-4f;

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
    screenPoint = glm::vec2(
        viewport.Pos.x + (ndc.x * 0.5f + 0.5f) * viewport.Size.x,
        viewport.Pos.y + (ndc.y * 0.5f + 0.5f) * viewport.Size.y);
    return std::isfinite(screenPoint.x) && std::isfinite(screenPoint.y);
}

bool projectWorldRadiusToImGui(const glm::vec3& worldPoint,
                               const glm::vec2& centerScreen,
                               const glm::mat4& editorView,
                               const glm::mat4& projection,
                               const ImGuiViewport& viewport,
                               float worldRadius,
                               float& screenRadius) noexcept {
    screenRadius = 0.0f;
    if (!isFiniteVector(worldPoint) || !std::isfinite(centerScreen.x) ||
        !std::isfinite(centerScreen.y) || !isFiniteMatrix(editorView) ||
        !std::isfinite(worldRadius) || worldRadius <= 0.0f) {
        return false;
    }

    const glm::mat4 inverseView = glm::inverse(editorView);
    if (!isFiniteMatrix(inverseView)) {
        return false;
    }

    glm::vec3 cameraRight;
    if (!normalizeFinite(glm::vec3(inverseView[0]), cameraRight)) {
        return false;
    }

    const glm::vec3 offsetWorldPoint =
        worldPoint + cameraRight * worldRadius;
    if (!isFiniteVector(offsetWorldPoint)) {
        return false;
    }

    glm::vec2 offsetScreen;
    if (!projectWorldToImGui(offsetWorldPoint, editorView, projection,
                             viewport, offsetScreen)) {
        return false;
    }

    screenRadius = glm::length(offsetScreen - centerScreen);
    return std::isfinite(screenRadius) && screenRadius >= 0.0f;
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
    if (!sdlBackendInitialized_) {
        return;
    }
    (void)ImGui_ImplSDL3_ProcessEvent(&event);
}

void ImGuiLayer::setInputEnabled(bool enabled) noexcept {
    inputEnabled_ = enabled;
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
    drawToolbar(runState);
    drawPersistenceDialogs();
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
        const bool disabled = !editorToolsEnabled(runState);
        drawSceneHierarchy(scene, disabled);
        updateSceneInteractionAreaHovered();
        processGizmoShortcuts(scene, runState);
        drawCameraVisualizations(scene, view, projection, runState);
        drawTransformGizmo(scene, view, projection, runState);
        processWorldSelection(scene, view, projection, runState);
        drawInspector(scene, disabled);
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
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup);
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
    if (openSaveAsOverwritePopup_) {
        ImGui::OpenPopup("Overwrite Scene##SaveAs");
        openSaveAsOverwritePopup_ = false;
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

void ImGuiLayer::finishRuntimeTransformDrag() noexcept {
    if (runtimeTransformDragActive_ && runtimeTransformObject_ != nullptr) {
        submitRuntimeTransformEdit(RuntimeTransformEdit{
            runtimeTransformObject_, runtimeTransformObject_->position,
            runtimeTransformObject_->rotation, false});
    }
    runtimeTransformDragActive_ = false;
    runtimeTransformObject_ = nullptr;
}

void ImGuiLayer::clearSelection() noexcept {
    finishRuntimeTransformDrag();
    if (editorSession_ != nullptr) {
        editorSession_->clearSelection();
    }
    inspectorError_.clear();
}

bool ImGuiLayer::sceneInteractionAreaHovered() const noexcept {
    return sceneInteractionAreaHovered_;
}

void ImGuiLayer::synchronizeSelection(Scene* scene) noexcept {
    if (editorSession_ == nullptr) {
        return;
    }

    if (scene != editorSession_->selectionScene()) {
        clearSelection();
    } else if (scene == nullptr) {
        clearSelection();
    } else if (editorSession_->selectedGameObject() != nullptr) {
        bool selectedObjectIsPresent = false;
        for (const auto& object : scene->gameObjects()) {
            if (object.get() == editorSession_->selectedGameObject()) {
                selectedObjectIsPresent = true;
                break;
            }
        }
        if (!selectedObjectIsPresent) {
            clearSelection();
        }
    }
    editorSession_->synchronizeSelection(scene);
}

void ImGuiLayer::selectGameObject(Scene* scene, GameObject* object) noexcept {
    if (editorSession_ == nullptr) {
        return;
    }
    if (scene == nullptr) {
        clearSelection();
        editorSession_->select(nullptr, nullptr);
        return;
    }

    if (object == nullptr) {
        clearSelection();
        editorSession_->select(scene, nullptr);
        return;
    }

    for (const auto& owner : scene->gameObjects()) {
        if (owner.get() == object) {
            if (editorSession_->selectedGameObject() != object) {
                finishRuntimeTransformDrag();
            }
            editorSession_->select(scene, object);
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
        const bool selected =
            selectedGameObjectForScene(scene) == object;
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

void ImGuiLayer::processGizmoShortcuts(Scene* scene,
                                       SceneRunState runState) noexcept {
    if (!editorToolsEnabled(runState) || !inputEnabled_ ||
        selectedGameObjectForScene(scene) == nullptr ||
        ImGui::GetIO().WantTextInput || ImGui::IsAnyItemActive() ||
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
    if (!editorToolsEnabled(runState) ||
        !sceneInteractionRect_.valid) {
        gizmoDragActive_ = false;
        runtimeTransformDragActive_ = false;
        runtimeTransformObject_ = nullptr;
        return;
    }
    GameObject* selected = const_cast<GameObject*>(
        selectedGameObjectForScene(scene));
    ImGuiViewport* viewport = ImGui::GetMainViewport();
    if (selected == nullptr || viewport == nullptr || viewport->Size.x <= 0.0f ||
        viewport->Size.y <= 0.0f) {
        gizmoDragActive_ = false;
        runtimeTransformDragActive_ = false;
        runtimeTransformObject_ = nullptr;
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
    const TransformTool transformTool = editorSession_->transformTool();
    if (runState == SceneRunState::Simulating && selected->physics.enabled &&
        selected->parent() != nullptr &&
        (transformTool == TransformTool::Translate ||
         transformTool == TransformTool::Rotate)) {
        gizmoDragActive_ = false;
        runtimeTransformDragActive_ = false;
        runtimeTransformObject_ = nullptr;
        inspectorError_ =
            "Runtime hierarchy transform gizmos are unavailable for physics "
            "bodies until hierarchy-aware physics support is implemented.";
        drawList->PopClipRect();
        return;
    }

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
    ImGuizmo::OPERATION operation = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE mode = ImGuizmo::WORLD;
    switch (transformTool) {
    case TransformTool::Translate: {
        operation = ImGuizmo::TRANSLATE;
        mode = ImGuizmo::WORLD;
        break;
    }
    case TransformTool::Scale:
        if (runState == SceneRunState::Simulating && selected->physics.enabled) {
            gizmoDragActive_ = false;
            inspectorError_ = "Runtime Scale is unavailable for physics bodies.";
            drawList->PopClipRect();
            return;
        }
        operation = ImGuizmo::SCALE;
        mode = ImGuizmo::LOCAL;
        break;
    case TransformTool::Rotate:
        operation = ImGuizmo::ROTATE;
        mode = ImGuizmo::LOCAL;
        break;
    }
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
                result = editor_mutation::applyScale(
                    *selected, localTransform.scale);
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

    const glm::vec3 apex = pose.position;
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

    EditorHelperGeometry geometry;
    geometry.kind = EditorHelperKind::Camera;
    geometry.selectionTarget = selectionTarget;
    geometry.viewDepth = viewDepthForWorldPoint(apex, editorView);

    const std::array<glm::vec3, 6> points{
        apex, topLeft, topRight, bottomRight, bottomLeft, planeCenter};
    const std::array<int, 9> firstIndices{0, 0, 0, 0, 1, 2, 3, 4, 0};
    const std::array<int, 9> secondIndices{1, 2, 3, 4, 2, 3, 4, 1, 5};
    const auto projectPoint = [&](const glm::vec3& worldPoint,
                                  glm::vec2& screenPoint) {
        return projectWorldToImGui(worldPoint, editorView, projection,
                                   *viewport, screenPoint);
    };

    glm::vec2 apexScreen;
    geometry.pointValid = projectPoint(apex, apexScreen);
    if (geometry.pointValid) {
        geometry.point = apexScreen;
    }
    for (std::size_t index = 0; index < firstIndices.size(); ++index) {
        glm::vec2 firstScreen;
        glm::vec2 secondScreen;
        if (!projectPoint(points[firstIndices[index]], firstScreen) ||
            !projectPoint(points[secondIndices[index]], secondScreen)) {
            continue;
        }
        geometry.segments[geometry.segmentCount++] = {
            firstScreen, secondScreen};
    }
    editorHelperGeometry_.push_back(geometry);

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
    for (std::size_t index = 0; index < geometry.segmentCount; ++index) {
        const EditorHelperSegment& segment = geometry.segments[index];
        drawList->AddLine(ImVec2(segment.start.x, segment.start.y),
                          ImVec2(segment.end.x, segment.end.y), color,
                          lineThickness);
    }
    if (geometry.pointValid) {
        drawList->AddCircleFilled(
            ImVec2(geometry.point.x, geometry.point.y), 4.0f, color);
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

    EditorHelperGeometry geometry;
    geometry.kind = EditorHelperKind::PointLight;
    geometry.selectionTarget = selectionTarget;
    geometry.point = center;
    geometry.pointValid = true;
    geometry.viewDepth = viewDepthForWorldPoint(worldPosition, editorView);

    float outerRadius = 0.0f;
    if (!projectWorldRadiusToImGui(
            worldPosition, center, editorView, projection, *viewport,
            pointLightVisualizationRadius, outerRadius)) {
        return;
    }
    outerRadius = std::max(outerRadius, minimumLightVisualizationPixelRadius);
    const float innerRadius =
        outerRadius * pointLightVisualizationInnerRadiusRatio;

    constexpr std::array<glm::vec2, 8> radialDirections{
        glm::vec2(0.0f, -1.0f), glm::vec2(0.70710677f, -0.70710677f),
        glm::vec2(1.0f, 0.0f), glm::vec2(0.70710677f, 0.70710677f),
        glm::vec2(0.0f, 1.0f), glm::vec2(-0.70710677f, 0.70710677f),
        glm::vec2(-1.0f, 0.0f), glm::vec2(-0.70710677f, -0.70710677f)};
    for (const glm::vec2& radialDirection : radialDirections) {
        geometry.segments[geometry.segmentCount++] = {
            center + radialDirection * innerRadius,
            center + radialDirection * outerRadius};
    }
    editorHelperGeometry_.push_back(geometry);

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    if (drawList == nullptr) {
        return;
    }
    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(
        sceneInteractionRect_.x + sceneInteractionRect_.width,
        sceneInteractionRect_.y + sceneInteractionRect_.height);
    const bool selected = editorSession_->selectedGameObject() == selectionTarget;
    const ImGuiCol accent = selected ? ImGuiCol_ButtonActive
                                     : ImGuiCol_ButtonHovered;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[accent]);
    drawList->PushClipRect(clipMin, clipMax, true);
    const float lineThickness = selected ? 2.5f : 2.0f;
    for (std::size_t index = 0; index < geometry.segmentCount; ++index) {
        const EditorHelperSegment& segment = geometry.segments[index];
        drawList->AddLine(ImVec2(segment.start.x, segment.start.y),
                          ImVec2(segment.end.x, segment.end.y), color,
                          lineThickness);
    }
    drawList->AddCircleFilled(
        ImVec2(center.x, center.y),
        outerRadius * pointLightVisualizationCenterRadiusRatio, color);
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

    EditorHelperGeometry geometry;
    geometry.kind = EditorHelperKind::DirectionalLight;
    geometry.selectionTarget = selectionTarget;
    geometry.point = center;
    geometry.pointValid = true;
    geometry.viewDepth = viewDepthForWorldPoint(worldPosition, editorView);

    float markerRadius = 0.0f;
    if (!projectWorldRadiusToImGui(
            worldPosition, center, editorView, projection, *viewport,
            directionalLightVisualizationRadius, markerRadius)) {
        return;
    }
    markerRadius =
        std::max(markerRadius, minimumLightVisualizationPixelRadius);
    const std::array<glm::vec2, 4> markerPoints{
        center + glm::vec2(0.0f, -markerRadius),
        center + glm::vec2(markerRadius, 0.0f),
        center + glm::vec2(0.0f, markerRadius),
        center + glm::vec2(-markerRadius, 0.0f)};
    for (std::size_t index = 0; index < markerPoints.size(); ++index) {
        geometry.segments[geometry.segmentCount++] = {
            markerPoints[index], markerPoints[(index + 1) % markerPoints.size()]};
    }

    glm::vec3 normalizedDirection;
    if (light.calculateWorldDirection(normalizedDirection)) {
        const glm::vec3 arrowEnd =
            worldPosition + normalizedDirection * directionalVisualizationDistance;
        glm::vec2 arrowEndScreen;
        if (isFiniteVector(arrowEnd) &&
            projectWorldToImGui(arrowEnd, editorView, projection, *viewport,
                                arrowEndScreen)) {
            const glm::vec2 screenDirection = arrowEndScreen - center;
            const float screenLength = glm::length(screenDirection);
            if (std::isfinite(screenLength) && screenLength > 1.0e-4f) {
                const glm::vec2 unitDirection = screenDirection / screenLength;
                const glm::vec2 perpendicular(-unitDirection.y,
                                               unitDirection.x);
                geometry.segments[geometry.segmentCount++] = {
                    center, arrowEndScreen};
                float arrowheadLength = 0.0f;
                float arrowheadHalfWidth = 0.0f;
                if (projectWorldRadiusToImGui(
                        worldPosition, center, editorView, projection,
                        *viewport, directionalLightArrowheadLength,
                        arrowheadLength) &&
                    projectWorldRadiusToImGui(
                        worldPosition, center, editorView, projection,
                        *viewport, directionalLightArrowheadHalfWidth,
                        arrowheadHalfWidth)) {
                    const glm::vec2 arrowBase =
                        arrowEndScreen - unitDirection * arrowheadLength;
                    const glm::vec2 firstWing =
                        arrowBase + perpendicular * arrowheadHalfWidth;
                    const glm::vec2 secondWing =
                        arrowBase - perpendicular * arrowheadHalfWidth;
                    geometry.segments[geometry.segmentCount++] = {
                        arrowEndScreen, firstWing};
                    geometry.segments[geometry.segmentCount++] = {
                        arrowEndScreen, secondWing};
                }
            }
        }
    }
    editorHelperGeometry_.push_back(geometry);

    ImDrawList* drawList = ImGui::GetForegroundDrawList(viewport);
    if (drawList == nullptr) {
        return;
    }
    const ImVec2 clipMin(sceneInteractionRect_.x, sceneInteractionRect_.y);
    const ImVec2 clipMax(
        sceneInteractionRect_.x + sceneInteractionRect_.width,
        sceneInteractionRect_.y + sceneInteractionRect_.height);
    const bool selected = editorSession_->selectedGameObject() == selectionTarget;
    const ImGuiCol accent = selected ? ImGuiCol_ButtonActive
                                     : ImGuiCol_ButtonHovered;
    const ImU32 color = ImGui::ColorConvertFloat4ToU32(
        ImGui::GetStyle().Colors[accent]);
    drawList->PushClipRect(clipMin, clipMax, true);
    const float lineThickness = selected ? 2.5f : 2.0f;
    for (std::size_t index = 0; index < geometry.segmentCount; ++index) {
        const EditorHelperSegment& segment = geometry.segments[index];
        drawList->AddLine(ImVec2(segment.start.x, segment.start.y),
                          ImVec2(segment.end.x, segment.end.y), color,
                          lineThickness);
    }
    float centerRadius = 0.0f;
    if (projectWorldRadiusToImGui(
            worldPosition, center, editorView, projection, *viewport,
            directionalLightCenterRadius, centerRadius)) {
        drawList->AddCircleFilled(ImVec2(center.x, center.y), centerRadius,
                                   color);
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
    if (!isFiniteVector(character.position) || !std::isfinite(height) ||
        !std::isfinite(radius) || radius <= 0.0f ||
        height <= 2.0f * radius) {
        return;
    }

    const float lowerHemisphereCenterY = radius;
    const float upperHemisphereCenterY = height - radius;
    const float diagonalRadius =
        radius * characterVisualizationDiagonalRatio;
    const glm::vec3 capsuleCenter =
        character.position + glm::vec3(0.0f, 0.5f * height, 0.0f);
    if (!isFiniteVector(capsuleCenter) ||
        !std::isfinite(lowerHemisphereCenterY) ||
        !std::isfinite(upperHemisphereCenterY) ||
        !std::isfinite(diagonalRadius)) {
        return;
    }

    EditorHelperGeometry geometry;
    geometry.kind = EditorHelperKind::Character;
    geometry.selectionTarget = selectionTarget;
    geometry.viewDepth = viewDepthForWorldPoint(capsuleCenter, editorView);

    const auto projectPoint = [&](const glm::vec3& worldPoint,
                                  glm::vec2& screenPoint) {
        return projectWorldToImGui(worldPoint, editorView, projection,
                                   *viewport, screenPoint);
    };
    const auto appendSegment = [&](const glm::vec3& first,
                                   const glm::vec3& second) {
        if (geometry.segmentCount >= geometry.segments.size()) {
            return;
        }

        glm::vec2 firstScreen;
        glm::vec2 secondScreen;
        if (!projectPoint(first, firstScreen) ||
            !projectPoint(second, secondScreen)) {
            return;
        }
        geometry.segments[geometry.segmentCount++] = {
            firstScreen, secondScreen};
    };
    const auto ringPoint = [&](float y, float ringRadius, float angle) {
        return character.position + glm::vec3(
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
            meridianPoints[profileIndex] = character.position + glm::vec3(
                profilePoint.x * axisX, profilePoint.y,
                profilePoint.x * axisZ);
        }
        for (std::size_t profileIndex = 1;
             profileIndex < meridianPoints.size(); ++profileIndex) {
            appendSegment(meridianPoints[profileIndex - 1],
                          meridianPoints[profileIndex]);
        }
    }

    editorHelperGeometry_.push_back(geometry);

    // The projected segments above remain only as editor picking metadata.
    // Character drawing itself is performed by the Jolt/Vulkan debug pass.
}

void ImGuiLayer::processWorldSelection(Scene* scene, const glm::mat4& view,
                                       const glm::mat4& projection,
                                       SceneRunState runState) {
    if (!editorToolsEnabled(runState) || !inputEnabled_ ||
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
    if (!std::isfinite(mouse.x) || !std::isfinite(mouse.y) ||
        mouse.x < sceneInteractionRect_.x ||
        mouse.y < sceneInteractionRect_.y ||
        mouse.x > sceneInteractionRect_.x + sceneInteractionRect_.width ||
        mouse.y > sceneInteractionRect_.y + sceneInteractionRect_.height) {
        return;
    }

    const glm::vec2 mousePoint(mouse.x, mouse.y);
    const float lineToleranceSquared =
        editorHelperLineHitTolerance * editorHelperLineHitTolerance;
    const float pointRadiusSquared =
        editorHelperPointHitRadius * editorHelperPointHitRadius;
    const GameObject* closestHelperTarget = nullptr;
    float closestHelperDistanceSquared =
        std::numeric_limits<float>::infinity();
    float closestHelperDepth = std::numeric_limits<float>::infinity();
    for (const EditorHelperGeometry& geometry : editorHelperGeometry_) {
        if (geometry.selectionTarget == nullptr) {
            continue;
        }

        float helperDistanceSquared =
            std::numeric_limits<float>::infinity();
        if (geometry.pointValid) {
            const glm::vec2 pointDifference = mousePoint - geometry.point;
            const float pointDistanceSquared = glm::dot(
                pointDifference, pointDifference);
            if (std::isfinite(pointDistanceSquared) &&
                pointDistanceSquared <= pointRadiusSquared) {
                helperDistanceSquared = pointDistanceSquared;
            }
        }
        for (std::size_t index = 0; index < geometry.segmentCount; ++index) {
            const EditorHelperSegment& segment = geometry.segments[index];
            const float segmentDistanceSquared =
                editor_picking::distanceSquaredToSegment(
                    mousePoint, segment.start, segment.end);
            if (std::isfinite(segmentDistanceSquared) &&
                segmentDistanceSquared <= lineToleranceSquared) {
                helperDistanceSquared = std::min(helperDistanceSquared,
                                                 segmentDistanceSquared);
            }
        }
        if (!std::isfinite(helperDistanceSquared)) {
            continue;
        }

        const float depth = std::isfinite(geometry.viewDepth)
                                ? geometry.viewDepth
                                : std::numeric_limits<float>::infinity();
        const bool closerOnScreen =
            helperDistanceSquared < closestHelperDistanceSquared -
                editorHelperHitTieEpsilonSquared;
        const bool tiedOnScreen = std::abs(helperDistanceSquared -
                                           closestHelperDistanceSquared) <=
                                  editorHelperHitTieEpsilonSquared;
        const bool closerInView = tiedOnScreen &&
                                  depth < closestHelperDepth -
                                      editorHelperHitTieEpsilonSquared;
        if (closerOnScreen || closerInView) {
            closestHelperTarget = geometry.selectionTarget;
            closestHelperDistanceSquared = helperDistanceSquared;
            closestHelperDepth = depth;
        }
    }

    if (closestHelperTarget != nullptr) {
        selectGameObject(scene, const_cast<GameObject*>(closestHelperTarget));
        return;
    }

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
    GameObject* closestObject = editor_picking::pickClosestObject(*scene, ray);
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

    GameObject* selectedGameObject = const_cast<GameObject*>(
        selectedGameObjectForScene(scene));
    if (selectedGameObject == nullptr) {
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
    glm::vec3 position = selectedGameObject->position;
    if (ImGui::DragFloat3("Position", &position.x, 0.1f)) {
        const Result result =
            editor_mutation::applyPosition(*selectedGameObject, position);
        if (result) {
            inspectorError_.clear();
        } else {
            inspectorError_ = result.error();
        }
    }

    glm::vec3 rotation = selectedGameObject->rotation;
    if (ImGui::DragFloat3("Rotation (degrees)", &rotation.x, 0.5f)) {
        const Result result =
            editor_mutation::applyRotation(*selectedGameObject, rotation);
        if (result) {
            inspectorError_.clear();
        } else {
            inspectorError_ = result.error();
        }
    }

    glm::vec3 scale = selectedGameObject->scale;
    if (ImGui::DragFloat3("Scale", &scale.x, 0.01f)) {
        const Result result =
            editor_mutation::applyScale(*selectedGameObject, scale);
        if (result) {
            inspectorError_.clear();
        } else {
            inspectorError_ = result.error();
        }
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
    stopNativeFileDialog();
    frameStarted_ = false;
    drawDataReady_ = false;
    physicsDiagnosticsObject_ = nullptr;
    physicsDiagnostics_.reset();
    physicsDiagnosticsError_.clear();
    sceneInteractionAreaHovered_ = false;
    sceneInteractionRect_ = {};
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
