#ifndef RENDERING_EDITOR_PICKING_H
#define RENDERING_EDITOR_PICKING_H

#include <array>
#include <cstddef>
#include <vector>

#include <glm/glm.hpp>

class Camera;
class GameObject;
struct CameraWorldPose;

namespace editor_picking {

// These dimensions use ImGui viewport logical pixels, before framebuffer scale.
inline constexpr float pointLightOuterRadiusPixels = 15.0f;
inline constexpr float pointLightInnerRayRadiusRatio = 0.45f;
inline constexpr float pointLightCenterRadiusRatio = 1.0f / 3.0f;
inline constexpr float directionalLightCenterRadiusPixels = 11.0f;
inline constexpr float directionalLightArrowTipDistancePixels = 34.0f;
inline constexpr float directionalLightArrowheadLengthPixels = 7.0f;
inline constexpr float directionalLightArrowheadHalfWidthPixels = 4.0f;
inline constexpr float directionalLightCenterFillRadiusPixels = 2.5f;
inline constexpr float cameraVisualizationMaximumDimensionPixels = 44.0f;
inline constexpr float cameraVisualizationApexRadiusPixels = 4.0f;
inline constexpr float helperLineHitRadiusPixels = 4.0f;

enum class EditorHelperKind {
    Camera,
    PointLight,
    DirectionalLight,
    Character,
};

struct ScreenSpaceSegment {
    glm::vec2 start{0.0f};
    glm::vec2 end{0.0f};
    float hitRadius = helperLineHitRadiusPixels;
};

struct ScreenSpaceCircle {
    glm::vec2 center{0.0f};
    float radius = 0.0f;
};

struct ScreenSpaceHitCircle {
    glm::vec2 center{0.0f};
    float radius = 0.0f;
    float filledRadius = 0.0f;
};

struct ScreenSpaceHitPolygon {
    std::array<glm::vec2, 8> vertices{};
    std::size_t vertexCount = 0;
};

struct ScreenSpaceHelperGeometry {
    static constexpr std::size_t maximumSegments = 128;
    static constexpr std::size_t maximumCircles = 4;
    static constexpr std::size_t maximumHitCircles = 4;
    static constexpr std::size_t maximumHitPolygons = 2;

    bool valid = false;
    bool constrainHitToCircle = false;
    glm::vec2 hitLimitCenter{0.0f};
    float hitLimitRadius = 0.0f;
    std::array<ScreenSpaceSegment, maximumSegments> segments{};
    std::size_t segmentCount = 0;
    std::array<ScreenSpaceCircle, maximumCircles> circles{};
    std::size_t circleCount = 0;
    std::array<ScreenSpaceHitCircle, maximumHitCircles> hitCircles{};
    std::size_t hitCircleCount = 0;
    std::array<ScreenSpaceHitPolygon, maximumHitPolygons> hitPolygons{};
    std::size_t hitPolygonCount = 0;
};

struct EditorHelperGeometry {
    EditorHelperKind kind = EditorHelperKind::Camera;
    const GameObject* selectionTarget = nullptr;
    float viewDepth = 0.0f;
    ScreenSpaceHelperGeometry screen;
};

struct CameraVisualizationEntry {
    const Camera* camera = nullptr;
    const GameObject* selectionTarget = nullptr;
    bool active = false;
};

[[nodiscard]] ScreenSpaceHelperGeometry makePointLightHelperGeometry(
    const glm::vec2& center, float outerRadius, float viewDepth) noexcept;
[[nodiscard]] ScreenSpaceHelperGeometry makeDirectionalLightHelperGeometry(
    const glm::vec2& center, const glm::vec2& projectedDirection,
    bool projectedDirectionValid, float viewDepth) noexcept;
[[nodiscard]] bool makeCameraHelperGeometry(
    const glm::vec2& apex, const glm::vec2& projectedForward,
    const glm::vec2& projectedRight, const glm::vec2& projectedUp,
    float verticalFovDegrees, float aspectRatio, float viewDepth,
    ScreenSpaceHelperGeometry& geometry) noexcept;
[[nodiscard]] float screenSpaceHelperHitDistanceSquared(
    const ScreenSpaceHelperGeometry& geometry,
    const glm::vec2& point) noexcept;
[[nodiscard]] std::size_t closestHelperGeometryIndex(
    const std::vector<EditorHelperGeometry>& geometries,
    const glm::vec2& point) noexcept;

[[nodiscard]] float distanceSquaredToSegment(
    const glm::vec2& point, const glm::vec2& segmentStart,
    const glm::vec2& segmentEnd) noexcept;
[[nodiscard]] bool isScreenPointInsideRect(
    const glm::vec2& point, const glm::vec2& minimum,
    const glm::vec2& size) noexcept;
[[nodiscard]] std::vector<CameraVisualizationEntry>
collectCameraVisualizationEntries(
    const std::vector<const GameObject*>& objects,
    const Camera* activeCamera);
[[nodiscard]] std::vector<const Camera*> collectCameraPointers(
    const std::vector<const GameObject*>& objects,
    const Camera* activeCamera);
[[nodiscard]] bool deriveWorldPosition(const GameObject& object,
                                       glm::vec3& worldPosition) noexcept;
[[nodiscard]] bool calculateCameraVisualizationPose(
    const Camera& camera, const GameObject* selectionTarget,
    CameraWorldPose& pose) noexcept;
[[nodiscard]] bool projectVulkanWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& vulkanProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept;
[[nodiscard]] bool projectVulkanDirectionToImGui(
    const glm::vec3& worldPosition, const glm::vec3& worldDirection,
    const glm::mat4& view, const glm::mat4& vulkanProjection,
    const glm::vec2& renderSize,
    glm::vec2& screenDirection) noexcept;
[[nodiscard]] bool projectImGuizmoWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& imguizmoProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept;

}  // namespace editor_picking

#endif
