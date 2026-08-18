#ifndef PHYSICS_MESH_BUILDER_H
#define PHYSICS_MESH_BUILDER_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "../core/result.h"
#include "../rendering/utils/vulkan_utils.h"

namespace physics {

// Local collision geometry in Jolt meters. Authored position and rotation are
// deliberately excluded; they belong to the static body's transform.
struct ScaledLocalTriangleMesh {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
};

// Local collision points in Jolt meters. Position and rotation deliberately
// do not participate in this conversion.
struct LocalConvexHull {
    std::vector<glm::vec3> points;
};

[[nodiscard]] Result buildScaledLocalConvexHull(
    const std::vector<MeshInstance>& instances, const glm::vec3& scale,
    LocalConvexHull& output);

// Dunamis' canonical authored convention is Rx * Ry * Rz, with degrees.
[[nodiscard]] glm::mat4 makeDunamisRotationMatrix(
    const glm::vec3& rotation) noexcept;
[[nodiscard]] bool extractDunamisRotation(const glm::mat4& matrix,
                                           glm::vec3& rotation) noexcept;

// Applies authored scale and converts the resulting local geometry from
// Dunamis units to Jolt meters.
[[nodiscard]] Result buildScaledLocalTriangleMesh(
    const std::vector<MeshInstance>& instances, const glm::vec3& scale,
    ScaledLocalTriangleMesh& output);

}  // namespace physics

#endif
