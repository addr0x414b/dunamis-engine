#ifndef SCENE_H
#define SCENE_H

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
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

class Scene {
public:
    virtual void init() = 0;
    virtual void start() = 0;
    virtual void update() = 0;
    virtual ~Scene() = default;

    [[nodiscard]] Result addGameObject(
        std::unique_ptr<GameObject> gameObject);
    [[nodiscard]] Result setActiveCamera(std::shared_ptr<Camera> camera);
    [[nodiscard]] Result setActiveCameraReference(Camera* camera);
    [[nodiscard]] Result validateForActivation() const;
    [[nodiscard]] Result validateAuthoredState() const;
    // A future engine property/serialization system will allow arbitrary
    // custom game properties exposed in the editor to transfer to the
    // runtime scene. Runtime reset remains automatic because the disposable
    // runtime scene is destroyed on Stop.
    [[nodiscard]] Result copyAuthoringStateTo(Scene& destination) const;

    [[nodiscard]] const std::vector<std::unique_ptr<GameObject>>&
    gameObjects() const noexcept;
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

    [[nodiscard]] Result activate();
    void deactivate() noexcept;

    std::vector<std::unique_ptr<GameObject>> gameObjects_;
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

#endif
