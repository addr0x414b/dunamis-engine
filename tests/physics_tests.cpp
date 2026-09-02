// Jolt requires this include ordering.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

#include "../core/time.h"
#include "../physics/collision_shapes.h"
#include "../physics/physics_shape_cache.h"
#include "../physics/physics_server.h"
#include "../physics/physics_units.h"
#include "../scene/character.h"
#include "../scene/game_object.h"
#include "../scene/model_renderable.h"
#include "../scene/scene.h"

class TimeTestAccess {
public:
    static void initialize() { Time::initialize(); }
    static void update() { Time::update(); }
};

class GameObjectTestAccess {
public:
    static auto& mutableMeshInstances(ModelRenderable& renderable) {
        return renderable.meshInstances_;
    }
};

namespace {

[[noreturn]] void fail(const char* message) {
    std::cerr << message << '\n';
    std::exit(EXIT_FAILURE);
}

void expect(bool condition, const char* message) {
    if (!condition) fail(message);
}

bool near(float actual, float expected, float tolerance = 1.0e-4f) {
    return std::abs(actual - expected) <= tolerance;
}

bool matrixNear(const glm::mat4& actual, const glm::mat4& expected,
                float tolerance = 2.0e-4f) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!near(actual[column][row], expected[column][row], tolerance)) {
                return false;
            }
        }
    }
    return true;
}

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

class PairFilter final : public JPH::ObjectLayerPairFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer first,
                       JPH::ObjectLayer second) const override {
        return first == 1 || (first == 0 && second == 1);
    }
};

class BroadPhaseInterface final : public JPH::BroadPhaseLayerInterface {
public:
    JPH::uint GetNumBroadPhaseLayers() const override { return 2; }
    JPH::BroadPhaseLayer GetBroadPhaseLayer(
        JPH::ObjectLayer layer) const override {
        return JPH::BroadPhaseLayer(layer);
    }
};

class ObjectVsBroadPhaseFilter final
    : public JPH::ObjectVsBroadPhaseLayerFilter {
public:
    bool ShouldCollide(JPH::ObjectLayer layer,
                       JPH::BroadPhaseLayer broadPhase) const override {
        return layer == 1 || broadPhase == JPH::BroadPhaseLayer(1);
    }
};

MeshInstance triangleInstance(const glm::vec3& vertex) {
    MeshInstance instance;
    instance.mesh.vertices = {
        {vertex, {}, {}, {}, {}},
        {glm::vec3(0.0f, 1.0f, 0.0f), {}, {}, {}, {}},
        {glm::vec3(0.0f, 0.0f, 1.0f), {}, {}, {}, {}}};
    instance.mesh.indices = {0, 1, 2};
    return instance;
}

JPH::ShapeRefC makeOffOriginHull();

void initializeJoltForShapeTests() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
}

void shutdownJoltForShapeTests() {
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void testShapeCenterOfMassTransform() {
    initializeJoltForShapeTests();

    const JPH::Vec3 knownOffset(0.75f, -0.25f, 0.5f);
    JPH::RotatedTranslatedShapeSettings knownSettings(
        knownOffset, JPH::Quat::sIdentity(),
        new JPH::BoxShape(JPH::Vec3(0.2f, 0.3f, 0.4f)));
    const JPH::ShapeSettings::ShapeResult knownResult = knownSettings.Create();
    expect(!knownResult.HasError(),
           "failed to create known-offset shape for COM transform test");
    const JPH::ShapeRefC knownShape = knownResult.Get();
    expect(knownShape->GetCenterOfMass().IsClose(knownOffset, 1.0e-5f),
           "known-offset shape did not preserve its local COM");

    const glm::vec3 knownBodyOrigin(-350.0f, 120.0f, 875.0f);
    const glm::vec3 knownBodyRotation(11.0f, 37.0f, -29.0f);
    const JPH::Quat knownRotation = physics::toJoltRotation(knownBodyRotation);
    const JPH::RVec3 knownExpected = physics::toJoltPosition(knownBodyOrigin) +
                                     knownRotation * knownOffset;
    const JPH::RMat44 knownActual = physics::makeShapeCenterOfMassTransform(
        *knownShape, knownBodyOrigin, knownBodyRotation);
    expect(knownActual.GetTranslation().IsClose(knownExpected, 2.0e-5f),
           "generic COM transform did not rotate a known local offset");

    const JPH::ShapeRefC hull = makeOffOriginHull();
    const JPH::Vec3 localCom = hull->GetCenterOfMass();
    expect(!localCom.IsNearZero(1.0e-4f),
           "off-origin convex hull did not produce a non-zero center of mass");
    const glm::vec3 bodyOrigin(700.0f, -400.0f, 1250.0f);
    const glm::vec3 bodyRotation(23.0f, -41.0f, 67.0f);
    const JPH::Quat rotation = physics::toJoltRotation(bodyRotation);
    const JPH::RVec3 expected = physics::toJoltPosition(bodyOrigin) +
                                rotation * localCom;
    const JPH::RMat44 actual = physics::makeShapeCenterOfMassTransform(
        *hull, bodyOrigin, bodyRotation);
    expect(actual.GetTranslation().IsClose(expected, 2.0e-5f),
           "shape COM transform did not rotate the local COM offset from the body origin");

    const glm::mat4 renderTransform = physics::joltTransformToDunamis(actual);
    const glm::vec3 expectedRenderPosition = physics::metersToDunamis(glm::vec3(
        static_cast<float>(expected.GetX()), static_cast<float>(expected.GetY()),
        static_cast<float>(expected.GetZ())));
    expect(glm::length(glm::vec3(renderTransform[3]) - expectedRenderPosition) <
               2.0e-3f,
           "Jolt-to-Dunamis transform conversion changed COM translation");

    shutdownJoltForShapeTests();
}

void testCharacterShapeCenterOfMassTransform() {
    initializeJoltForShapeTests();

    Character character;
    character.position = glm::vec3(10.0f, 20.0f, -30.0f);
    character.capsuleHeight = 180.0f;
    character.capsuleRadius = 35.0f;
    physics::CookedShape cooked;
    expect(static_cast<bool>(physics::buildCharacterShape(character, cooked)),
           "failed to build character capsule for COM placement test");
    const JPH::RMat44 transform = physics::makeShapeCenterOfMassTransform(
        *cooked.shape, character.position, character.rotation);
    const JPH::AABox bounds = cooked.shape->GetLocalBounds();
    const float bottom = physics::metersToDunamis(static_cast<float>(
        transform.GetTranslation().GetY() + bounds.mMin.GetY()));
    const float top = physics::metersToDunamis(static_cast<float>(
        transform.GetTranslation().GetY() + bounds.mMax.GetY()));
    expect(near(bottom, character.position.y, 2.0e-3f) &&
               near(top, character.position.y + character.capsuleHeight, 2.0e-3f),
           "character COM transform did not place capsule between feet and authored height");

    shutdownJoltForShapeTests();
}

void testPhysicsServerStaticShapeSharing() {
    TestScene runtimeScene;
    const std::string modelIdentity =
        "__dunamis_static_shape_sharing_" +
        std::to_string(reinterpret_cast<std::uintptr_t>(&runtimeScene));

    GameObject editingObject;
    editingObject.name = "Static Shape Editing Object";
    editingObject.persistentId = "shared-static-shape";
    editingObject.modelPath = modelIdentity.c_str();
    editingObject.physics.enabled = true;
    editingObject.physics.motionType = GameObject::PhysicsMotionType::Static;
    editingObject.physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(editingObject.modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(0.0f)))),
           "failed to create editing static mesh");

    auto runtimeObject = std::make_unique<GameObject>();
    runtimeObject->name = "Static Shape Runtime Clone";
    runtimeObject->persistentId = editingObject.persistentId;
    runtimeObject->modelPath = modelIdentity.c_str();
    runtimeObject->physics = editingObject.physics;
    expect(static_cast<bool>(runtimeObject->modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(0.0f)))),
           "failed to create runtime static mesh");
    expect(static_cast<bool>(runtimeScene.addGameObject(std::move(runtimeObject))),
           "failed to add runtime static mesh");

    PhysicsServer server;
    expect(static_cast<bool>(server.initialize()),
           "failed to initialize PhysicsServer for shared-shape test");
    physics::CookedShape first;
    physics::CookedShape second;
    expect(static_cast<bool>(server.acquireCollisionShape(editingObject, first)),
           "failed to acquire editing static collision shape");
    expect(static_cast<bool>(server.acquireCollisionShape(editingObject, second)),
           "failed to reacquire editing static collision shape");
    expect(first.shape && second.shape && first.shape.GetPtr() == second.shape.GetPtr(),
           "repeated static collision acquisition did not reuse the Jolt shape");
    expect(first.diagnostics.inputVertices == 3 &&
               first.diagnostics.inputTriangles == 1 &&
               first.diagnostics.joltTriangles > 0 &&
               first.diagnostics.joltBytes > 0 &&
               second.diagnostics.inputVertices == 3 &&
               second.diagnostics.inputTriangles == 1 &&
               second.diagnostics.joltTriangles == first.diagnostics.joltTriangles &&
               second.diagnostics.joltBytes == first.diagnostics.joltBytes,
           "shared static collision diagnostics were not populated");

    expect(static_cast<bool>(server.beginRuntimeSession(runtimeScene)),
           "runtime clone could not acquire the shared static collision shape");
    server.endRuntimeSession();

    physics::CookedShape afterRuntime;
    expect(static_cast<bool>(server.acquireCollisionShape(editingObject, afterRuntime)),
           "static collision acquisition failed after runtime teardown");
    expect(afterRuntime.shape && afterRuntime.shape.GetPtr() == first.shape.GetPtr(),
           "static collision RAM cache did not survive runtime teardown");
    expect(afterRuntime.diagnostics.inputVertices == 3 &&
               afterRuntime.diagnostics.inputTriangles == 1 &&
               afterRuntime.diagnostics.joltTriangles == first.diagnostics.joltTriangles &&
               afterRuntime.diagnostics.joltBytes == first.diagnostics.joltBytes,
           "static collision diagnostics did not survive runtime teardown");
    server.shutdown();

    physics::PhysicsShapeCache cache;
    const physics::StaticMeshShapeCacheKey key =
        physics::makeStaticMeshShapeCacheKey(
            modelIdentity, editingObject.modelRenderable().meshInstances(),
            editingObject.scale);
    std::error_code error;
    std::filesystem::remove(cache.pathFor(key), error);
}

void testShapeDiagnostics() {
    initializeJoltForShapeTests();

    GameObject meshObject;
    meshObject.physics.enabled = true;
    meshObject.physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(meshObject.modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(0.0f)))),
           "failed to add mesh diagnostic input");
    physics::CookedShape mesh;
    expect(static_cast<bool>(physics::buildGameObjectShape(meshObject, mesh)),
           "failed to build mesh diagnostic shape");
    expect(mesh.diagnostics.representation ==
               physics::ShapeDiagnostics::Representation::TriangleMesh &&
               mesh.diagnostics.inputVertices == 3 &&
               mesh.diagnostics.inputTriangles == 1 &&
               mesh.diagnostics.joltTriangles > 0 && mesh.diagnostics.joltBytes > 0,
           "mesh diagnostics were not populated");

    MeshInstance convexInput;
    convexInput.mesh.vertices = {
        {{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{100.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 200.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 0.0f, 100.0f}, {}, {}, {}, {}}};
    GameObject convexObject;
    convexObject.physics.enabled = true;
    convexObject.physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    expect(static_cast<bool>(convexObject.modelRenderable().addMeshInstance(
               std::move(convexInput))),
           "failed to add convex diagnostic input");
    physics::CookedShape convex;
    expect(static_cast<bool>(physics::buildGameObjectShape(convexObject, convex)),
           "failed to build convex diagnostic shape");
    expect(convex.diagnostics.representation ==
               physics::ShapeDiagnostics::Representation::ConvexHull &&
               convex.diagnostics.inputPoints == 4 &&
               convex.diagnostics.cookedHullVertices > 0 &&
               convex.diagnostics.joltTriangles > 0 && convex.diagnostics.joltBytes > 0,
           "convex hull diagnostics were not populated");

    GameObject sphereObject;
    sphereObject.physics.enabled = true;
    sphereObject.physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    sphereObject.physics.sphereRadius = 42.0f;
    physics::CookedShape sphere;
    expect(static_cast<bool>(physics::buildGameObjectShape(sphereObject, sphere)),
           "failed to build sphere diagnostic shape");
    expect(sphere.diagnostics.representation ==
               physics::ShapeDiagnostics::Representation::AnalyticSphere &&
               near(sphere.diagnostics.radius, 42.0f) &&
               sphere.diagnostics.joltBytes > 0,
           "sphere diagnostics were not populated as analytic data");

    Character character;
    character.capsuleHeight = 180.0f;
    character.capsuleRadius = 35.0f;
    physics::CookedShape capsule;
    expect(static_cast<bool>(physics::buildCharacterShape(character, capsule)),
           "failed to build capsule diagnostic shape");
    expect(capsule.diagnostics.representation ==
               physics::ShapeDiagnostics::Representation::AnalyticCapsule &&
               near(capsule.diagnostics.height, 180.0f) &&
               near(capsule.diagnostics.radius, 35.0f) &&
               capsule.diagnostics.joltBytes > 0,
           "character diagnostics were not populated as analytic data");

    shutdownJoltForShapeTests();
}

void testAccumulator() {
    PhysicsStepAccumulator accumulator;
    expect(accumulator.addFrameDelta(1.0f / 120.0f) == 0,
           "half fixed delta must not step");
    expect(accumulator.addFrameDelta(1.0f / 120.0f) == 1,
           "two half deltas must step exactly once");
    accumulator.reset();
    expect(accumulator.addFrameDelta(0.1f) == PhysicsStepAccumulator::maxSubsteps,
           "0.1 seconds must respect the six-step bound");
}

void testPhysicsUnits() {
    expect(near(physics::dunamisToMeters(0.0f), 0.0f), "0 DU conversion failed");
    expect(near(physics::dunamisToMeters(1.0f), 0.01f), "1 DU conversion failed");
    expect(near(physics::dunamisToMeters(100.0f), 1.0f), "100 DU conversion failed");
    expect(near(physics::dunamisToMeters(250.0f), 2.5f), "250 DU conversion failed");
    expect(near(physics::dunamisToMeters(-100.0f), -1.0f), "negative DU conversion failed");
    const glm::vec3 authored(250.0f, -100.0f, 1.0f);
    const glm::vec3 meters = physics::dunamisToMeters(authored);
    expect(meters == glm::vec3(2.5f, -1.0f, 0.01f), "vector DU conversion failed");
    const glm::vec3 roundTrip = physics::metersToDunamis(meters);
    expect(near(roundTrip.x, authored.x) && near(roundTrip.y, authored.y) &&
               near(roundTrip.z, authored.z),
           "DU to meters round trip failed");
}

void testScaledLocalTriangleMesh() {
    initializeJoltForShapeTests();
    GameObject object;
    object.physics.enabled = true;
    object.physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    object.scale = glm::vec3(2.0f, 3.0f, 4.0f);
    expect(static_cast<bool>(object.modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(1.0f, 2.0f, 3.0f)))),
           "failed to add scaled mesh input");

    physics::CookedShape output;
    expect(static_cast<bool>(physics::buildGameObjectShape(object, output)),
           "scaled local triangle conversion failed");
    const JPH::AABox bounds = output.shape->GetLocalBounds();
    expect(near(bounds.mMin.GetX(), 0.0f) && near(bounds.mMin.GetY(), 0.0f) &&
               near(bounds.mMin.GetZ(), 0.0f) &&
               near(bounds.mMax.GetX(), 0.02f) &&
               near(bounds.mMax.GetY(), 0.06f) &&
               near(bounds.mMax.GetZ(), 0.12f),
           "static mesh did not preserve authored scale and DU-to-meter conversion");

    GameObject invalidObject;
    invalidObject.physics.enabled = true;
    invalidObject.physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(invalidObject.modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(0.0f)))),
           "failed to add invalid mesh input");
    GameObjectTestAccess::mutableMeshInstances(invalidObject.modelRenderable())
        .front()
        .mesh.indices = {0, 1, 3};
    expect(!physics::buildGameObjectShape(invalidObject, output),
           "invalid mesh index was accepted");

    object.scale = glm::vec3(NAN, 0.0f, 0.0f);
    expect(!physics::buildGameObjectShape(object, output),
           "non-finite transform was accepted");
    shutdownJoltForShapeTests();
}

void testConvexHullInputAndRotation() {
    initializeJoltForShapeTests();
    MeshInstance instance;
    instance.mesh.vertices = {
        {{1.0f, 2.0f, 3.0f}, {}, {}, {}, {}},
        {{2.0f, 2.0f, 3.0f}, {}, {}, {}, {}},
        {{1.0f, 3.0f, 3.0f}, {}, {}, {}, {}},
        {{1.0f, 2.0f, 4.0f}, {}, {}, {}, {}}};
    MeshInstance nonFiniteInstance = instance;
    GameObject object;
    object.physics.enabled = true;
    object.physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    object.scale = glm::vec3(2.0f, 3.0f, 4.0f);
    expect(static_cast<bool>(object.modelRenderable().addMeshInstance(
               std::move(instance))),
           "failed to add convex hull input");
    physics::CookedShape cooked;
    expect(static_cast<bool>(physics::buildGameObjectShape(object, cooked)),
           "scaled convex hull conversion failed");
    expect(cooked.shape->GetSubType() == JPH::EShapeSubType::ConvexHull,
           "convex hull did not produce a Jolt convex hull shape");
    const JPH::AABox bounds = cooked.shape->GetLocalBounds();
    const JPH::Vec3 centerOfMass = cooked.shape->GetCenterOfMass();
    expect(near(centerOfMass.GetX() + bounds.mMin.GetX(), 0.02f) &&
               near(centerOfMass.GetY() + bounds.mMin.GetY(), 0.06f) &&
               near(centerOfMass.GetZ() + bounds.mMin.GetZ(), 0.12f) &&
               near(centerOfMass.GetX() + bounds.mMax.GetX(), 0.04f) &&
               near(centerOfMass.GetY() + bounds.mMax.GetY(), 0.09f) &&
               near(centerOfMass.GetZ() + bounds.mMax.GetZ(), 0.16f),
           "convex hull points did not preserve scaled local meter coordinates");

    object.scale = glm::vec3(0.0f);
    expect(!physics::buildGameObjectShape(object, cooked),
           "near-zero convex scale was accepted");
    GameObject emptyObject;
    emptyObject.physics.enabled = true;
    emptyObject.physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    expect(!physics::buildGameObjectShape(emptyObject, cooked),
           "empty convex geometry was accepted");
    object.scale = glm::vec3(1.0f);
    GameObject nonFiniteObject;
    nonFiniteObject.physics.enabled = true;
    nonFiniteObject.physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    expect(static_cast<bool>(nonFiniteObject.modelRenderable().addMeshInstance(
               std::move(nonFiniteInstance))),
           "failed to add non-finite convex input");
    GameObjectTestAccess::mutableMeshInstances(nonFiniteObject.modelRenderable())
        .front()
        .mesh.vertices[0]
        .pos.x = std::numeric_limits<float>::quiet_NaN();
    expect(!physics::buildGameObjectShape(nonFiniteObject, cooked),
           "non-finite convex vertex was accepted");

    const glm::vec3 rotations[] = {
        {37.0f, 0.0f, 0.0f}, {0.0f, -51.0f, 0.0f}, {0.0f, 0.0f, 83.0f},
        {23.0f, -41.0f, 67.0f}, {-31.0f, 44.0f, -72.0f}, {10.0f, 89.0f, -15.0f}};
    for (const glm::vec3& rotation : rotations) {
        glm::vec3 extracted;
        glm::mat4 original(1.0f);
        original = glm::rotate(original, glm::radians(rotation.x),
                               glm::vec3(1.0f, 0.0f, 0.0f));
        original = glm::rotate(original, glm::radians(rotation.y),
                               glm::vec3(0.0f, 1.0f, 0.0f));
        original = glm::rotate(original, glm::radians(rotation.z),
                               glm::vec3(0.0f, 0.0f, 1.0f));
        expect(physics::extractDunamisRotation(original, extracted),
               "Dunamis rotation extraction failed");
        glm::mat4 roundTrip(1.0f);
        roundTrip = glm::rotate(roundTrip, glm::radians(extracted.x),
                                glm::vec3(1.0f, 0.0f, 0.0f));
        roundTrip = glm::rotate(roundTrip, glm::radians(extracted.y),
                                glm::vec3(0.0f, 1.0f, 0.0f));
        roundTrip = glm::rotate(roundTrip, glm::radians(extracted.z),
                                glm::vec3(0.0f, 0.0f, 1.0f));
        expect(matrixNear(roundTrip, original),
               "Dunamis rotation matrix round trip changed orientation");
        const glm::quat expectedQuaternion = glm::quat_cast(original);
        const JPH::Quat expectedJoltRotation(
            expectedQuaternion.x, expectedQuaternion.y, expectedQuaternion.z,
            expectedQuaternion.w);
        expect(physics::toJoltRotation(rotation).IsClose(
                   expectedJoltRotation.Normalized(), 2.0e-5f),
               "Dunamis authored rotation convention changed");
    }
    shutdownJoltForShapeTests();
}

JPH::ShapeRefC makeOffOriginHull() {
    JPH::Array<JPH::Vec3> points = {
        JPH::Vec3(0.0f, 0.0f, 0.0f), JPH::Vec3(1.0f, 0.0f, 0.0f),
        JPH::Vec3(0.0f, 3.0f, 0.0f), JPH::Vec3(0.0f, 0.0f, 1.0f)};
    JPH::ConvexHullShapeSettings settings(points);
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    expect(!result.HasError(), "failed to create off-origin convex hull");
    return result.Get();
}

JPH::ShapeRefC makeCachedMeshShape() {
    JPH::TriangleList triangles;
    triangles.emplace_back(JPH::Vec3(0.0f, 0.0f, 0.0f),
                           JPH::Vec3(1.0f, 0.0f, 0.0f),
                           JPH::Vec3(0.0f, 0.0f, 1.0f));
    JPH::MeshShapeSettings settings(std::move(triangles));
    settings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult result = settings.Create();
    expect(!result.HasError(), "failed to create cache test mesh shape");
    return result.Get();
}

void testCookedShapeCache() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    std::error_code error;
    const std::filesystem::path directory =
        std::filesystem::temp_directory_path() / "dunamis-physics-shape-cache-tests";
    std::filesystem::remove_all(directory, error);
    const std::vector<MeshInstance> geometry = {triangleInstance(glm::vec3(0.0f))};
    const physics::StaticMeshShapeCacheKey key =
        physics::makeStaticMeshShapeCacheKey("test-model", geometry, glm::vec3(1.0f));
    const physics::StaticMeshShapeCacheKey sameKey =
        physics::makeStaticMeshShapeCacheKey("test-model", geometry, glm::vec3(1.0f));
    expect(key.stableHash == sameKey.stableHash,
           "persistent cache key is not stable for identical input");

    std::vector<MeshInstance> changedGeometry = geometry;
    changedGeometry[0].mesh.vertices[0].pos.x = 2.0f;
    const physics::StaticMeshShapeCacheKey changedGeometryKey =
        physics::makeStaticMeshShapeCacheKey("test-model", changedGeometry, glm::vec3(1.0f));
    expect(key.geometryFingerprint != changedGeometryKey.geometryFingerprint &&
               key.stableHash != changedGeometryKey.stableHash,
           "geometry changes did not invalidate persistent cache identity");
    const physics::StaticMeshShapeCacheKey changedScaleKey =
        physics::makeStaticMeshShapeCacheKey("test-model", geometry, glm::vec3(2.0f, 1.0f, 1.0f));
    expect(key.stableHash != changedScaleKey.stableHash,
           "scale changes did not invalidate persistent cache identity");
    GameObject firstObject;
    firstObject.position = glm::vec3(10.0f, 20.0f, 30.0f);
    firstObject.rotation = glm::vec3(5.0f, 15.0f, 25.0f);
    expect(static_cast<bool>(firstObject.modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(0.0f)))),
           "failed to create first position/rotation cache-key object");
    GameObject secondObject;
    secondObject.position = glm::vec3(-70.0f, 80.0f, -90.0f);
    secondObject.rotation = glm::vec3(-45.0f, 90.0f, 180.0f);
    expect(static_cast<bool>(secondObject.modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(0.0f)))),
           "failed to create second position/rotation cache-key object");
    // Position and rotation are deliberately body state, not key inputs.
    const physics::StaticMeshShapeCacheKey firstObjectKey =
        physics::makeStaticMeshShapeCacheKey(
            "test-model", firstObject.modelRenderable().meshInstances(),
                                              glm::vec3(1.0f));
    const physics::StaticMeshShapeCacheKey secondObjectKey =
        physics::makeStaticMeshShapeCacheKey(
            "test-model", secondObject.modelRenderable().meshInstances(),
                                              glm::vec3(1.0f));
    expect(firstObjectKey == secondObjectKey,
           "position or rotation changed persistent cache identity");

    physics::PhysicsShapeCache firstCache(directory);
    const physics::ShapeCacheLoadResult missing = firstCache.load(key);
    expect(missing.status == physics::ShapeCacheLoadStatus::Miss,
           "missing cooked shape cache entry was not a miss");
    JPH::ShapeRefC original = makeCachedMeshShape();
    std::string saveError;
    expect(firstCache.save(key, *original, saveError),
           "failed to write cooked shape cache entry");
    const std::filesystem::path cachePath = firstCache.pathFor(key);
    expect(std::filesystem::exists(cachePath), "cooked cache final file missing");
    expect(!std::filesystem::exists(cachePath.string() + ".tmp"),
           "cooked cache temporary file remained after successful write");
    original = nullptr;

    physics::PhysicsShapeCache secondCache(directory);
    const physics::ShapeCacheLoadResult restored = secondCache.load(key);
    expect(restored.status == physics::ShapeCacheLoadStatus::Hit && restored.shape,
           "second cache instance did not restore cooked shape");
    expect(restored.shape->GetType() == JPH::EShapeType::Mesh,
           "restored cooked shape is not a mesh");
    expect(restored.shape->GetLocalBounds().Contains(JPH::Vec3(0.0f, 0.0f, 0.0f)),
           "restored cooked mesh bounds are invalid");

    const physics::StaticMeshShapeCacheKey corruptKey =
        physics::makeStaticMeshShapeCacheKey("corrupt-model", geometry, glm::vec3(1.0f));
    const std::filesystem::path corruptPath = secondCache.pathFor(corruptKey);
    std::filesystem::create_directories(directory, error);
    {
        std::ofstream corrupt(corruptPath, std::ios::binary | std::ios::trunc);
        corrupt << "garbage";
    }
    expect(secondCache.load(corruptKey).status == physics::ShapeCacheLoadStatus::Invalid,
           "corrupt cooked cache entry was accepted");

    const physics::StaticMeshShapeCacheKey versionKey =
        physics::makeStaticMeshShapeCacheKey("version-model", geometry, glm::vec3(1.0f));
    expect(secondCache.save(versionKey, *restored.shape, saveError),
           "failed to create version cache entry");
    {
        std::fstream versionFile(secondCache.pathFor(versionKey),
                                 std::ios::binary | std::ios::in | std::ios::out);
        const std::uint32_t incompatibleVersion = physics::physicsShapeCacheVersion + 1;
        versionFile.seekp(8);
        versionFile.write(reinterpret_cast<const char*>(&incompatibleVersion),
                          sizeof(incompatibleVersion));
    }
    expect(secondCache.load(versionKey).status == physics::ShapeCacheLoadStatus::Invalid,
           "incompatible cooked cache version was accepted");

    std::filesystem::remove_all(directory, error);
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void testOffOriginBodyPivot() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    BroadPhaseInterface broadPhaseInterface;
    ObjectVsBroadPhaseFilter objectVsBroadPhaseFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    system.Init(32, 0, 64, 32, broadPhaseInterface, objectVsBroadPhaseFilter, pairFilter);
    JPH::BodyInterface& bodies = system.GetBodyInterface();
    const JPH::ShapeRefC shape = makeOffOriginHull();
    const JPH::RVec3 authoredOrigin(7.0f, 8.0f, 9.0f);
    JPH::BodyCreationSettings settings(shape.GetPtr(), authoredOrigin,
                                       JPH::Quat::sIdentity(),
                                       JPH::EMotionType::Dynamic, 1);
    const JPH::BodyID body = bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    expect(!body.IsInvalid(), "failed to create off-origin test body");
    expect(bodies.GetPosition(body).IsClose(authoredOrigin, 1.0e-5f),
           "body origin was shifted to convex hull center of mass");
    expect(!bodies.GetCenterOfMassPosition(body).IsClose(authoredOrigin, 1.0e-5f),
           "off-origin hull unexpectedly has its center of mass at the object origin");
    const JPH::Quat rotated = JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 0.8f);
    bodies.SetPositionAndRotation(body, authoredOrigin, rotated, JPH::EActivation::Activate);
    expect(bodies.GetPosition(body).IsClose(authoredOrigin, 1.0e-5f) &&
               bodies.GetRotation(body).IsClose(rotated, 1.0e-5f),
           "body-origin transform did not survive rotation around an off-origin hull");
    bodies.RemoveBody(body);
    bodies.DestroyBody(body);
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void testDynamicConvexRotationIntegration() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    BroadPhaseInterface broadPhaseInterface;
    ObjectVsBroadPhaseFilter objectVsBroadPhaseFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    system.Init(128, 0, 1024, 256, broadPhaseInterface, objectVsBroadPhaseFilter, pairFilter);
    system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    JPH::TempAllocatorImpl allocator(2U * 1024U * 1024U);
    JPH::JobSystemThreadPool jobs(JPH::cMaxPhysicsJobs, JPH::cMaxPhysicsBarriers, 1);
    JPH::BodyInterface& bodies = system.GetBodyInterface();
    JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(10.0f, 0.5f, 10.0f)),
                                    JPH::RVec3(0.0f, -0.5f, 0.0f),
                                    JPH::Quat::sIdentity(), JPH::EMotionType::Static, 0);
    const JPH::BodyID floorId = bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate);
    const JPH::ShapeRefC hull = makeOffOriginHull();
    const JPH::Quat initialRotation =
        JPH::Quat::sRotation(JPH::Vec3::sAxisX(), 0.5f) *
        JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 0.2f);
    JPH::BodyCreationSettings prop(hull.GetPtr(), JPH::RVec3(0.0f, 4.0f, 0.0f),
                                   initialRotation, JPH::EMotionType::Dynamic, 1);
    const JPH::BodyID propId = bodies.CreateAndAddBody(prop, JPH::EActivation::Activate);
    expect(!floorId.IsInvalid() && !propId.IsInvalid(), "failed to create convex integration bodies");
    for (int frame = 0; frame < 360; ++frame) {
        (void)system.Update(1.0f / 60.0f, 1, &allocator, &jobs);
    }
    const JPH::RVec3 position = bodies.GetPosition(propId);
    const JPH::Quat rotation = bodies.GetRotation(propId);
    expect(std::isfinite(static_cast<float>(position.GetX())) &&
               std::isfinite(static_cast<float>(position.GetY())) &&
               std::isfinite(static_cast<float>(position.GetZ())) &&
               std::isfinite(rotation.GetX()) && std::isfinite(rotation.GetY()) &&
               std::isfinite(rotation.GetZ()) && std::isfinite(rotation.GetW()),
           "dynamic convex integration produced a non-finite transform");
    expect(position.GetY() < 4.0 && !rotation.IsClose(initialRotation, 1.0e-3f),
           "asymmetric dynamic convex body did not fall and rotate naturally");
    bodies.RemoveBody(propId);
    bodies.DestroyBody(propId);
    bodies.RemoveBody(floorId);
    bodies.DestroyBody(floorId);
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

MeshInstance floorInstance() {
    MeshInstance instance;
    instance.mesh.vertices = {
        {{-10.0f, 0.0f, -10.0f}, {}, {}, {}, {}},
        {{10.0f, 0.0f, -10.0f}, {}, {}, {}, {}},
        {{10.0f, 0.0f, 10.0f}, {}, {}, {}, {}},
        {{-10.0f, 0.0f, 10.0f}, {}, {}, {}, {}}};
    instance.mesh.indices = {0, 1, 2, 0, 2, 3};
    return instance;
}

MeshInstance characterFloorInstance() {
    MeshInstance instance;
    instance.mesh.vertices = {
        {{-1000.0f, 0.0f, -1000.0f}, {}, {}, {}, {}},
        {{1000.0f, 0.0f, -1000.0f}, {}, {}, {}, {}},
        {{1000.0f, 0.0f, 1000.0f}, {}, {}, {}, {}},
        {{-1000.0f, 0.0f, 1000.0f}, {}, {}, {}, {}}};
    instance.mesh.indices = {0, 2, 1, 0, 3, 2};
    return instance;
}

MeshInstance characterWallInstance() {
    MeshInstance instance;
    instance.mesh.vertices = {
        {{-1000.0f, 0.0f, -250.0f}, {}, {}, {}, {}},
        {{1000.0f, 0.0f, -250.0f}, {}, {}, {}, {}},
        {{1000.0f, 500.0f, -250.0f}, {}, {}, {}, {}},
        {{-1000.0f, 500.0f, -250.0f}, {}, {}, {}, {}}};
    instance.mesh.indices = {0, 1, 2, 0, 2, 3};
    return instance;
}

MeshInstance tetrahedronInstance() {
    MeshInstance instance;
    instance.mesh.vertices = {
        {{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 2.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 0.0f, 1.0f}, {}, {}, {}, {}}};
    return instance;
}

void advanceRuntimePhysics(PhysicsServer& server, int frameCount);

void testPhysicsServerColliderCombinations() {
    TestScene validScene;

    auto staticConvex = std::make_unique<GameObject>();
    GameObject* staticConvexPointer = staticConvex.get();
    staticConvex->name = "Static Convex Hull";
    staticConvex->position = {0.0f, 300.0f, 0.0f};
    staticConvex->physics.enabled = true;
    staticConvex->physics.motionType = GameObject::PhysicsMotionType::Static;
    staticConvex->physics.colliderType =
        GameObject::PhysicsColliderType::ConvexHull;
    expect(static_cast<bool>(staticConvex->modelRenderable().addMeshInstance(
               tetrahedronInstance())),
           "failed to add static convex-hull mesh");
    expect(static_cast<bool>(validScene.addGameObject(std::move(staticConvex))),
           "failed to add static convex-hull object");

    auto dynamicConvex = std::make_unique<GameObject>();
    dynamicConvex->name = "Dynamic Convex Hull";
    dynamicConvex->position = {100.0f, 300.0f, 0.0f};
    dynamicConvex->physics.enabled = true;
    dynamicConvex->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    dynamicConvex->physics.colliderType =
        GameObject::PhysicsColliderType::ConvexHull;
    expect(static_cast<bool>(dynamicConvex->modelRenderable().addMeshInstance(
               tetrahedronInstance())),
           "failed to add dynamic convex-hull mesh");
    expect(static_cast<bool>(validScene.addGameObject(std::move(dynamicConvex))),
           "failed to add dynamic convex-hull object");

    auto dynamicSphere = std::make_unique<GameObject>();
    dynamicSphere->name = "Dynamic Sphere";
    dynamicSphere->position = {-100.0f, 300.0f, 0.0f};
    dynamicSphere->physics.enabled = true;
    dynamicSphere->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    dynamicSphere->physics.colliderType =
        GameObject::PhysicsColliderType::Sphere;
    dynamicSphere->physics.sphereRadius = 20.0f;
    expect(static_cast<bool>(validScene.addGameObject(std::move(dynamicSphere))),
           "failed to add dynamic sphere object");

    PhysicsServer server;
    expect(static_cast<bool>(server.initialize()),
           "failed to initialize PhysicsServer for collider combinations");
    expect(static_cast<bool>(server.beginRuntimeSession(validScene)),
           "static convex hull or existing dynamic collider could not create a runtime body");
    expect(server.runtimeSessionActive(),
           "valid collider combinations did not keep the runtime session active");

    const glm::vec3 stationaryPosition{0.0f, 450.0f, 0.0f};
    server.applyRuntimeTransform(*staticConvexPointer, stationaryPosition,
                                 glm::vec3(0.0f), false);
    advanceRuntimePhysics(server, 6);
    expect(near(staticConvexPointer->position.y, stationaryPosition.y),
           "static convex hull was integrated as a dynamic body");
    server.endRuntimeSession();

    TestScene rejectedScene;
    auto dynamicMesh = std::make_unique<GameObject>();
    dynamicMesh->name = "Dynamic Mesh";
    dynamicMesh->physics.enabled = true;
    dynamicMesh->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    dynamicMesh->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(dynamicMesh->modelRenderable().addMeshInstance(
               triangleInstance(glm::vec3(0.0f)))),
           "failed to add rejected dynamic-mesh input");
    expect(static_cast<bool>(rejectedScene.addGameObject(std::move(dynamicMesh))),
           "failed to add rejected dynamic-mesh object");
    expect(!server.beginRuntimeSession(rejectedScene),
           "dynamic mesh collider was accepted by the runtime session");
    expect(!server.runtimeSessionActive(),
           "rejected dynamic-mesh session remained active");
    server.shutdown();
}

void testPhysicsServerEditorRelease() {
    TestScene scene;
    auto floor = std::make_unique<GameObject>();
    floor->physics.enabled = true;
    floor->physics.motionType = GameObject::PhysicsMotionType::Static;
    floor->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(floor->modelRenderable().addMeshInstance(
               floorInstance())), "failed to add floor mesh");
    expect(static_cast<bool>(scene.addGameObject(std::move(floor))), "failed to add floor object");
    auto prop = std::make_unique<GameObject>();
    GameObject* propPointer = prop.get();
    prop->position = {0.0f, 4.0f, 0.0f};
    prop->rotation = {15.0f, 20.0f, 0.0f};
    prop->physics.enabled = true;
    prop->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    prop->physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    expect(static_cast<bool>(prop->modelRenderable().addMeshInstance(
               tetrahedronInstance())), "failed to add prop mesh");
    expect(static_cast<bool>(scene.addGameObject(std::move(prop))), "failed to add prop object");

    PhysicsServer server;
    expect(static_cast<bool>(server.initialize()), "failed to initialize PhysicsServer");
    expect(static_cast<bool>(server.beginRuntimeSession(scene)), "failed to begin physics session");
    const glm::vec3 heldPosition{0.0f, 6.0f, 0.0f};
    const glm::vec3 heldRotation{25.0f, 30.0f, 10.0f};
    server.applyRuntimeTransform(*propPointer, heldPosition, heldRotation, true);
    TimeTestAccess::initialize();
    TimeTestAccess::update();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TimeTestAccess::update();
    server.update();
    expect(near(propPointer->position.y, 6.0f, 1.0e-3f),
           "physics overwrote an active editor transform override");
    server.applyRuntimeTransform(*propPointer, heldPosition, heldRotation, false);
    TimeTestAccess::initialize();
    TimeTestAccess::update();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TimeTestAccess::update();
    server.update();
    expect(propPointer->position.y < 6.0f,
           "released editor transform did not resume gravity");
    server.endRuntimeSession();
    server.shutdown();
}

void advanceRuntimePhysics(PhysicsServer& server, int frameCount) {
    TimeTestAccess::initialize();
    for (int frame = 0; frame < frameCount; ++frame) {
        std::this_thread::sleep_for(std::chrono::milliseconds(18));
        TimeTestAccess::update();
        server.update();
    }
}

void testRuntimeCharacterFloorWallAndLifecycle() {
    TestScene scene;

    auto floor = std::make_unique<GameObject>();
    floor->name = "Character Test Floor";
    floor->modelPath = "character_test_floor";
    floor->physics.enabled = true;
    floor->physics.motionType = GameObject::PhysicsMotionType::Static;
    floor->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(floor->modelRenderable().addMeshInstance(
               characterFloorInstance())),
           "failed to add character test floor mesh");
    expect(static_cast<bool>(scene.addGameObject(std::move(floor))),
           "failed to add character test floor");

    auto wall = std::make_unique<GameObject>();
    wall->name = "Character Test Wall";
    wall->modelPath = "character_test_wall";
    wall->physics.enabled = true;
    wall->physics.motionType = GameObject::PhysicsMotionType::Static;
    wall->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(wall->modelRenderable().addMeshInstance(
               characterWallInstance())),
           "failed to add character test wall mesh");
    expect(static_cast<bool>(scene.addGameObject(std::move(wall))),
           "failed to add character test wall");

    auto character = std::make_unique<Character>();
    Character* characterPointer = character.get();
    character->position = {0.0f, 300.0f, 0.0f};
    character->desiredVelocity = {200.0f, 0.0f, -300.0f};
    expect(static_cast<bool>(scene.addGameObject(std::move(character))),
           "failed to add runtime character");

    PhysicsServer server;
    expect(static_cast<bool>(server.initialize()),
           "failed to initialize character PhysicsServer");
    expect(static_cast<bool>(server.beginRuntimeSession(scene)),
           "failed to begin character physics session");
    advanceRuntimePhysics(server, 150);
    expect(characterPointer->grounded,
           "runtime character did not become supported by the static floor");
    expect(characterPointer->position.y >= -1.0f &&
               characterPointer->position.y <= 5.0f,
           "runtime character did not settle at the static floor");
    expect(characterPointer->position.z > -220.0f,
           "runtime character passed through the static wall");
    expect(characterPointer->position.x > 100.0f,
           "runtime character did not slide along the static wall");

    server.endRuntimeSession();
    expect(!server.runtimeSessionActive(),
           "character physics session remained active after teardown");
    expect(static_cast<bool>(server.beginRuntimeSession(scene)),
           "character physics session could not be recreated after teardown");
    server.endRuntimeSession();
    server.shutdown();
}

void testDynamicPositionAndGravityWorldScale() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    BroadPhaseInterface broadPhaseInterface;
    ObjectVsBroadPhaseFilter objectVsBroadPhaseFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    system.Init(32, 0, 64, 32, broadPhaseInterface, objectVsBroadPhaseFilter,
                pairFilter);
    system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    JPH::TempAllocatorImpl allocator(2U * 1024U * 1024U);
    JPH::JobSystemThreadPool jobs(JPH::cMaxPhysicsJobs,
                                  JPH::cMaxPhysicsBarriers, 1);
    JPH::BodyInterface& bodies = system.GetBodyInterface();

    const glm::vec3 authoredPosition(0.0f, 300.0f, 0.0f);
    const glm::vec3 physicalPosition = physics::dunamisToMeters(authoredPosition);
    JPH::BodyCreationSettings settings(
        new JPH::SphereShape(0.05f),
        JPH::RVec3(physicalPosition.x, physicalPosition.y, physicalPosition.z),
        JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 1);
    const JPH::BodyID body =
        bodies.CreateAndAddBody(settings, JPH::EActivation::Activate);
    expect(!body.IsInvalid(), "failed to create world-scale dynamic body");
    expect(near(static_cast<float>(bodies.GetPosition(body).GetY()), 3.0f),
           "300 authored DU was not represented as 3 Jolt meters");
    for (int step = 0; step < 60; ++step) {
        (void)system.Update(1.0f / 60.0f, 1, &allocator, &jobs);
    }
    const float resultingDunamisY = physics::metersToDunamis(
        static_cast<float>(bodies.GetPosition(body).GetY()));
    const float displacementDunamisY = resultingDunamisY - authoredPosition.y;
    expect(displacementDunamisY < -450.0f && displacementDunamisY > -550.0f,
           "one second of -9.81 m/s^2 did not produce meter-scaled DU fall");
    bodies.RemoveBody(body);
    bodies.DestroyBody(body);
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void testStaticTransformAndWake() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    BroadPhaseInterface broadPhaseInterface;
    ObjectVsBroadPhaseFilter objectVsBroadPhaseFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    system.Init(32, 0, 64, 32, broadPhaseInterface, objectVsBroadPhaseFilter,
                pairFilter);
    system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    JPH::TempAllocatorImpl allocator(2U * 1024U * 1024U);
    JPH::JobSystemThreadPool jobs(JPH::cMaxPhysicsJobs,
                                  JPH::cMaxPhysicsBarriers, 1);
    JPH::BodyInterface& bodies = system.GetBodyInterface();
    JPH::BodyCreationSettings floorSettings(
        new JPH::BoxShape(JPH::Vec3(10.0f, 0.5f, 10.0f)),
        JPH::RVec3(0.0f, -0.5f, 0.0f), JPH::Quat::sIdentity(),
        JPH::EMotionType::Static, 0);
    const JPH::BodyID floor = bodies.CreateAndAddBody(
        floorSettings, JPH::EActivation::DontActivate);
    JPH::BodyCreationSettings propSettings(
        new JPH::SphereShape(0.5f), JPH::RVec3(0.0f, 3.0f, 0.0f),
        JPH::Quat::sIdentity(), JPH::EMotionType::Dynamic, 1);
    const JPH::BodyID prop = bodies.CreateAndAddBody(
        propSettings, JPH::EActivation::Activate);
    expect(!floor.IsInvalid() && !prop.IsInvalid(),
           "failed to create static transform regression bodies");
    for (int step = 0; step < 360; ++step) {
        (void)system.Update(1.0f / 60.0f, 1, &allocator, &jobs);
    }
    const float restedY = static_cast<float>(bodies.GetPosition(prop).GetY());
    expect(restedY >= 0.45f && restedY <= 0.60f,
           "dynamic body did not settle on static floor");

    const JPH::RVec3 loweredFloor(0.0f, -3.0f, 0.0f);
    const JPH::Quat rotatedFloor =
        JPH::Quat::sRotation(JPH::Vec3::sAxisZ(), 0.35f);
    bodies.SetPositionAndRotation(floor, loweredFloor, rotatedFloor,
                                  JPH::EActivation::DontActivate);
    const JPH::BodyID dynamicBodies[] = {prop};
    bodies.ActivateBodies(dynamicBodies, 1);
    expect(bodies.GetPosition(floor).IsClose(loweredFloor, 1.0e-5f) &&
               bodies.GetRotation(floor).IsClose(rotatedFloor, 1.0e-5f),
           "static body did not retain runtime translation and rotation");
    for (int step = 0; step < 30; ++step) {
        (void)system.Update(1.0f / 60.0f, 1, &allocator, &jobs);
    }
    expect(static_cast<float>(bodies.GetPosition(prop).GetY()) < restedY - 0.5f,
           "woken dynamic body did not fall after static floor moved");
    bodies.RemoveBody(prop);
    bodies.DestroyBody(prop);
    bodies.RemoveBody(floor);
    bodies.DestroyBody(floor);
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

void testFloorSphere() {
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();

    BroadPhaseInterface broadPhaseInterface;
    ObjectVsBroadPhaseFilter objectVsBroadPhaseFilter;
    PairFilter pairFilter;
    JPH::PhysicsSystem system;
    system.Init(128, 0, 1024, 256, broadPhaseInterface,
                objectVsBroadPhaseFilter, pairFilter);
    system.SetGravity(JPH::Vec3(0.0f, -9.81f, 0.0f));
    JPH::TempAllocatorImpl allocator(2U * 1024U * 1024U);
    JPH::JobSystemThreadPool jobs(JPH::cMaxPhysicsJobs,
                                  JPH::cMaxPhysicsBarriers, 1);
    JPH::BodyInterface& bodies = system.GetBodyInterface();

    JPH::BodyCreationSettings floor(new JPH::BoxShape(JPH::Vec3(10.0f, 0.5f, 10.0f)),
                                    JPH::RVec3(0.0f, -0.5f, 0.0f),
                                    JPH::Quat::sIdentity(),
                                    JPH::EMotionType::Static, 0);
    const JPH::BodyID floorId = bodies.CreateAndAddBody(floor, JPH::EActivation::DontActivate);
    JPH::BodyCreationSettings sphere(new JPH::SphereShape(0.5f),
                                     JPH::RVec3(0.0f, 3.0f, 0.0f),
                                     JPH::Quat::sIdentity(),
                                     JPH::EMotionType::Dynamic, 1);
    const JPH::BodyID sphereId = bodies.CreateAndAddBody(sphere, JPH::EActivation::Activate);
    expect(!floorId.IsInvalid() && !sphereId.IsInvalid(), "failed to create smoke-test bodies");
    const float startY = static_cast<float>(bodies.GetPosition(sphereId).GetY());
    for (int frame = 0; frame < 30; ++frame) {
        (void)system.Update(1.0f / 60.0f, 1, &allocator, &jobs);
    }
    const float fallingY = static_cast<float>(bodies.GetPosition(sphereId).GetY());
    expect(fallingY < startY, "sphere did not fall under gravity");
    for (int frame = 0; frame < 300; ++frame) {
        (void)system.Update(1.0f / 60.0f, 1, &allocator, &jobs);
    }
    const float finalY = static_cast<float>(bodies.GetPosition(sphereId).GetY());
    expect(finalY >= 0.45f && finalY <= 0.60f,
           "sphere did not reach a stable nonpenetrating floor rest height");
    bodies.RemoveBody(sphereId);
    bodies.DestroyBody(sphereId);
    bodies.RemoveBody(floorId);
    bodies.DestroyBody(floorId);
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
}

}  // namespace

int main() {
    testAccumulator();
    testPhysicsUnits();
    testScaledLocalTriangleMesh();
    testConvexHullInputAndRotation();
    testShapeCenterOfMassTransform();
    testCharacterShapeCenterOfMassTransform();
    testShapeDiagnostics();
    testCookedShapeCache();
    testPhysicsServerStaticShapeSharing();
    testPhysicsServerColliderCombinations();
    testOffOriginBodyPivot();
    testDynamicConvexRotationIntegration();
    testFloorSphere();
    testPhysicsServerEditorRelease();
    testRuntimeCharacterFloorWallAndLifecycle();
    testDynamicPositionAndGravityWorldScale();
    testStaticTransformAndWake();
    return EXIT_SUCCESS;
}
