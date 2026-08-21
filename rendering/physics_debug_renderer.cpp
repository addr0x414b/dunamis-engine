#include "physics_debug_renderer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <unordered_map>
#include <unordered_set>

#include "../physics/physics_units.h"
#include "../physics/jolt_shape_builder.h"

namespace {
struct PositionKey {
    std::array<std::uint32_t, 3> values{};
    bool operator==(const PositionKey& other) const noexcept { return values == other.values; }
};
struct PositionKeyHash {
    std::size_t operator()(const PositionKey& key) const noexcept {
        return (static_cast<std::size_t>(key.values[0]) << 1) ^
               (static_cast<std::size_t>(key.values[1]) << 11) ^ key.values[2];
    }
};
PositionKey keyFor(const JPH::Float3& point) noexcept {
    PositionKey key;
    const float values[] = {point.x, point.y, point.z};
    std::memcpy(key.values.data(), values, sizeof(values));
    return key;
}
glm::vec3 toRender(const JPH::Float3& point) noexcept {
    return physics::metersToDunamis(glm::vec3(point.x, point.y, point.z));
}
} // namespace

PhysicsDebugRenderer::PhysicsDebugRenderer() { Initialize(); }
void PhysicsDebugRenderer::beginFrame() noexcept { drawCommands_.clear(); }
const std::vector<PhysicsDebugRenderer::DrawCommand>& PhysicsDebugRenderer::drawCommands() const noexcept { return drawCommands_; }

bool PhysicsDebugRenderer::makeLineIndices(const std::vector<std::uint32_t>& triangles,
                                           std::size_t vertexCount,
                                           std::vector<std::uint32_t>& output) noexcept {
    output.clear();
    if (triangles.size() % 3 != 0) return false;
    try {
        std::unordered_set<std::uint64_t> edges;
        edges.reserve(triangles.size());
        output.reserve(triangles.size() * 2);
        const auto append = [&edges, &output, vertexCount](std::uint32_t first, std::uint32_t second) {
            if (first >= vertexCount || second >= vertexCount || first == second) return false;
            const std::uint32_t low = std::min(first, second);
            const std::uint32_t high = std::max(first, second);
            const std::uint64_t key = (static_cast<std::uint64_t>(low) << 32) | high;
            if (edges.insert(key).second) { output.push_back(low); output.push_back(high); }
            return true;
        };
        for (std::size_t i = 0; i < triangles.size(); i += 3)
            if (!append(triangles[i], triangles[i + 1]) || !append(triangles[i + 1], triangles[i + 2]) || !append(triangles[i + 2], triangles[i])) {
                output.clear(); return false;
            }
        return true;
    } catch (...) { output.clear(); return false; }
}

PhysicsDebugRenderer::Batch PhysicsDebugRenderer::makeBatch(
    const std::vector<glm::vec3>& vertices, const std::vector<std::uint32_t>& triangles) {
    auto* batch = new BatchData();
    batch->vertices = vertices;
    if (!makeLineIndices(triangles, batch->vertices.size(), batch->lineIndices)) return {};
    return batch;
}

void PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg, JPH::RVec3Arg, JPH::ColorArg) {}
void PhysicsDebugRenderer::DrawTriangle(JPH::RVec3Arg, JPH::RVec3Arg, JPH::RVec3Arg, JPH::ColorArg, ECastShadow) {}
void PhysicsDebugRenderer::DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) {}

PhysicsDebugRenderer::Batch PhysicsDebugRenderer::CreateTriangleBatch(const Triangle* triangles, int triangleCount) {
    if (triangles == nullptr || triangleCount <= 0) return {};
    std::vector<glm::vec3> vertices;
    std::vector<std::uint32_t> indices;
    std::unordered_map<PositionKey, std::uint32_t, PositionKeyHash> unique;
    vertices.reserve(static_cast<std::size_t>(triangleCount) * 3);
    indices.reserve(static_cast<std::size_t>(triangleCount) * 3);
    unique.reserve(static_cast<std::size_t>(triangleCount) * 3);
    for (int triangle = 0; triangle < triangleCount; ++triangle) for (int vertex = 0; vertex < 3; ++vertex) {
        const JPH::Float3& position = triangles[triangle].mV[vertex].mPosition;
        const PositionKey key = keyFor(position);
        const auto [iterator, inserted] = unique.emplace(key, static_cast<std::uint32_t>(vertices.size()));
        if (inserted) vertices.push_back(toRender(position));
        indices.push_back(iterator->second);
    }
    return makeBatch(vertices, indices);
}

PhysicsDebugRenderer::Batch PhysicsDebugRenderer::CreateTriangleBatch(const Vertex* vertices, int vertexCount,
                                                                        const JPH::uint32* indices, int indexCount) {
    if (vertices == nullptr || indices == nullptr || vertexCount <= 0 || indexCount <= 0) return {};
    std::vector<glm::vec3> copied;
    copied.reserve(vertexCount);
    for (int i = 0; i < vertexCount; ++i) copied.push_back(toRender(vertices[i].mPosition));
    std::vector<std::uint32_t> copiedIndices(indices, indices + indexCount);
    return makeBatch(copied, copiedIndices);
}

void PhysicsDebugRenderer::DrawGeometry(JPH::RMat44Arg model, const JPH::AABox&, float,
                                        JPH::ColorArg, const GeometryRef& geometry, ECullMode,
                                        ECastShadow, EDrawMode drawMode) {
    if (drawMode != EDrawMode::Wireframe || geometry == nullptr || geometry->mLODs.empty()) return;
    const auto* batch = static_cast<const BatchData*>(geometry->mLODs.front().mTriangleBatch.GetPtr());
    if (batch != nullptr && !batch->lineIndices.empty()) drawCommands_.push_back({batch, physics::joltTransformToDunamis(model)});
}
