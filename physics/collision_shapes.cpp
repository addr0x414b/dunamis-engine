#include "collision_shapes.h"

#include <Jolt/Geometry/Triangle.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>

#include <cmath>
#include <chrono>
#include <cstdint>
#include <limits>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include "physics_units.h"
#include "../assets/model_asset.h"
#include "../scene/character.h"
#include "../scene/game_object.h"
#include "../scene/model_renderable.h"

namespace physics {
namespace {

// Local collision geometry in Jolt meters. Authored position and rotation are
// deliberately excluded; they belong to the body's transform.
struct ScaledLocalTriangleMesh {
    std::vector<glm::vec3> vertices;
    std::vector<std::uint32_t> indices;
};

// Local collision points in Jolt meters. Position and rotation deliberately do
// not participate in this conversion.
struct LocalConvexHull {
    std::vector<glm::vec3> points;
};

bool isFinite(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

glm::mat4 makeDunamisRotationMatrix(const glm::vec3& rotation) noexcept {
    glm::mat4 result(1.0f);
    result = glm::rotate(result, glm::radians(rotation.x),
                         glm::vec3(1.0f, 0.0f, 0.0f));
    result = glm::rotate(result, glm::radians(rotation.y),
                         glm::vec3(0.0f, 1.0f, 0.0f));
    return glm::rotate(result, glm::radians(rotation.z),
                       glm::vec3(0.0f, 0.0f, 1.0f));
}

Result buildScaledLocalConvexHull(const std::vector<MeshInstance>& instances,
                                  const glm::vec3& scale,
                                  LocalConvexHull& output) {
    output = {};
    if (!isFinite(scale)) {
        return Result::failure("GameObject scale contains a non-finite value");
    }
    constexpr float minimumScale = 1.0e-6f;
    if (std::abs(scale.x) < minimumScale || std::abs(scale.y) < minimumScale ||
        std::abs(scale.z) < minimumScale) {
        return Result::failure("Dynamic convex hull scale must not be near zero");
    }
    for (const MeshInstance& instance : instances) {
        const Mesh& mesh = instance.mesh;
        if (mesh.vertices.empty()) {
            return Result::failure("Mesh has no vertices");
        }
        for (const Vertex& vertex : mesh.vertices) {
            if (!isFinite(vertex.pos)) {
                return Result::failure("Mesh contains a non-finite vertex position");
            }
            const glm::vec3 scaledDunamisUnits = vertex.pos * scale;
            if (!isFinite(scaledDunamisUnits)) {
                return Result::failure("Scale produced a non-finite convex hull point");
            }
            output.points.push_back(dunamisToMeters(scaledDunamisUnits));
        }
    }
    if (output.points.empty()) {
        return Result::failure("Configured convex hull has no vertices");
    }
    if (output.points.size() < 4) {
        return Result::failure("Convex hull requires at least four points");
    }
    constexpr float degeneracyTolerance = 1.0e-8f;
    bool hasVolume = false;
    const glm::vec3 origin = output.points.front();
    for (std::size_t first = 1; first < output.points.size() && !hasVolume;
         ++first) {
        for (std::size_t second = first + 1;
             second < output.points.size() && !hasVolume; ++second) {
            const glm::vec3 normal = glm::cross(output.points[first] - origin,
                                                output.points[second] - origin);
            if (glm::dot(normal, normal) <= degeneracyTolerance) {
                continue;
            }
            for (std::size_t third = second + 1;
                 third < output.points.size(); ++third) {
                if (std::abs(glm::dot(normal, output.points[third] - origin)) >
                    degeneracyTolerance) {
                    hasVolume = true;
                    break;
                }
            }
        }
    }
    if (!hasVolume) {
        return Result::failure("Convex hull points are degenerate");
    }
    return Result::success();
}

Result buildScaledLocalTriangleMesh(
    const std::vector<MeshInstance>& instances, const glm::vec3& scale,
    ScaledLocalTriangleMesh& output) {
    output = {};
    if (!isFinite(scale)) {
        return Result::failure("GameObject scale contains a non-finite value");
    }
    for (const MeshInstance& instance : instances) {
        const Mesh& mesh = instance.mesh;
        if (mesh.vertices.empty()) {
            return Result::failure("Mesh has no vertices");
        }
        if (mesh.indices.empty() || mesh.indices.size() % 3 != 0) {
            return Result::failure("Mesh indices must contain complete triangles");
        }
        if (output.vertices.size() + mesh.vertices.size() >
            std::numeric_limits<std::uint32_t>::max()) {
            return Result::failure("Combined mesh has too many vertices");
        }
        const std::uint32_t offset =
            static_cast<std::uint32_t>(output.vertices.size());
        for (const Vertex& vertex : mesh.vertices) {
            if (!isFinite(vertex.pos)) {
                return Result::failure("Mesh contains a non-finite vertex position");
            }
            const glm::vec3 scaledDunamisUnits = vertex.pos * scale;
            if (!isFinite(scaledDunamisUnits)) {
                return Result::failure("Mesh scale produced a non-finite vertex position");
            }
            output.vertices.push_back(dunamisToMeters(scaledDunamisUnits));
        }
        for (const std::uint32_t index : mesh.indices) {
            if (index >= mesh.vertices.size()) {
                return Result::failure("Mesh index is outside its vertex range");
            }
            output.indices.push_back(offset + index);
        }
    }
    if (output.indices.empty()) {
        return Result::failure("Configured mesh collider has no triangles");
    }
    return Result::success();
}

void setStats(const JPH::Shape& shape, ShapeDiagnostics& diagnostics) {
    const JPH::Shape::Stats stats = shape.GetStats();
    diagnostics.joltTriangles = stats.mNumTriangles;
    diagnostics.joltBytes = stats.mSizeBytes;
}

Result cookMesh(const GameObject& object, CookedShape& output) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point geometryStart = Clock::now();
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
    const Clock::duration geometryConversion = Clock::now() - geometryStart;
    const Clock::time_point cookingStart = Clock::now();
    JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    const Clock::duration joltCooking = Clock::now() - cookingStart;
    if (shapeResult.HasError()) {
        return Result::failure(shapeResult.GetError().c_str());
    }

    output = {};
    output.shape = shapeResult.Get();
    output.timings.geometryConversion = geometryConversion;
    output.timings.joltCooking = joltCooking;
    output.diagnostics.representation =
        ShapeDiagnostics::Representation::TriangleMesh;
    output.diagnostics.inputVertices = mesh.vertices.size();
    output.diagnostics.inputTriangles = mesh.indices.size() / 3;
    setStats(*output.shape, output.diagnostics);
    return Result::success();
}

Result cookConvexHull(const GameObject& object, CookedShape& output) {
    using Clock = std::chrono::steady_clock;
    const Clock::time_point geometryStart = Clock::now();
    LocalConvexHull hull;
    Result result = buildScaledLocalConvexHull(
        object.modelRenderable().meshInstances(), object.scale, hull);
    if (!result) return result;

    JPH::Array<JPH::Vec3> points;
    points.reserve(hull.points.size());
    for (const glm::vec3& point : hull.points) {
        points.emplace_back(point.x, point.y, point.z);
    }
    JPH::ConvexHullShapeSettings settings(points);
    const Clock::duration geometryConversion = Clock::now() - geometryStart;
    const Clock::time_point cookingStart = Clock::now();
    JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    const Clock::duration joltCooking = Clock::now() - cookingStart;
    if (shapeResult.HasError()) {
        return Result::failure(shapeResult.GetError().c_str());
    }

    output = {};
    output.shape = shapeResult.Get();
    output.timings.geometryConversion = geometryConversion;
    output.timings.joltCooking = joltCooking;
    output.diagnostics.representation =
        ShapeDiagnostics::Representation::ConvexHull;
    output.diagnostics.inputPoints = hull.points.size();
    if (output.shape->GetSubType() == JPH::EShapeSubType::ConvexHull) {
        output.diagnostics.cookedHullVertices = static_cast<
            const JPH::ConvexHullShape*>(output.shape.GetPtr())->GetNumPoints();
    }
    setStats(*output.shape, output.diagnostics);
    return Result::success();
}

}  // namespace

JPH::RVec3 toJoltPosition(const glm::vec3& position) noexcept {
    const glm::vec3 meters = dunamisToMeters(position);
    return JPH::RVec3(meters.x, meters.y, meters.z);
}

JPH::Quat toJoltRotation(const glm::vec3& rotation) noexcept {
    const glm::quat quaternion =
        glm::quat_cast(makeDunamisRotationMatrix(rotation));
    return JPH::Quat(quaternion.x, quaternion.y, quaternion.z, quaternion.w)
        .Normalized();
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
    result[3] = glm::vec4(
        metersToDunamis(glm::vec3(static_cast<float>(translation.GetX()),
                                  static_cast<float>(translation.GetY()),
                                  static_cast<float>(translation.GetZ()))),
        1.0f);
    return result;
}

bool extractDunamisRotation(const glm::mat4& matrix,
                            glm::vec3& rotation) noexcept {
    rotation = {};
    glm::vec3 basis[3];
    for (int axis = 0; axis < 3; ++axis) {
        const glm::vec3 column(matrix[axis]);
        const float lengthSquared = glm::dot(column, column);
        if (!std::isfinite(lengthSquared) || lengthSquared <= 1.0e-12f) {
            return false;
        }
        basis[axis] = column / std::sqrt(lengthSquared);
        if (!isFinite(basis[axis])) {
            return false;
        }
    }
    const float r00 = basis[0].x;
    const float r01 = basis[1].x;
    const float r02 = basis[2].x;
    const float r12 = basis[2].y;
    const float r22 = basis[2].z;
    const float yDenominator = std::sqrt(r00 * r00 + r01 * r01);
    if (!std::isfinite(yDenominator)) {
        return false;
    }
    rotation.x = glm::degrees(std::atan2(-r12, r22));
    rotation.y = glm::degrees(std::atan2(r02, yDenominator));
    rotation.z = glm::degrees(std::atan2(-r01, r00));
    return isFinite(rotation);
}

Result buildGameObjectShape(const GameObject& object, CookedShape& output) {
    if (!object.physics.enabled) return Result::failure("physics is disabled");
    switch (object.physics.colliderType) {
    case GameObject::PhysicsColliderType::Mesh:
        return cookMesh(object, output);
    case GameObject::PhysicsColliderType::ConvexHull:
        return cookConvexHull(object, output);
    case GameObject::PhysicsColliderType::Sphere:
        if (!(object.physics.sphereRadius > 0.0f) ||
            !std::isfinite(object.physics.sphereRadius)) {
            return Result::failure("sphere radius must be finite and positive");
        }
        output = {};
        {
            const auto cookingStart = std::chrono::steady_clock::now();
            output.shape = new JPH::SphereShape(
                dunamisToMeters(object.physics.sphereRadius));
            output.timings.joltCooking =
                std::chrono::steady_clock::now() - cookingStart;
        }
        output.diagnostics.representation =
            ShapeDiagnostics::Representation::AnalyticSphere;
        output.diagnostics.radius = object.physics.sphereRadius;
        setStats(*output.shape, output.diagnostics);
        return Result::success();
    }
    return Result::failure("unsupported collider type");
}

Result buildCharacterShape(const Character& character, CookedShape& output) {
    if (!(character.capsuleHeight > 2.0f * character.capsuleRadius) ||
        !(character.capsuleRadius > 0.0f) ||
        !std::isfinite(character.capsuleHeight) ||
        !std::isfinite(character.capsuleRadius)) {
        return Result::failure(
            "capsule dimensions must be finite, positive, and taller than its diameter");
    }

    const auto geometryStart = std::chrono::steady_clock::now();
    const float radius = dunamisToMeters(character.capsuleRadius);
    const float halfCylinder =
        dunamisToMeters(0.5f * character.capsuleHeight - character.capsuleRadius);
    const auto geometryConversion =
        std::chrono::steady_clock::now() - geometryStart;
    const auto cookingStart = std::chrono::steady_clock::now();
    JPH::RotatedTranslatedShapeSettings settings(
        JPH::Vec3(0.0f, 0.5f * dunamisToMeters(character.capsuleHeight), 0.0f),
        JPH::Quat::sIdentity(), new JPH::CapsuleShape(halfCylinder, radius));
    JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    const auto joltCooking = std::chrono::steady_clock::now() - cookingStart;
    if (shapeResult.HasError()) return Result::failure(shapeResult.GetError().c_str());

    output = {};
    output.shape = shapeResult.Get();
    output.timings.geometryConversion = geometryConversion;
    output.timings.joltCooking = joltCooking;
    output.diagnostics.representation =
        ShapeDiagnostics::Representation::AnalyticCapsule;
    output.diagnostics.radius = character.capsuleRadius;
    output.diagnostics.height = character.capsuleHeight;
    setStats(*output.shape, output.diagnostics);
    return Result::success();
}

}  // namespace physics
