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
    // Clipping distances are world meters: 0.1 m (10 cm) near and
    // 10,000 m (10 km) far by default.
    static constexpr float defaultNearClipMeters = 0.1f;
    static constexpr float defaultFarClipMeters = 10000.0f;

    Camera();

    [[nodiscard]] bool calculateWorldPose(
        CameraWorldPose& pose) const noexcept;

    // Vertical field of view in degrees.
    [[nodiscard]] float fov() const noexcept;
    [[nodiscard]] bool setFov(float fovDegrees) noexcept;

    [[nodiscard]] float nearClip() const noexcept;
    [[nodiscard]] float farClip() const noexcept;
    [[nodiscard]] bool setNearClip(float value) noexcept;
    [[nodiscard]] bool setFarClip(float value) noexcept;
    [[nodiscard]] bool setClipPlanes(float nearValue,
                                     float farValue) noexcept;

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
    float nearClipMeters_ = defaultNearClipMeters;
    float farClipMeters_ = defaultFarClipMeters;
};

#endif
