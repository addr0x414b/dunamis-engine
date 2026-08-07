#include "editor_picking.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "../scene/camera.h"
#include "../scene/game_object.h"

namespace editor_picking {
namespace {

constexpr float epsilon = 1.0e-6f;
constexpr float minimumLocalDirectionLength = 1.0e-12f;
constexpr double triangleRelativeTolerance = 1.0e-12;

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

glm::mat4 makeModelMatrix(const glm::vec3& position,
                          const glm::vec3& rotation,
                          const glm::vec3& scale) noexcept {
    glm::mat4 model(1.0f);
    model = glm::translate(model, position);
    model *= makeRotationMatrix(rotation);
    return glm::scale(model, scale);
}

glm::mat4 makeRotationMatrix(const glm::vec3& rotation) noexcept {
    glm::mat4 result(1.0f);
    result = glm::rotate(result, glm::radians(rotation.x),
                         glm::vec3(1.0f, 0.0f, 0.0f));
    result = glm::rotate(result, glm::radians(rotation.y),
                         glm::vec3(0.0f, 1.0f, 0.0f));
    result = glm::rotate(result, glm::radians(rotation.z),
                         glm::vec3(0.0f, 0.0f, 1.0f));
    return result;
}

glm::mat4 makeTranslationMatrix(const glm::vec3& translation) noexcept {
    return glm::translate(glm::mat4(1.0f), translation);
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

AggregateBounds aggregateBounds(
    const std::vector<MeshInstance>& instances) noexcept {
    AggregateBounds aggregate;
    for (const MeshInstance& instance : instances) {
        const Mesh::Bounds& bounds = instance.mesh.bounds;
        if (!bounds.valid || !isFiniteVector(bounds.minimum) ||
            !isFiniteVector(bounds.maximum) ||
            glm::any(glm::greaterThan(bounds.minimum, bounds.maximum))) {
            continue;
        }
        if (!aggregate.valid) {
            aggregate.minimum = bounds.minimum;
            aggregate.maximum = bounds.maximum;
            aggregate.valid = true;
            continue;
        }
        aggregate.minimum = glm::min(aggregate.minimum, bounds.minimum);
        aggregate.maximum = glm::max(aggregate.maximum, bounds.maximum);
    }
    return aggregate;
}

glm::vec3 worldBoundsCenter(const AggregateBounds& bounds,
                            const glm::mat4& model,
                            const glm::vec3& fallback) noexcept {
    if (!bounds.valid || !isFiniteMatrix(model)) {
        return fallback;
    }
    const glm::vec3 localCenter = (bounds.minimum + bounds.maximum) * 0.5f;
    const glm::vec3 worldCenter = glm::vec3(
        model * glm::vec4(localCenter, 1.0f));
    return isFiniteVector(worldCenter) ? worldCenter : fallback;
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
    const glm::vec4 clip = vulkanProjection * view * glm::vec4(worldPoint, 1.0f);
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
        !isFiniteMatrix(imguizmoProjection) || !std::isfinite(renderPosition.x) ||
        !std::isfinite(renderPosition.y) || !std::isfinite(renderSize.x) ||
        !std::isfinite(renderSize.y) || renderSize.x <= 0.0f ||
        renderSize.y <= 0.0f) {
        return false;
    }
    const glm::vec4 clip = imguizmoProjection * view * glm::vec4(worldPoint, 1.0f);
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

bool intersectAabb(const Ray& ray, const Mesh::Bounds& bounds,
                   float& distance) noexcept {
    distance = 0.0f;
    if (!bounds.valid || !isFiniteVector(ray.origin) ||
        !isFiniteVector(ray.direction) || !isFiniteVector(bounds.minimum) ||
        !isFiniteVector(bounds.maximum) ||
        glm::any(glm::greaterThan(bounds.minimum, bounds.maximum))) {
        return false;
    }

    float nearDistance = 0.0f;
    float farDistance = std::numeric_limits<float>::infinity();
    for (int axis = 0; axis < 3; ++axis) {
        if (std::abs(ray.direction[axis]) < epsilon) {
            if (ray.origin[axis] < bounds.minimum[axis] ||
                ray.origin[axis] > bounds.maximum[axis]) {
                return false;
            }
            continue;
        }

        float first = (bounds.minimum[axis] - ray.origin[axis]) /
                      ray.direction[axis];
        float second = (bounds.maximum[axis] - ray.origin[axis]) /
                       ray.direction[axis];
        if (first > second) {
            std::swap(first, second);
        }
        nearDistance = glm::max(nearDistance, first);
        farDistance = glm::min(farDistance, second);
        if (nearDistance > farDistance) {
            return false;
        }
    }
    if (!std::isfinite(farDistance) || farDistance < 0.0f) {
        return false;
    }
    distance = nearDistance;
    return std::isfinite(distance);
}

bool intersectTriangle(const Ray& ray, const glm::vec3& first,
                       const glm::vec3& second, const glm::vec3& third,
                       float& distance) noexcept {
    distance = 0.0f;
    if (!isFiniteVector(ray.origin) || !isFiniteVector(ray.direction) ||
        !isFiniteVector(first) || !isFiniteVector(second) ||
        !isFiniteVector(third)) {
        return false;
    }
    const glm::dvec3 firstEdge = glm::dvec3(second) - glm::dvec3(first);
    const glm::dvec3 secondEdge = glm::dvec3(third) - glm::dvec3(first);
    const double edgeScale = glm::length(firstEdge) * glm::length(secondEdge);
    if (!std::isfinite(edgeScale) || edgeScale <=
        std::numeric_limits<double>::min()) {
        return false;
    }
    const glm::dvec3 direction(ray.direction);
    const glm::dvec3 perpendicular = glm::cross(direction, secondEdge);
    const double determinant = glm::dot(firstEdge, perpendicular);
    if (!std::isfinite(determinant) ||
        std::abs(determinant) <= edgeScale * triangleRelativeTolerance) {
        return false;
    }
    const double inverseDeterminant = 1.0 / determinant;
    const glm::dvec3 offset = glm::dvec3(ray.origin) - glm::dvec3(first);
    const double u = glm::dot(offset, perpendicular) * inverseDeterminant;
    if (!std::isfinite(u) || u < 0.0f || u > 1.0f) {
        return false;
    }
    const glm::dvec3 q = glm::cross(offset, firstEdge);
    const double v = glm::dot(direction, q) * inverseDeterminant;
    if (!std::isfinite(v) || v < 0.0f || u + v > 1.0f) {
        return false;
    }
    const double hitDistance = glm::dot(secondEdge, q) * inverseDeterminant;
    if (!std::isfinite(hitDistance) || hitDistance < 0.0 ||
        hitDistance > std::numeric_limits<float>::max()) {
        return false;
    }
    distance = static_cast<float>(hitDistance);
    return true;
}

bool intersectMeshWorld(const Ray& worldRay, const Mesh& mesh,
                        const glm::mat4& model, float& worldDistance,
                        MeshPickDiagnostics* diagnostics) noexcept {
    worldDistance = 0.0f;
    if (diagnostics != nullptr) {
        *diagnostics = {};
    }
    if (!mesh.bounds.valid || mesh.indices.size() < 3 ||
        !isFiniteMatrix(model) || !isFiniteVector(worldRay.origin) ||
        !isFiniteVector(worldRay.direction)) {
        return false;
    }
    const float determinant = glm::determinant(model);
    if (!std::isfinite(determinant) || std::abs(determinant) < epsilon) {
        return false;
    }
    if (diagnostics != nullptr) {
        diagnostics->transformInvertible = true;
    }
    const glm::mat4 inverseModel = glm::inverse(model);
    const glm::vec3 localOrigin = glm::vec3(inverseModel *
                                             glm::vec4(worldRay.origin, 1.0f));
    glm::vec3 localDirection = glm::vec3(inverseModel *
                                         glm::vec4(worldRay.direction, 0.0f));
    const float localDirectionLength = glm::length(localDirection);
    if (!isFiniteVector(localOrigin) || !isFiniteVector(localDirection) ||
        !std::isfinite(localDirectionLength) ||
        localDirectionLength <= minimumLocalDirectionLength) {
        return false;
    }
    localDirection /= localDirectionLength;
    float ignoredDistance = 0.0f;
    if (!intersectAabb({localOrigin, localDirection}, mesh.bounds,
                       ignoredDistance)) {
        return false;
    }
    if (diagnostics != nullptr) {
        diagnostics->broadPhasePassed = true;
        diagnostics->triangleTestingReached = true;
    }

    bool hit = false;
    float closestDistance = std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index + 2 < mesh.indices.size(); index += 3) {
        const std::uint32_t firstIndex = mesh.indices[index];
        const std::uint32_t secondIndex = mesh.indices[index + 1];
        const std::uint32_t thirdIndex = mesh.indices[index + 2];
        if (firstIndex >= mesh.vertices.size() || secondIndex >= mesh.vertices.size() ||
            thirdIndex >= mesh.vertices.size()) {
            continue;
        }
        float localDistance = 0.0f;
        if (!intersectTriangle({localOrigin, localDirection},
                               mesh.vertices[firstIndex].pos,
                               mesh.vertices[secondIndex].pos,
                               mesh.vertices[thirdIndex].pos, localDistance)) {
            continue;
        }
        const glm::vec3 localHit = localOrigin + localDistance * localDirection;
        const glm::vec3 worldHit = glm::vec3(model * glm::vec4(localHit, 1.0f));
        const float distance = glm::length(worldHit - worldRay.origin);
        if (std::isfinite(distance) && distance < closestDistance) {
            closestDistance = distance;
            hit = true;
        }
    }
    if (hit) {
        worldDistance = closestDistance;
        if (diagnostics != nullptr) {
            diagnostics->closestWorldDistance = closestDistance;
        }
    }
    return hit;
}

}  // namespace editor_picking
