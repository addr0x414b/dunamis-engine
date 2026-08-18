// Jolt requires this include ordering.
#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/JobSystemThreadPool.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/RegisterTypes.h>

#include <cmath>
#include <cstdlib>
#include <iostream>

#include "../physics/physics_mesh_builder.h"
#include "../physics/physics_server.h"

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
    testFloorSphere();
    return EXIT_SUCCESS;
}
