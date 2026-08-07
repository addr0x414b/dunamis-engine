#ifndef PLAYER_H
#define PLAYER_H

#include <glm/glm.hpp>
#include <memory>

#include "../input/input_manager.h"
#include "../scene/camera.h"
#include "../scene/game_object.h"

class PlayerTestAccess;

class Player : public GameObject {
public:
    void init();
    void start(std::shared_ptr<InputManager> input);
    void update(std::shared_ptr<InputManager> input);
    Camera* attachedCamera() noexcept override { return camera.get(); }
    const Camera* attachedCamera() const noexcept override {
        return camera.get();
    }
    std::shared_ptr<Camera> camera;

private:
    friend class PlayerTestAccess;

    void applyMovementDelta(const glm::vec3& delta) noexcept;

    float speed = 1.0f;
    double yaw = -90.0f;
    double pitch = 0.0f;
};

#endif
