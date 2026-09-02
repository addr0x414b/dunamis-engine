#ifndef VISUAL_SERVER_H
#define VISUAL_SERVER_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "spdlog/spdlog.h"
#include "../editor/editor_state.h"
#include "../core/result.h"
#include "../scene/camera.h"
#include "../scene/scene.h"

#include "vulkan_context.h"

class PhysicsServer;
class EditorSession;

class VisualServer {
public:
    ~VisualServer() noexcept;

    [[nodiscard]] Result initialize(SDL_Window* window, Scene* scene,
                                    EditorSession& editorSession,
                                    PhysicsServer* physicsServer = nullptr);
    [[nodiscard]] Result run(Scene* scene, const Camera& renderCamera,
                             SceneRunState runState);
    void processEvent(const SDL_Event& event) noexcept;
    void setImGuiInputEnabled(bool enabled) noexcept;
    void clearEditorSelection() noexcept;
    [[nodiscard]] bool sceneInteractionAreaHovered() const noexcept;
    void setCurrentScenePath(const std::string& path);
    void setEditorError(std::string error);
    void requestLoadConfirmation();
    void requestSaveAsOverwriteConfirmation(const std::string& path);
    void requestQuitConfirmation();
    [[nodiscard]] Result loadSceneResources(Scene* scene);
    [[nodiscard]] Result attachGameObject(Scene* scene, GameObject* gameObject);
    [[nodiscard]] Result unloadSceneResources(Scene* scene);
    [[nodiscard]] Result switchScene(Scene* scene);
    [[nodiscard]] Scene* renderScene() const noexcept;
    bool shutdown() noexcept;

private:
    VulkanContext vulkanContext;
    [[nodiscard]] Result initGameObjects();
    Scene* currentScene = nullptr;
    bool initializationAttempted = false;
    bool initialized = false;

};

#endif
