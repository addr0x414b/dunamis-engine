#ifndef COLLISION_SHAPES_H
#define COLLISION_SHAPES_H

// Jolt requires this include before all other Jolt headers.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <chrono>

#include <glm/glm.hpp>

#include "../core/result.h"
#include "shape_diagnostics.h"

class Character;
class GameObject;

namespace physics {

struct ShapeBuildTimings {
    std::chrono::steady_clock::duration geometryConversion{};
    std::chrono::steady_clock::duration joltCooking{};
};

struct CookedShape {
    JPH::ShapeRefC shape;
    ShapeDiagnostics diagnostics;
    ShapeBuildTimings timings;
};

// These functions are the single source of shape cooking semantics for
// runtime bodies and editor inspection. Imported Mesh::vertices are already
// meter-valued; scale is dimensionless and the explicit Dunamis/Jolt boundary
// remains mathematically 1:1.
// shapes and transforms returned here are in Jolt meters.
[[nodiscard]] Result buildGameObjectShape(const GameObject& object,
                                          CookedShape& output);
[[nodiscard]] Result buildCharacterShape(const Character& character,
                                         CookedShape& output);
[[nodiscard]] JPH::RVec3 toJoltPosition(const glm::vec3& position) noexcept;
[[nodiscard]] JPH::Quat toJoltRotation(const glm::vec3& rotation) noexcept;
[[nodiscard]] JPH::RMat44 makeShapeCenterOfMassTransform(
    const JPH::Shape& shape, const glm::vec3& bodyOriginPosition,
    const glm::vec3& bodyRotation) noexcept;
[[nodiscard]] JPH::RMat44 makeShapeCenterOfMassPreviewTransform(
    const JPH::Shape& shape, const glm::vec3& bodyOriginPosition,
    const glm::vec3& bodyRotation, const glm::vec3& relativeScale) noexcept;
[[nodiscard]] glm::mat4 joltTransformToDunamis(
    const JPH::RMat44& transform) noexcept;

// Used by runtime transform writeback to preserve Dunamis' authored Euler
// convention when a Jolt body rotation is converted back to scene state.
[[nodiscard]] bool extractDunamisRotation(const glm::mat4& matrix,
                                          glm::vec3& rotation) noexcept;

}  // namespace physics

#endif
