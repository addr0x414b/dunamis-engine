#ifndef PLAYER_H
#define PLAYER_H

#include <glm/glm.hpp>
#include <memory>

#include "../input/input_manager.h"
#include "../scene/camera.h"
#include "../scene/character.h"

class PlayerTestAccess;

class Player : public Character {
public:
    static constexpr float eyeHeightMeters = 1.5f;
    static constexpr float walkSpeedMetersPerSecond = 4.0f;
    static constexpr float sprintSpeedMetersPerSecond = 7.0f;

    void init();
    void start(std::shared_ptr<InputManager> input);
    void update(std::shared_ptr<InputManager> input);
    void onPhysicsTransformResolved() noexcept override;
    Camera* attachedCamera() noexcept override { return camera.get(); }
    const Camera* attachedCamera() const noexcept override {
        return camera.get();
    }
    std::shared_ptr<Camera> camera;

private:
    friend class PlayerTestAccess;

    void synchronizeCameraPosition() noexcept;

    double yaw = -90.0f;
    double pitch = 0.0f;
};

#endif
