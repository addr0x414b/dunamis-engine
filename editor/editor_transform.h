#ifndef EDITOR_EDITOR_TRANSFORM_H
#define EDITOR_EDITOR_TRANSFORM_H

#include "../core/result.h"
#include "../math/transform_math.h"

#include <glm/mat4x4.hpp>

class GameObject;

namespace editor_transform {

// Converts a world-space gizmo result into the selected object's authored
// local TRS. The output is only populated on success; the GameObject is never
// mutated by this helper.
[[nodiscard]] Result deriveLocalTransformFromWorld(
    const GameObject& object, const glm::mat4& candidateWorld,
    transform_math::DecomposedTransform& local);

}  // namespace editor_transform

#endif
