#include "physics_mesh_builder.h"

#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace physics {
namespace {

bool isFinite(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

glm::mat4 makeModelMatrix(const glm::vec3& position,
                          const glm::vec3& rotation,
                          const glm::vec3& scale) noexcept {
    glm::mat4 model(1.0f);
    model = glm::translate(model, position);
    model = glm::rotate(model, glm::radians(rotation.x),
                        glm::vec3(1.0f, 0.0f, 0.0f));
    model = glm::rotate(model, glm::radians(rotation.y),
                        glm::vec3(0.0f, 1.0f, 0.0f));
    model = glm::rotate(model, glm::radians(rotation.z),
                        glm::vec3(0.0f, 0.0f, 1.0f));
    return glm::scale(model, scale);
}

}  // namespace

Result buildWorldTriangleMesh(const std::vector<MeshInstance>& instances,
                              const glm::vec3& position,
                              const glm::vec3& rotation,
                              const glm::vec3& scale,
                              WorldTriangleMesh& output) {
    output = {};
    if (!isFinite(position) || !isFinite(rotation) || !isFinite(scale)) {
        return Result::failure("GameObject transform contains a non-finite value");
    }

    const glm::mat4 model = makeModelMatrix(position, rotation, scale);
    for (const MeshInstance& instance : instances) {
        const Mesh& mesh = instance.mesh;
        if (mesh.vertices.empty()) {
            return Result::failure("Mesh has no vertices");
        }
        if (mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
            return Result::failure("Mesh indices must contain complete triangles");
        }
        if (output.vertices.size() + mesh.vertices.size() >
            std::numeric_limits<uint32_t>::max()) {
            return Result::failure("Combined mesh has too many vertices");
        }
        const uint32_t offset = static_cast<uint32_t>(output.vertices.size());
        for (const Vertex& vertex : mesh.vertices) {
            if (!isFinite(vertex.pos)) {
                return Result::failure("Mesh contains a non-finite vertex position");
            }
            const glm::vec3 transformed(model * glm::vec4(vertex.pos, 1.0f));
            if (!isFinite(transformed)) {
                return Result::failure("Mesh transform produced a non-finite vertex position");
            }
            output.vertices.push_back(transformed);
        }
        for (const uint32_t index : mesh.indices) {
            if (index >= mesh.vertices.size()) {
                return Result::failure("Mesh index is outside its vertex range");
            }
            output.indices.push_back(offset + index);
        }
    }
    if (output.indices.empty()) {
        return Result::failure("Configured mesh collider has no triangles");
    }
    return Result::success();
}

}  // namespace physics
