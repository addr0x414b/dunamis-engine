#ifndef EDITOR_MUTATION_H
#define EDITOR_MUTATION_H

#include "../core/result.h"

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <string>

class DirectionalLight;
class Camera;
class GameObject;
class PointLight;

namespace editor_mutation {

struct CameraTransformState {
    Camera* camera = nullptr;
    glm::vec3 position{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

// Captures the auxiliary camera pose associated with an owner. A GameObject
// without a standalone or attached Camera produces an empty state.
[[nodiscard]] Result captureCameraTransformState(
    const GameObject& owner, CameraTransformState& state);

// Prepares the auxiliary camera pose for an owner world/local transform
// change without mutating either the owner or the Camera. The original state
// must be the state captured before the current editor drag.
[[nodiscard]] Result prepareCameraTransformState(
    const GameObject& owner, const glm::mat4& originalOwnerWorld,
    const glm::mat4& candidateOwnerWorld,
    const glm::mat4& originalParentWorld,
    const glm::mat4& candidateParentWorld,
    const glm::vec3& originalLocalRotation,
    const glm::vec3& candidateLocalRotation,
    const CameraTransformState& originalState,
    CameraTransformState& candidateState,
    bool preserveStandaloneWorldPose);

[[nodiscard]] Result applyName(GameObject& object,
                               const std::string& newName);
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
