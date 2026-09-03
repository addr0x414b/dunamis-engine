#include "editor_picking.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

#include "../scene/game_object.h"
#include "../scene/model_renderable.h"
#include "../scene/scene.h"

namespace editor_picking {
namespace {

constexpr float epsilon = 1.0e-6f;
constexpr float minimumLocalDirectionLength = 1.0e-12f;
constexpr double triangleRelativeTolerance = 1.0e-12;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
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
    if (!std::isfinite(edgeScale) ||
        edgeScale <= std::numeric_limits<double>::min()) {
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
    const glm::vec3 localOrigin = glm::vec3(
        inverseModel * glm::vec4(worldRay.origin, 1.0f));
    glm::vec3 localDirection = glm::vec3(
        inverseModel * glm::vec4(worldRay.direction, 0.0f));
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
        if (firstIndex >= mesh.vertices.size() ||
            secondIndex >= mesh.vertices.size() ||
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
        const glm::vec3 worldHit = glm::vec3(
            model * glm::vec4(localHit, 1.0f));
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

GameObject* pickClosestObject(Scene& scene, const Ray& worldRay) noexcept {
    GameObject* closestObject = nullptr;
    float closestDistance = std::numeric_limits<float>::infinity();
    for (const auto& owner : scene.gameObjects()) {
        GameObject* object = owner.get();
        if (object == nullptr) {
            continue;
        }
        const glm::mat4 model = object->worldTransformMatrix();
        for (const MeshInstance& instance :
             object->modelRenderable().meshInstances()) {
            float distance = 0.0f;
            if (intersectMeshWorld(worldRay, instance.mesh, model, distance) &&
                distance < closestDistance) {
                closestDistance = distance;
                closestObject = object;
            }
        }
    }
    return closestObject;
}

}  // namespace editor_picking
