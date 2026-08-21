#include "jolt_shape_builder.h"

#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <cmath>
#include <cstring>

#include <glm/gtc/quaternion.hpp>

#include "physics_mesh_builder.h"
#include "physics_units.h"
#include "../scene/character.h"
#include "../scene/game_object.h"
#include "../scene/model_renderable.h"

namespace physics {
namespace {

std::uint32_t floatBits(float value) noexcept {
    const float normalized = value == 0.0f ? 0.0f : value;
    std::uint32_t bits = 0;
    std::memcpy(&bits, &normalized, sizeof(bits));
    return bits;
}

std::string modelIdentity(const GameObject& object) {
    std::string identity = object.authoredModelPath();
    if (!identity.empty()) return identity;
    for (const MeshInstance& instance : object.modelRenderable().meshInstances()) {
        if (instance.mesh.modelPath != nullptr && instance.mesh.modelPath[0] != '\0') {
            return instance.mesh.modelPath;
        }
    }
    return {};
}

void setStats(const JPH::Shape& shape, ShapeDiagnostics& diagnostics) {
    const JPH::Shape::Stats stats = shape.GetStats();
    diagnostics.joltTriangles = stats.mNumTriangles;
    diagnostics.joltBytes = stats.mSizeBytes;
}

Result cookMesh(const GameObject& object, CookedShape& output) {
    ScaledLocalTriangleMesh mesh;
    Result result = buildScaledLocalTriangleMesh(
        object.modelRenderable().meshInstances(), object.scale, mesh);
    if (!result) return result;
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
    JPH::MeshShapeSettings settings(std::move(triangles));
    settings.SetEmbedded();
    JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) return Result::failure(shapeResult.GetError().c_str());
    output = {};
    output.shape = shapeResult.Get();
    output.diagnostics.representation = ShapeDiagnostics::Representation::TriangleMesh;
    output.diagnostics.inputVertices = mesh.vertices.size();
    output.diagnostics.inputTriangles = mesh.indices.size() / 3;
    setStats(*output.shape, output.diagnostics);
    return Result::success();
}

Result cookConvexHull(const GameObject& object, CookedShape& output) {
    LocalConvexHull hull;
    Result result = buildScaledLocalConvexHull(
        object.modelRenderable().meshInstances(), object.scale, hull);
    if (!result) return result;
    JPH::Array<JPH::Vec3> points;
    points.reserve(hull.points.size());
    for (const glm::vec3& point : hull.points) points.emplace_back(point.x, point.y, point.z);
    JPH::ConvexHullShapeSettings settings(points);
    JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) return Result::failure(shapeResult.GetError().c_str());
    output = {};
    output.shape = shapeResult.Get();
    output.diagnostics.representation = ShapeDiagnostics::Representation::ConvexHull;
    output.diagnostics.inputPoints = hull.points.size();
    if (output.shape->GetSubType() == JPH::EShapeSubType::ConvexHull)
        output.diagnostics.cookedHullVertices = static_cast<const JPH::ConvexHullShape*>(output.shape.GetPtr())->GetNumPoints();
    setStats(*output.shape, output.diagnostics);
    return Result::success();
}

}  // namespace

ShapeDefinitionSignature makeGameObjectShapeDefinitionSignature(
    const GameObject& object) {
    ShapeDefinitionSignature signature;
    switch (object.physics.colliderType) {
    case GameObject::PhysicsColliderType::Mesh:
        signature.type = ShapeDefinitionSignature::Type::Mesh;
        signature.modelIdentity = modelIdentity(object);
        signature.scaleBits = {floatBits(object.scale.x), floatBits(object.scale.y),
                               floatBits(object.scale.z)};
        break;
    case GameObject::PhysicsColliderType::ConvexHull:
        signature.type = ShapeDefinitionSignature::Type::ConvexHull;
        signature.modelIdentity = modelIdentity(object);
        signature.scaleBits = {floatBits(object.scale.x), floatBits(object.scale.y),
                               floatBits(object.scale.z)};
        break;
    case GameObject::PhysicsColliderType::Sphere:
        signature.type = ShapeDefinitionSignature::Type::Sphere;
        signature.radiusBits = floatBits(object.physics.sphereRadius);
        break;
    }
    return signature;
}

ShapeDefinitionSignature makeCharacterShapeDefinitionSignature(
    const Character& character) {
    ShapeDefinitionSignature signature;
    signature.type = ShapeDefinitionSignature::Type::CharacterCapsule;
    signature.radiusBits = floatBits(character.capsuleRadius);
    signature.heightBits = floatBits(character.capsuleHeight);
    return signature;
}

JPH::RVec3 toJoltPosition(const glm::vec3& position) noexcept {
    const glm::vec3 meters = dunamisToMeters(position);
    return JPH::RVec3(meters.x, meters.y, meters.z);
}

JPH::Quat toJoltRotation(const glm::vec3& rotation) noexcept {
    const glm::quat quaternion = glm::quat_cast(makeDunamisRotationMatrix(rotation));
    return JPH::Quat(quaternion.x, quaternion.y, quaternion.z, quaternion.w).Normalized();
}

JPH::RMat44 makeShapeCenterOfMassTransform(
    const JPH::Shape& shape, const glm::vec3& bodyOriginPosition,
    const glm::vec3& bodyRotation) noexcept {
    // Jolt's body creation position is the authored body origin, while Draw
    // expects a transform for the shape's center of mass. PreTranslated uses
    // the rotation part of this transform to rotate the local COM offset.
    return JPH::RMat44::sRotationTranslation(
               toJoltRotation(bodyRotation), toJoltPosition(bodyOriginPosition))
        .PreTranslated(shape.GetCenterOfMass());
}

glm::mat4 joltTransformToDunamis(const JPH::RMat44& transform) noexcept {
    glm::mat4 result(1.0f);
    for (JPH::uint column = 0; column < 3; ++column) {
        const JPH::Vec3 axis = transform.GetColumn3(column);
        result[column] = glm::vec4(axis.GetX(), axis.GetY(), axis.GetZ(), 0.0f);
    }
    const auto translation = transform.GetTranslation();
    result[3] = glm::vec4(metersToDunamis(glm::vec3(
        static_cast<float>(translation.GetX()), static_cast<float>(translation.GetY()),
        static_cast<float>(translation.GetZ()))), 1.0f);
    return result;
}

Result buildGameObjectShape(const GameObject& object, CookedShape& output) {
    if (!object.physics.enabled) return Result::failure("physics is disabled");
    switch (object.physics.colliderType) {
    case GameObject::PhysicsColliderType::Mesh: return cookMesh(object, output);
    case GameObject::PhysicsColliderType::ConvexHull: return cookConvexHull(object, output);
    case GameObject::PhysicsColliderType::Sphere:
        if (!(object.physics.sphereRadius > 0.0f) || !std::isfinite(object.physics.sphereRadius))
            return Result::failure("sphere radius must be finite and positive");
        output = {};
        output.shape = new JPH::SphereShape(dunamisToMeters(object.physics.sphereRadius));
        output.diagnostics.representation = ShapeDiagnostics::Representation::AnalyticSphere;
        output.diagnostics.radius = object.physics.sphereRadius;
        setStats(*output.shape, output.diagnostics);
        return Result::success();
    }
    return Result::failure("unsupported collider type");
}

Result buildCharacterShape(const Character& character, CookedShape& output) {
    if (!(character.capsuleHeight > 2.0f * character.capsuleRadius) ||
        !(character.capsuleRadius > 0.0f) || !std::isfinite(character.capsuleHeight) ||
        !std::isfinite(character.capsuleRadius))
        return Result::failure("capsule dimensions must be finite, positive, and taller than its diameter");
    const float radius = dunamisToMeters(character.capsuleRadius);
    const float halfCylinder = dunamisToMeters(0.5f * character.capsuleHeight - character.capsuleRadius);
    JPH::RotatedTranslatedShapeSettings settings(
        JPH::Vec3(0.0f, 0.5f * dunamisToMeters(character.capsuleHeight), 0.0f),
        JPH::Quat::sIdentity(), new JPH::CapsuleShape(halfCylinder, radius));
    JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    if (shapeResult.HasError()) return Result::failure(shapeResult.GetError().c_str());
    output = {};
    output.shape = shapeResult.Get();
    output.diagnostics.representation = ShapeDiagnostics::Representation::AnalyticCapsule;
    output.diagnostics.radius = character.capsuleRadius;
    output.diagnostics.height = character.capsuleHeight;
    setStats(*output.shape, output.diagnostics);
    return Result::success();
}

}  // namespace physics
