#include "editor_mutation.h"

#include "../math/transform_math.h"
#include "../scene/camera.h"
#include "../scene/directional_light.h"
#include "../scene/game_object.h"
#include "../scene/point_light.h"

#include <cmath>

#include <glm/mat3x3.hpp>

namespace {

constexpr const char* invalidTransformMessage =
    "Transform values must be finite.";
constexpr const char* invalidColorMessage =
    "Color components must be finite and nonnegative.";
constexpr const char* invalidIntensityMessage =
    "Intensity must be finite and nonnegative.";

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

Result invalidTransform() {
    return Result::failure(invalidTransformMessage);
}

Result invalidColor() {
    return Result::failure(invalidColorMessage);
}

Result invalidIntensity() {
    return Result::failure(invalidIntensityMessage);
}

bool isFiniteNonnegativeVector(const glm::vec3& value) noexcept {
    return isFiniteVector(value) && value.x >= 0.0f && value.y >= 0.0f &&
           value.z >= 0.0f;
}

bool isFiniteNonnegative(float value) noexcept {
    return std::isfinite(value) && value >= 0.0f;
}

}  // namespace

namespace editor_mutation {

Result applyPosition(GameObject& object,
                     const glm::vec3& newPosition) {
    if (!isFiniteVector(object.position) || !isFiniteVector(newPosition)) {
        return invalidTransform();
    }

    const glm::vec3 delta = newPosition - object.position;
    if (!isFiniteVector(delta)) {
        return invalidTransform();
    }

    const bool isStandaloneCamera = dynamic_cast<Camera*>(&object) != nullptr;
    Camera* attachedCamera =
        isStandaloneCamera ? nullptr : object.attachedCamera();
    glm::vec3 newCameraPosition;
    if (attachedCamera != nullptr) {
        if (!isFiniteVector(attachedCamera->position)) {
            return invalidTransform();
        }
        newCameraPosition = attachedCamera->position + delta;
        if (!isFiniteVector(newCameraPosition)) {
            return invalidTransform();
        }
    }

    object.position = newPosition;
    if (attachedCamera != nullptr) {
        attachedCamera->position = newCameraPosition;
    }
    return Result::success();
}

Result applyRotation(GameObject& object,
                     const glm::vec3& newRotation) {
    if (!isFiniteVector(object.rotation) || !isFiniteVector(newRotation)) {
        return invalidTransform();
    }

    const glm::mat4 oldRotation =
        transform_math::makeRotationMatrix(object.rotation);
    const glm::mat4 updatedRotation =
        transform_math::makeRotationMatrix(newRotation);
    if (!isFiniteMatrix(oldRotation) || !isFiniteMatrix(updatedRotation)) {
        return invalidTransform();
    }

    const glm::mat4 deltaRotation =
        updatedRotation * glm::inverse(oldRotation);
    if (!isFiniteMatrix(deltaRotation)) {
        return invalidTransform();
    }

    Camera* standaloneCamera = dynamic_cast<Camera*>(&object);
    Camera* attachedCamera =
        standaloneCamera == nullptr ? object.attachedCamera() : nullptr;
    Camera* orientationCamera = standaloneCamera != nullptr
                                    ? standaloneCamera
                                    : attachedCamera;
    glm::vec3 newCameraPosition;
    if (orientationCamera != nullptr) {
        if (!isFiniteVector(orientationCamera->position)) {
            return invalidTransform();
        }

        if (attachedCamera != nullptr) {
            if (!isFiniteVector(object.position)) {
                return invalidTransform();
            }
            const glm::vec3 offset =
                attachedCamera->position - object.position;
            const glm::vec3 rotatedOffset = glm::vec3(
                deltaRotation * glm::vec4(offset, 0.0f));
            if (!isFiniteVector(offset) || !isFiniteVector(rotatedOffset)) {
                return invalidTransform();
            }
            newCameraPosition = object.position + rotatedOffset;
            if (!isFiniteVector(newCameraPosition)) {
                return invalidTransform();
            }
        }

        const glm::vec3 originalFront = orientationCamera->front;
        const glm::vec3 originalUp = orientationCamera->up;
        if (!orientationCamera->applyOrientationDelta(
                glm::mat3(deltaRotation))) {
            orientationCamera->front = originalFront;
            orientationCamera->up = originalUp;
            return invalidTransform();
        }
    }

    object.rotation = newRotation;
    if (attachedCamera != nullptr) {
        attachedCamera->position = newCameraPosition;
    }
    return Result::success();
}

Result applyScale(GameObject& object, const glm::vec3& newScale) {
    if (!isFiniteVector(newScale)) {
        return invalidTransform();
    }

    object.scale = newScale;
    return Result::success();
}

Result extractDunamisRotation(const glm::mat4& matrix,
                              const glm::vec3& authoredScale,
                              glm::vec3& rotation) {
    rotation = {};
    if (!isFiniteMatrix(matrix) || !isFiniteVector(authoredScale)) {
        return invalidTransform();
    }

    glm::vec3 basis[3];
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 column(matrix[axis]);
        const float lengthSquared = glm::dot(column, column);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
            return invalidTransform();
        }

        const float length = std::sqrt(lengthSquared);
        if (!std::isfinite(length) || length <= 0.0f) {
            return invalidTransform();
        }

        basis[axis] = column / length;
        if (authoredScale[axis] < 0.0f) {
            basis[axis] *= -1.0f;
        }
        if (!isFiniteVector(basis[axis])) {
            return invalidTransform();
        }
    }

    // Dunamis composes Rx * Ry * Rz. Extract the authored angles from the
    // normalized manipulated basis instead of using the widget convention.
    const float r00 = basis[0].x;
    const float r01 = basis[1].x;
    const float r02 = basis[2].x;
    const float r12 = basis[2].y;
    const float r22 = basis[2].z;
    const float yDenominator = std::sqrt(r00 * r00 + r01 * r01);
    if (!std::isfinite(yDenominator)) {
        return invalidTransform();
    }

    rotation.x = glm::degrees(std::atan2(-r12, r22));
    rotation.y = glm::degrees(std::atan2(r02, yDenominator));
    rotation.z = glm::degrees(std::atan2(-r01, r00));
    return isFiniteVector(rotation) ? Result::success() : invalidTransform();
}

Result extractDunamisScale(const glm::mat4& matrix,
                           const glm::vec3& authoredScale,
                           glm::vec3& scale) {
    scale = {};
    if (!isFiniteMatrix(matrix) || !isFiniteVector(authoredScale)) {
        return invalidTransform();
    }

    for (int axis = 0; axis < 3; ++axis) {
        const float lengthSquared = glm::dot(glm::vec3(matrix[axis]),
                                             glm::vec3(matrix[axis]));
        if (!std::isfinite(lengthSquared) || lengthSquared < 0.0f) {
            return invalidTransform();
        }

        const float magnitude = std::sqrt(lengthSquared);
        if (!std::isfinite(magnitude)) {
            return invalidTransform();
        }
        scale[axis] = authoredScale[axis] < 0.0f ? -magnitude : magnitude;
    }

    return isFiniteVector(scale) ? Result::success() : invalidTransform();
}

Result applyPointLightColor(PointLight& light, const glm::vec3& color) {
    if (!isFiniteNonnegativeVector(color)) {
        return invalidColor();
    }

    light.color = color;
    return Result::success();
}

Result applyPointLightIntensity(PointLight& light, float intensity) {
    if (!isFiniteNonnegative(intensity)) {
        return invalidIntensity();
    }

    light.intensity = intensity;
    return Result::success();
}

Result applyDirectionalLightColor(DirectionalLight& light,
                                  const glm::vec3& color) {
    if (!isFiniteNonnegativeVector(color)) {
        return invalidColor();
    }

    light.color = color;
    return Result::success();
}

Result applyDirectionalLightIntensity(DirectionalLight& light,
                                       float intensity) {
    if (!isFiniteNonnegative(intensity)) {
        return invalidIntensity();
    }

    light.intensity = intensity;
    return Result::success();
}

}  // namespace editor_mutation
