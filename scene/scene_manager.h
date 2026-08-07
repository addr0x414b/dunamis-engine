#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include <functional>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "../core/result.h"
#include "scene.h"

class InputManager;

class SceneManager {
public:
    template <typename SceneType>
    [[nodiscard]] Result initialize(
        std::string sceneName,
        std::shared_ptr<InputManager> inputManager) {
        static_assert(std::is_base_of_v<Scene, SceneType>,
                      "SceneType must derive from Scene");

        if (initialized()) {
            return Result::failure("Scene Manager is already initialized");
        }
        sceneConstructor_ = []() -> std::unique_ptr<Scene> {
            return std::make_unique<SceneType>();
        };
        sceneName_ = std::move(sceneName);
        inputManager_ = std::move(inputManager);

        Result result = initializeEditingScene();
        if (!result) {
            shutdown();
        }
        return result;
    }

    [[nodiscard]] Scene* editingScene() noexcept;
    [[nodiscard]] const Scene* editingScene() const noexcept;
    [[nodiscard]] Scene* runtimeScene() noexcept;
    [[nodiscard]] const Scene* runtimeScene() const noexcept;
    [[nodiscard]] Scene* activeScene() noexcept;
    [[nodiscard]] const Scene* activeScene() const noexcept;

    [[nodiscard]] Result prepareRuntimeScene();
    [[nodiscard]] Result commitRuntimeScene();
    void cancelPreparedRuntimeScene() noexcept;
    [[nodiscard]] Result returnToEditingScene();
    [[nodiscard]] Result destroyRuntimeScene();

    [[nodiscard]] bool isRuntimeSceneActive() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    void shutdown() noexcept;

private:
    using SceneConstructor = std::function<std::unique_ptr<Scene>()>;

    [[nodiscard]] Result initializeEditingScene();
    [[nodiscard]] Result constructInitializedScene(
        std::unique_ptr<Scene>& scene) const;

    SceneConstructor sceneConstructor_;
    std::unique_ptr<Scene> editingScene_;
    std::unique_ptr<Scene> runtimeScene_;
    Scene* activeScene_ = nullptr;
    std::shared_ptr<InputManager> inputManager_;
    std::string sceneName_;
};

#endif
