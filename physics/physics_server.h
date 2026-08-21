#ifndef PHYSICS_SERVER_H
#define PHYSICS_SERVER_H

#include <cstddef>
#include <memory>

#include "../core/result.h"
#include "../scene/runtime_transform_edit.h"

class Scene;
class GameObject;

namespace physics {
struct CookedShape;
}

class PhysicsStepAccumulator {
public:
    static constexpr float fixedDeltaTime = 1.0f / 60.0f;
    static constexpr std::size_t maxSubsteps = 6;

    [[nodiscard]] std::size_t addFrameDelta(float deltaTime) noexcept;
    void reset() noexcept;

private:
    double accumulator_ = 0.0;
};

class PhysicsServer {
public:
    PhysicsServer();
    ~PhysicsServer() noexcept;
    PhysicsServer(const PhysicsServer&) = delete;
    PhysicsServer& operator=(const PhysicsServer&) = delete;

    [[nodiscard]] Result initialize();
    // Acquires the authored collision shape without creating a body or
    // changing runtime physics state. Static mesh shapes use the same RAM /
    // persistent cache path as runtime physics.
    [[nodiscard]] Result acquireCollisionShape(const GameObject& object,
                                               physics::CookedShape& output);
    [[nodiscard]] Result beginRuntimeSession(Scene& runtimeScene);
    void applyRuntimeTransformEdit(const RuntimeTransformEdit& edit);
    void update();
    void endRuntimeSession() noexcept;
    void shutdown() noexcept;
    [[nodiscard]] bool runtimeSessionActive() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    PhysicsStepAccumulator accumulator_;
};

#endif
