#ifndef DUNAMIS_TRANSFORM_MATH_H
#define DUNAMIS_TRANSFORM_MATH_H

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

namespace transform_math {

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

}  // namespace transform_math

#endif
