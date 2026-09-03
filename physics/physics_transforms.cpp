#include "physics_transforms.h"

#include "../math/transform_math.h"
#include "../scene/character.h"
#include "../scene/game_object.h"

#include <glm/gtc/matrix_inverse.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <unordered_set>
#include <vector>

namespace physics {
namespace {

constexpr float identityScaleTolerance = 1.0e-4f;
constexpr float rigidTransformTolerance = 2.0e-3f;

std::string objectDescription(const GameObject& object) {
    return "object '" + object.name + "' (persistentId '" +
           object.persistentId + "')";
}

bool isIdentityScale(const glm::vec3& scale) noexcept {
    return transform_math::isFiniteVector(scale) &&
           std::fabs(scale.x - 1.0f) <= identityScaleTolerance &&
           std::fabs(scale.y - 1.0f) <= identityScaleTolerance &&
           std::fabs(scale.z - 1.0f) <= identityScaleTolerance;
}

bool isRigidTransform(const glm::mat4& matrix) noexcept {
    if (!transform_math::isFiniteMatrix(matrix)) return false;

    if (std::fabs(matrix[0][3]) > rigidTransformTolerance ||
        std::fabs(matrix[1][3]) > rigidTransformTolerance ||
        std::fabs(matrix[2][3]) > rigidTransformTolerance ||
        std::fabs(matrix[3][3] - 1.0f) > rigidTransformTolerance) {
        return false;
    }

    const glm::mat3 linear(matrix);
    for (int axis = 0; axis < 3; ++axis) {
        const float lengthSquared = glm::dot(linear[axis], linear[axis]);
        if (!std::isfinite(lengthSquared) ||
            std::fabs(lengthSquared - 1.0f) > rigidTransformTolerance) {
            return false;
        }
        for (int other = axis + 1; other < 3; ++other) {
            const float dot = glm::dot(linear[axis], linear[other]);
            if (!std::isfinite(dot) || std::fabs(dot) > rigidTransformTolerance) {
                return false;
            }
        }
    }

    const float determinant = glm::determinant(linear);
    return std::isfinite(determinant) && determinant > 0.0f &&
           std::fabs(determinant - 1.0f) <= 3.0f * rigidTransformTolerance;
}

Result validateObjectTransform(const GameObject& object,
                               const glm::vec3& localPosition,
                               const glm::vec3& localRotation) {
    if (!transform_math::isFiniteVector(localPosition) ||
        !transform_math::isFiniteVector(localRotation) ||
        !transform_math::isFiniteVector(object.scale)) {
        return Result::failure("Physics transform data is not finite for " +
                               objectDescription(object));
    }
    const glm::mat4 localRigid = transform_math::makeModelMatrix(
        localPosition, localRotation, glm::vec3(1.0f));
    if (!isRigidTransform(localRigid)) {
        return Result::failure(
            "Physics rigid pose is not representable for " +
            objectDescription(object));
    }
    return Result::success();
}

Result makeValidatedParentWorld(const GameObject& object,
                                glm::mat4& parentWorld) {
    parentWorld = glm::mat4(1.0f);
    std::vector<const GameObject*> ancestors;
    std::unordered_set<const GameObject*> visited;
    for (const GameObject* ancestor = object.parent(); ancestor != nullptr;
         ancestor = ancestor->parent()) {
        if (ancestor == &object || !visited.insert(ancestor).second) {
            return Result::failure(
                "Physics hierarchy contains a cycle while inspecting " +
                objectDescription(object));
        }
        if (!transform_math::isFiniteVector(ancestor->position) ||
            !transform_math::isFiniteVector(ancestor->rotation) ||
            !transform_math::isFiniteVector(ancestor->scale)) {
            return Result::failure(
                "Physics hierarchy ancestor " + objectDescription(*ancestor) +
                " has non-finite transform data");
        }
        if (dynamic_cast<const Character*>(ancestor) != nullptr) {
            return Result::failure(
                "Physics hierarchy ancestor " + objectDescription(*ancestor) +
                " is a Character physics participant; attached physics participant hierarchies are unsupported");
        }
        if (ancestor->physics.enabled) {
            return Result::failure(
                "Physics hierarchy ancestor " + objectDescription(*ancestor) +
                " has physics enabled; physics ancestor/descendant bodies are unsupported");
        }
        if (!isIdentityScale(ancestor->scale)) {
            return Result::failure(
                "Physics hierarchy ancestor " + objectDescription(*ancestor) +
                " has non-identity scale; scaled physics ancestry is unsupported");
        }
        ancestors.push_back(ancestor);
    }

    for (auto ancestor = ancestors.rbegin(); ancestor != ancestors.rend();
         ++ancestor) {
        const glm::mat4 localRigid = transform_math::makeModelMatrix(
            (*ancestor)->position, (*ancestor)->rotation,
            glm::vec3(1.0f));
        if (!isRigidTransform(localRigid)) {
            return Result::failure(
                "Physics hierarchy ancestor " + objectDescription(**ancestor) +
                " produces a non-representable rigid transform");
        }
        parentWorld *= localRigid;
        if (!isRigidTransform(parentWorld)) {
            return Result::failure(
                "Physics hierarchy for " + objectDescription(object) +
                " produces a non-representable world rigid transform");
        }
    }
    return Result::success();
}

Result makePhysicsWorldMatrix(const GameObject& object,
                              const glm::vec3& localPosition,
                              const glm::vec3& localRotation,
                              glm::mat4& worldMatrix) {
    worldMatrix = glm::mat4(1.0f);
    const Result objectResult =
        validateObjectTransform(object, localPosition, localRotation);
    if (!objectResult) return objectResult;

    glm::mat4 parentWorld;
    const Result parentResult = makeValidatedParentWorld(object, parentWorld);
    if (!parentResult) return parentResult;

    worldMatrix = parentWorld * transform_math::makeModelMatrix(
        localPosition, localRotation, glm::vec3(1.0f));
    if (!isRigidTransform(worldMatrix)) {
        return Result::failure(
            "Physics hierarchy for " + objectDescription(object) +
            " produces a non-representable world rigid transform");
    }
    return Result::success();
}

Result extractPhysicsWorldPose(const GameObject& object,
                               const glm::mat4& worldMatrix,
                               PhysicsWorldPose& pose) {
    pose = {};
    if (!isRigidTransform(worldMatrix)) {
        return Result::failure(
            "Physics world transform for " + objectDescription(object) +
            " is not a finite rigid transform");
    }

    transform_math::DecomposedTransform decomposition;
    const Result decompositionResult = transform_math::decomposeModelMatrix(
        worldMatrix, glm::vec3(1.0f), decomposition);
    if (!decompositionResult) {
        return Result::failure(
            "Physics world transform for " + objectDescription(object) +
            " cannot be represented as Dunamis rigid TRS: " +
            decompositionResult.error());
    }
    if (!isIdentityScale(decomposition.scale)) {
        return Result::failure(
            "Physics world transform for " + objectDescription(object) +
            " contains unsupported scale or shear");
    }
    pose.position = decomposition.position;
    pose.rotation = decomposition.rotation;
    if (!transform_math::isFiniteVector(pose.position) ||
        !transform_math::isFiniteVector(pose.rotation)) {
        return Result::failure(
            "Physics world pose for " + objectDescription(object) +
            " is not finite");
    }
    return Result::success();
}

Result calculateSafeInverse(const glm::mat4& matrix, glm::mat4& inverse) {
    inverse = glm::mat4(1.0f);
    if (!transform_math::isFiniteMatrix(matrix)) {
        return Result::failure("Physics parent world transform is not finite");
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
        return Result::failure(
            "Physics parent world transform is not safely invertible");
    }

    inverse = glm::inverse(matrix);
    if (!transform_math::isFiniteMatrix(inverse)) {
        return Result::failure(
            "Physics parent world transform inverse is not finite");
    }
    return Result::success();
}

Result deriveLocalPoseFromWorldMatrixImpl(const GameObject& object,
                                          const glm::mat4& worldMatrix,
                                          glm::vec3& localPosition,
                                          glm::vec3& localRotation) {
    localPosition = {};
    localRotation = {};
    if (!isRigidTransform(worldMatrix)) {
        return Result::failure(
            "Physics world transform for " + objectDescription(object) +
            " is not a finite rigid transform");
    }

    const Result objectResult = validateObjectTransform(
        object, object.position, object.rotation);
    if (!objectResult) return objectResult;
    glm::mat4 parentWorld;
    const Result parentResult = makeValidatedParentWorld(object, parentWorld);
    if (!parentResult) return parentResult;

    glm::mat4 inverseParent;
    const Result inverseResult = calculateSafeInverse(parentWorld, inverseParent);
    if (!inverseResult) return inverseResult;

    const glm::mat4 candidateLocal = inverseParent * worldMatrix;
    if (!isRigidTransform(candidateLocal)) {
        return Result::failure(
            "Physics world-to-local conversion for " +
            objectDescription(object) +
            " produced a non-representable local rigid transform");
    }

    transform_math::DecomposedTransform decomposition;
    const Result decompositionResult = transform_math::decomposeModelMatrix(
        candidateLocal, glm::vec3(1.0f), decomposition);
    if (!decompositionResult || !isIdentityScale(decomposition.scale)) {
        return Result::failure(
            "Physics world-to-local conversion for " +
            objectDescription(object) +
            " cannot be represented as local rigid TRS");
    }

    if (!transform_math::isFiniteVector(decomposition.position) ||
        !transform_math::isFiniteVector(decomposition.rotation)) {
        return Result::failure(
            "Physics world-to-local conversion produced non-finite local state");
    }
    localPosition = decomposition.position;
    localRotation = decomposition.rotation;
    return Result::success();
}

}  // namespace

Result validatePhysicsHierarchy(const GameObject& object) {
    glm::mat4 worldMatrix;
    const Result result = makePhysicsWorldMatrix(
        object, object.position, object.rotation, worldMatrix);
    if (!result) return result;
    PhysicsWorldPose pose;
    return extractPhysicsWorldPose(object, worldMatrix, pose);
}

Result derivePhysicsWorldPose(const GameObject& object,
                              PhysicsWorldPose& pose) {
    return derivePhysicsWorldPose(object, object.position, object.rotation,
                                  pose);
}

Result derivePhysicsWorldPose(const GameObject& object,
                              const glm::vec3& localPosition,
                              const glm::vec3& localRotation,
                              PhysicsWorldPose& pose) {
    pose = {};
    glm::mat4 worldMatrix;
    const Result result = makePhysicsWorldMatrix(
        object, localPosition, localRotation, worldMatrix);
    if (!result) return result;
    return extractPhysicsWorldPose(object, worldMatrix, pose);
}

Result deriveLocalPoseFromPhysicsWorld(const GameObject& object,
                                       const glm::vec3& worldPosition,
                                       const glm::vec3& worldRotation,
                                       glm::vec3& localPosition,
                                       glm::vec3& localRotation) {
    localPosition = {};
    localRotation = {};
    if (!transform_math::isFiniteVector(worldPosition) ||
        !transform_math::isFiniteVector(worldRotation)) {
        return Result::failure(
            "Physics world pose input must be finite");
    }
    const glm::mat4 worldMatrix = transform_math::makeModelMatrix(
        worldPosition, worldRotation, glm::vec3(1.0f));
    return deriveLocalPoseFromWorldMatrixImpl(
        object, worldMatrix, localPosition, localRotation);
}

Result deriveLocalPoseFromPhysicsWorld(const GameObject& object,
                                       const glm::mat4& worldMatrix,
                                       glm::vec3& localPosition,
                                       glm::vec3& localRotation) {
    return deriveLocalPoseFromWorldMatrixImpl(
        object, worldMatrix, localPosition, localRotation);
}

Result validateCharacterPhysicsHierarchy(const Character& character) {
    glm::vec3 worldPosition;
    return deriveCharacterWorldPosition(character, worldPosition);
}

Result deriveCharacterWorldPosition(const Character& character,
                                    glm::vec3& worldPosition) {
    worldPosition = {};
    glm::mat4 worldMatrix;
    const Result result = makePhysicsWorldMatrix(
        character, character.position, character.rotation, worldMatrix);
    if (!result) return result;

    worldPosition = glm::vec3(worldMatrix[3]);
    if (!transform_math::isFiniteVector(worldPosition)) {
        worldPosition = {};
        return Result::failure(
            "Character physics world position is not finite");
    }
    return Result::success();
}

Result deriveCharacterLocalPositionFromPhysicsWorld(
    const Character& character, const glm::vec3& worldPosition,
    glm::vec3& localPosition) {
    localPosition = {};
    if (!transform_math::isFiniteVector(worldPosition)) {
        return Result::failure(
            "Character physics world position input must be finite");
    }

    const Result objectResult = validateObjectTransform(
        character, character.position, character.rotation);
    if (!objectResult) return objectResult;

    glm::mat4 parentWorld;
    const Result parentResult = makeValidatedParentWorld(character, parentWorld);
    if (!parentResult) return parentResult;

    glm::mat4 inverseParent;
    const Result inverseResult = calculateSafeInverse(parentWorld, inverseParent);
    if (!inverseResult) return inverseResult;

    glm::vec4 candidateLocal = inverseParent * glm::vec4(worldPosition, 1.0f);
    if (!std::isfinite(candidateLocal.w) ||
        std::fabs(candidateLocal.w) <= std::numeric_limits<float>::epsilon()) {
        return Result::failure(
            "Character physics world-to-local conversion produced an invalid homogeneous position");
    }
    candidateLocal /= candidateLocal.w;
    localPosition = glm::vec3(candidateLocal);
    if (!transform_math::isFiniteVector(localPosition)) {
        localPosition = {};
        return Result::failure(
            "Character physics world-to-local conversion produced non-finite local state");
    }
    return Result::success();
}

}  // namespace physics
