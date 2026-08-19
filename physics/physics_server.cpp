#include "physics_server.h"

// Jolt requires this to be included before every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdarg>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <exception>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <spdlog/spdlog.h>

#include <glm/gtc/quaternion.hpp>

#include "../core/time.h"
#include "../scene/game_object.h"
#include "../scene/loading_cache_key.h"
#include "../scene/scene.h"
#include "physics_mesh_builder.h"
#include "physics_units.h"

namespace {

namespace layers {
constexpr JPH::ObjectLayer nonMoving = 0;
constexpr JPH::ObjectLayer moving = 1;
constexpr JPH::ObjectLayer count = 2;
}  // namespace layers

namespace broadPhaseLayers {
const JPH::BroadPhaseLayer nonMoving(0);
const JPH::BroadPhaseLayer moving(1);
constexpr JPH::uint count = 2;
}  // namespace broadPhaseLayers

class ObjectLayerPairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer first,
                       JPH::ObjectLayer second) const override {
        return first == layers::moving ||
               (first == layers::nonMoving && second == layers::moving);
    }
};

class BroadPhaseLayerInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override {
        return broadPhaseLayers::count;
    }

    JPH::BroadPhaseLayer GetBroadPhaseLayer(
        JPH::ObjectLayer layer) const override {
        JPH_ASSERT(layer < layers::count);
        return layer == layers::nonMoving ? broadPhaseLayers::nonMoving
                                          : broadPhaseLayers::moving;
    }
};

class ObjectVsBroadPhaseLayerFilter final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer,
                       JPH::BroadPhaseLayer broadPhaseLayer) const override {
        return layer == layers::moving ||
               (layer == layers::nonMoving &&
                broadPhaseLayer == broadPhaseLayers::moving);
    }
};

void joltTrace(const char* format, ...) noexcept {
    try {
        char buffer[1024];
        va_list arguments;
        va_start(arguments, format);
        std::vsnprintf(buffer, sizeof(buffer), format, arguments);
        va_end(arguments);
        spdlog::info("Jolt: {}", buffer);
    } catch (...) {
        // Jolt callbacks must never leak exceptions across the C interface.
    }
}

#ifdef JPH_ENABLE_ASSERTS
bool joltAssertFailed(const char* expression, const char* message,
                      const char* file, JPH::uint line) noexcept {
    try {
        spdlog::error("Jolt assertion: {}:{} ({}) {}", file ? file : "?",
                      line, expression ? expression : "?",
                      message ? message : "");
    } catch (...) {
    }
    return false;
}
#endif

int safeWorkerCount() noexcept {
    const unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads <= 1) {
        return 1;
    }
    // Leave a core for rendering/main-thread work and avoid a large implicit
    // thread pool on workstation-class machines.
    return static_cast<int>(std::min(hardwareThreads - 1U, 8U));
}

std::string objectDescription(const GameObject& object) {
    return "object '" + object.name + "' (persistentId '" +
           object.persistentId + "')";
}

JPH::RVec3 toJoltPosition(const glm::vec3& positionDunamisUnits) {
    const glm::vec3 positionMeters =
        physics::dunamisToMeters(positionDunamisUnits);
    return JPH::RVec3(positionMeters.x, positionMeters.y, positionMeters.z);
}

JPH::Quat toJoltRotation(const glm::vec3& rotation) {
    const glm::quat quaternion = glm::quat_cast(
        physics::makeDunamisRotationMatrix(rotation));
    return JPH::Quat(quaternion.x, quaternion.y, quaternion.z, quaternion.w)
        .Normalized();
}

bool fromJoltRotation(const JPH::Quat& quaternion, glm::vec3& rotation) {
    const glm::quat glmQuaternion(quaternion.GetW(), quaternion.GetX(),
                                  quaternion.GetY(), quaternion.GetZ());
    return physics::extractDunamisRotation(glm::mat4_cast(glmQuaternion),
                                           rotation);
}

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

}  // namespace

struct PhysicsServer::Impl {
    struct StaticShapeKey {
        model_loading::ModelAssetCacheKey model;
        std::array<std::uint32_t, 3> scaleBits{};
        GameObject::PhysicsColliderType collider =
            GameObject::PhysicsColliderType::Mesh;

        bool operator==(const StaticShapeKey& other) const noexcept {
            return model == other.model && scaleBits == other.scaleBits &&
                   collider == other.collider;
        }
    };

    struct StaticShapeKeyHash {
        std::size_t operator()(const StaticShapeKey& key) const noexcept {
            std::size_t result =
                model_loading::ModelAssetCacheKeyHash{}(key.model);
            const auto combine = [&result](std::size_t value) {
                result ^= value + static_cast<std::size_t>(0x9e3779b9) +
                          (result << 6) + (result >> 2);
            };
            for (std::uint32_t value : key.scaleBits) combine(value);
            combine(static_cast<std::size_t>(key.collider));
            return result;
        }
    };

    static StaticShapeKey makeStaticShapeKey(const GameObject& object) {
        const auto normalizedBits = [](float value) {
            const float normalized = value == 0.0f ? 0.0f : value;
            std::uint32_t bits = 0;
            std::memcpy(&bits, &normalized, sizeof(bits));
            return bits;
        };
        StaticShapeKey key;
        key.model = model_loading::makeModelAssetCacheKey(
            object.modelPath, object.texturePath);
        key.scaleBits = std::array<std::uint32_t, 3>{
            normalizedBits(object.scale.x), normalizedBits(object.scale.y),
            normalizedBits(object.scale.z)};
        key.collider = object.physics.colliderType;
        return key;
    }

    struct RuntimeBody {
        JPH::BodyID id;
        GameObject* object = nullptr;
        bool isDynamic = false;
        bool editorOverride = false;
    };

    // These filters outlive physicsSystem because Jolt retains references.
    BroadPhaseLayerInterface broadPhaseLayerInterface;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilter objectLayerPairFilter;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
    std::unique_ptr<JPH::PhysicsSystem> physicsSystem;
    std::vector<RuntimeBody> runtimeBodies;
    std::unordered_map<StaticShapeKey, JPH::ShapeRefC, StaticShapeKeyHash>
        staticShapeCache;
    bool factoryCreated = false;
    bool typesRegistered = false;
    bool initialized = false;
};

std::size_t PhysicsStepAccumulator::addFrameDelta(float deltaTime) noexcept {
    if (!(deltaTime > 0.0f) || !std::isfinite(deltaTime)) {
        return 0;
    }
    accumulator_ += static_cast<double>(deltaTime);
    constexpr double fixed = 1.0 / 60.0;
    std::size_t steps = 0;
    while (accumulator_ + 1.0e-12 >= fixed && steps < maxSubsteps) {
        accumulator_ -= fixed;
        ++steps;
    }
    // Time already caps rendered deltas at 0.1 seconds. This second bound
    // discards leftover catch-up work instead of accumulating a spiral.
    if (steps == maxSubsteps && accumulator_ >= fixed) {
        accumulator_ = 0.0;
    }
    return steps;
}

void PhysicsStepAccumulator::reset() noexcept { accumulator_ = 0.0; }

PhysicsServer::PhysicsServer() : impl_(std::make_unique<Impl>()) {}

PhysicsServer::~PhysicsServer() noexcept { shutdown(); }

Result PhysicsServer::initialize() {
    if (impl_->initialized) {
        return Result::success();
    }
    try {
        JPH::RegisterDefaultAllocator();
        JPH::Trace = joltTrace;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = joltAssertFailed;
#endif
        JPH::Factory::sInstance = new JPH::Factory();
        impl_->factoryCreated = true;
        JPH::RegisterTypes();
        impl_->typesRegistered = true;

        // 10 MiB follows Jolt's documented practical starting point and
        // avoids allocator churn during simulation updates.
        impl_->tempAllocator = std::make_unique<JPH::TempAllocatorImpl>(
            10U * 1024U * 1024U);
        impl_->jobSystem = std::make_unique<JPH::JobSystemThreadPool>(
            JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers,
            safeWorkerCount());
        impl_->initialized = true;
        spdlog::info("Jolt Physics initialized ({} worker threads)",
                     safeWorkerCount());
        return Result::success();
    } catch (const std::exception& exception) {
        shutdown();
        return Result::failure("Jolt initialization failed: " +
                               std::string(exception.what()));
    } catch (...) {
        shutdown();
        return Result::failure("Jolt initialization failed unexpectedly");
    }
}

Result PhysicsServer::beginRuntimeSession(Scene& runtimeScene) {
    if (!impl_->initialized) {
        return Result::failure("PhysicsServer is not initialized");
    }
    if (impl_->physicsSystem) {
        return Result::failure("Runtime physics session already exists");
    }

    accumulator_.reset();
    try {
        using Clock = std::chrono::steady_clock;
        const Clock::time_point totalStart = Clock::now();
        Clock::duration staticMeshConversion{};
        Clock::duration staticShapeCooking{};
        Clock::duration convexCooking{};
        Clock::duration bodyCreation{};
        Clock::duration broadPhaseOptimization{};
        impl_->physicsSystem = std::make_unique<JPH::PhysicsSystem>();
        // Engine-v1 capacity: supports large static levels and many dynamic
        // bodies without the intentionally tiny HelloWorld limits.
        impl_->physicsSystem->Init(65536, 0, 65536, 10240,
                                   impl_->broadPhaseLayerInterface,
                                   impl_->objectVsBroadPhaseLayerFilter,
                                   impl_->objectLayerPairFilter);
        impl_->physicsSystem->SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
        JPH::BodyInterface& bodies = impl_->physicsSystem->GetBodyInterface();
        std::size_t staticCount = 0;
        std::size_t dynamicCount = 0;
        std::size_t staticShapeCacheHits = 0;
        std::size_t staticShapeCacheMisses = 0;

        for (const std::unique_ptr<GameObject>& objectOwner :
             runtimeScene.gameObjects()) {
            if (!objectOwner || !objectOwner->physics.enabled) {
                continue;
            }
            GameObject& object = *objectOwner;
            const GameObject::PhysicsBodySettings& settings = object.physics;
            if (settings.motionType == GameObject::PhysicsMotionType::Static &&
                settings.colliderType != GameObject::PhysicsColliderType::Mesh) {
                endRuntimeSession();
                return Result::failure("Static physics requires a mesh collider for " +
                                       objectDescription(object));
            }
            if (settings.motionType == GameObject::PhysicsMotionType::Dynamic &&
                settings.colliderType != GameObject::PhysicsColliderType::Sphere &&
                settings.colliderType != GameObject::PhysicsColliderType::ConvexHull) {
                endRuntimeSession();
                return Result::failure("Dynamic physics requires a sphere or convex-hull collider for " +
                                       objectDescription(object));
            }

            JPH::BodyID bodyId;
            if (settings.motionType == GameObject::PhysicsMotionType::Static) {
                const Impl::StaticShapeKey key =
                    Impl::makeStaticShapeKey(object);
                JPH::ShapeRefC shape;
                const auto cached = impl_->staticShapeCache.find(key);
                if (cached != impl_->staticShapeCache.end()) {
                    shape = cached->second;
                    ++staticShapeCacheHits;
                } else {
                    ++staticShapeCacheMisses;
                    physics::ScaledLocalTriangleMesh mesh;
                    const Clock::time_point conversionStart = Clock::now();
                    Result meshResult = physics::buildScaledLocalTriangleMesh(
                        object.meshInstances(), object.scale, mesh);
                    staticMeshConversion += Clock::now() - conversionStart;
                    if (!meshResult) {
                        endRuntimeSession();
                        return Result::failure("Failed to build static collision for " +
                                               objectDescription(object) + ": " +
                                               meshResult.error());
                    }
                    JPH::TriangleList triangles;
                    triangles.reserve(mesh.indices.size() / 3);
                    for (std::size_t index = 0; index < mesh.indices.size(); index += 3) {
                        const glm::vec3& a = mesh.vertices[mesh.indices[index]];
                        const glm::vec3& b = mesh.vertices[mesh.indices[index + 1]];
                        const glm::vec3& c = mesh.vertices[mesh.indices[index + 2]];
                        triangles.emplace_back(JPH::Vec3(a.x, a.y, a.z),
                                               JPH::Vec3(b.x, b.y, b.z),
                                               JPH::Vec3(c.x, c.y, c.z));
                    }
                    const Clock::time_point cookingStart = Clock::now();
                    JPH::MeshShapeSettings shapeSettings(std::move(triangles));
                    shapeSettings.SetEmbedded();
                    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
                    staticShapeCooking += Clock::now() - cookingStart;
                    if (shapeResult.HasError()) {
                        endRuntimeSession();
                        return Result::failure("Failed to create mesh shape for " +
                                               objectDescription(object) + ": " +
                                               shapeResult.GetError().c_str());
                    }
                    shape = shapeResult.Get();
                    impl_->staticShapeCache.emplace(key, shape);
                }
                JPH::BodyCreationSettings bodySettings(
                    shape.GetPtr(), toJoltPosition(object.position),
                    toJoltRotation(object.rotation), JPH::EMotionType::Static,
                    layers::nonMoving);
                const Clock::time_point creationStart = Clock::now();
                bodyId = bodies.CreateAndAddBody(bodySettings,
                                                  JPH::EActivation::DontActivate);
                bodyCreation += Clock::now() - creationStart;
                if (bodyId.IsInvalid()) {
                    endRuntimeSession();
                    return Result::failure("Failed to create static body for " +
                                           objectDescription(object));
                }
                ++staticCount;
            } else {
                JPH::ShapeRefC shape;
                if (settings.colliderType == GameObject::PhysicsColliderType::Sphere) {
                    if (!(settings.sphereRadius > 0.0f) ||
                        !std::isfinite(settings.sphereRadius)) {
                        endRuntimeSession();
                        return Result::failure("Dynamic sphere radius must be finite and positive for " +
                                               objectDescription(object));
                    }
                    shape = new JPH::SphereShape(
                        physics::dunamisToMeters(settings.sphereRadius));
                } else {
                    physics::LocalConvexHull hull;
                    Result hullResult = physics::buildScaledLocalConvexHull(
                        object.meshInstances(), object.scale, hull);
                    if (!hullResult) {
                        endRuntimeSession();
                        return Result::failure("Failed to build dynamic convex hull for " +
                                               objectDescription(object) + ": " + hullResult.error());
                    }
                    JPH::Array<JPH::Vec3> points;
                    points.reserve(hull.points.size());
                    for (const glm::vec3& point : hull.points) {
                        points.emplace_back(point.x, point.y, point.z);
                    }
                    const Clock::time_point cookingStart = Clock::now();
                    JPH::ConvexHullShapeSettings shapeSettings(points);
                    JPH::ShapeSettings::ShapeResult shapeResult = shapeSettings.Create();
                    convexCooking += Clock::now() - cookingStart;
                    if (shapeResult.HasError()) {
                        endRuntimeSession();
                        return Result::failure("Failed to create dynamic convex hull for " +
                                               objectDescription(object) + " (" +
                                               std::to_string(hull.points.size()) + " input points): " +
                                               shapeResult.GetError().c_str());
                    }
                    shape = shapeResult.Get();
                    spdlog::info("{} convex hull collider: {} local scaled input points",
                                 object.name, hull.points.size());
                }
                JPH::BodyCreationSettings bodySettings(
                    shape.GetPtr(), toJoltPosition(object.position),
                    toJoltRotation(object.rotation), JPH::EMotionType::Dynamic,
                    layers::moving);
                const Clock::time_point creationStart = Clock::now();
                bodyId = bodies.CreateAndAddBody(bodySettings,
                                                  JPH::EActivation::Activate);
                bodyCreation += Clock::now() - creationStart;
                if (bodyId.IsInvalid()) {
                    endRuntimeSession();
                    return Result::failure("Failed to create dynamic body for " +
                                           objectDescription(object));
                }
                ++dynamicCount;
            }
            impl_->runtimeBodies.push_back(
                {bodyId, &object,
                 settings.motionType == GameObject::PhysicsMotionType::Dynamic});
        }
        const Clock::time_point broadPhaseStart = Clock::now();
        impl_->physicsSystem->OptimizeBroadPhase();
        broadPhaseOptimization += Clock::now() - broadPhaseStart;
        spdlog::info("Runtime physics world created: {} static, {} dynamic",
                     staticCount, dynamicCount);
        spdlog::info("Physics startup: static mesh conversion {:.2f} ms, static shape cooking {:.2f} ms, dynamic convex cooking {:.2f} ms, body creation {:.2f} ms, broadphase optimization {:.2f} ms, total {:.2f} ms; static shape cache hits {}, misses {}",
                     milliseconds(staticMeshConversion), milliseconds(staticShapeCooking),
                     milliseconds(convexCooking), milliseconds(bodyCreation),
                     milliseconds(broadPhaseOptimization),
                     milliseconds(Clock::now() - totalStart),
                     staticShapeCacheHits, staticShapeCacheMisses);
        return Result::success();
    } catch (const std::exception& exception) {
        endRuntimeSession();
        return Result::failure("Runtime physics setup failed: " +
                               std::string(exception.what()));
    } catch (...) {
        endRuntimeSession();
        return Result::failure("Runtime physics setup failed unexpectedly");
    }
}

void PhysicsServer::applyRuntimeTransformEdit(const RuntimeTransformEdit& edit) {
    if (!impl_ || !impl_->physicsSystem || edit.object == nullptr) {
        return;
    }
    if (!std::isfinite(edit.position.x) || !std::isfinite(edit.position.y) ||
        !std::isfinite(edit.position.z) || !std::isfinite(edit.rotation.x) ||
        !std::isfinite(edit.rotation.y) || !std::isfinite(edit.rotation.z)) {
        return;
    }
    JPH::BodyInterface& bodies = impl_->physicsSystem->GetBodyInterface();
    for (Impl::RuntimeBody& body : impl_->runtimeBodies) {
        if (body.object != edit.object) {
            continue;
        }
        bodies.SetPositionAndRotation(body.id, toJoltPosition(edit.position),
                                      toJoltRotation(edit.rotation),
                                      body.isDynamic ? JPH::EActivation::Activate
                                                     : JPH::EActivation::DontActivate);
        if (body.isDynamic) {
            bodies.SetLinearAndAngularVelocity(body.id, JPH::Vec3::sZero(),
                                               JPH::Vec3::sZero());
        } else {
            std::vector<JPH::BodyID> dynamicBodyIds;
            dynamicBodyIds.reserve(impl_->runtimeBodies.size());
            for (const Impl::RuntimeBody& runtimeBody : impl_->runtimeBodies) {
                if (runtimeBody.isDynamic && !runtimeBody.id.IsInvalid()) {
                    dynamicBodyIds.push_back(runtimeBody.id);
                }
            }
            if (!dynamicBodyIds.empty()) {
                bodies.ActivateBodies(dynamicBodyIds.data(),
                                     static_cast<int>(dynamicBodyIds.size()));
            }
        }
        body.object->position = edit.position;
        body.object->rotation = edit.rotation;
        body.editorOverride = body.isDynamic && edit.manipulating;
        return;
    }
}

void PhysicsServer::update() {
    if (!impl_->physicsSystem) {
        return;
    }
    const std::size_t steps = accumulator_.addFrameDelta(Time::deltaTime());
    for (std::size_t step = 0; step < steps; ++step) {
        (void)impl_->physicsSystem->Update(PhysicsStepAccumulator::fixedDeltaTime,
                                           1, impl_->tempAllocator.get(),
                                           impl_->jobSystem.get());
    }
    JPH::BodyInterface& bodies = impl_->physicsSystem->GetBodyInterface();
    for (const Impl::RuntimeBody& body : impl_->runtimeBodies) {
        if (!body.isDynamic || body.object == nullptr || body.editorOverride) {
            continue;
        }
        const JPH::RVec3 position = bodies.GetPosition(body.id);
        body.object->position = physics::metersToDunamis(glm::vec3(
            static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
            static_cast<float>(position.GetZ())));
        glm::vec3 rotation;
        if (fromJoltRotation(bodies.GetRotation(body.id), rotation)) {
            body.object->rotation = rotation;
        }
    }
}

void PhysicsServer::endRuntimeSession() noexcept {
    accumulator_.reset();
    if (!impl_ || !impl_->physicsSystem) {
        return;
    }
    try {
        JPH::BodyInterface& bodies = impl_->physicsSystem->GetBodyInterface();
        for (const Impl::RuntimeBody& body : impl_->runtimeBodies) {
            if (!body.id.IsInvalid()) {
                bodies.RemoveBody(body.id);
                bodies.DestroyBody(body.id);
            }
        }
        impl_->runtimeBodies.clear();
        impl_->physicsSystem.reset();
        spdlog::info("Runtime physics world destroyed");
    } catch (...) {
        // Teardown is best-effort and must remain safe during rollback.
        impl_->runtimeBodies.clear();
        impl_->physicsSystem.reset();
    }
}

void PhysicsServer::shutdown() noexcept {
    if (!impl_) {
        return;
    }
    endRuntimeSession();
    try {
        impl_->staticShapeCache.clear();
        impl_->jobSystem.reset();
        impl_->tempAllocator.reset();
        if (impl_->typesRegistered) {
            JPH::UnregisterTypes();
            impl_->typesRegistered = false;
        }
        if (impl_->factoryCreated) {
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
            impl_->factoryCreated = false;
        }
        JPH::Trace = nullptr;
#ifdef JPH_ENABLE_ASSERTS
        JPH::AssertFailed = nullptr;
#endif
        if (impl_->initialized) {
            spdlog::info("Jolt Physics shutdown");
        }
        impl_->initialized = false;
    } catch (...) {
        // This is called from the engine destructor and cannot throw.
    }
}

bool PhysicsServer::runtimeSessionActive() const noexcept {
    return impl_ && static_cast<bool>(impl_->physicsSystem);
}
