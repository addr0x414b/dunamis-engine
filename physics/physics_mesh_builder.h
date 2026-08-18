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

// Uses Dunamis' canonical T * Rx * Ry * Rz * S transform convention.
[[nodiscard]] Result buildWorldTriangleMesh(
    const std::vector<MeshInstance>& instances, const glm::vec3& position,
    const glm::vec3& rotation, const glm::vec3& scale,
    WorldTriangleMesh& output);

}  // namespace physics

#endif
