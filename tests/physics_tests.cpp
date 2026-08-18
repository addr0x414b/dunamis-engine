// Jolt requires this include ordering.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <thread>

#include "../core/time.h"
#include "../physics/physics_mesh_builder.h"
#include "../physics/physics_server.h"
#include "../scene/game_object.h"
#include "../scene/scene.h"

class TimeTestAccess {
public:
    static void initialize() { Time::initialize(); }
    static void update() { Time::update(); }
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
    void init() override {}
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

void testMeshTransforms() {
    physics::WorldTriangleMesh output;
    const glm::vec3 zero(0.0f);
    const glm::vec3 one(1.0f);
    MeshInstance instance = triangleInstance(glm::vec3(0.0f));
    expect(static_cast<bool>(physics::buildWorldTriangleMesh(
               {instance}, glm::vec3(3.0f, -2.0f, 5.0f), zero, one, output)),
           "translation conversion failed");
    expect(near(output.vertices[0].x, 3.0f) && near(output.vertices[0].y, -2.0f) &&
               near(output.vertices[0].z, 5.0f),
           "translation conversion is incorrect");

    instance = triangleInstance(glm::vec3(0.0f, 1.0f, 0.0f));
    expect(static_cast<bool>(physics::buildWorldTriangleMesh(
               {instance}, zero, glm::vec3(90.0f, 0.0f, 0.0f), one, output)),
           "X rotation conversion failed");
    expect(near(output.vertices[0].z, 1.0f), "X rotation is incorrect");
    instance = triangleInstance(glm::vec3(1.0f, 0.0f, 0.0f));
    expect(static_cast<bool>(physics::buildWorldTriangleMesh(
               {instance}, zero, glm::vec3(0.0f, 90.0f, 0.0f), one, output)),
           "Y rotation conversion failed");
    expect(near(output.vertices[0].z, -1.0f), "Y rotation is incorrect");
    expect(static_cast<bool>(physics::buildWorldTriangleMesh(
               {instance}, zero, glm::vec3(0.0f, 0.0f, 90.0f), one, output)),
           "Z rotation conversion failed");
    expect(near(output.vertices[0].y, 1.0f), "Z rotation is incorrect");
    expect(static_cast<bool>(physics::buildWorldTriangleMesh(
               {instance}, zero, zero, glm::vec3(2.0f, 3.0f, 4.0f), output)),
           "scale conversion failed");
    expect(near(output.vertices[0].x, 2.0f), "scale conversion is incorrect");

    expect(static_cast<bool>(physics::buildWorldTriangleMesh(
               {instance}, glm::vec3(4.0f, 5.0f, 6.0f),
               glm::vec3(90.0f, 90.0f, 90.0f), glm::vec3(2.0f, 3.0f, 1.0f),
               output)),
           "combined transform conversion failed");
    expect(near(output.vertices[0].x, 4.0f) && near(output.vertices[0].y, 5.0f) &&
               near(output.vertices[0].z, 8.0f),
           "T * Rx * Ry * Rz * S conversion is incorrect");

    instance.mesh.indices = {0, 1, 3};
    expect(!physics::buildWorldTriangleMesh({instance}, zero, zero, one, output),
           "invalid mesh index was accepted");
    instance = triangleInstance(glm::vec3(0.0f));
    expect(!physics::buildWorldTriangleMesh({instance},
                                            glm::vec3(NAN, 0.0f, 0.0f), zero,
                                            one, output),
           "non-finite transform was accepted");
}

void testConvexHullInputAndRotation() {
    MeshInstance instance;
    instance.mesh.vertices = {
        {{1.0f, 2.0f, 3.0f}, {}, {}, {}, {}},
        {{2.0f, 2.0f, 3.0f}, {}, {}, {}, {}},
        {{1.0f, 3.0f, 3.0f}, {}, {}, {}, {}},
        {{1.0f, 2.0f, 4.0f}, {}, {}, {}, {}}};
    physics::LocalConvexHull hull;
    expect(static_cast<bool>(physics::buildScaledLocalConvexHull(
               {instance}, glm::vec3(2.0f, 3.0f, 4.0f), hull)),
           "scaled convex hull conversion failed");
    expect(hull.points.size() == 4 && hull.points[0] == glm::vec3(2.0f, 6.0f, 12.0f),
           "convex hull points do not preserve scaled local coordinates");
    expect(!physics::buildScaledLocalConvexHull({instance}, glm::vec3(0.0f), hull),
           "near-zero convex scale was accepted");
    expect(!physics::buildScaledLocalConvexHull({}, glm::vec3(1.0f), hull),
           "empty convex geometry was accepted");
    instance.mesh.vertices[0].pos.x = std::numeric_limits<float>::quiet_NaN();
    expect(!physics::buildScaledLocalConvexHull({instance}, glm::vec3(1.0f), hull),
           "non-finite convex vertex was accepted");

    const glm::vec3 rotations[] = {
        {37.0f, 0.0f, 0.0f}, {0.0f, -51.0f, 0.0f}, {0.0f, 0.0f, 83.0f},
        {23.0f, -41.0f, 67.0f}, {-31.0f, 44.0f, -72.0f}, {10.0f, 89.0f, -15.0f}};
    for (const glm::vec3& rotation : rotations) {
        glm::vec3 extracted;
        const glm::mat4 original = physics::makeDunamisRotationMatrix(rotation);
        expect(physics::extractDunamisRotation(original, extracted),
               "Dunamis rotation extraction failed");
        expect(matrixNear(physics::makeDunamisRotationMatrix(extracted), original),
               "Dunamis rotation matrix round trip changed orientation");
    }
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

MeshInstance tetrahedronInstance() {
    MeshInstance instance;
    instance.mesh.vertices = {
        {{0.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{1.0f, 0.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 2.0f, 0.0f}, {}, {}, {}, {}},
        {{0.0f, 0.0f, 1.0f}, {}, {}, {}, {}}};
    return instance;
}

void testPhysicsServerEditorRelease() {
    TestScene scene;
    auto floor = std::make_unique<GameObject>();
    floor->physics.enabled = true;
    floor->physics.motionType = GameObject::PhysicsMotionType::Static;
    floor->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    expect(static_cast<bool>(floor->addMeshInstance(floorInstance())), "failed to add floor mesh");
    expect(static_cast<bool>(scene.addGameObject(std::move(floor))), "failed to add floor object");
    auto prop = std::make_unique<GameObject>();
    GameObject* propPointer = prop.get();
    prop->position = {0.0f, 4.0f, 0.0f};
    prop->rotation = {15.0f, 20.0f, 0.0f};
    prop->physics.enabled = true;
    prop->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    prop->physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    expect(static_cast<bool>(prop->addMeshInstance(tetrahedronInstance())), "failed to add prop mesh");
    expect(static_cast<bool>(scene.addGameObject(std::move(prop))), "failed to add prop object");

    PhysicsServer server;
    expect(static_cast<bool>(server.initialize()), "failed to initialize PhysicsServer");
    expect(static_cast<bool>(server.beginRuntimeSession(scene)), "failed to begin physics session");
    const RuntimeTransformEdit held{propPointer, {0.0f, 6.0f, 0.0f}, {25.0f, 30.0f, 10.0f}, true};
    server.applyRuntimeTransformEdit(held);
    TimeTestAccess::initialize();
    TimeTestAccess::update();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    TimeTestAccess::update();
    server.update();
    expect(near(propPointer->position.y, 6.0f, 1.0e-3f),
           "physics overwrote an active editor transform override");
    RuntimeTransformEdit released = held;
    released.manipulating = false;
    server.applyRuntimeTransformEdit(released);
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
    testMeshTransforms();
    testConvexHullInputAndRotation();
    testOffOriginBodyPivot();
    testDynamicConvexRotationIntegration();
    testFloorSphere();
    testPhysicsServerEditorRelease();
    return EXIT_SUCCESS;
}
