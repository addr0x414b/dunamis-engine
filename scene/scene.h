#ifndef SCENE_H
#define SCENE_H

#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "../core/result.h"
#include "camera.h"
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
    [[nodiscard]] Result validateForActivation() const;

    [[nodiscard]] const std::vector<std::unique_ptr<GameObject>>&
    gameObjects() const noexcept;
    [[nodiscard]] std::size_t pointLightCount() const noexcept;
    [[nodiscard]] const PointLight& pointLightAt(std::size_t index) const;
    [[nodiscard]] const Camera* activeCamera() const noexcept;
    [[nodiscard]] bool isActive() const noexcept;

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
    std::shared_ptr<Camera> activeCamera_;
    bool active_ = false;
};

#endif
