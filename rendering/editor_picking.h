#ifndef RENDERING_EDITOR_PICKING_H
#define RENDERING_EDITOR_PICKING_H

#include <vector>

#include <glm/glm.hpp>

class Camera;
class GameObject;

namespace editor_picking {

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
[[nodiscard]] bool projectVulkanWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& vulkanProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept;
[[nodiscard]] bool projectImGuizmoWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& imguizmoProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept;

}  // namespace editor_picking

#endif
