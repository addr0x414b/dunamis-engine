#include "directional_light.h"

#include <cmath>

namespace {

constexpr float minDirectionLengthSquared = 1.0e-8f;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool isFiniteMatrix(const glm::mat4& value) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(value[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool isValidDirection(const glm::vec3& value) noexcept {
    if (!isFiniteVector(value)) {
        return false;
    }

    const float lengthSquared = glm::dot(value, value);
    return std::isfinite(lengthSquared) &&
           lengthSquared > minDirectionLengthSquared;
}

}  // namespace

DirectionalLight::DirectionalLight() {
    name = "DirectionalLight";
}

bool DirectionalLight::calculateWorldDirection(
    glm::vec3& worldDirection) const noexcept {
    worldDirection = {};
    const glm::mat4 worldTransform = worldTransformMatrix();
    if (!isFiniteMatrix(worldTransform)) {
        return false;
    }

    const glm::vec3 transformedDirection = glm::vec3(
        worldTransform * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f));
    if (!isValidDirection(transformedDirection)) {
        return false;
    }

    const float length = std::sqrt(glm::dot(transformedDirection,
                                            transformedDirection));
    if (!std::isfinite(length) || length <= 0.0f) {
        return false;
    }

    const glm::vec3 normalizedDirection = transformedDirection / length;
    if (!isFiniteVector(normalizedDirection)) {
        return false;
    }

    worldDirection = normalizedDirection;
    return true;
}
