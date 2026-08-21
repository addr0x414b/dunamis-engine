#ifndef PHYSICS_DEBUG_RENDERER_H
#define PHYSICS_DEBUG_RENDERER_H

// Jolt requires this include ordering.
#include <Jolt/Jolt.h>
#include <Jolt/Renderer/DebugRenderer.h>

#include <cstdint>
#include <atomic>
#include <vector>

#include <glm/glm.hpp>

class PhysicsDebugRenderer final : public JPH::DebugRenderer {
public:
    struct BatchData final : public JPH::RefTargetVirtual {
        void AddRef() override { references_.fetch_add(1, std::memory_order_relaxed); }
        void Release() override { if (references_.fetch_sub(1, std::memory_order_acq_rel) == 1) delete this; }
        std::vector<glm::vec3> vertices; // Already converted to render units.
        std::vector<std::uint32_t> lineIndices;
    private:
        std::atomic_uint32_t references_{0};
    };
    struct DrawCommand {
        const BatchData* batch = nullptr;
        glm::mat4 model{1.0f};
    };

    PhysicsDebugRenderer();
    ~PhysicsDebugRenderer() override = default;
    PhysicsDebugRenderer(const PhysicsDebugRenderer&) = delete;
    PhysicsDebugRenderer& operator=(const PhysicsDebugRenderer&) = delete;

    void beginFrame() noexcept;
    [[nodiscard]] const std::vector<DrawCommand>& drawCommands() const noexcept;

    static bool makeLineIndices(const std::vector<std::uint32_t>& triangles,
                                std::size_t vertexCount,
                                std::vector<std::uint32_t>& output) noexcept;

    void DrawLine(JPH::RVec3Arg from, JPH::RVec3Arg to, JPH::ColorArg color) override;
    void DrawTriangle(JPH::RVec3Arg first, JPH::RVec3Arg second,
                      JPH::RVec3Arg third, JPH::ColorArg color,
                      ECastShadow castShadow) override;
    void DrawText3D(JPH::RVec3Arg position, const JPH::string_view& text,
                    JPH::ColorArg color, float height) override;
    Batch CreateTriangleBatch(const Triangle* triangles, int triangleCount) override;
    Batch CreateTriangleBatch(const Vertex* vertices, int vertexCount,
                              const JPH::uint32* indices, int indexCount) override;
    void DrawGeometry(JPH::RMat44Arg model, const JPH::AABox& worldBounds,
                      float lodScaleSq, JPH::ColorArg color,
                      const GeometryRef& geometry, ECullMode cullMode,
                      ECastShadow castShadow, EDrawMode drawMode) override;

private:
    [[nodiscard]] Batch makeBatch(const std::vector<glm::vec3>& vertices,
                                  const std::vector<std::uint32_t>& triangles);
    std::vector<DrawCommand> drawCommands_;
};

#endif
