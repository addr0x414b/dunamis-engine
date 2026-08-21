#include "physics_debug_renderer.h"

#include <limits>

#include "../physics/physics_units.h"
#include "../physics/jolt_shape_builder.h"

namespace {
glm::vec3 toRender(const JPH::Float3& point) noexcept {
    return physics::metersToDunamis(glm::vec3(point.x, point.y, point.z));
}
} // namespace

PhysicsDebugRenderer::PhysicsDebugRenderer() {
    Initialize();
    preparationStats_ = {};
}

void PhysicsDebugRenderer::beginFrame() noexcept {
    drawCommands_.clear();
    preparationStats_ = {};
}

const std::vector<PhysicsDebugRenderer::DrawCommand>& PhysicsDebugRenderer::drawCommands() const noexcept { return drawCommands_; }

bool PhysicsDebugRenderer::makeTriangleEdgeIndices(
    const std::vector<std::uint32_t>& triangles, std::size_t vertexCount,
    std::vector<std::uint32_t>& output) noexcept {
    output.clear();
    if (triangles.size() % 3 != 0) return false;
    try {
        if (triangles.size() > std::numeric_limits<std::size_t>::max() / 2) return false;
        output.reserve(triangles.size() * 2);
        for (std::size_t i = 0; i < triangles.size(); i += 3) {
            const std::uint32_t first = triangles[i];
            const std::uint32_t second = triangles[i + 1];
            const std::uint32_t third = triangles[i + 2];
            if (first >= vertexCount || second >= vertexCount || third >= vertexCount) {
                output.clear();
                return false;
            }
            output.push_back(first);
            output.push_back(second);
            output.push_back(second);
            output.push_back(third);
            output.push_back(third);
            output.push_back(first);
        }
        return true;
    } catch (...) { output.clear(); return false; }
}

PhysicsDebugRenderer::PreparationStats
PhysicsDebugRenderer::consumePreparationStats() noexcept {
    const PreparationStats result = preparationStats_;
    preparationStats_ = {};
    return result;
}

PhysicsDebugRenderer::Batch PhysicsDebugRenderer::makeBatch(
    const std::vector<glm::vec3>& vertices, const std::vector<std::uint32_t>& triangles) {
    auto* batch = new BatchData();
    batch->vertices = vertices;
    const auto conversionStart = std::chrono::steady_clock::now();
    const bool converted = makeTriangleEdgeIndices(
        triangles, batch->vertices.size(), batch->lineIndices);
    preparationStats_.triangleEdgeConversion +=
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - conversionStart);
    if (!converted) {
        delete batch;
        return {};
    }
    ++preparationStats_.batches;
    return batch;
}

void PhysicsDebugRenderer::DrawLine(JPH::RVec3Arg, JPH::RVec3Arg, JPH::ColorArg) {}
void PhysicsDebugRenderer::DrawTriangle(JPH::RVec3Arg, JPH::RVec3Arg, JPH::RVec3Arg, JPH::ColorArg, ECastShadow) {}
void PhysicsDebugRenderer::DrawText3D(JPH::RVec3Arg, const JPH::string_view&, JPH::ColorArg, float) {}

PhysicsDebugRenderer::Batch PhysicsDebugRenderer::CreateTriangleBatch(const Triangle* triangles, int triangleCount) {
    if (triangles == nullptr || triangleCount <= 0) return {};
    const auto triangleCountSize = static_cast<std::size_t>(triangleCount);
    if (triangleCountSize > std::numeric_limits<std::size_t>::max() / 3 ||
        triangleCountSize > std::numeric_limits<std::uint32_t>::max() / 3) {
        return {};
    }
    std::vector<glm::vec3> vertices;
    std::vector<std::uint32_t> indices;
    vertices.reserve(triangleCountSize * 3);
    indices.reserve(triangleCountSize * 3);
    for (int triangle = 0; triangle < triangleCount; ++triangle) for (int vertex = 0; vertex < 3; ++vertex) {
        const JPH::Float3& position = triangles[triangle].mV[vertex].mPosition;
        vertices.push_back(toRender(position));
        indices.push_back(static_cast<std::uint32_t>(indices.size()));
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
