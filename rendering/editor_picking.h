#ifndef EDITOR_PICKING_H
#define EDITOR_PICKING_H

#include <vector>

#include <glm/glm.hpp>

#include "utils/vulkan_utils.h"

class Camera;
class GameObject;

namespace editor_picking {

struct Ray {
    glm::vec3 origin{0.0f};
    glm::vec3 direction{0.0f};
};

struct AggregateBounds {
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};
    bool valid = false;
};

struct MeshPickDiagnostics {
    bool transformInvertible = false;
    bool broadPhasePassed = false;
    bool triangleTestingReached = false;
    float closestWorldDistance = 0.0f;
};

[[nodiscard]] glm::mat4 makeModelMatrix(const glm::vec3& position,
                                         const glm::vec3& rotation,
                                         const glm::vec3& scale) noexcept;
[[nodiscard]] glm::mat4 makeRotationMatrix(
    const glm::vec3& rotation) noexcept;
[[nodiscard]] glm::mat4 makeTranslationMatrix(
    const glm::vec3& translation) noexcept;
[[nodiscard]] std::vector<const Camera*> collectCameraPointers(
    const std::vector<const GameObject*>& objects,
    const Camera* activeCamera);
[[nodiscard]] AggregateBounds aggregateBounds(
    const std::vector<MeshInstance>& instances) noexcept;
[[nodiscard]] glm::vec3 worldBoundsCenter(
    const AggregateBounds& bounds, const glm::mat4& model,
    const glm::vec3& fallback) noexcept;
[[nodiscard]] bool projectVulkanWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& vulkanProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept;
[[nodiscard]] bool projectImGuizmoWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& imguizmoProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept;
[[nodiscard]] bool intersectAabb(const Ray& ray, const Mesh::Bounds& bounds,
                                 float& distance) noexcept;
[[nodiscard]] bool intersectTriangle(const Ray& ray, const glm::vec3& first,
                                     const glm::vec3& second,
                                     const glm::vec3& third,
                                     float& distance) noexcept;
[[nodiscard]] bool intersectMeshWorld(const Ray& worldRay, const Mesh& mesh,
                                      const glm::mat4& model,
                                      float& worldDistance,
                                      MeshPickDiagnostics* diagnostics = nullptr) noexcept;

}  // namespace editor_picking

#endif
