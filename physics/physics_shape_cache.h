#ifndef PHYSICS_SHAPE_CACHE_H
#define PHYSICS_SHAPE_CACHE_H

// Jolt requires this include ordering.
#include <Jolt/Jolt.h>

#include <Jolt/Physics/Collision/Shape/Shape.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include <glm/glm.hpp>

#include "../assets/model_asset.h"

namespace physics {

// Increment when the cache wrapper or static mesh cooking semantics change.
constexpr std::uint32_t physicsShapeCacheVersion = 1;
constexpr std::uint32_t physicsShapeCookingVersion = 1;
// Version 2 invalidates cooked shapes made with the former 100 DU/meter
// convention, even though the cache identity also records the conversion.
constexpr std::uint32_t physicsUnitConventionVersion = 2;
constexpr const char* joltShapeCacheCompatibilityTag = "Jolt-5.6.0";

struct StaticMeshShapeCacheKey {
    std::string modelIdentity;
    std::uint64_t geometryFingerprint = 0;
    std::array<std::uint32_t, 3> scaleBits{};
    std::uint8_t colliderType = 0;
    std::uint64_t stableHash = 0;

    [[nodiscard]] bool operator==(const StaticMeshShapeCacheKey& other) const
        noexcept;
};

// Raw local positions, mesh boundaries, and indices are the only model data
// in this identity. Position and rotation deliberately remain body state.
[[nodiscard]] StaticMeshShapeCacheKey makeStaticMeshShapeCacheKey(
    const std::string& modelIdentity,
    const std::vector<MeshInstance>& meshInstances, const glm::vec3& scale,
    std::uint8_t colliderType = 0);

enum class ShapeCacheLoadStatus { Hit, Miss, Invalid };

struct ShapeCacheLoadResult {
    ShapeCacheLoadStatus status = ShapeCacheLoadStatus::Miss;
    JPH::ShapeRefC shape;
    std::string message;
    std::chrono::steady_clock::duration readDuration{};
    std::chrono::steady_clock::duration restoreDuration{};
};

// Disposable project-local storage. Jolt Factory and types must be registered
// before load is called.
class PhysicsShapeCache {
public:
    explicit PhysicsShapeCache(std::filesystem::path directory =
                                   defaultDirectory());

    [[nodiscard]] static std::filesystem::path defaultDirectory();
    [[nodiscard]] std::filesystem::path pathFor(
        const StaticMeshShapeCacheKey& key) const;
    [[nodiscard]] ShapeCacheLoadResult load(
        const StaticMeshShapeCacheKey& key) const;
    void discard(const StaticMeshShapeCacheKey& key) const noexcept;
    [[nodiscard]] bool save(const StaticMeshShapeCacheKey& key,
                            const JPH::Shape& shape,
                            std::string& error) const noexcept;

private:
    std::filesystem::path directory_;
};

}  // namespace physics

#endif
