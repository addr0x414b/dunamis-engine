#include "editor_picking.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "../scene/camera.h"
#include "../scene/game_object.h"

namespace editor_picking {
namespace {

constexpr float epsilon = 1.0e-6f;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool isFiniteVector(const glm::vec2& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y);
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

}  // namespace

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

bool projectVulkanWorldToImGui(
    const glm::vec3& worldPoint, const glm::mat4& view,
    const glm::mat4& vulkanProjection, const glm::vec2& renderPosition,
    const glm::vec2& renderSize, glm::vec2& screenPosition) noexcept {
    screenPosition = {};
    if (!isFiniteVector(worldPoint) || !isFiniteMatrix(view) ||
        !isFiniteMatrix(vulkanProjection) || !std::isfinite(renderPosition.x) ||
        !std::isfinite(renderPosition.y) || !std::isfinite(renderSize.x) ||
        !std::isfinite(renderSize.y) || renderSize.x <= 0.0f ||
        renderSize.y <= 0.0f) {
        return false;
    }
    const glm::vec4 clip =
        vulkanProjection * view * glm::vec4(worldPoint, 1.0f);
    if (!std::isfinite(clip.w) || std::abs(clip.w) < epsilon) {
        return false;
    }
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (!isFiniteVector(ndc)) {
        return false;
    }
    screenPosition = renderPosition + glm::vec2(
        (ndc.x * 0.5f + 0.5f) * renderSize.x,
        (ndc.y * 0.5f + 0.5f) * renderSize.y);
    return std::isfinite(screenPosition.x) && std::isfinite(screenPosition.y);
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
