#ifndef DUNAMIS_TRANSFORM_MATH_H
#define DUNAMIS_TRANSFORM_MATH_H

#include "../core/result.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cmath>

namespace transform_math {

struct DecomposedTransform {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
};

// Dunamis authored rotations are degrees. Keep the existing sequential GLM
// construction explicit: X, then Y, then Z.
[[nodiscard]] inline glm::mat4 makeRotationMatrix(
    const glm::vec3& rotationDegrees) noexcept {
    glm::mat4 result(1.0f);
    result = glm::rotate(result, glm::radians(rotationDegrees.x),
                         glm::vec3(1.0f, 0.0f, 0.0f));
    result = glm::rotate(result, glm::radians(rotationDegrees.y),
                         glm::vec3(0.0f, 1.0f, 0.0f));
    result = glm::rotate(result, glm::radians(rotationDegrees.z),
                         glm::vec3(0.0f, 0.0f, 1.0f));
    return result;
}

[[nodiscard]] inline glm::mat4 makeTranslationMatrix(
    const glm::vec3& position) noexcept {
    return glm::translate(glm::mat4(1.0f), position);
}

[[nodiscard]] inline glm::mat4 makeScaleMatrix(
    const glm::vec3& scale) noexcept {
    return glm::scale(glm::mat4(1.0f), scale);
}

[[nodiscard]] inline glm::mat4 makeModelMatrix(
    const glm::vec3& position, const glm::vec3& rotationDegrees,
    const glm::vec3& scale) noexcept {
    glm::mat4 model = makeTranslationMatrix(position);
    model *= makeRotationMatrix(rotationDegrees);
    model *= makeScaleMatrix(scale);
    return model;
}

[[nodiscard]] inline bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

[[nodiscard]] inline bool isFiniteMatrix(const glm::mat4& value) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(value[column][row])) return false;
        }
    }
    return true;
}

namespace detail {

[[nodiscard]] inline float maximumLinearMatrixMagnitude(
    const glm::mat4& matrix) noexcept {
    float maximum = 0.0f;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            maximum = std::max(maximum, std::fabs(matrix[column][row]));
        }
    }
    return maximum;
}

[[nodiscard]] inline Result finishDecomposition(
    const glm::mat4& matrix, const glm::mat3& rotationBasis,
    const glm::vec3& scale, DecomposedTransform& output) {
    output = {};
    output.position = glm::vec3(matrix[3]);
    output.scale = scale;

    // Dunamis composes Rx * Ry * Rz. Extract the authored angles from the
    // normalized basis instead of using a widget or quaternion convention.
    const float r00 = rotationBasis[0].x;
    const float r01 = rotationBasis[1].x;
    const float r02 = rotationBasis[2].x;
    const float r12 = rotationBasis[2].y;
    const float r22 = rotationBasis[2].z;
    const float yDenominator = std::sqrt(r00 * r00 + r01 * r01);
    if (!std::isfinite(yDenominator)) {
        return Result::failure("Transform matrix rotation is not finite");
    }

    output.rotation.x = glm::degrees(std::atan2(-r12, r22));
    output.rotation.y = glm::degrees(std::atan2(r02, yDenominator));
    output.rotation.z = glm::degrees(std::atan2(-r01, r00));
    if (!isFiniteVector(output.position) ||
        !isFiniteVector(output.rotation) || !isFiniteVector(output.scale)) {
        return Result::failure("Decomposed transform is not finite");
    }

    const glm::mat4 rebuilt = makeModelMatrix(
        output.position, output.rotation, output.scale);
    if (!isFiniteMatrix(rebuilt)) {
        return Result::failure("Rebuilt transform is not finite");
    }

    // A TRS-only GameObject cannot represent shear. The relative tolerance
    // allows normal float reconstruction noise while rejecting meaningful
    // differences between the candidate matrix and rebuilt Dunamis TRS.
    constexpr float reconstructionTolerance = 1.0e-4f;
    const float linearScale = maximumLinearMatrixMagnitude(matrix);
    if (!std::isfinite(linearScale)) {
        return Result::failure("Transform matrix has no usable linear part");
    }
    const float linearComparisonScale =
        linearScale > 0.0f ? linearScale : 1.0f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const float difference =
                std::fabs(matrix[column][row] - rebuilt[column][row]);
            const float comparisonScale =
                column < 3 && row < 3
                    ? linearComparisonScale
                    : std::max({1.0f, std::fabs(matrix[column][row]),
                                std::fabs(rebuilt[column][row])});
            if (difference > reconstructionTolerance * comparisonScale) {
                return Result::failure(
                    "Transform matrix contains non-representable shear");
            }
        }
    }

    return Result::success();
}

[[nodiscard]] inline Result decomposeModelMatrixWithSigns(
    const glm::mat4& matrix, const glm::vec3& scaleSignHint,
    DecomposedTransform& output) {
    output = {};
    if (!isFiniteMatrix(matrix) || !isFiniteVector(scaleSignHint)) {
        return Result::failure(
            "Transform matrix and scale hint must be finite");
    }

    glm::vec3 scale(0.0f);
    glm::mat3 rotationBasis(1.0f);
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 column(matrix[axis]);
        const float lengthSquared = glm::dot(column, column);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
            return Result::failure(
                "Transform matrix has an unusable scale axis");
        }

        const float magnitude = std::sqrt(lengthSquared);
        if (!std::isfinite(magnitude) || magnitude <= 0.0f) {
            return Result::failure(
                "Transform matrix has an unusable scale axis");
        }

        scale[axis] = scaleSignHint[axis] < 0.0f ? -magnitude : magnitude;
        rotationBasis[axis] = column / magnitude;
        if (scale[axis] < 0.0f) rotationBasis[axis] *= -1.0f;
    }

    return finishDecomposition(matrix, rotationBasis, scale, output);
}

[[nodiscard]] inline glm::vec3 perpendicularReference(
    const glm::vec3& vector) noexcept {
    const glm::vec3 absolute = glm::abs(vector);
    if (absolute.x <= absolute.y && absolute.x <= absolute.z) {
        return glm::vec3(1.0f, 0.0f, 0.0f);
    }
    if (absolute.y <= absolute.z) return glm::vec3(0.0f, 1.0f, 0.0f);
    return glm::vec3(0.0f, 0.0f, 1.0f);
}

}  // namespace detail

// The scale-sign overload preserves the authored sign convention used by the
// editor extraction APIs. It is intentionally strict: a matrix that needs a
// different sign pattern is not the hinted Dunamis TRS.
[[nodiscard]] inline Result decomposeModelMatrix(
    const glm::mat4& matrix, const glm::vec3& authoredScale,
    DecomposedTransform& output) {
    return detail::decomposeModelMatrixWithSigns(
        matrix, authoredScale, output);
}

// When no sign hint is available, choose a signed scale pattern that gives the
// normalized basis a proper rotation. This is sufficient for reparenting,
// where matrix equivalence matters more than preserving an ambiguous sign
// assignment across parents with reflections.
[[nodiscard]] inline Result decomposeModelMatrix(
    const glm::mat4& matrix, DecomposedTransform& output) {
    output = {};
    if (!isFiniteMatrix(matrix)) {
        return Result::failure("Transform matrix must be finite");
    }

    glm::mat3 normalizedBasis(1.0f);
    glm::vec3 magnitudes(0.0f);
    int nonzeroAxes = 0;
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 column(matrix[axis]);
        const float lengthSquared = glm::dot(column, column);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
            magnitudes[axis] = 0.0f;
            normalizedBasis[axis] = glm::vec3(0.0f);
            continue;
        }
        magnitudes[axis] = std::sqrt(lengthSquared);
        if (!std::isfinite(magnitudes[axis]) || magnitudes[axis] <= 0.0f) {
            return Result::failure(
                "Transform matrix has an unusable scale axis");
        }
        normalizedBasis[axis] = column / magnitudes[axis];
        ++nonzeroAxes;
    }

    if (nonzeroAxes != 3) {
        glm::mat3 completedBasis(1.0f);
        glm::vec3 scale = magnitudes;
        for (int axis = 0; axis < 3; ++axis) {
            if (magnitudes[axis] > 0.0f) {
                completedBasis[axis] = normalizedBasis[axis];
            }
        }

        if (nonzeroAxes == 0) {
            completedBasis = glm::mat3(1.0f);
        } else if (nonzeroAxes == 1) {
            int knownAxis = 0;
            while (magnitudes[knownAxis] <= 0.0f) ++knownAxis;
            const glm::vec3 reference = detail::perpendicularReference(
                completedBasis[knownAxis]);
            const glm::vec3 perpendicular = glm::normalize(glm::cross(
                reference, completedBasis[knownAxis]));
            if (!isFiniteVector(perpendicular) ||
                glm::dot(perpendicular, perpendicular) <= 0.0f) {
                return Result::failure(
                    "Transform matrix has an unusable rotation basis");
            }
            if (knownAxis == 0) {
                completedBasis[1] = perpendicular;
                completedBasis[2] = glm::cross(completedBasis[0],
                                               completedBasis[1]);
            } else if (knownAxis == 1) {
                completedBasis[0] = perpendicular;
                completedBasis[2] = glm::cross(completedBasis[0],
                                               completedBasis[1]);
            } else {
                completedBasis[0] = perpendicular;
                completedBasis[1] = glm::cross(completedBasis[2],
                                               completedBasis[0]);
            }
        } else {
            int missingAxis = 0;
            while (magnitudes[missingAxis] > 0.0f) ++missingAxis;
            if (missingAxis == 0) {
                completedBasis[0] = glm::cross(completedBasis[1],
                                               completedBasis[2]);
            } else if (missingAxis == 1) {
                completedBasis[1] = glm::cross(completedBasis[2],
                                               completedBasis[0]);
            } else {
                completedBasis[2] = glm::cross(completedBasis[0],
                                               completedBasis[1]);
            }
            const float missingLength = glm::length(
                completedBasis[missingAxis]);
            if (!std::isfinite(missingLength) || missingLength <= 0.0f) {
                return Result::failure(
                    "Transform matrix has an unusable rotation basis");
            }
            completedBasis[missingAxis] /= missingLength;
        }

        return detail::finishDecomposition(
            matrix, completedBasis, scale, output);
    }

    const float basisDeterminant = glm::determinant(normalizedBasis);
    if (!std::isfinite(basisDeterminant) ||
        std::fabs(basisDeterminant) <= 1.0e-6f) {
        return Result::failure(
            "Transform matrix has an unusable rotation basis");
    }

    glm::vec3 scaleSignHint(1.0f);
    if (basisDeterminant < 0.0f) scaleSignHint.x = -1.0f;
    return detail::decomposeModelMatrixWithSigns(
        matrix, scaleSignHint, output);
}

[[nodiscard]] inline Result extractScale(
    const glm::mat4& matrix, const glm::vec3& authoredScale,
    glm::vec3& scale) {
    scale = {};
    if (!isFiniteMatrix(matrix) || !isFiniteVector(authoredScale)) {
        return Result::failure(
            "Transform matrix and scale hint must be finite");
    }

    for (int axis = 0; axis < 3; ++axis) {
        const float lengthSquared = glm::dot(glm::vec3(matrix[axis]),
                                             glm::vec3(matrix[axis]));
        if (!std::isfinite(lengthSquared) || lengthSquared < 0.0f) {
            return Result::failure("Transform matrix scale is not finite");
        }
        const float magnitude = std::sqrt(lengthSquared);
        if (!std::isfinite(magnitude)) {
            return Result::failure("Transform matrix scale is not finite");
        }
        scale[axis] = authoredScale[axis] < 0.0f ? -magnitude : magnitude;
    }

    return isFiniteVector(scale)
               ? Result::success()
               : Result::failure("Decomposed scale is not finite");
}

}  // namespace transform_math

#endif
