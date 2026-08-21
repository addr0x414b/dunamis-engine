#ifndef IMGUI_LAYER_H
#define IMGUI_LAYER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <unordered_set>

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>

#include "../core/result.h"
#include "../core/editor_state.h"
#include "../physics/shape_diagnostics.h"
#include "../scene/runtime_transform_edit.h"

class Scene;
class GameObject;
class Camera;
class PointLight;
class DirectionalLight;
class Character;
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
        std::uint32_t minimumImageCount, std::uint32_t imageCount);

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
    [[nodiscard]] EditorCommand consumeEditorCommand() noexcept;
    [[nodiscard]] std::optional<RuntimeTransformEdit>
    consumeRuntimeTransformEdit() noexcept;
    [[nodiscard]] bool sceneInteractionAreaHovered() const noexcept;
    void setCurrentScenePath(const std::string& path);
    [[nodiscard]] std::string requestedScenePath() const;
    [[nodiscard]] std::string requestedSaveAsPath() const;
    void requestLoadConfirmation();
    void requestSaveAsOverwriteConfirmation(const std::string& path);
    void requestQuitConfirmation();
    void setPhysicsDiagnostics(
        const GameObject* object,
        std::optional<physics::ShapeDiagnostics> diagnostics,
        std::string error);
    void setRenderColliderIds(const std::vector<std::string>& ids);
    [[nodiscard]] std::vector<std::string> renderColliderIds() const;
    [[nodiscard]] bool renderColliderEnabled(const GameObject& object) const noexcept;

    [[nodiscard]] Result onSwapchainRecreated(
        VkRenderPass renderPass, VkSampleCountFlagBits msaaSamples,
        std::uint32_t minimumImageCount, std::uint32_t imageCount);

    [[nodiscard]] bool initialized() const noexcept;

    void shutdown() noexcept;
    void abandon() noexcept;

private:
    enum class GizmoMode {
        Translate,
        Scale,
        Rotate,
    };

    [[nodiscard]] Result initializeVulkanBackend();
    void synchronizeSelection(Scene* scene) noexcept;
    void selectGameObject(Scene* scene, GameObject* object) noexcept;
    void drawToolbar(SceneRunState runState);
    void drawPersistenceDialogs();
    [[nodiscard]] bool requestNativeFileDialog(bool saveAs);
    void consumeNativeFileDialogResult();
    [[nodiscard]] bool nativeFileDialogBusy() const;
    void stopNativeFileDialog() noexcept;
    void drawSceneHierarchy(Scene* scene, bool disabled);
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
    bool runtimeTransformDragActive_ = false;
    GameObject* runtimeTransformObject_ = nullptr;
    std::optional<RuntimeTransformEdit> pendingRuntimeTransformEdit_;
    GizmoMode gizmoMode_ = GizmoMode::Translate;
    Scene* selectionScene_ = nullptr;
    GameObject* selectedGameObject_ = nullptr;
    std::string inspectorError_;
    EditorCommand pendingEditorCommand_ = EditorCommand::None;
    std::shared_ptr<NativeFileDialogState> nativeFileDialogState_;
    bool sceneInteractionAreaHovered_ = false;
    SceneInteractionRect sceneInteractionRect_;
    std::unordered_set<std::string> renderColliderIds_;
    const GameObject* physicsDiagnosticsObject_ = nullptr;
    std::optional<physics::ShapeDiagnostics> physicsDiagnostics_;
    std::string physicsDiagnosticsError_;
    std::vector<EditorHelperGeometry> editorHelperGeometry_;
    std::string currentScenePath_;
    std::string requestedScenePath_;
    std::string requestedSaveAsPath_;
    std::string saveAsOverwritePath_;
    bool openSaveAsOverwritePopup_ = false;
    bool openLoadConfirmationPopup_ = false;
    bool openQuitConfirmationPopup_ = false;
};

#endif
