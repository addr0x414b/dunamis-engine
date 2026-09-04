#ifndef IMGUI_LAYER_H
#define IMGUI_LAYER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>

#include "../core/result.h"
#include "../editor/editor_state.h"
#include "../editor/editor_transform.h"
#include "../editor/runtime_transform_edit.h"
#include "../physics/shape_diagnostics.h"

class Scene;
class GameObject;
class Camera;
class PointLight;
class DirectionalLight;
class Character;
class EditorSession;
struct NativeFileDialogState;

class ImGuiLayer final {
public:
    ImGuiLayer();
    ~ImGuiLayer() noexcept;

    ImGuiLayer(const ImGuiLayer&) = delete;
    ImGuiLayer& operator=(const ImGuiLayer&) = delete;

    [[nodiscard]] Result initialize(
        SDL_Window* window, VkInstance instance,
        VkPhysicalDevice physicalDevice, VkDevice device,
        std::uint32_t graphicsQueueFamily, VkQueue graphicsQueue,
        VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
        std::uint32_t minimumImageCount, std::uint32_t imageCount,
        EditorSession& editorSession);

    void processEvent(const SDL_Event& event) noexcept;
    void setInputEnabled(bool enabled) noexcept;

    [[nodiscard]] Result beginFrame(SceneRunState runState);
    void drawEditor(Scene* scene, const glm::mat4& view,
                    const glm::mat4& projection, SceneRunState runState);
    void finishFrame();
    void recordDrawData(VkCommandBuffer commandBuffer);
    [[nodiscard]] const GameObject*
    selectedGameObjectForScene(const Scene* scene) const noexcept;
    void clearSelection() noexcept;
    [[nodiscard]] bool sceneInteractionAreaHovered() const noexcept;
    void setCurrentScenePath(const std::string& path);
    void setEditorError(std::string error);
    void requestLoadConfirmation();
    void requestSaveAsOverwriteConfirmation(const std::string& path);
    void requestQuitConfirmation();
    void setPhysicsDiagnostics(
        const GameObject* object,
        std::optional<physics::ShapeDiagnostics> diagnostics,
        std::string error);

    [[nodiscard]] Result onSwapchainRecreated(
        VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
        std::uint32_t minimumImageCount, std::uint32_t imageCount);

    [[nodiscard]] bool initialized() const noexcept;

    void shutdown() noexcept;
    void abandon() noexcept;

private:
    [[nodiscard]] Result initializeVulkanBackend();
    [[nodiscard]] bool editorActionPending() const noexcept;
    void submitEditorAction(EditorCommand command);
    void submitEditorAction(EditorAction action);
    void submitRuntimeTransformEdit(
        const RuntimeTransformEdit& edit) noexcept;
    void finishRuntimeTransformDrag() noexcept;
    void cancelEditorTransformDrag() noexcept;
    void clearEditorTransformDrag() noexcept;
    void synchronizeSelection(Scene* scene);
    void selectGameObject(Scene* scene, GameObject* object,
                          SelectionOperation operation);
    void drawToolbar(SceneRunState runState);
    void drawPersistenceDialogs();
    [[nodiscard]] bool requestNativeFileDialog(bool saveAs);
    void consumeNativeFileDialogResult();
    [[nodiscard]] bool nativeFileDialogBusy() const;
    void stopNativeFileDialog() noexcept;
    void drawSceneHierarchy(Scene* scene, bool disabled);
    void drawSceneHierarchyNode(Scene* scene, GameObject* object);
    void drawInspector(Scene* scene, bool disabled);
    void updateSceneInteractionAreaHovered() noexcept;
    void processGizmoShortcuts(Scene* scene,
                               SceneRunState runState) noexcept;
    void drawTransformGizmo(Scene* scene, const glm::mat4& view,
                            const glm::mat4& projection,
                            SceneRunState runState);
    void drawCameraVisualizations(Scene* scene, const glm::mat4& editorView,
                                  const glm::mat4& projection,
                                  SceneRunState runState);
    void drawCameraVisualization(const Camera& camera,
                                 const glm::mat4& editorView,
                                 const glm::mat4& projection,
                                 const GameObject* selectionTarget,
                                 bool active);
    void drawPointLightVisualization(
        const PointLight& light, const GameObject* selectionTarget,
        const glm::mat4& editorView, const glm::mat4& projection);
    void drawDirectionalLightVisualization(
        const DirectionalLight& light, const GameObject* selectionTarget,
        const glm::mat4& editorView, const glm::mat4& projection);
    void drawCharacterVisualization(
        const Character& character, const GameObject* selectionTarget,
        const glm::mat4& editorView, const glm::mat4& projection);
    void processWorldSelection(Scene* scene, const glm::mat4& view,
                               const glm::mat4& projection,
                               SceneRunState runState);

    enum class EditorHelperKind {
        Camera,
        PointLight,
        DirectionalLight,
        Character,
    };

    struct EditorHelperSegment {
        glm::vec2 start{0.0f};
        glm::vec2 end{0.0f};
    };

    struct EditorHelperGeometry {
        EditorHelperKind kind = EditorHelperKind::Camera;
        const GameObject* selectionTarget = nullptr;
        glm::vec2 point{0.0f};
        bool pointValid = false;
        std::array<EditorHelperSegment, 128> segments{};
        std::size_t segmentCount = 0;
        float viewDepth = 0.0f;
    };

    struct SceneInteractionRect {
        float x = 0.0f;
        float y = 0.0f;
        float width = 0.0f;
        float height = 0.0f;
        bool valid = false;
    };

    struct HierarchyReorderRequest {
        Scene* scene = nullptr;
        GameObject* object = nullptr;
        GameObject* expectedParent = nullptr;
        std::size_t siblingIndex = 0;
    };

    VkInstance instance_ = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    std::uint32_t graphicsQueueFamily_ = 0;
    VkQueue graphicsQueue_ = VK_NULL_HANDLE;
    VkRenderPass renderPass_ = VK_NULL_HANDLE;
    VkSampleCountFlagBits msaaSamples_ = VK_SAMPLE_COUNT_1_BIT;
    std::uint32_t minimumImageCount_ = 0;
    std::uint32_t imageCount_ = 0;
    SDL_Window* window_ = nullptr;

    bool contextCreated_ = false;
    bool sdlBackendInitialized_ = false;
    bool vulkanBackendInitialized_ = false;
    bool frameStarted_ = false;
    bool drawDataReady_ = false;
    bool inputEnabled_ = true;
    bool gizmoDragActive_ = false;
    bool editorTransformDragActive_ = false;
    bool editorTransformDragFailed_ = false;
    editor_transform::TransformDragSnapshot editorTransformSnapshot_;
    glm::mat4 editorTransformGizmoMatrix_{1.0f};
    bool runtimeTransformDragActive_ = false;
    GameObject* runtimeTransformObject_ = nullptr;
    EditorSession* editorSession_ = nullptr;
    std::string inspectorError_;
    std::shared_ptr<NativeFileDialogState> nativeFileDialogState_;
    bool sceneInteractionAreaHovered_ = false;
    SceneInteractionRect sceneInteractionRect_;
    std::optional<HierarchyReorderRequest> pendingHierarchyReorder_;
    const GameObject* physicsDiagnosticsObject_ = nullptr;
    std::optional<physics::ShapeDiagnostics> physicsDiagnostics_;
    std::string physicsDiagnosticsError_;
    std::vector<EditorHelperGeometry> editorHelperGeometry_;
    std::string currentScenePath_;
    std::string saveAsOverwritePath_;
    bool openSaveAsOverwritePopup_ = false;
    bool openLoadConfirmationPopup_ = false;
    bool openQuitConfirmationPopup_ = false;
};

#endif
