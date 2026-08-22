#ifndef EDITOR_MUTATION_H
#define EDITOR_MUTATION_H

#include "../core/result.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class DirectionalLight;
class GameObject;
class PointLight;

namespace editor_mutation {

[[nodiscard]] Result applyPosition(GameObject& object,
                                    const glm::vec3& newPosition);
[[nodiscard]] Result applyRotation(GameObject& object,
                                    const glm::vec3& newRotation);
[[nodiscard]] Result applyScale(GameObject& object,
                                 const glm::vec3& newScale);

[[nodiscard]] Result extractDunamisRotation(
    const glm::mat4& matrix, const glm::vec3& authoredScale,
    glm::vec3& rotation);
[[nodiscard]] Result extractDunamisScale(
    const glm::mat4& matrix, const glm::vec3& authoredScale,
    glm::vec3& scale);

[[nodiscard]] Result applyPointLightColor(PointLight& light,
                                           const glm::vec3& color);
[[nodiscard]] Result applyPointLightIntensity(PointLight& light,
                                               float intensity);
[[nodiscard]] Result applyDirectionalLightColor(
    DirectionalLight& light, const glm::vec3& color);
[[nodiscard]] Result applyDirectionalLightIntensity(
    DirectionalLight& light, float intensity);

}  // namespace editor_mutation

#endif
