#ifndef VISUAL_SERVER_H
#define VISUAL_SERVER_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "spdlog/spdlog.h"
#include "../core/editor_state.h"
#include "../core/result.h"
#include "../scene/camera.h"
#include "../scene/scene.h"

#include "vulkan_context.h"

class VisualServer {
public:
    ~VisualServer() noexcept;

    [[nodiscard]] Result initialize(SDL_Window* window, Scene* scene);
    [[nodiscard]] Result run(Scene* scene, const Camera& renderCamera,
                             SceneRunState runState);
    void processEvent(const SDL_Event& event) noexcept;
    void setImGuiInputEnabled(bool enabled) noexcept;
    void clearEditorSelection() noexcept;
    [[nodiscard]] EditorCommand consumeEditorCommand() noexcept;
    [[nodiscard]] bool sceneInteractionAreaHovered() const noexcept;
    void setCurrentScenePath(const std::string& path);
    [[nodiscard]] std::string requestedScenePath() const;
    void requestLoadConfirmation();
    void requestQuitConfirmation();
    void setPersistenceStatus(std::string status, bool error);
    [[nodiscard]] Result loadSceneResources(Scene* scene);
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
