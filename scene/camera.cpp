#include "camera.h"

#include <algorithm>
#include <cmath>

namespace {

constexpr float minimumVectorLengthSquared = 1.0e-8f;
constexpr double degreesToRadians =
    0.017453292519943295769236907684886;
constexpr double radiansToDegrees =
    57.295779513082320876798154814105;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool isFiniteMatrix(const glm::mat3& value) noexcept {
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            if (!std::isfinite(value[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool normalizeFinite(const glm::vec3& value,
                     glm::vec3& normalized) noexcept {
    normalized = {};
    if (!isFiniteVector(value)) {
        return false;
    }

    const float lengthSquared = glm::dot(value, value);
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= minimumVectorLengthSquared) {
        return false;
    }

    const float length = std::sqrt(lengthSquared);
    if (!std::isfinite(length) || length <= 0.0f) {
        return false;
    }

    normalized = value / length;
    return isFiniteVector(normalized);
}

bool hasValidBasis(const glm::vec3& front,
                   const glm::vec3& up) noexcept {
    const glm::vec3 cross = glm::cross(front, up);
    const float crossLengthSquared = glm::dot(cross, cross);
    return isFiniteVector(cross) && std::isfinite(crossLengthSquared) &&
           crossLengthSquared > minimumVectorLengthSquared;
}

}  // namespace

Camera::Camera() {
    name = "Camera";
}

bool Camera::deriveYawPitchDegrees(double& yawDegrees,
                                   double& pitchDegrees) const noexcept {
    glm::vec3 normalizedFront;
    if (!normalizeFinite(front, normalizedFront)) {
        return false;
    }

    const double clampedY = std::clamp(
        static_cast<double>(normalizedFront.y), -1.0, 1.0);
    const double derivedPitch =
        std::asin(clampedY) * radiansToDegrees;
    const double derivedYaw =
        std::atan2(static_cast<double>(normalizedFront.z),
                   static_cast<double>(normalizedFront.x)) *
        radiansToDegrees;
    if (!std::isfinite(derivedYaw) || !std::isfinite(derivedPitch)) {
        return false;
    }

    yawDegrees = derivedYaw;
    pitchDegrees = derivedPitch;
    return true;
}

bool Camera::setYawPitchDegrees(double yawDegrees, double pitchDegrees,
                                const glm::vec3& referenceUp) noexcept {
    if (!std::isfinite(yawDegrees) || !std::isfinite(pitchDegrees)) {
        return false;
    }

    const double yawRadians = yawDegrees * degreesToRadians;
    const double pitchRadians = pitchDegrees * degreesToRadians;
    const double cosPitch = std::cos(pitchRadians);
    const glm::vec3 requestedForward{
        static_cast<float>(std::cos(yawRadians) * cosPitch),
        static_cast<float>(std::sin(pitchRadians)),
        static_cast<float>(std::sin(yawRadians) * cosPitch)};

    glm::vec3 normalizedForward;
    glm::vec3 normalizedUp;
    if (!normalizeFinite(requestedForward, normalizedForward) ||
        !normalizeFinite(referenceUp, normalizedUp) ||
        !hasValidBasis(normalizedForward, normalizedUp)) {
        return false;
    }

    front = normalizedForward;
    up = normalizedUp;
    return true;
}

bool Camera::applyOrientationDelta(const glm::mat3& rotationDelta) noexcept {
    if (!isFiniteMatrix(rotationDelta)) {
        return false;
    }

    const glm::vec3 rotatedFront = rotationDelta * front;
    const glm::vec3 rotatedUp = rotationDelta * up;
    glm::vec3 normalizedFront;
    glm::vec3 normalizedUp;
    if (!normalizeFinite(rotatedFront, normalizedFront) ||
        !normalizeFinite(rotatedUp, normalizedUp) ||
        !hasValidBasis(normalizedFront, normalizedUp)) {
        return false;
    }

    front = normalizedFront;
    up = normalizedUp;
    return true;
}
