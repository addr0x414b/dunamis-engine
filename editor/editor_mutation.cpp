#include "editor_mutation.h"

#include "../math/transform_math.h"
#include "../scene/camera.h"
#include "../scene/directional_light.h"
#include "../scene/game_object.h"
#include "../scene/point_light.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/mat3x3.hpp>

namespace {

constexpr const char* invalidTransformMessage =
    "Transform values must be finite.";
constexpr const char* invalidColorMessage =
    "Color components must be finite and nonnegative.";
constexpr const char* invalidIntensityMessage =
    "Intensity must be finite and nonnegative.";
constexpr float minimumVectorLengthSquared = 1.0e-8f;

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

bool isFiniteVector(const glm::vec4& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
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

bool hasValidCameraBasis(const glm::vec3& front,
                         const glm::vec3& up) noexcept {
    const glm::vec3 cross = glm::cross(front, up);
    const float crossLengthSquared = glm::dot(cross, cross);
    return isFiniteVector(cross) && std::isfinite(crossLengthSquared) &&
           crossLengthSquared > minimumVectorLengthSquared;
}

bool normalizeCameraBasis(const glm::vec3& front, const glm::vec3& up,
                          glm::vec3& normalizedFront,
                          glm::vec3& normalizedUp) noexcept {
    normalizedFront = {};
    normalizedUp = {};
    if (!normalizeFinite(front, normalizedFront) ||
        !normalizeFinite(up, normalizedUp) ||
        !hasValidCameraBasis(normalizedFront, normalizedUp)) {
        return false;
    }
    return true;
}

bool makeWorldTransformForLocalState(
    const GameObject& object, const glm::vec3& localPosition,
    const glm::vec3& localRotation, const glm::vec3& localScale,
    glm::mat4& worldTransform) noexcept {
    worldTransform = glm::mat4(1.0f);
    if (!isFiniteVector(localPosition) || !isFiniteVector(localRotation) ||
        !isFiniteVector(localScale)) {
        return false;
    }

    const glm::mat4 localTransform = transform_math::makeModelMatrix(
        localPosition, localRotation, localScale);
    if (!isFiniteMatrix(localTransform)) {
        return false;
    }

    glm::mat4 parentWorld(1.0f);
    if (const GameObject* parent = object.parent()) {
        parentWorld = parent->worldTransformMatrix();
        if (!isFiniteMatrix(parentWorld)) {
            return false;
        }
    }

    worldTransform = parentWorld * localTransform;
    return isFiniteMatrix(worldTransform);
}

bool calculateFiniteInverse(const glm::mat4& matrix,
                            glm::mat4& inverse) noexcept {
    inverse = glm::mat4(1.0f);
    if (!isFiniteMatrix(matrix)) {
        return false;
    }

    const glm::mat3 linearPart(matrix);
    float maximumLinearMagnitude = 0.0f;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            maximumLinearMagnitude = std::max(
                maximumLinearMagnitude, std::fabs(linearPart[column][row]));
        }
    }
    const float determinant = glm::determinant(linearPart);
    const float determinantTolerance =
        128.0f * std::numeric_limits<float>::epsilon() *
        maximumLinearMagnitude * maximumLinearMagnitude *
        maximumLinearMagnitude;
    if (!std::isfinite(maximumLinearMagnitude) ||
        maximumLinearMagnitude <= 0.0f || !std::isfinite(determinant) ||
        std::fabs(determinant) <= determinantTolerance) {
        return false;
    }

    inverse = glm::inverse(matrix);
    return isFiniteMatrix(inverse);
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

Result applyName(GameObject& object, const std::string& newName) {
    object.name = newName;
    return Result::success();
}

Result applyPosition(GameObject& object,
                     const glm::vec3& newPosition) {
    if (!isFiniteVector(object.position) || !isFiniteVector(newPosition)) {
        return invalidTransform();
    }

    const bool isStandaloneCamera = dynamic_cast<Camera*>(&object) != nullptr;
    Camera* attachedCamera =
        isStandaloneCamera ? nullptr : object.attachedCamera();
    glm::vec3 newCameraPosition;
    if (attachedCamera != nullptr) {
        glm::mat4 currentOwnerWorld;
        glm::mat4 candidateOwnerWorld;
        if (!makeWorldTransformForLocalState(
                object, object.position, object.rotation, object.scale,
                currentOwnerWorld) ||
            !makeWorldTransformForLocalState(
                object, newPosition, object.rotation, object.scale,
                candidateOwnerWorld)) {
            return invalidTransform();
        }

        const glm::vec3 currentOwnerWorldPosition =
            glm::vec3(currentOwnerWorld[3]);
        const glm::vec3 candidateOwnerWorldPosition =
            glm::vec3(candidateOwnerWorld[3]);
        const glm::vec3 worldDelta =
            candidateOwnerWorldPosition - currentOwnerWorldPosition;
        if (!isFiniteVector(currentOwnerWorldPosition) ||
            !isFiniteVector(candidateOwnerWorldPosition) ||
            !isFiniteVector(worldDelta) ||
            !isFiniteVector(attachedCamera->position)) {
            return invalidTransform();
        }

        newCameraPosition = attachedCamera->position + worldDelta;
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

    Camera* standaloneCamera = dynamic_cast<Camera*>(&object);
    Camera* attachedCamera =
        standaloneCamera == nullptr ? object.attachedCamera() : nullptr;

    if (attachedCamera != nullptr) {
        if (!isFiniteVector(object.position) ||
            !isFiniteVector(object.scale) ||
            !isFiniteVector(attachedCamera->position)) {
            return invalidTransform();
        }

        glm::vec3 unusedNormalizedFront;
        glm::vec3 unusedNormalizedUp;
        if (!normalizeCameraBasis(attachedCamera->front,
                                  attachedCamera->up,
                                  unusedNormalizedFront,
                                  unusedNormalizedUp)) {
            return invalidTransform();
        }

        glm::mat4 oldOwnerWorld;
        glm::mat4 newOwnerWorld;
        if (!makeWorldTransformForLocalState(
                object, object.position, object.rotation, object.scale,
                oldOwnerWorld) ||
            !makeWorldTransformForLocalState(
                object, object.position, newRotation, object.scale,
                newOwnerWorld)) {
            return invalidTransform();
        }

        glm::mat4 inverseOldOwnerWorld;
        if (!calculateFiniteInverse(oldOwnerWorld, inverseOldOwnerWorld)) {
            return invalidTransform();
        }

        const glm::vec4 cameraRelative = inverseOldOwnerWorld *
            glm::vec4(attachedCamera->position, 1.0f);
        const glm::vec4 localFront = inverseOldOwnerWorld *
            glm::vec4(attachedCamera->front, 0.0f);
        const glm::vec4 localUp = inverseOldOwnerWorld *
            glm::vec4(attachedCamera->up, 0.0f);
        if (!isFiniteVector(cameraRelative) || !isFiniteVector(localFront) ||
            !isFiniteVector(localUp)) {
            return invalidTransform();
        }

        const glm::vec4 candidateCameraPosition4 =
            newOwnerWorld * cameraRelative;
        const glm::vec4 candidateFront4 = newOwnerWorld * localFront;
        const glm::vec4 candidateUp4 = newOwnerWorld * localUp;
        if (!isFiniteVector(candidateCameraPosition4) ||
            !isFiniteVector(candidateFront4) ||
            !isFiniteVector(candidateUp4)) {
            return invalidTransform();
        }

        const glm::vec3 candidateCameraPosition =
            glm::vec3(candidateCameraPosition4);
        glm::vec3 candidateFront;
        glm::vec3 candidateUp;
        if (!isFiniteVector(candidateCameraPosition) ||
            !normalizeCameraBasis(glm::vec3(candidateFront4),
                                  glm::vec3(candidateUp4), candidateFront,
                                  candidateUp)) {
            return invalidTransform();
        }

        object.rotation = newRotation;
        attachedCamera->position = candidateCameraPosition;
        attachedCamera->front = candidateFront;
        attachedCamera->up = candidateUp;
        return Result::success();
    }

    if (standaloneCamera != nullptr) {
        if (!isFiniteVector(standaloneCamera->position)) {
            return invalidTransform();
        }

        const glm::mat4 oldRotation =
            transform_math::makeRotationMatrix(object.rotation);
        const glm::mat4 updatedRotation =
            transform_math::makeRotationMatrix(newRotation);
        if (!isFiniteMatrix(oldRotation) ||
            !isFiniteMatrix(updatedRotation)) {
            return invalidTransform();
        }

        const glm::mat4 deltaRotation =
            updatedRotation * glm::inverse(oldRotation);
        if (!isFiniteMatrix(deltaRotation)) {
            return invalidTransform();
        }

        glm::vec3 candidateFront;
        glm::vec3 candidateUp;
        if (!normalizeCameraBasis(
                glm::mat3(deltaRotation) * standaloneCamera->front,
                glm::mat3(deltaRotation) * standaloneCamera->up,
                candidateFront, candidateUp)) {
            return invalidTransform();
        }

        object.rotation = newRotation;
        standaloneCamera->front = candidateFront;
        standaloneCamera->up = candidateUp;
        return Result::success();
    }

    object.rotation = newRotation;
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
    transform_math::DecomposedTransform decomposition;
    const Result result = transform_math::decomposeModelMatrix(
        matrix, authoredScale, decomposition);
    if (!result) return invalidTransform();
    rotation = decomposition.rotation;
    return Result::success();
}

Result extractDunamisScale(const glm::mat4& matrix,
                           const glm::vec3& authoredScale,
                           glm::vec3& scale) {
    const Result result = transform_math::extractScale(
        matrix, authoredScale, scale);
    return result ? Result::success() : invalidTransform();
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
