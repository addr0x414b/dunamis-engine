#ifndef SCENE_H
#define SCENE_H

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <glm/glm.hpp>

#include "../core/result.h"
#include "camera.h"
#include "directional_light.h"
#include "game_object.h"
#include "point_light.h"
#include "scene_limits.h"

class InputManager;
class VisualServer;
class SceneTestAccess;
namespace editor {
class EditorObjectCoordinator;
}

class Scene {
public:
    enum class ReparentMode { PreserveWorld, PreserveLocal };

    virtual void buildDefaults() = 0;
    virtual void start() = 0;
    virtual void update() = 0;
    virtual ~Scene() = default;

    [[nodiscard]] Result addGameObject(
        std::unique_ptr<GameObject> gameObject);
    [[nodiscard]] Result reparentGameObject(
        GameObject& child, GameObject* newParent, ReparentMode mode);
    // Reorders an object inside the sibling list belonging to expectedParent.
    // The expected parent is part of the request so callers cannot turn this
    // presentation-only operation into a reparenting operation.
    [[nodiscard]] Result reorderGameObject(
        GameObject& object, GameObject* expectedParent,
        std::size_t siblingIndex);
    [[nodiscard]] Result reorderGameObject(
        GameObject& object, std::size_t siblingIndex);
    [[nodiscard]] Result setActiveCamera(std::shared_ptr<Camera> camera);
    [[nodiscard]] Result setActiveCameraReference(Camera* camera);
    [[nodiscard]] Result validateForActivation() const;
    [[nodiscard]] Result validateAuthoredState() const;

    [[nodiscard]] const std::vector<std::unique_ptr<GameObject>>&
    gameObjects() const noexcept;
    [[nodiscard]] const std::vector<GameObject*>&
    rootObjects() const noexcept;
    [[nodiscard]] std::size_t pointLightCount() const noexcept;
    [[nodiscard]] const PointLight& pointLightAt(std::size_t index) const;
    [[nodiscard]] const DirectionalLight*
    directionalLight() const noexcept;
    [[nodiscard]] const Camera* activeCamera() const noexcept;
    [[nodiscard]] GameObject* findGameObject(const std::string& persistentId) noexcept;
    [[nodiscard]] const GameObject* findGameObject(
        const std::string& persistentId) const noexcept;
    [[nodiscard]] bool isActive() const noexcept;
    [[nodiscard]] Result setBackgroundColor(const glm::vec4& color);
    [[nodiscard]] Result setAmbientLight(const glm::vec3& color,
                                         float intensity);
    [[nodiscard]] const glm::vec4& backgroundColor() const noexcept;
    [[nodiscard]] const glm::vec3& ambientColor() const noexcept;
    [[nodiscard]] float ambientIntensity() const noexcept;

    std::string name;
    std::shared_ptr<InputManager> inputManager;

private:
    friend class VisualServer;
    friend class SceneTestAccess;
    friend class editor::EditorObjectCoordinator;

    [[nodiscard]] Result validateEditorGameObjectInsertion(
        const GameObject& gameObject) const;
    [[nodiscard]] Result addGameObjectForEditor(
        std::unique_ptr<GameObject> gameObject, GameObject*& inserted);
    [[nodiscard]] Result removeGameObjectForEditor(
        GameObject* gameObject, std::unique_ptr<GameObject>& removed) noexcept;
    [[nodiscard]] Result validateEditorGameObjectRemoval(
        GameObject* gameObject, bool requireNoChildren = true) const;
    [[nodiscard]] Result reserveEditorHierarchyStorage(
        const std::vector<std::pair<GameObject*, std::size_t>>& requirements);
    [[nodiscard]] Result validateGameObjectInsertion(
        const GameObject& gameObject) const;
    [[nodiscard]] Result addGameObjectInternal(
        std::unique_ptr<GameObject> gameObject, bool allowActive,
        GameObject*& inserted);
    [[nodiscard]] Result validateReparentGameObject(
        const GameObject& child, GameObject* newParent, ReparentMode mode,
        std::optional<std::size_t> destinationIndex) const;
    [[nodiscard]] Result reparentGameObjectAt(
        GameObject& child, GameObject* newParent, ReparentMode mode,
        std::size_t destinationIndex);
    [[nodiscard]] bool ownsGameObject(const GameObject* gameObject) const noexcept;

    [[nodiscard]] Result activate();
    void deactivate() noexcept;

    std::vector<std::unique_ptr<GameObject>> gameObjects_;
    // Hierarchy presentation order is deliberately separate from lifetime
    // ownership. Every pointer here is non-owning and is owned by
    // gameObjects_.
    std::vector<GameObject*> rootObjects_;
    std::array<std::size_t, scene_limits::maxPointLights>
        pointLightIndices_{};
    std::size_t pointLightCount_ = 0;
    std::optional<std::size_t> directionalLightIndex_;
    std::shared_ptr<Camera> activeCamera_;
    glm::vec4 backgroundColor_{
        0.639f, 0.965f, 1.0f, 1.0f};
    glm::vec3 ambientColor_{1.0f, 1.0f, 1.0f};
    float ambientIntensity_ = 0.1f;
    bool active_ = false;
};

using ReparentMode = Scene::ReparentMode;

#endif
