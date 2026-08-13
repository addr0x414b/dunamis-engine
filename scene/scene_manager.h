#ifndef SCENE_MANAGER_H
#define SCENE_MANAGER_H

#include <functional>
#include <filesystem>
#include <memory>
#include <string>
#include <type_traits>
#include <utility>

#include "../core/result.h"
#include "scene.h"
#include "scene_serializer.h"
#include "type_registry.h"

class InputManager;

class SceneManager {
public:
    SceneManager();

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
        if constexpr (HasTypeRegistration<SceneType>::value) {
            Result registration = SceneType::registerTypes(typeRegistry_);
            if (!registration) {
                return Result::failure("Game type registration failed: " +
                                       registration.error());
            }
        }
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

    [[nodiscard]] Result saveEditingScene(const Camera& editorCamera);
    [[nodiscard]] Result prepareEditingSceneLoad(
        const std::filesystem::path& path);
    [[nodiscard]] Scene* preparedEditingScene() noexcept;
    [[nodiscard]] const std::optional<EditorCameraState>&
    preparedEditorCamera() const noexcept;
    [[nodiscard]] Result commitPreparedEditingSceneLoad();
    void cancelPreparedEditingSceneLoad() noexcept;
    [[nodiscard]] Scene* previousEditingScene() noexcept;
    void finishEditingSceneLoad() noexcept;
    [[nodiscard]] bool hasUnsavedChanges() const;
    [[nodiscard]] Result captureCurrentAuthoredBaseline();
    void setCurrentScenePath(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& currentScenePath() const noexcept;
    [[nodiscard]] const std::vector<std::string>& persistenceWarnings() const noexcept;
    [[nodiscard]] TypeRegistry& typeRegistry() noexcept;
    [[nodiscard]] const TypeRegistry& typeRegistry() const noexcept;

    [[nodiscard]] bool isRuntimeSceneActive() const noexcept;
    [[nodiscard]] bool initialized() const noexcept;
    void shutdown() noexcept;

private:
    template <typename T, typename = void>
    struct HasTypeRegistration : std::false_type {};
    template <typename T>
    struct HasTypeRegistration<T, std::void_t<decltype(
        T::registerTypes(std::declval<TypeRegistry&>()))>> : std::true_type {};

    using SceneConstructor = std::function<std::unique_ptr<Scene>()>;

    [[nodiscard]] Result initializeEditingScene();
    [[nodiscard]] Result constructInitializedScene(
        std::unique_ptr<Scene>& scene) const;

    SceneConstructor sceneConstructor_;
    std::unique_ptr<Scene> editingScene_;
    std::unique_ptr<Scene> runtimeScene_;
    std::unique_ptr<Scene> preparedEditingScene_;
    std::unique_ptr<Scene> previousEditingScene_;
    Scene* activeScene_ = nullptr;
    std::shared_ptr<InputManager> inputManager_;
    std::string sceneName_;
    TypeRegistry typeRegistry_;
    Result registryInitialization_ = Result::success();
    nlohmann::json authoredBaseline_;
    bool authoredBaselineAvailable_ = false;
    std::filesystem::path currentScenePath_;
    std::filesystem::path preparedScenePath_;
    SceneLoadData preparedLoadData_;
    std::vector<std::string> persistenceWarnings_;
};

#endif
