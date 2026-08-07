#ifndef CAMERA_H
#define CAMERA_H

#include "game_object.h"

class Camera : public GameObject {
public:
    Camera();

    [[nodiscard]] bool deriveYawPitchDegrees(
        double& yawDegrees, double& pitchDegrees) const noexcept;
    [[nodiscard]] bool setYawPitchDegrees(
        double yawDegrees, double pitchDegrees,
        const glm::vec3& referenceUp) noexcept;
    [[nodiscard]] bool applyOrientationDelta(
        const glm::mat3& rotationDelta) noexcept;

    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);
};

#endif
