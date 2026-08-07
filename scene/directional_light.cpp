#include "directional_light.h"

#include <cmath>

namespace {

constexpr float minDirectionLengthSquared = 1.0e-8f;

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

bool DirectionalLight::calculateDirectionAfterDelta(
    const glm::mat3& rotationDelta, glm::vec3& updatedDirection) const noexcept {
    updatedDirection = {};
    if (!isFiniteMatrix(rotationDelta) || !isValidDirection(direction)) {
        return false;
    }

    const float determinant = glm::determinant(rotationDelta);
    if (!std::isfinite(determinant) || std::abs(determinant) <= 1.0e-6f) {
        return false;
    }

    const glm::vec3 rotatedDirection = rotationDelta * direction;
    if (!isValidDirection(rotatedDirection)) {
        return false;
    }

    updatedDirection = rotatedDirection;
    return true;
}

bool DirectionalLight::applyDirectionDelta(
    const glm::mat3& rotationDelta) noexcept {
    glm::vec3 updatedDirection;
    if (!calculateDirectionAfterDelta(rotationDelta, updatedDirection)) {
        return false;
    }

    direction = updatedDirection;
    return true;
}
