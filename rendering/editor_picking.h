#ifndef EDITOR_PICKING_H
#define EDITOR_PICKING_H

#include <vector>

#include <glm/glm.hpp>

#include "../assets/model_asset.h"

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

struct CameraVisualizationEntry {
    const Camera* camera = nullptr;
    const GameObject* selectionTarget = nullptr;
    bool active = false;
};

[[nodiscard]] float distanceSquaredToSegment(
    const glm::vec2& point, const glm::vec2& segmentStart,
    const glm::vec2& segmentEnd) noexcept;
[[nodiscard]] std::vector<CameraVisualizationEntry>
collectCameraVisualizationEntries(
    const std::vector<const GameObject*>& objects,
    const Camera* activeCamera);
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
