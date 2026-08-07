#ifndef IMGUI_LAYER_H
#define IMGUI_LAYER_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <glm/vec2.hpp>
#include <glm/mat4x4.hpp>
#include <vulkan/vulkan.h>

#include "../core/result.h"
#include "../core/editor_state.h"

class Scene;
class GameObject;
class Camera;
class PointLight;
class DirectionalLight;

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

    [[nodiscard]] Result beginFrame(SceneRunState runState);
    void drawEditor(Scene* scene, const glm::mat4& view,
                    const glm::mat4& projection, SceneRunState runState);
    void finishFrame();
    void recordDrawData(VkCommandBuffer commandBuffer);
    [[nodiscard]] const GameObject*
    selectedGameObjectForScene(const Scene* scene) const noexcept;
    void clearSelection() noexcept;
    [[nodiscard]] EditorCommand consumeEditorCommand() noexcept;
    [[nodiscard]] bool sceneInteractionAreaHovered() const noexcept;

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
    void processWorldSelection(Scene* scene, const glm::mat4& view,
                               const glm::mat4& projection,
                               SceneRunState runState);

    enum class EditorHelperKind {
        Camera,
        PointLight,
        DirectionalLight,
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
        std::array<EditorHelperSegment, 12> segments{};
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

    bool contextCreated_ = false;
    bool sdlBackendInitialized_ = false;
    bool vulkanBackendInitialized_ = false;
    bool frameStarted_ = false;
    bool drawDataReady_ = false;
    bool inputEnabled_ = true;
    bool gizmoDragActive_ = false;
    GizmoMode gizmoMode_ = GizmoMode::Translate;
    Scene* selectionScene_ = nullptr;
    GameObject* selectedGameObject_ = nullptr;
    std::string inspectorError_;
    EditorCommand pendingEditorCommand_ = EditorCommand::None;
    bool sceneInteractionAreaHovered_ = false;
    SceneInteractionRect sceneInteractionRect_;
    std::vector<EditorHelperGeometry> editorHelperGeometry_;
};

#endif
