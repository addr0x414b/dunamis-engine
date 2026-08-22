#ifndef EDITOR_EDITOR_PICKING_H
#define EDITOR_EDITOR_PICKING_H

#include <vector>

#include <glm/glm.hpp>

#include "../assets/model_asset.h"

class GameObject;
class Scene;

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

[[nodiscard]] AggregateBounds aggregateBounds(
    const std::vector<MeshInstance>& instances) noexcept;
[[nodiscard]] glm::vec3 worldBoundsCenter(
    const AggregateBounds& bounds, const glm::mat4& model,
    const glm::vec3& fallback) noexcept;
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
[[nodiscard]] GameObject* pickClosestObject(Scene& scene,
                                             const Ray& worldRay) noexcept;

}  // namespace editor_picking

#endif
