#ifndef PHYSICS_TRANSFORMS_H
#define PHYSICS_TRANSFORMS_H

#include <glm/glm.hpp>

#include "../core/result.h"

class GameObject;
class Character;

namespace physics {

struct PhysicsWorldPose {
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
};

// B2.3A rigid-body hierarchy policy. The object itself may have an authored
// scale for collider cooking, but every ancestor must be a non-physics object
// with identity scale.
[[nodiscard]] Result validatePhysicsHierarchy(const GameObject& object);

[[nodiscard]] Result derivePhysicsWorldPose(const GameObject& object,
                                            PhysicsWorldPose& pose);

// Candidate-local overload used by transactional runtime editor edits. The
// candidate is never written to the GameObject by this helper.
[[nodiscard]] Result derivePhysicsWorldPose(
    const GameObject& object, const glm::vec3& localPosition,
    const glm::vec3& localRotation, PhysicsWorldPose& pose);

// Convert a Jolt-authored world rigid pose back to the object's authored local
// position and rotation. The object's existing local scale is deliberately
// not an output and must remain unchanged by callers.
[[nodiscard]] Result deriveLocalPoseFromPhysicsWorld(
    const GameObject& object, const glm::vec3& worldPosition,
    const glm::vec3& worldRotation, glm::vec3& localPosition,
    glm::vec3& localRotation);

// Matrix overload avoids an unnecessary Euler round trip when a Jolt
// quaternion is already available.
[[nodiscard]] Result deriveLocalPoseFromPhysicsWorld(
    const GameObject& object, const glm::mat4& worldMatrix,
    glm::vec3& localPosition, glm::vec3& localRotation);

// CharacterVirtual is position-driven and operates in world space. Character
// position remains authored in the local space of its GameObject parent.
[[nodiscard]] Result validateCharacterPhysicsHierarchy(
    const Character& character);

[[nodiscard]] Result deriveCharacterWorldPosition(
    const Character& character, glm::vec3& worldPosition);

[[nodiscard]] Result deriveCharacterLocalPositionFromPhysicsWorld(
    const Character& character, const glm::vec3& worldPosition,
    glm::vec3& localPosition);

}  // namespace physics

#endif
