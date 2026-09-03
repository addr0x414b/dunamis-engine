#include "editor_transform.h"

#include "../scene/game_object.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_inverse.hpp>

namespace editor_transform {
namespace {

[[nodiscard]] bool calculateSafeInverse(const glm::mat4& matrix,
                                        glm::mat4& inverse) {
    inverse = glm::mat4(1.0f);
    if (!transform_math::isFiniteMatrix(matrix)) {
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
    return transform_math::isFiniteMatrix(inverse);
}

}  // namespace

Result deriveLocalTransformFromWorld(
    const GameObject& object, const glm::mat4& candidateWorld,
    transform_math::DecomposedTransform& local) {
    local = {};
    if (!transform_math::isFiniteMatrix(candidateWorld)) {
        return Result::failure(
            "Gizmo candidate world transform must be finite.");
    }

    glm::mat4 candidateLocal = candidateWorld;
    if (const GameObject* parent = object.parent()) {
        const glm::mat4 parentWorld = parent->worldTransformMatrix();
        if (!transform_math::isFiniteMatrix(parentWorld)) {
            return Result::failure(
                "Gizmo parent world transform must be finite.");
        }

        glm::mat4 inverseParent;
        if (!calculateSafeInverse(parentWorld, inverseParent)) {
            return Result::failure(
                "Gizmo parent world transform is not safely invertible.");
        }
        candidateLocal = inverseParent * candidateWorld;
    }

    if (!transform_math::isFiniteMatrix(candidateLocal)) {
        return Result::failure(
            "Gizmo world transform produced a non-finite local transform.");
    }

    const Result decomposition =
        transform_math::decomposeModelMatrixAllowingZeroScale(
            candidateLocal, object.scale, local);
    if (!decomposition) {
        local = {};
        return Result::failure(
            "Gizmo world transform cannot be represented as local TRS: " +
            decomposition.error());
    }
    return Result::success();
}

}  // namespace editor_transform
