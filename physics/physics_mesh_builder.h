#ifndef PHYSICS_MESH_BUILDER_H
#define PHYSICS_MESH_BUILDER_H

#include <cstdint>
#include <vector>

#include <glm/glm.hpp>

#include "../core/result.h"
#include "../rendering/utils/vulkan_utils.h"

namespace physics {

struct WorldTriangleMesh {
    std::vector<glm::vec3> vertices;
    std::vector<uint32_t> indices;
};

// Points remain in the authored GameObject coordinate space. Position and
// rotation deliberately do not participate in this conversion.
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

// Uses Dunamis' canonical T * Rx * Ry * Rz * S transform convention.
[[nodiscard]] Result buildWorldTriangleMesh(
    const std::vector<MeshInstance>& instances, const glm::vec3& position,
    const glm::vec3& rotation, const glm::vec3& scale,
    WorldTriangleMesh& output);

}  // namespace physics

#endif
