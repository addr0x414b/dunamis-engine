#ifndef JOLT_SHAPE_BUILDER_H
#define JOLT_SHAPE_BUILDER_H

// Jolt requires this include before all other Jolt headers.
#include <Jolt/Jolt.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <cstddef>

#include <glm/glm.hpp>

#include "../core/result.h"

class GameObject;
class Character;

namespace physics {

struct ShapeDiagnostics {
    enum class Representation { TriangleMesh, ConvexHull, AnalyticSphere, AnalyticCapsule };
    Representation representation = Representation::TriangleMesh;
    std::size_t inputVertices = 0;
    std::size_t inputTriangles = 0;
    std::size_t inputPoints = 0;
    std::size_t cookedHullVertices = 0;
    std::size_t joltTriangles = 0;
    std::size_t joltBytes = 0;
    float radius = 0.0f;
    float height = 0.0f;
};

struct CookedShape {
    JPH::ShapeRefC shape;
    ShapeDiagnostics diagnostics;
};

// These functions are the single source of shape cooking semantics for runtime
// bodies and editor inspection. Geometry is authored in Dunamis units; shapes
// and transforms returned here are in Jolt meters.
[[nodiscard]] Result buildGameObjectShape(const GameObject& object,
                                          CookedShape& output);
[[nodiscard]] Result buildCharacterShape(const Character& character,
                                         CookedShape& output);
[[nodiscard]] JPH::RVec3 toJoltPosition(const glm::vec3& position) noexcept;
[[nodiscard]] JPH::Quat toJoltRotation(const glm::vec3& rotation) noexcept;
[[nodiscard]] JPH::RMat44 centerOfMassTransform(const glm::vec3& position,
                                                 const glm::vec3& rotation) noexcept;
[[nodiscard]] glm::mat4 joltTransformToDunamis(const JPH::RMat44& transform) noexcept;

}  // namespace physics

#endif
