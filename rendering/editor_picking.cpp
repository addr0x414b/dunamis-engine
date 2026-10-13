#include "editor_picking.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../scene/camera.h"
#include "../scene/game_object.h"

namespace editor_picking {
namespace {

constexpr float epsilon = 1.0e-6f;
constexpr float maximumImGuiCoordinateMagnitude = 1.0e7f;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool isFiniteVector(const glm::vec2& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
}

bool isFiniteVector(const glm::vec4& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z) && std::isfinite(value.w);
}

bool normalizeScreenDirection(const glm::vec2& direction,
                              glm::vec2& normalized) noexcept {
    normalized = {};
    if (!isFiniteVector(direction)) {
        return false;
    }
    const float length = std::hypot(direction.x, direction.y);
    if (!std::isfinite(length) || length <= 1.0e-4f) {
        return false;
    }
    normalized = direction / length;
    return isFiniteVector(normalized);
}

bool validHelperDepth(float depth) noexcept {
    return std::isfinite(depth) && depth > 0.0f;
}

bool appendSegment(ScreenSpaceHelperGeometry& geometry,
                   const glm::vec2& start, const glm::vec2& end,
                   float hitRadius = helperLineHitRadiusPixels) noexcept {
    if (geometry.segmentCount >= geometry.segments.size() ||
        !isFiniteVector(start) || !isFiniteVector(end) ||
        !std::isfinite(hitRadius) || hitRadius < 0.0f) {
        return false;
    }
    geometry.segments[geometry.segmentCount++] = {start, end, hitRadius};
    return true;
}

bool appendCircle(ScreenSpaceHelperGeometry& geometry,
                  const glm::vec2& center, float radius) noexcept {
    if (geometry.circleCount >= geometry.circles.size() ||
        !isFiniteVector(center) || !std::isfinite(radius) || radius <= 0.0f) {
        return false;
    }
    geometry.circles[geometry.circleCount++] = {center, radius};
    return true;
}

bool appendHitCircle(ScreenSpaceHelperGeometry& geometry,
                     const glm::vec2& center, float radius,
                     float filledRadius = 0.0f) noexcept {
    if (geometry.hitCircleCount >= geometry.hitCircles.size() ||
        !isFiniteVector(center) || !std::isfinite(radius) || radius <= 0.0f ||
        !std::isfinite(filledRadius) || filledRadius < 0.0f ||
        filledRadius > radius) {
        return false;
    }
    geometry.hitCircles[geometry.hitCircleCount++] = {
        center, radius, filledRadius};
    return true;
}

bool appendHitPolygon(ScreenSpaceHelperGeometry& geometry,
                      const glm::vec2* vertices,
                      std::size_t vertexCount) noexcept {
    if (geometry.hitPolygonCount >= geometry.hitPolygons.size() ||
        vertices == nullptr || vertexCount < 3 ||
        vertexCount > ScreenSpaceHitPolygon{}.vertices.size()) {
        return false;
    }
    ScreenSpaceHitPolygon polygon;
    polygon.vertexCount = vertexCount;
    for (std::size_t index = 0; index < vertexCount; ++index) {
        if (!isFiniteVector(vertices[index])) {
            return false;
        }
        polygon.vertices[index] = vertices[index];
    }
    geometry.hitPolygons[geometry.hitPolygonCount++] = polygon;
    return true;
}

bool pointInsidePolygon(const glm::vec2& point,
                        const ScreenSpaceHitPolygon& polygon) noexcept {
    if (polygon.vertexCount < 3 ||
        polygon.vertexCount > polygon.vertices.size()) {
        return false;
    }

    bool inside = false;
    for (std::size_t index = 0, previous = polygon.vertexCount - 1;
         index < polygon.vertexCount; previous = index++) {
        const glm::vec2& first = polygon.vertices[previous];
        const glm::vec2& second = polygon.vertices[index];
        if (!isFiniteVector(first) || !isFiniteVector(second)) {
            return false;
        }
        const float edgeDistanceSquared = distanceSquaredToSegment(
            point, first, second);
        if (edgeDistanceSquared <= 1.0e-6f) {
            return true;
        }

        const bool crossesY = (first.y > point.y) != (second.y > point.y);
        if (!crossesY) {
            continue;
        }
        const float crossingX = first.x +
            (point.y - first.y) * (second.x - first.x) /
                (second.y - first.y);
        if (!std::isfinite(crossingX)) {
            return false;
        }
        if (point.x < crossingX) {
            inside = !inside;
        }
    }
    return inside;
}

bool isFiniteMatrix(const glm::mat4& matrix) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) {
                return false;
            }
        }
    }
    return true;
}

bool getVisibleProjection(const glm::vec3& worldPoint,
                          const glm::mat4& view,
                          const glm::mat4& projection,
                          glm::vec4& clip,
                          glm::vec3& ndc) noexcept {
    clip = {};
    ndc = {};
    if (!isFiniteVector(worldPoint) || !isFiniteMatrix(view) ||
        !isFiniteMatrix(projection)) {
        return false;
    }
    clip = projection * view * glm::vec4(worldPoint, 1.0f);
    if (!isFiniteVector(clip) || clip.w <= epsilon) {
        return false;
    }
    ndc = glm::vec3(clip) / clip.w;
    return isFiniteVector(ndc) && ndc.z >= 0.0f && ndc.z <= 1.0f &&
           std::abs(ndc.x) <= 64.0f && std::abs(ndc.y) <= 64.0f;
}

}  // namespace

ScreenSpaceHelperGeometry makePointLightHelperGeometry(
    const glm::vec2& center, float outerRadius, float viewDepth) noexcept {
    ScreenSpaceHelperGeometry geometry;
    if (!isFiniteVector(center) || !std::isfinite(outerRadius) ||
        outerRadius <= 0.0f || outerRadius > 256.0f ||
        !validHelperDepth(viewDepth)) {
        return geometry;
    }

    constexpr std::array<glm::vec2, 8> radialDirections{
        glm::vec2(0.0f, -1.0f), glm::vec2(0.70710678f, -0.70710678f),
        glm::vec2(1.0f, 0.0f), glm::vec2(0.70710678f, 0.70710678f),
        glm::vec2(0.0f, 1.0f), glm::vec2(-0.70710678f, 0.70710678f),
        glm::vec2(-1.0f, 0.0f), glm::vec2(-0.70710678f, -0.70710678f)};
    const float innerRadius = outerRadius * pointLightInnerRayRadiusRatio;
    for (const glm::vec2& direction : radialDirections) {
        if (!appendSegment(geometry, center + direction * innerRadius,
                           center + direction * outerRadius)) {
            return {};
        }
    }

    const float centerRadius =
        outerRadius * pointLightCenterRadiusRatio;
    if (!appendCircle(geometry, center, centerRadius) ||
        !appendHitCircle(geometry, center, outerRadius, 0.0f) ||
        !appendHitCircle(geometry, center, centerRadius, centerRadius)) {
        return {};
    }
    geometry.constrainHitToCircle = true;
    geometry.hitLimitCenter = center;
    geometry.hitLimitRadius = outerRadius;
    geometry.valid = true;
    return geometry;
}

ScreenSpaceHelperGeometry makeDirectionalLightHelperGeometry(
    const glm::vec2& center, const glm::vec2& projectedDirection,
    bool projectedDirectionValid, float viewDepth) noexcept {
    ScreenSpaceHelperGeometry geometry;
    if (!isFiniteVector(center) || !validHelperDepth(viewDepth)) {
        return geometry;
    }

    const float markerRadius = directionalLightCenterRadiusPixels;
    const std::array<glm::vec2, 4> markerPoints{
        center + glm::vec2(0.0f, -markerRadius),
        center + glm::vec2(markerRadius, 0.0f),
        center + glm::vec2(0.0f, markerRadius),
        center + glm::vec2(-markerRadius, 0.0f)};
    for (std::size_t index = 0; index < markerPoints.size(); ++index) {
        if (!appendSegment(geometry, markerPoints[index],
                           markerPoints[(index + 1) % markerPoints.size()])) {
            return {};
        }
    }
    if (!appendCircle(geometry, center,
                      directionalLightCenterFillRadiusPixels) ||
        !appendHitCircle(geometry, center,
                         directionalLightCenterFillRadiusPixels + 3.0f,
                         directionalLightCenterFillRadiusPixels) ||
        !appendHitPolygon(geometry, markerPoints.data(), markerPoints.size())) {
        return {};
    }

    glm::vec2 direction;
    if (projectedDirectionValid &&
        normalizeScreenDirection(projectedDirection, direction)) {
        const glm::vec2 perpendicular(-direction.y, direction.x);
        const glm::vec2 arrowTip =
            center + direction * directionalLightArrowTipDistancePixels;
        const glm::vec2 arrowBase = arrowTip -
            direction * directionalLightArrowheadLengthPixels;
        const glm::vec2 firstWing =
            arrowBase + perpendicular * directionalLightArrowheadHalfWidthPixels;
        const glm::vec2 secondWing =
            arrowBase - perpendicular * directionalLightArrowheadHalfWidthPixels;
        const std::array<glm::vec2, 3> arrowhead{
            arrowTip, firstWing, secondWing};
        if (!appendSegment(geometry, center, arrowTip) ||
            !appendSegment(geometry, arrowTip, firstWing) ||
            !appendSegment(geometry, arrowTip, secondWing) ||
            !appendHitPolygon(geometry, arrowhead.data(), arrowhead.size())) {
            return {};
        }
    }

    geometry.valid = true;
    return geometry;
}

bool makeCameraHelperGeometry(
    const glm::vec2& apex, const glm::vec2& projectedForward,
    const glm::vec2& projectedRight, const glm::vec2& projectedUp,
    float verticalFovDegrees, float aspectRatio, float viewDepth,
    ScreenSpaceHelperGeometry& geometry) noexcept {
    geometry = {};
    if (!isFiniteVector(apex) || !std::isfinite(verticalFovDegrees) ||
        verticalFovDegrees <= 0.0f || verticalFovDegrees >= 180.0f ||
        !std::isfinite(aspectRatio) || aspectRatio <= 0.0f ||
        !validHelperDepth(viewDepth)) {
        return false;
    }

    glm::vec2 forward;
    glm::vec2 right;
    glm::vec2 up;
    const bool hasForward = normalizeScreenDirection(projectedForward, forward);
    bool hasRight = normalizeScreenDirection(projectedRight, right);
    bool hasUp = normalizeScreenDirection(projectedUp, up);

    if (!hasRight && hasUp) {
        right = glm::vec2(up.y, -up.x);
        hasRight = true;
    }
    if (!hasUp && hasRight) {
        up = glm::vec2(-right.y, right.x);
        hasUp = true;
    }
    if (!hasRight && !hasUp) {
        right = glm::vec2(1.0f, 0.0f);
        up = glm::vec2(0.0f, -1.0f);
        hasRight = true;
        hasUp = true;
    }
    if (!hasForward) {
        // A camera pointed directly along the editor view has no projected
        // forward vector. Use its projected right axis as a stable 2D proxy.
        forward = right;
    }

    const float rightUpDot = glm::dot(right, up);
    if (!std::isfinite(rightUpDot)) {
        return false;
    }
    if (std::abs(rightUpDot) > 0.98f) {
        glm::vec2 perpendicular(-right.y, right.x);
        if (glm::dot(perpendicular, up) < 0.0f) {
            perpendicular = -perpendicular;
        }
        up = perpendicular;
    }

    const float tangent = std::tan(glm::radians(verticalFovDegrees) * 0.5f);
    const float halfHeight = tangent;
    const float halfWidth = tangent * aspectRatio;
    if (!std::isfinite(tangent) || tangent <= 0.0f ||
        !std::isfinite(halfWidth) || !std::isfinite(halfHeight)) {
        return false;
    }

    const glm::vec2 planeCenterOffset = forward;
    const std::array<glm::vec2, 5> unscaledPoints{
        glm::vec2(0.0f),
        planeCenterOffset - right * halfWidth + up * halfHeight,
        planeCenterOffset + right * halfWidth + up * halfHeight,
        planeCenterOffset + right * halfWidth - up * halfHeight,
        planeCenterOffset - right * halfWidth - up * halfHeight};
    glm::vec2 minimum(0.0f);
    glm::vec2 maximum(0.0f);
    for (const glm::vec2& point : unscaledPoints) {
        if (!isFiniteVector(point)) {
            return false;
        }
        minimum = glm::min(minimum, point);
        maximum = glm::max(maximum, point);
    }
    const glm::vec2 extent = maximum - minimum;
    const float maximumExtent = std::max(extent.x, extent.y);
    if (!std::isfinite(maximumExtent) || maximumExtent <= 1.0e-5f) {
        return false;
    }
    const float screenScale =
        cameraVisualizationMaximumDimensionPixels / maximumExtent;

    std::array<glm::vec2, 5> points{};
    for (std::size_t index = 0; index < points.size(); ++index) {
        points[index] = apex + unscaledPoints[index] * screenScale;
        if (!isFiniteVector(points[index])) {
            return false;
        }
    }
    const glm::vec2 planeCenter = points[0] + forward * screenScale;
    constexpr std::array<std::size_t, 8> firstEdgePoints{
        0, 0, 0, 0, 1, 2, 3, 4};
    constexpr std::array<std::size_t, 8> secondEdgePoints{
        1, 2, 3, 4, 2, 3, 4, 1};
    for (std::size_t index = 0; index < firstEdgePoints.size(); ++index) {
        if (!appendSegment(geometry, points[firstEdgePoints[index]],
                           points[secondEdgePoints[index]])) {
            geometry = {};
            return false;
        }
    }
    if (!appendSegment(geometry, points[0], planeCenter) ||
        !appendCircle(geometry, apex, cameraVisualizationApexRadiusPixels) ||
        !appendHitCircle(geometry, apex,
                         cameraVisualizationApexRadiusPixels + 3.0f,
                         cameraVisualizationApexRadiusPixels) ||
        !appendHitPolygon(geometry, points.data(), points.size())) {
        geometry = {};
        return false;
    }
    geometry.valid = true;
    return true;
}

float screenSpaceHelperHitDistanceSquared(
    const ScreenSpaceHelperGeometry& geometry,
    const glm::vec2& point) noexcept {
    if (!geometry.valid || !isFiniteVector(point)) {
        return std::numeric_limits<float>::infinity();
    }
    if (geometry.constrainHitToCircle) {
        if (!isFiniteVector(geometry.hitLimitCenter) ||
            !std::isfinite(geometry.hitLimitRadius) ||
            geometry.hitLimitRadius <= 0.0f) {
            return std::numeric_limits<float>::infinity();
        }
        const glm::vec2 difference = point - geometry.hitLimitCenter;
        const float distanceSquared = glm::dot(difference, difference);
        if (!std::isfinite(distanceSquared) ||
            distanceSquared > geometry.hitLimitRadius *
                                  geometry.hitLimitRadius) {
            return std::numeric_limits<float>::infinity();
        }
    }

    float nearestHitDistanceSquared =
        std::numeric_limits<float>::infinity();
    for (std::size_t index = 0;
         index < std::min(geometry.segmentCount, geometry.segments.size());
         ++index) {
        const ScreenSpaceSegment& segment = geometry.segments[index];
        if (!std::isfinite(segment.hitRadius) || segment.hitRadius < 0.0f) {
            continue;
        }
        const float distanceSquared = distanceSquaredToSegment(
            point, segment.start, segment.end);
        if (distanceSquared <= segment.hitRadius * segment.hitRadius) {
            nearestHitDistanceSquared =
                std::min(nearestHitDistanceSquared, distanceSquared);
        }
    }

    for (std::size_t index = 0;
         index < std::min(geometry.hitCircleCount,
                          geometry.hitCircles.size());
         ++index) {
        const ScreenSpaceHitCircle& circle = geometry.hitCircles[index];
        if (!isFiniteVector(circle.center) || !std::isfinite(circle.radius) ||
            !std::isfinite(circle.filledRadius) || circle.radius <= 0.0f ||
            circle.filledRadius < 0.0f || circle.filledRadius > circle.radius) {
            continue;
        }
        const glm::vec2 difference = point - circle.center;
        const float distanceSquared = glm::dot(difference, difference);
        if (std::isfinite(distanceSquared) &&
            distanceSquared <= circle.radius * circle.radius) {
            const float score = distanceSquared <=
                                        circle.filledRadius *
                                            circle.filledRadius
                                    ? 0.0f
                                    : distanceSquared;
            nearestHitDistanceSquared =
                std::min(nearestHitDistanceSquared, score);
        }
    }

    for (std::size_t index = 0;
         index < std::min(geometry.hitPolygonCount,
                          geometry.hitPolygons.size());
         ++index) {
        if (pointInsidePolygon(point, geometry.hitPolygons[index])) {
            nearestHitDistanceSquared = 0.0f;
            break;
        }
    }

    if (geometry.constrainHitToCircle) {
        const glm::vec2 difference = point - geometry.hitLimitCenter;
        nearestHitDistanceSquared = std::min(
            nearestHitDistanceSquared, glm::dot(difference, difference));
    }
    return std::isfinite(nearestHitDistanceSquared)
               ? nearestHitDistanceSquared
               : std::numeric_limits<float>::infinity();
}

std::size_t closestHelperGeometryIndex(
    const std::vector<EditorHelperGeometry>& geometries,
    const glm::vec2& point) noexcept {
    constexpr float tieEpsilonSquared = 1.0e-4f;
    std::size_t closestIndex = std::numeric_limits<std::size_t>::max();
    float closestDistanceSquared = std::numeric_limits<float>::infinity();
    float closestDepth = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index < geometries.size(); ++index) {
        const EditorHelperGeometry& geometry = geometries[index];
        if (geometry.selectionTarget == nullptr ||
            !validHelperDepth(geometry.viewDepth)) {
            continue;
        }
        const float distanceSquared =
            screenSpaceHelperHitDistanceSquared(geometry.screen, point);
        if (!std::isfinite(distanceSquared)) {
            continue;
        }
        const bool closerOnScreen =
            distanceSquared < closestDistanceSquared - tieEpsilonSquared;
        const bool tiedOnScreen = std::abs(distanceSquared -
                                           closestDistanceSquared) <=
                                  tieEpsilonSquared;
        const bool closerInView = tiedOnScreen &&
            geometry.viewDepth < closestDepth - tieEpsilonSquared;
        if (closerOnScreen || closerInView) {
            closestIndex = index;
            closestDistanceSquared = distanceSquared;
            closestDepth = geometry.viewDepth;
        }
        // Exact screen and depth ties keep the first entry. The caller builds
        // entries in stable camera-discovery and scene-object order.
    }
    return closestIndex;
}

float distanceSquaredToSegment(const glm::vec2& point,
                               const glm::vec2& segmentStart,
                               const glm::vec2& segmentEnd) noexcept {
    if (!isFiniteVector(point) || !isFiniteVector(segmentStart) ||
        !isFiniteVector(segmentEnd)) {
        return std::numeric_limits<float>::infinity();
    }

    const glm::vec2 segment = segmentEnd - segmentStart;
    const float segmentLengthSquared = glm::dot(segment, segment);
    if (!std::isfinite(segmentLengthSquared)) {
        return std::numeric_limits<float>::infinity();
    }
    if (segmentLengthSquared <= 0.0f) {
        const glm::vec2 difference = point - segmentStart;
        const float distanceSquared = glm::dot(difference, difference);
        return std::isfinite(distanceSquared)
                   ? distanceSquared
                   : std::numeric_limits<float>::infinity();
    }

    const float unclampedParameter =
        glm::dot(point - segmentStart, segment) / segmentLengthSquared;
    if (!std::isfinite(unclampedParameter)) {
        return std::numeric_limits<float>::infinity();
    }
    const float parameter = std::clamp(unclampedParameter, 0.0f, 1.0f);
    const glm::vec2 closestPoint = segmentStart + parameter * segment;
    const glm::vec2 difference = point - closestPoint;
    const float distanceSquared = glm::dot(difference, difference);
    return std::isfinite(distanceSquared)
               ? distanceSquared
               : std::numeric_limits<float>::infinity();
}

bool isScreenPointInsideRect(const glm::vec2& point,
                             const glm::vec2& minimum,
                             const glm::vec2& size) noexcept {
    if (!isFiniteVector(point) || !isFiniteVector(minimum) ||
        !isFiniteVector(size) || size.x <= 0.0f || size.y <= 0.0f) {
        return false;
    }
    const glm::vec2 maximum = minimum + size;
    return isFiniteVector(maximum) &&
           point.x >= minimum.x && point.y >= minimum.y &&
           point.x <= maximum.x && point.y <= maximum.y;
}

std::vector<CameraVisualizationEntry> collectCameraVisualizationEntries(
    const std::vector<const GameObject*>& objects,
    const Camera* activeCamera) {
    std::vector<CameraVisualizationEntry> entries;
    const auto appendUnique = [&entries, activeCamera](
                                  const Camera* camera,
                                  const GameObject* selectionTarget) {
        if (camera == nullptr) {
            return;
        }

        for (CameraVisualizationEntry& entry : entries) {
            if (entry.camera != camera) {
                continue;
            }
            entry.active = entry.active || camera == activeCamera;
            if (entry.selectionTarget == nullptr) {
                entry.selectionTarget = selectionTarget;
            }
            return;
        }

        entries.push_back({camera, selectionTarget, camera == activeCamera});
    };

    for (const GameObject* object : objects) {
        if (object == nullptr) {
            continue;
        }
        const Camera* standaloneCamera = dynamic_cast<const Camera*>(object);
        appendUnique(standaloneCamera,
                     standaloneCamera != nullptr ? object : nullptr);
        appendUnique(object->attachedCamera(), object);
    }
    appendUnique(activeCamera, nullptr);
    return entries;
}

std::vector<const Camera*> collectCameraPointers(
    const std::vector<const GameObject*>& objects,
    const Camera* activeCamera) {
    std::vector<const Camera*> cameras;
    const std::vector<CameraVisualizationEntry> entries =
        collectCameraVisualizationEntries(objects, activeCamera);
    cameras.reserve(entries.size());
    for (const CameraVisualizationEntry& entry : entries) {
        cameras.push_back(entry.camera);
    }
    return cameras;
}

bool deriveWorldPosition(const GameObject& object,
                         glm::vec3& worldPosition) noexcept {
    worldPosition = {};
    const glm::mat4 worldTransform = object.worldTransformMatrix();
    if (!isFiniteMatrix(worldTransform)) {
        return false;
    }

    worldPosition = glm::vec3(worldTransform[3]);
    return isFiniteVector(worldPosition);
}

bool calculateCameraVisualizationPose(
    const Camera& camera, const GameObject* selectionTarget,
    CameraWorldPose& pose) noexcept {
    pose = {};
    const bool isStandaloneCamera =
        selectionTarget == static_cast<const GameObject*>(&camera) ||
        (selectionTarget == nullptr && camera.parent() != nullptr);
    if (isStandaloneCamera) {
        return camera.calculateWorldPose(pose);
    }

    if (!isFiniteVector(camera.position) || !isFiniteVector(camera.front) ||
        !isFiniteVector(camera.up)) {
        return false;
    }
    pose.position = camera.position;
    pose.front = camera.front;
    pose.up = camera.up;
    return true;
}

bool projectVulkanWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& vulkanProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept {
    screenPosition = {};
    if (!std::isfinite(renderPosition.x) || !std::isfinite(renderPosition.y) ||
        !std::isfinite(renderSize.x) ||
        !std::isfinite(renderSize.y) || renderSize.x <= 0.0f ||
        renderSize.y <= 0.0f) {
        return false;
    }
    glm::vec4 clip;
    glm::vec3 ndc;
    if (!getVisibleProjection(worldPoint, view, vulkanProjection, clip, ndc)) {
        return false;
    }
    screenPosition = renderPosition + glm::vec2(
        (ndc.x * 0.5f + 0.5f) * renderSize.x,
        (ndc.y * 0.5f + 0.5f) * renderSize.y);
    return isFiniteVector(screenPosition) &&
           std::abs(screenPosition.x) <= maximumImGuiCoordinateMagnitude &&
           std::abs(screenPosition.y) <= maximumImGuiCoordinateMagnitude;
}

bool projectVulkanDirectionToImGui(
    const glm::vec3& worldPosition, const glm::vec3& worldDirection,
    const glm::mat4& view, const glm::mat4& vulkanProjection,
    const glm::vec2& renderSize,
    glm::vec2& screenDirection) noexcept {
    screenDirection = {};
    if (!isFiniteVector(worldDirection) ||
        !std::isfinite(renderSize.x) || !std::isfinite(renderSize.y) ||
        renderSize.x <= 0.0f || renderSize.y <= 0.0f) {
        return false;
    }

    glm::vec4 clipPosition;
    glm::vec3 ndc;
    if (!getVisibleProjection(worldPosition, view, vulkanProjection,
                              clipPosition, ndc)) {
        return false;
    }
    const glm::vec4 clipDirection =
        vulkanProjection * view * glm::vec4(worldDirection, 0.0f);
    if (!isFiniteVector(clipDirection)) {
        return false;
    }
    const float inverseW = 1.0f / clipPosition.w;
    const glm::vec2 ndcDirection(
        (clipDirection.x - ndc.x * clipDirection.w) * inverseW,
        (clipDirection.y - ndc.y * clipDirection.w) * inverseW);
    screenDirection = ndcDirection * (renderSize * 0.5f);
    return isFiniteVector(screenDirection) &&
           std::abs(screenDirection.x) <= maximumImGuiCoordinateMagnitude &&
           std::abs(screenDirection.y) <= maximumImGuiCoordinateMagnitude;
}

bool projectImGuizmoWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& imguizmoProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept {
    screenPosition = {};
    if (!isFiniteVector(worldPoint) || !isFiniteMatrix(view) ||
        !isFiniteMatrix(imguizmoProjection) ||
        !std::isfinite(renderPosition.x) ||
        !std::isfinite(renderPosition.y) || !std::isfinite(renderSize.x) ||
        !std::isfinite(renderSize.y) || renderSize.x <= 0.0f ||
        renderSize.y <= 0.0f) {
        return false;
    }
    const glm::vec4 clip =
        imguizmoProjection * view * glm::vec4(worldPoint, 1.0f);
    if (!std::isfinite(clip.w) || std::abs(clip.w) < epsilon) {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (!isFiniteVector(ndc)) {
        return false;
    }
    screenPosition = renderPosition + glm::vec2(
        (ndc.x * 0.5f + 0.5f) * renderSize.x,
        (1.0f - (ndc.y * 0.5f + 0.5f)) * renderSize.y);
    return std::isfinite(screenPosition.x) && std::isfinite(screenPosition.y);
}

}  // namespace editor_picking
