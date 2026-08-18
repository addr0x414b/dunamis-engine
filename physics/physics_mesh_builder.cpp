#include "physics_mesh_builder.h"
#include "physics_units.h"

#include <cmath>
#include <limits>

namespace physics {
namespace {

bool isFinite(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

}  // namespace

glm::mat4 makeDunamisRotationMatrix(const glm::vec3& rotation) noexcept {
    glm::mat4 result(1.0f);
    result = glm::rotate(result, glm::radians(rotation.x),
                         glm::vec3(1.0f, 0.0f, 0.0f));
    result = glm::rotate(result, glm::radians(rotation.y),
                         glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::rotate(result, glm::radians(rotation.z),
                       glm::vec3(0.0f, 0.0f, 1.0f));
}

bool extractDunamisRotation(const glm::mat4& matrix,
                            glm::vec3& rotation) noexcept {
    rotation = {};
    glm::vec3 basis[3];
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 column(matrix[axis]);
        const float lengthSquared = glm::dot(column, column);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
            return false;
        }
        basis[axis] = column / std::sqrt(lengthSquared);
        if (!isFinite(basis[axis])) {
            return false;
        }
    }
    const float r00 = basis[0].x;
    const float r01 = basis[1].x;
    const float r02 = basis[2].x;
    const float r12 = basis[2].y;
    const float r22 = basis[2].z;
    const float yDenominator = std::sqrt(r00 * r00 + r01 * r01);
    if (!std::isfinite(yDenominator)) {
        return false;
    }
    rotation.x = glm::degrees(std::atan2(-r12, r22));
    rotation.y = glm::degrees(std::atan2(r02, yDenominator));
    rotation.z = glm::degrees(std::atan2(-r01, r00));
    return isFinite(rotation);
}

Result buildScaledLocalConvexHull(const std::vector<MeshInstance>& instances,
                                  const glm::vec3& scale,
                                  LocalConvexHull& output) {
    output = {};
    if (!isFinite(scale)) {
        return Result::failure("GameObject scale contains a non-finite value");
    }
    constexpr float minimumScale = 1.0e-6f;
    if (std::abs(scale.x) < minimumScale || std::abs(scale.y) < minimumScale ||
        std::abs(scale.z) < minimumScale) {
        return Result::failure("Dynamic convex hull scale must not be near zero");
    }
    for (const MeshInstance& instance : instances) {
        const Mesh& mesh = instance.mesh;
        if (mesh.vertices.empty()) {
            return Result::failure("Mesh has no vertices");
        }
        for (const Vertex& vertex : mesh.vertices) {
            if (!isFinite(vertex.pos)) {
                return Result::failure("Mesh contains a non-finite vertex position");
            }
            const glm::vec3 scaledDunamisUnits = vertex.pos * scale;
            if (!isFinite(scaledDunamisUnits)) {
                return Result::failure("Scale produced a non-finite convex hull point");
            }
            output.points.push_back(dunamisToMeters(scaledDunamisUnits));
        }
    }
    if (output.points.empty()) {
        return Result::failure("Configured convex hull has no vertices");
    }
    if (output.points.size() < 4) {
        return Result::failure("Convex hull requires at least four points");
    }
    constexpr float degeneracyTolerance = 1.0e-8f;
    bool hasVolume = false;
    const glm::vec3 origin = output.points.front();
    for (std::size_t first = 1; first < output.points.size() && !hasVolume;
         ++first) {
        for (std::size_t second = first + 1;
             second < output.points.size() && !hasVolume; ++second) {
            const glm::vec3 normal = glm::cross(output.points[first] - origin,
                                                output.points[second] - origin);
            if (glm::dot(normal, normal) <= degeneracyTolerance) {
                continue;
            }
            for (std::size_t third = second + 1;
                 third < output.points.size(); ++third) {
                if (std::abs(glm::dot(normal, output.points[third] - origin)) >
                    degeneracyTolerance) {
                    hasVolume = true;
                    break;
                }
            }
        }
    }
    if (!hasVolume) {
        return Result::failure("Convex hull points are degenerate");
    }
    return Result::success();
}

Result buildScaledLocalTriangleMesh(
    const std::vector<MeshInstance>& instances, const glm::vec3& scale,
    ScaledLocalTriangleMesh& output) {
    output = {};
    if (!isFinite(scale)) {
        return Result::failure("GameObject scale contains a non-finite value");
    }
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
            const glm::vec3 scaledDunamisUnits = vertex.pos * scale;
            if (!isFinite(scaledDunamisUnits)) {
                return Result::failure("Mesh scale produced a non-finite vertex position");
            }
            output.vertices.push_back(dunamisToMeters(scaledDunamisUnits));
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
