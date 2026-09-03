#ifndef CAMERA_H
#define CAMERA_H

#include "game_object.h"

struct CameraWorldPose {
    glm::vec3 position{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

class Camera : public GameObject {
public:
    Camera();

    [[nodiscard]] bool calculateWorldPose(
        CameraWorldPose& pose) const noexcept;

    // Vertical field of view in degrees.
    [[nodiscard]] float fov() const noexcept;
    [[nodiscard]] bool setFov(float fovDegrees) noexcept;

    [[nodiscard]] bool deriveYawPitchDegrees(
        double& yawDegrees, double& pitchDegrees) const noexcept;
    [[nodiscard]] bool setYawPitchDegrees(
        double yawDegrees, double pitchDegrees,
        const glm::vec3& referenceUp) noexcept;
    [[nodiscard]] bool applyOrientationDelta(
        const glm::mat3& rotationDelta) noexcept;

    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

private:
    float fovDegrees_ = 60.0f;
};

#endif
