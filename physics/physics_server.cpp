#include "physics_server.h"

// Jolt requires this to be included before every other Jolt header.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
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
#include "../math/transform_math.h"
#include "../scene/character.h"
#include "../scene/game_object.h"
#include "../scene/loading_cache_key.h"
#include "../scene/model_renderable.h"
#include "../scene/scene.h"
#include "collision_shapes.h"
#include "physics_shape_cache.h"
#include "physics_transforms.h"
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

bool makeDunamisWorldMatrix(const JPH::RVec3& position,
                            const JPH::Quat& quaternion,
                            glm::mat4& worldMatrix) {
    const glm::vec3 worldPosition = physics::metersToDunamis(glm::vec3(
        static_cast<float>(position.GetX()), static_cast<float>(position.GetY()),
        static_cast<float>(position.GetZ())));
    if (!transform_math::isFiniteVector(worldPosition) ||
        !std::isfinite(quaternion.GetX()) || !std::isfinite(quaternion.GetY()) ||
        !std::isfinite(quaternion.GetZ()) || !std::isfinite(quaternion.GetW())) {
        worldMatrix = glm::mat4(1.0f);
        return false;
    }
    const glm::quat glmQuaternion(quaternion.GetW(), quaternion.GetX(),
                                  quaternion.GetY(), quaternion.GetZ());
    const float quaternionLengthSquared = glm::dot(glmQuaternion, glmQuaternion);
    if (!std::isfinite(quaternionLengthSquared) ||
        quaternionLengthSquared <= 1.0e-12f) {
        worldMatrix = glm::mat4(1.0f);
        return false;
    }
    worldMatrix = glm::mat4_cast(glm::normalize(glmQuaternion));
    worldMatrix[3] = glm::vec4(worldPosition, 1.0f);
    return transform_math::isFiniteMatrix(worldMatrix);
}

bool samePhysicsWorldPose(const physics::PhysicsWorldPose& first,
                          const physics::PhysicsWorldPose& second) noexcept {
    constexpr float positionTolerance = 1.0e-4f;
    if (!transform_math::isFiniteVector(first.position) ||
        !transform_math::isFiniteVector(second.position) ||
        !transform_math::isFiniteVector(first.rotation) ||
        !transform_math::isFiniteVector(second.rotation)) {
        return false;
    }
    for (int axis = 0; axis < 3; ++axis) {
        if (std::fabs(first.position[axis] - second.position[axis]) >
            positionTolerance) {
            return false;
        }
    }

    const glm::mat4 firstRotation =
        transform_math::makeRotationMatrix(first.rotation);
    const glm::mat4 secondRotation =
        transform_math::makeRotationMatrix(second.rotation);
    constexpr float rotationTolerance = 1.0e-5f;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            if (std::fabs(firstRotation[column][row] -
                          secondRotation[column][row]) >
                rotationTolerance) {
                return false;
            }
        }
    }
    return true;
}

double milliseconds(std::chrono::steady_clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

using Clock = std::chrono::steady_clock;

struct StaticShapeAcquisitionStats {
    Clock::duration geometryFingerprint{};
    Clock::duration diskRead{};
    Clock::duration diskRestore{};
    Clock::duration diskWrite{};
    Clock::duration meshConversion{};
    Clock::duration shapeCooking{};
    std::size_t ramHits = 0;
    std::size_t ramMisses = 0;
    std::size_t diskHits = 0;
    std::size_t diskMisses = 0;
    std::size_t diskInvalid = 0;
};

struct CachedStaticShape {
    JPH::ShapeRefC shape;
    physics::ShapeDiagnostics diagnostics;
};

physics::ShapeDiagnostics makeStaticMeshDiagnostics(
    const GameObject& object, const JPH::Shape& shape) {
    physics::ShapeDiagnostics diagnostics;
    diagnostics.representation =
        physics::ShapeDiagnostics::Representation::TriangleMesh;
    for (const MeshInstance& instance :
         object.modelRenderable().meshInstances()) {
        diagnostics.inputVertices += instance.mesh.vertices.size();
        diagnostics.inputTriangles += instance.mesh.indices.size() / 3;
    }
    const JPH::Shape::Stats stats = shape.GetStats();
    diagnostics.joltTriangles = stats.mNumTriangles;
    diagnostics.joltBytes = stats.mSizeBytes;
    return diagnostics;
}

Result makeCachedStaticMeshShape(const CachedStaticShape& cached,
                                 physics::CookedShape& output) {
    if (cached.shape == nullptr ||
        cached.shape->GetType() != JPH::EShapeType::Mesh) {
        return Result::failure("cached static shape is not a Jolt mesh");
    }
    output = {};
    output.shape = cached.shape;
    output.diagnostics = cached.diagnostics;
    return Result::success();
}

}  // namespace

struct PhysicsServer::Impl {
    struct StaticShapeKey {
        std::string modelIdentity;
        std::array<std::uint32_t, 3> scaleBits{};
        GameObject::PhysicsColliderType collider =
            GameObject::PhysicsColliderType::Mesh;

        bool operator==(const StaticShapeKey& other) const noexcept {
            return modelIdentity == other.modelIdentity && scaleBits == other.scaleBits &&
                   collider == other.collider;
        }
    };

    struct StaticShapeKeyHash {
        std::size_t operator()(const StaticShapeKey& key) const noexcept {
            std::size_t result = std::hash<std::string>{}(key.modelIdentity);
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
        key.modelIdentity = model_loading::normalizedFilesystemIdentity(
            object.modelPath ? object.modelPath : "");
        key.scaleBits = std::array<std::uint32_t, 3>{
            normalizedBits(object.scale.x), normalizedBits(object.scale.y),
            normalizedBits(object.scale.z)};
        key.collider = object.physics.colliderType;
        return key;
    }

    [[nodiscard]] Result acquireStaticShape(
        const GameObject& object, physics::CookedShape& output,
        StaticShapeAcquisitionStats* stats);

    struct RuntimeBody {
        JPH::BodyID id;
        GameObject* object = nullptr;
        bool isDynamic = false;
        bool editorOverride = false;
        physics::PhysicsWorldPose submittedWorldPose;
        bool hasSubmittedWorldPose = false;
    };

    struct RuntimeCharacter {
        Character* object = nullptr;
        JPH::Ref<JPH::CharacterVirtual> virtualCharacter;
    };

    // These filters outlive physicsSystem because Jolt retains references.
    BroadPhaseLayerInterface broadPhaseLayerInterface;
    ObjectVsBroadPhaseLayerFilter objectVsBroadPhaseLayerFilter;
    ObjectLayerPairFilter objectLayerPairFilter;
    std::unique_ptr<JPH::TempAllocatorImpl> tempAllocator;
    std::unique_ptr<JPH::JobSystemThreadPool> jobSystem;
    std::unique_ptr<JPH::PhysicsSystem> physicsSystem;
    std::vector<RuntimeBody> runtimeBodies;
    std::vector<RuntimeCharacter> runtimeCharacters;
    std::unordered_map<StaticShapeKey, CachedStaticShape, StaticShapeKeyHash>
        staticShapeCache;
    physics::PhysicsShapeCache persistentShapeCache;
    bool factoryCreated = false;
    bool typesRegistered = false;
    bool initialized = false;

    void activateDynamicBodies(JPH::BodyInterface& bodies) const;
    void synchronizeStaticBodies();
};

void PhysicsServer::Impl::activateDynamicBodies(
    JPH::BodyInterface& bodies) const {
    std::vector<JPH::BodyID> dynamicBodyIds;
    dynamicBodyIds.reserve(runtimeBodies.size());
    for (const RuntimeBody& runtimeBody : runtimeBodies) {
        if (runtimeBody.isDynamic && !runtimeBody.id.IsInvalid()) {
            dynamicBodyIds.push_back(runtimeBody.id);
        }
    }
    if (!dynamicBodyIds.empty()) {
        bodies.ActivateBodies(dynamicBodyIds.data(),
                              static_cast<int>(dynamicBodyIds.size()));
    }
}

void PhysicsServer::Impl::synchronizeStaticBodies() {
    if (!physicsSystem) return;
    JPH::BodyInterface& bodies = physicsSystem->GetBodyInterface();
    bool staticBodyMoved = false;
    for (RuntimeBody& body : runtimeBodies) {
        if (body.isDynamic || body.object == nullptr || body.id.IsInvalid()) {
            continue;
        }

        physics::PhysicsWorldPose currentPose;
        const Result result =
            physics::derivePhysicsWorldPose(*body.object, currentPose);
        if (!result) {
            spdlog::error(
                "Static physics body synchronization failed for {}: {}",
                objectDescription(*body.object), result.error());
            continue;
        }
        if (body.hasSubmittedWorldPose &&
            samePhysicsWorldPose(body.submittedWorldPose, currentPose)) {
            continue;
        }

        bodies.SetPositionAndRotation(
            body.id, physics::toJoltPosition(currentPose.position),
            physics::toJoltRotation(currentPose.rotation),
            JPH::EActivation::DontActivate);
        body.submittedWorldPose = currentPose;
        body.hasSubmittedWorldPose = true;
        staticBodyMoved = true;
    }
    if (staticBodyMoved) activateDynamicBodies(bodies);
}

Result validateRigidBodyConfiguration(const GameObject& object) {
    const GameObject::PhysicsBodySettings& settings = object.physics;
    if (settings.motionType == GameObject::PhysicsMotionType::Static &&
        settings.colliderType != GameObject::PhysicsColliderType::Mesh &&
        settings.colliderType != GameObject::PhysicsColliderType::ConvexHull) {
        return Result::failure(
            "Static physics requires a mesh or convex-hull collider for " +
            objectDescription(object));
    }
    if (settings.motionType == GameObject::PhysicsMotionType::Dynamic &&
        settings.colliderType != GameObject::PhysicsColliderType::Sphere &&
        settings.colliderType != GameObject::PhysicsColliderType::ConvexHull) {
        return Result::failure(
            "Dynamic physics requires a sphere or convex-hull collider for " +
            objectDescription(object));
    }

    const Result hierarchyResult = physics::validatePhysicsHierarchy(object);
    if (!hierarchyResult) {
        return Result::failure(
            "Unsupported rigid-body hierarchy for " +
            objectDescription(object) + ": " + hierarchyResult.error());
    }
    return Result::success();
}

Result PhysicsServer::Impl::acquireStaticShape(
    const GameObject& object, physics::CookedShape& output,
    StaticShapeAcquisitionStats* stats) {
    output = {};
    if (!object.physics.enabled) {
        return Result::failure("physics is disabled");
    }
    if (object.physics.motionType != GameObject::PhysicsMotionType::Static ||
        object.physics.colliderType != GameObject::PhysicsColliderType::Mesh) {
        return Result::failure("shared static shape acquisition requires a static mesh collider");
    }

    const StaticShapeKey key = makeStaticShapeKey(object);
    const auto cached = staticShapeCache.find(key);
    if (cached != staticShapeCache.end()) {
        if (stats != nullptr) ++stats->ramHits;
        spdlog::info("Static Jolt shape cache RAM HIT: {}", object.name);
        return makeCachedStaticMeshShape(cached->second, output);
    }

    if (stats != nullptr) ++stats->ramMisses;
    spdlog::info("Static Jolt shape cache MISS: {}", object.name);
    const Clock::time_point fingerprintStart = Clock::now();
    const physics::StaticMeshShapeCacheKey diskKey =
        physics::makeStaticMeshShapeCacheKey(
            key.modelIdentity, object.modelRenderable().meshInstances(),
            object.scale, static_cast<std::uint8_t>(key.collider));
    if (stats != nullptr) {
        stats->geometryFingerprint += Clock::now() - fingerprintStart;
    }

    physics::ShapeCacheLoadResult diskResult = persistentShapeCache.load(diskKey);
    if (stats != nullptr) {
        stats->diskRead += diskResult.readDuration;
        stats->diskRestore += diskResult.restoreDuration;
    }
    JPH::ShapeRefC shape;
    if (diskResult.status == physics::ShapeCacheLoadStatus::Hit) {
        if (diskResult.shape == nullptr) {
            return Result::failure("cached static shape is not a Jolt mesh");
        }
        const CachedStaticShape restored{
            diskResult.shape,
            makeStaticMeshDiagnostics(object, *diskResult.shape)};
        if (stats != nullptr) ++stats->diskHits;
        spdlog::info("Static Jolt shape cache DISK HIT: {} ({:.2f} ms restore)",
                     object.name, milliseconds(diskResult.restoreDuration));
        const auto [cached, inserted] = staticShapeCache.emplace(key, restored);
        (void)inserted;
        return makeCachedStaticMeshShape(cached->second, output);
    }

    if (diskResult.status == physics::ShapeCacheLoadStatus::Invalid) {
        if (stats != nullptr) ++stats->diskInvalid;
        spdlog::warn("Physics disk cache INVALID: {} ({}) for {}",
                     persistentShapeCache.pathFor(diskKey).string(),
                     diskResult.message, objectDescription(object));
        persistentShapeCache.discard(diskKey);
    } else {
        if (stats != nullptr) ++stats->diskMisses;
        spdlog::info("Static Jolt shape cache DISK MISS/COOK: {}", object.name);
    }

    physics::CookedShape cooked;
    const Result buildResult = physics::buildGameObjectShape(object, cooked);
    if (!buildResult) return buildResult;
    if (stats != nullptr) {
        stats->meshConversion += cooked.timings.geometryConversion;
        stats->shapeCooking += cooked.timings.joltCooking;
    }
    if (cooked.shape == nullptr) {
        return Result::failure("Jolt did not produce a static mesh shape");
    }

    shape = cooked.shape;
    const Clock::time_point writeStart = Clock::now();
    std::string cacheWriteError;
    if (persistentShapeCache.save(diskKey, *shape, cacheWriteError)) {
        spdlog::info("Physics disk cache WRITE: {}",
                     persistentShapeCache.pathFor(diskKey).string());
    } else {
        spdlog::warn("Physics disk cache write failed for {}: {}",
                     objectDescription(object), cacheWriteError);
    }
    if (stats != nullptr) stats->diskWrite += Clock::now() - writeStart;
    staticShapeCache.emplace(key,
                             CachedStaticShape{shape, cooked.diagnostics});
    output = std::move(cooked);
    return Result::success();
}

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

Result PhysicsServer::acquireCollisionShape(const GameObject& object,
                                             physics::CookedShape& output) {
    output = {};
    if (!impl_ || !impl_->initialized) {
        return Result::failure("PhysicsServer is not initialized");
    }
    try {
        if (object.physics.motionType == GameObject::PhysicsMotionType::Static &&
            object.physics.colliderType == GameObject::PhysicsColliderType::Mesh) {
            return impl_->acquireStaticShape(object, output, nullptr);
        }
        return physics::buildGameObjectShape(object, output);
    } catch (const std::exception& exception) {
        return Result::failure("Collision shape acquisition failed: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure("Collision shape acquisition failed unexpectedly");
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
        // Validate every ordinary rigid body before allocating shapes or
        // bodies so unsupported hierarchy configurations fail atomically.
        for (const std::unique_ptr<GameObject>& objectOwner :
             runtimeScene.gameObjects()) {
            if (!objectOwner) continue;
            GameObject& object = *objectOwner;
            if (dynamic_cast<Character*>(&object) != nullptr ||
                !object.physics.enabled) {
                continue;
            }
            const Result configuration = validateRigidBodyConfiguration(object);
            if (!configuration) return configuration;
        }

        const Clock::time_point totalStart = Clock::now();
        StaticShapeAcquisitionStats staticShapeStats;
        Clock::duration convexGeometryConversion{};
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
        std::vector<Character*> characters;

        for (const std::unique_ptr<GameObject>& objectOwner :
             runtimeScene.gameObjects()) {
            if (!objectOwner) {
                continue;
            }
            GameObject& object = *objectOwner;
            if (auto* character = dynamic_cast<Character*>(&object)) {
                characters.push_back(character);
                character->grounded = false;
                continue;
            }
            if (!object.physics.enabled) {
                continue;
            }
            const GameObject::PhysicsBodySettings& settings = object.physics;

            physics::PhysicsWorldPose worldPose;
            const Result worldPoseResult =
                physics::derivePhysicsWorldPose(object, worldPose);
            if (!worldPoseResult) {
                endRuntimeSession();
                return Result::failure(
                    "Failed to derive rigid-body world pose for " +
                    objectDescription(object) + ": " +
                    worldPoseResult.error());
            }

            JPH::BodyID bodyId;
            if (settings.motionType == GameObject::PhysicsMotionType::Static) {
                physics::CookedShape cooked;
                const Result shapeResult =
                    settings.colliderType == GameObject::PhysicsColliderType::Mesh
                        ? impl_->acquireStaticShape(object, cooked,
                                                    &staticShapeStats)
                        : physics::buildGameObjectShape(object, cooked);
                if (!shapeResult) {
                    endRuntimeSession();
                    return Result::failure("Failed to build static collision for " +
                                           objectDescription(object) + ": " +
                                           shapeResult.error());
                }
                JPH::ShapeRefC shape = cooked.shape;
                if (shape == nullptr) {
                    endRuntimeSession();
                    return Result::failure("Failed to create static shape for " +
                                           objectDescription(object));
                }
                JPH::BodyCreationSettings bodySettings(
                    shape.GetPtr(), physics::toJoltPosition(worldPose.position),
                    physics::toJoltRotation(worldPose.rotation),
                    JPH::EMotionType::Static, layers::nonMoving);
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
                physics::CookedShape cooked;
                Result shapeBuild = physics::buildGameObjectShape(object, cooked);
                convexGeometryConversion += cooked.timings.geometryConversion;
                convexCooking += cooked.timings.joltCooking;
                if (!shapeBuild) {
                    endRuntimeSession();
                    return Result::failure("Failed to create dynamic collider for " +
                                           objectDescription(object) + ": " + shapeBuild.error());
                }
                shape = cooked.shape;
                JPH::BodyCreationSettings bodySettings(
                    shape.GetPtr(), physics::toJoltPosition(worldPose.position),
                    physics::toJoltRotation(worldPose.rotation),
                    JPH::EMotionType::Dynamic, layers::moving);
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
            const bool isDynamic =
                settings.motionType == GameObject::PhysicsMotionType::Dynamic;
            impl_->runtimeBodies.push_back(
                {bodyId, &object, isDynamic, false, worldPose, !isDynamic});
        }
        const Clock::time_point broadPhaseStart = Clock::now();
        impl_->physicsSystem->OptimizeBroadPhase();
        broadPhaseOptimization += Clock::now() - broadPhaseStart;
        for (Character* character : characters) {
            if (character == nullptr ||
                !(character->capsuleHeight > 2.0f * character->capsuleRadius) ||
                !(character->capsuleRadius > 0.0f) ||
                !std::isfinite(character->capsuleHeight) ||
                !std::isfinite(character->capsuleRadius)) {
                endRuntimeSession();
                return Result::failure("Character capsule dimensions must be finite, positive, and taller than its diameter");
            }

            physics::CookedShape cooked;
            Result shapeBuild = physics::buildCharacterShape(*character, cooked);
            if (!shapeBuild) {
                endRuntimeSession();
                return Result::failure("Failed to create character capsule for " +
                                       objectDescription(*character) + ": " +
                                       shapeBuild.error());
            }

            JPH::CharacterVirtualSettings characterSettings;
            characterSettings.mShape = cooked.shape;
            characterSettings.mSupportingVolume = JPH::Plane(
                JPH::Vec3::sAxisY(), -physics::dunamisToMeters(character->capsuleRadius));
            characterSettings.mEnhancedInternalEdgeRemoval = false;
            characterSettings.mBackFaceMode = JPH::EBackFaceMode::IgnoreBackFaces;
            JPH::Ref<JPH::CharacterVirtual> virtualCharacter =
                new JPH::CharacterVirtual(&characterSettings,
                                          physics::toJoltPosition(character->position),
                                          JPH::Quat::sIdentity(),
                                          impl_->physicsSystem.get());
            impl_->runtimeCharacters.push_back(
                {character, std::move(virtualCharacter)});
        }
        spdlog::info("Runtime physics world created: {} static, {} dynamic, {} characters",
                     staticCount, dynamicCount, characters.size());
        spdlog::info("Physics startup: geometry fingerprint {:.2f} ms, disk read {:.2f} ms, Jolt restore {:.2f} ms, static mesh conversion {:.2f} ms, static Jolt shape cooking {:.2f} ms, disk write {:.2f} ms, dynamic convex conversion {:.2f} ms, dynamic convex Jolt cooking {:.2f} ms, body creation {:.2f} ms, broadphase optimization {:.2f} ms, total {:.2f} ms; RAM hits {}, misses {}; disk hits {}, misses {}, invalid {}",
                     milliseconds(staticShapeStats.geometryFingerprint),
                     milliseconds(staticShapeStats.diskRead),
                     milliseconds(staticShapeStats.diskRestore),
                     milliseconds(staticShapeStats.meshConversion),
                     milliseconds(staticShapeStats.shapeCooking),
                     milliseconds(staticShapeStats.diskWrite),
                     milliseconds(convexGeometryConversion),
                     milliseconds(convexCooking), milliseconds(bodyCreation),
                     milliseconds(broadPhaseOptimization),
                     milliseconds(Clock::now() - totalStart),
                     staticShapeStats.ramHits, staticShapeStats.ramMisses,
                     staticShapeStats.diskHits, staticShapeStats.diskMisses,
                     staticShapeStats.diskInvalid);
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

void PhysicsServer::applyRuntimeTransform(GameObject& object,
                                          const glm::vec3& position,
                                          const glm::vec3& rotation,
                                          bool manipulating) {
    if (!impl_ || !impl_->physicsSystem) {
        return;
    }
    JPH::BodyInterface& bodies = impl_->physicsSystem->GetBodyInterface();
    for (Impl::RuntimeBody& body : impl_->runtimeBodies) {
        if (body.object != &object) {
            continue;
        }

        physics::PhysicsWorldPose worldPose;
        const Result worldPoseResult = physics::derivePhysicsWorldPose(
            object, position, rotation, worldPose);
        if (!worldPoseResult) {
            spdlog::error(
                "Runtime rigid-body transform edit rejected for {}: {}",
                objectDescription(object), worldPoseResult.error());
            return;
        }

        bodies.SetPositionAndRotation(body.id,
                                      physics::toJoltPosition(worldPose.position),
                                      physics::toJoltRotation(worldPose.rotation),
                                      body.isDynamic ? JPH::EActivation::Activate
                                                     : JPH::EActivation::DontActivate);
        if (body.isDynamic) {
            bodies.SetLinearAndAngularVelocity(body.id, JPH::Vec3::sZero(),
                                               JPH::Vec3::sZero());
        } else {
            impl_->activateDynamicBodies(bodies);
        }
        // These are authored local values, even though worldPose is what was
        // submitted to Jolt.
        body.object->position = position;
        body.object->rotation = rotation;
        if (!body.isDynamic) {
            body.submittedWorldPose = worldPose;
            body.hasSubmittedWorldPose = true;
        }
        body.editorOverride = body.isDynamic && manipulating;
        return;
    }
}

void PhysicsServer::update() {
    if (!impl_->physicsSystem) {
        return;
    }
    // Static bodies are authored/environmental. Keep their Jolt world pose in
    // sync with supported hierarchy movement before the next simulation step.
    impl_->synchronizeStaticBodies();
    const std::size_t steps = accumulator_.addFrameDelta(Time::deltaTime());
    for (std::size_t step = 0; step < steps; ++step) {
        const JPH::Vec3 gravity = impl_->physicsSystem->GetGravity();
        const JPH::BroadPhaseLayerFilter& broadPhaseFilter =
            impl_->physicsSystem->GetDefaultBroadPhaseLayerFilter(
                layers::moving);
        const JPH::ObjectLayerFilter& objectLayerFilter =
            impl_->physicsSystem->GetDefaultLayerFilter(layers::moving);
        for (Impl::RuntimeCharacter& runtimeCharacter :
             impl_->runtimeCharacters) {
            if (runtimeCharacter.object == nullptr ||
                runtimeCharacter.virtualCharacter == nullptr) {
                continue;
            }

            Character& object = *runtimeCharacter.object;
            JPH::CharacterVirtual& virtualCharacter =
                *runtimeCharacter.virtualCharacter;
            glm::vec3 desiredVelocity = object.desiredVelocity;
            if (!std::isfinite(desiredVelocity.x) ||
                !std::isfinite(desiredVelocity.z)) {
                desiredVelocity = glm::vec3(0.0f);
            }
            const glm::vec3 desiredVelocityMeters =
                physics::dunamisToMeters(glm::vec3(
                    desiredVelocity.x, 0.0f, desiredVelocity.z));
            const JPH::Vec3 horizontalVelocity(
                desiredVelocityMeters.x, 0.0f, desiredVelocityMeters.z);

            virtualCharacter.UpdateGroundVelocity();
            const JPH::Vec3 up = virtualCharacter.GetUp();
            const JPH::Vec3 currentVelocity =
                virtualCharacter.GetLinearVelocity();
            const JPH::Vec3 currentVerticalVelocity =
                currentVelocity.Dot(up) * up;
            const JPH::Vec3 groundVelocity =
                virtualCharacter.GetGroundVelocity();
            const bool movingTowardsGround =
                currentVerticalVelocity.Dot(up) - groundVelocity.Dot(up) <
                0.1f;
            JPH::Vec3 newVelocity;
            if (virtualCharacter.GetGroundState() ==
                    JPH::CharacterVirtual::EGroundState::OnGround &&
                movingTowardsGround) {
                newVelocity = groundVelocity + horizontalVelocity;
            } else {
                newVelocity = currentVerticalVelocity + horizontalVelocity;
            }
            newVelocity += gravity * PhysicsStepAccumulator::fixedDeltaTime;
            virtualCharacter.SetLinearVelocity(newVelocity);

            JPH::CharacterVirtual::ExtendedUpdateSettings updateSettings;
            virtualCharacter.ExtendedUpdate(
                PhysicsStepAccumulator::fixedDeltaTime, gravity,
                updateSettings, broadPhaseFilter, objectLayerFilter, {}, {},
                *impl_->tempAllocator);
            object.grounded = virtualCharacter.GetGroundState() ==
                JPH::CharacterVirtual::EGroundState::OnGround;
        }
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
        glm::mat4 worldMatrix;
        if (!makeDunamisWorldMatrix(position, bodies.GetRotation(body.id),
                                     worldMatrix)) {
            spdlog::error(
                "Dynamic physics produced a non-finite world transform for {}",
                objectDescription(*body.object));
            continue;
        }

        glm::vec3 localPosition;
        glm::vec3 localRotation;
        const Result localResult = physics::deriveLocalPoseFromPhysicsWorld(
            *body.object, worldMatrix, localPosition, localRotation);
        if (!localResult) {
            spdlog::error(
                "Dynamic physics world-to-local writeback failed for {}: {}",
                objectDescription(*body.object), localResult.error());
            continue;
        }
        // Commit both fields only after the complete conversion succeeds.
        body.object->position = localPosition;
        body.object->rotation = localRotation;
    }
    for (const Impl::RuntimeCharacter& runtimeCharacter :
         impl_->runtimeCharacters) {
        if (runtimeCharacter.object == nullptr ||
            runtimeCharacter.virtualCharacter == nullptr) {
            continue;
        }
        const JPH::RVec3 position =
            runtimeCharacter.virtualCharacter->GetPosition();
        runtimeCharacter.object->position = physics::metersToDunamis(
            glm::vec3(static_cast<float>(position.GetX()),
                      static_cast<float>(position.GetY()),
                      static_cast<float>(position.GetZ())));
        runtimeCharacter.object->onPhysicsTransformResolved();
    }
}

void PhysicsServer::endRuntimeSession() noexcept {
    accumulator_.reset();
    if (!impl_ || !impl_->physicsSystem) {
        return;
    }
    try {
        impl_->runtimeCharacters.clear();
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
        impl_->runtimeCharacters.clear();
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
