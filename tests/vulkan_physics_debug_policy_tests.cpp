#include "rendering/vulkan_context.h"
#include "editor/editor_transform.h"
#include "editor/editor_session.h"
#include "physics/collision_shapes.h"
#include "scene/character.h"
#include "scene/scene.h"

#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <limits>
#include <utility>

class VulkanContextTestAccess {
public:
    [[nodiscard]] static bool shouldAcquire(
        const GameObject& object, bool character, bool renderColliderEnabled) {
        return VulkanContext::shouldAcquirePhysicsDebugShape(
            object, character, renderColliderEnabled);
    }

    [[nodiscard]] static bool shouldDeferScale(
        const GameObject& object, bool character, bool renderColliderEnabled,
        bool activeScaleEdit) {
        return VulkanContext::shouldDeferPhysicsDebugShapeAcquisition(
            object, character, renderColliderEnabled, activeScaleEdit);
    }

    [[nodiscard]] static bool makeRelativeScale(
        const glm::vec3& cachedScale, const glm::vec3& currentScale,
        glm::vec3& relativeScale) {
        return VulkanContext::makePhysicsDebugPreviewRelativeScale(
            cachedScale, currentScale, relativeScale);
    }

    [[nodiscard]] static const physics::CookedShape* probeMesh(
        const VulkanContext& context, const GameObject& object) {
        return context.probePhysicsDebugShape(
            object, false, VulkanContext::makePhysicsDebugShapeSignature(object));
    }

    [[nodiscard]] static const physics::CookedShape* previewMesh(
        const VulkanContext& context, const GameObject& object,
        glm::vec3& relativeScale) {
        const auto* entry = context.findPhysicsDebugScalePreview(
            object, VulkanContext::makePhysicsDebugShapeSignature(object),
            relativeScale);
        return entry != nullptr && entry->cooked
                   ? &*entry->cooked
                   : nullptr;
    }

    [[nodiscard]] static bool cacheMesh(
        VulkanContext& context, const GameObject& object, bool cooked) {
        VulkanContext::PhysicsDebugShapeCacheEntry entry;
        entry.signature = VulkanContext::makePhysicsDebugShapeSignature(object);
        if (cooked) {
            entry.cooked.emplace();
            JPH::ShapeSettings::ShapeResult shapeResult;
            if (object.physics.colliderType ==
                GameObject::PhysicsColliderType::ConvexHull) {
                JPH::Array<JPH::Vec3> points = {
                    JPH::Vec3(0.0f, 0.0f, 0.0f),
                    JPH::Vec3(1.0f, 0.0f, 0.0f),
                    JPH::Vec3(0.0f, 1.0f, 0.0f),
                    JPH::Vec3(0.0f, 0.0f, 1.0f)};
                JPH::ConvexHullShapeSettings settings(points, 0.0f);
                shapeResult = settings.Create();
            } else {
                JPH::TriangleList triangles;
                triangles.emplace_back(JPH::Vec3(-1.0f, -1.0f, 0.0f),
                                       JPH::Vec3(1.0f, -1.0f, 0.0f),
                                       JPH::Vec3(0.0f, 1.0f, 0.0f));
                JPH::MeshShapeSettings settings(std::move(triangles));
                settings.SetEmbedded();
                shapeResult = settings.Create();
            }
            if (shapeResult.HasError() || !shapeResult.IsValid()) return false;
            entry.cooked->shape = shapeResult.Get();
            entry.cooked->diagnostics.inputVertices = 123;
        } else {
            entry.error = "shape preparation failed";
        }
        return context.physicsDebugShapes_.emplace(
                   VulkanContext::makePhysicsDebugShapeKey(object, false),
                   std::move(entry))
            .second;
    }

    [[nodiscard]] static std::size_t cacheSize(
        const VulkanContext& context) {
        return context.physicsDebugShapes_.size();
    }

    static void prepareSelectedDiagnostics(
        VulkanContext& context, Scene* scene, EditorSession& editorSession) {
        context.editorSession_ = &editorSession;
        context.physicsDebugRenderer_ = std::make_unique<PhysicsDebugRenderer>();
        context.prepareSelectedPhysicsDiagnostics(scene, SceneRunState::Editing);
    }
};

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

void expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        std::exit(EXIT_FAILURE);
    }
}

void configurePhysicsObject(GameObject& object) {
    object.persistentId = "city";
    object.physics.enabled = true;
    object.physics.colliderType = GameObject::PhysicsColliderType::Mesh;
}

void testVisibilityAcquisitionPolicy() {
    GameObject object;
    configurePhysicsObject(object);
    expect(!VulkanContextTestAccess::shouldAcquire(object, false, false),
           "hidden ordinary physics objects should not acquire debug shapes");
    expect(VulkanContextTestAccess::shouldAcquire(object, false, true),
           "visible ordinary physics objects should acquire debug shapes");

    Character character;
    expect(VulkanContextTestAccess::shouldAcquire(character, true, false),
           "Character debug acquisition should remain enabled when hidden");
}

void testScaleAcquisitionDeferralPolicy() {
    GameObject object;
    configurePhysicsObject(object);
    object.scale = glm::vec3(2.0f);

    expect(VulkanContextTestAccess::shouldDeferScale(
               object, false, true, true),
           "active visible Mesh scale edits did not defer acquisition");
    expect(!VulkanContextTestAccess::shouldDeferScale(
               object, false, true, false),
           "released Mesh scale edits still deferred acquisition");
    expect(!VulkanContextTestAccess::shouldDeferScale(
               object, false, false, true),
           "hidden Mesh scale edits requested a debug preview");

    object.physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    expect(!VulkanContextTestAccess::shouldDeferScale(
               object, false, true, true),
           "Sphere scale edits entered the Mesh/Convex preview policy");

    Character character;
    expect(!VulkanContextTestAccess::shouldDeferScale(
               character, true, true, true),
           "Character scale edits entered the Mesh/Convex preview policy");
}

void testMatchingProbeAndRetention() {
    VulkanContext context;
    GameObject object;
    configurePhysicsObject(object);

    expect(VulkanContextTestAccess::cacheMesh(context, object, true),
           "failed to seed successful physics debug cache entry");
    const physics::CookedShape* cached =
        VulkanContextTestAccess::probeMesh(context, object);
    expect(cached != nullptr && cached->diagnostics.inputVertices == 123,
           "matching successful debug cache entry was not probed");
    expect(VulkanContextTestAccess::shouldDeferScale(
               object, false, true, true),
           "active Mesh scale edit was not recognized by the deferral policy");
    expect(VulkanContextTestAccess::probeMesh(context, object) == cached,
           "exact cached Mesh shape did not remain usable during Scale edit");
    expect(!VulkanContextTestAccess::shouldAcquire(object, false, false),
           "a warm hidden cache should not request shape acquisition");
    expect(VulkanContextTestAccess::cacheSize(context) == 1,
           "hidden debug visualization unexpectedly evicted its cache entry");

    object.scale.x = 2.0f;
    expect(VulkanContextTestAccess::probeMesh(context, object) == nullptr,
           "stale debug diagnostics were accepted for a changed signature");
    glm::vec3 relativeScale;
    expect(VulkanContextTestAccess::previewMesh(
               context, object, relativeScale) != nullptr &&
               glm::length(relativeScale - glm::vec3(2.0f, 1.0f, 1.0f)) <=
                   1.0e-6f,
           "stale successful Mesh cache was not available as a scale preview");
    expect(!VulkanContextTestAccess::shouldAcquire(object, false, false),
           "stale hidden diagnostics requested replacement acquisition");
    expect(VulkanContextTestAccess::cacheSize(context) == 1,
           "probing a stale hidden cache entry mutated cache state");
    expect(VulkanContextTestAccess::previewMesh(
               context, object, relativeScale) != nullptr,
           "scale preview lookup erased or replaced the stale cache entry");
}

void testScalePreviewCompatibility() {
    VulkanContext context;
    GameObject object;
    configurePhysicsObject(object);
    object.modelPath = "model-a";
    expect(VulkanContextTestAccess::cacheMesh(context, object, true),
           "failed to seed compatibility preview cache entry");

    object.scale.x = 2.0f;
    glm::vec3 relativeScale;
    expect(VulkanContextTestAccess::previewMesh(
               context, object, relativeScale) != nullptr,
           "same-model Mesh scale mismatch was not previewable");

    object.modelPath = "model-b";
    expect(VulkanContextTestAccess::previewMesh(
               context, object, relativeScale) == nullptr,
           "changed model identity was accepted as a stale scale preview");

    object.modelPath = "model-a";
    object.physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    expect(VulkanContextTestAccess::previewMesh(
               context, object, relativeScale) == nullptr,
           "changed collider type was accepted as a stale scale preview");

    object.physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    object.physics.sphereRadius = 2.0f;
    expect(VulkanContextTestAccess::previewMesh(
               context, object, relativeScale) == nullptr,
           "unrelated Sphere signature state was accepted as a scale preview");
    expect(VulkanContextTestAccess::cacheSize(context) == 1,
           "compatibility probing mutated the stale cache entry");
}

void testConvexScalePreview() {
    VulkanContext context;
    GameObject object;
    configurePhysicsObject(object);
    object.physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    expect(VulkanContextTestAccess::cacheMesh(context, object, true),
           "failed to seed ConvexHull preview cache entry");
    object.scale = glm::vec3(2.0f);
    glm::vec3 relativeScale;
    expect(VulkanContextTestAccess::previewMesh(
               context, object, relativeScale) != nullptr &&
               glm::length(relativeScale - glm::vec3(2.0f)) <= 1.0e-6f,
           "stale successful ConvexHull cache was not available as a scale preview");
}

void testRelativeScaleMath() {
    glm::vec3 relativeScale;
    expect(VulkanContextTestAccess::makeRelativeScale(
               {2.0f, 3.0f, 4.0f}, {3.0f, 6.0f, 2.0f}, relativeScale) &&
               glm::length(relativeScale - glm::vec3(1.5f, 2.0f, 0.5f)) <=
                   1.0e-6f,
           "component-wise scale preview ratio was incorrect");
    expect(VulkanContextTestAccess::makeRelativeScale(
               {-2.0f, 3.0f, 4.0f}, {3.0f, 6.0f, -2.0f}, relativeScale) &&
               glm::length(relativeScale - glm::vec3(-1.5f, 2.0f, -0.5f)) <=
                   1.0e-6f,
           "supported reflected scale preview ratio was incorrect");

    const float infinity = std::numeric_limits<float>::infinity();
    const float nan = std::numeric_limits<float>::quiet_NaN();
    expect(!VulkanContextTestAccess::makeRelativeScale(
               {0.0f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, relativeScale),
           "zero cached scale was accepted for preview division");
    expect(!VulkanContextTestAccess::makeRelativeScale(
               {1.0f, 1.0f, 1.0f}, {infinity, 1.0f, 1.0f}, relativeScale),
           "non-finite current scale was accepted for preview");
    expect(!VulkanContextTestAccess::makeRelativeScale(
               {1.0f, 1.0f, 1.0f}, {nan, 1.0f, 1.0f}, relativeScale),
           "NaN current scale was accepted for preview");
    expect(!VulkanContextTestAccess::makeRelativeScale(
               {1.0e-7f, 1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}, relativeScale),
           "near-zero cached scale was accepted for preview");
}

void testScalePreviewMembership() {
    TestScene scene;
    GameObject first;
    GameObject second;
    GameObject unrelated;
    editor_transform::TransformDragSnapshot snapshot;
    snapshot.scene = &scene;
    snapshot.tool = TransformTool::Scale;
    snapshot.valid = true;
    editor_transform::TransformObjectSnapshot firstSnapshot;
    firstSnapshot.object = &first;
    snapshot.objects.push_back(firstSnapshot);
    editor_transform::TransformObjectSnapshot secondSnapshot;
    secondSnapshot.object = &second;
    snapshot.objects.push_back(secondSnapshot);

    expect(editor_transform::isScalePreviewParticipant(
               snapshot, &scene, &first) &&
               editor_transform::isScalePreviewParticipant(
                   snapshot, &scene, &second),
           "multi-selection Scale transaction omitted an affected object");
    expect(!editor_transform::isScalePreviewParticipant(
               snapshot, &scene, &unrelated),
           "unrelated object became a Scale preview participant");
    snapshot.tool = TransformTool::Rotate;
    expect(!editor_transform::isScalePreviewParticipant(
               snapshot, &scene, &first),
           "non-Scale transaction became a Scale preview participant");
}

void testScalePreviewComCompensation() {
    JPH::Array<JPH::Vec3> points = {
        JPH::Vec3(2.0f, 1.0f, -1.0f), JPH::Vec3(4.0f, 1.0f, -1.0f),
        JPH::Vec3(2.0f, 3.0f, -1.0f), JPH::Vec3(4.0f, 3.0f, -1.0f),
        JPH::Vec3(2.0f, 1.0f, 1.0f),  JPH::Vec3(4.0f, 1.0f, 1.0f),
        JPH::Vec3(2.0f, 3.0f, 1.0f),  JPH::Vec3(4.0f, 3.0f, 1.0f)};
    JPH::ConvexHullShapeSettings settings(points, 0.0f);
    const JPH::ShapeSettings::ShapeResult shapeResult = settings.Create();
    expect(!shapeResult.HasError() && shapeResult.IsValid(),
           "failed to create offset COM ConvexHull test shape");
    if (shapeResult.HasError() || !shapeResult.IsValid()) return;

    const auto* hull = static_cast<const JPH::ConvexHullShape*>(
        shapeResult.Get().GetPtr());
    const JPH::Vec3 cachedCenterOfMass = hull->GetCenterOfMass();
    const JPH::Vec3 cachedPoint = hull->GetPoint(0) + cachedCenterOfMass;
    const glm::vec3 relativeScale(1.5f, 2.0f, 0.5f);
    const glm::vec3 bodyPosition(7.0f, -2.0f, 3.0f);
    const glm::vec3 bodyRotations[] = {
        glm::vec3(0.0f), glm::vec3(17.0f, -31.0f, 23.0f)};

    for (const glm::vec3& bodyRotation : bodyRotations) {
        const JPH::RMat44 bodyTransform = JPH::RMat44::sRotationTranslation(
            physics::toJoltRotation(bodyRotation),
            physics::toJoltPosition(bodyPosition));
        const JPH::RMat44 previewTransform =
            physics::makeShapeCenterOfMassPreviewTransform(
                *shapeResult.Get(), bodyPosition, bodyRotation,
                relativeScale);
        const JPH::Vec3 scaledCachedPoint(
            relativeScale.x * cachedPoint.GetX(),
            relativeScale.y * cachedPoint.GetY(),
            relativeScale.z * cachedPoint.GetZ());
        const JPH::Vec3 scaledPointFromCom(
            relativeScale.x * (cachedPoint.GetX() - cachedCenterOfMass.GetX()),
            relativeScale.y * (cachedPoint.GetY() - cachedCenterOfMass.GetY()),
            relativeScale.z * (cachedPoint.GetZ() - cachedCenterOfMass.GetZ()));
        const auto expectedLinear =
            bodyTransform.Multiply3x3(scaledCachedPoint);
        const auto actualLinear =
            previewTransform.Multiply3x3(scaledPointFromCom);
        const double expectedX = bodyTransform.GetTranslation().GetX() +
                                 expectedLinear.GetX();
        const double expectedY = bodyTransform.GetTranslation().GetY() +
                                 expectedLinear.GetY();
        const double expectedZ = bodyTransform.GetTranslation().GetZ() +
                                 expectedLinear.GetZ();
        const double actualX = previewTransform.GetTranslation().GetX() +
                               actualLinear.GetX();
        const double actualY = previewTransform.GetTranslation().GetY() +
                               actualLinear.GetY();
        const double actualZ = previewTransform.GetTranslation().GetZ() +
                               actualLinear.GetZ();
        expect(std::fabs(actualX - expectedX) <= 1.0e-5 &&
                   std::fabs(actualY - expectedY) <= 1.0e-5 &&
                   std::fabs(actualZ - expectedZ) <= 1.0e-5,
               "scale preview COM compensation drifted an offset collider");
    }
}

void testMatchingErrorIsNotProbed() {
    VulkanContext context;
    GameObject object;
    configurePhysicsObject(object);
    expect(VulkanContextTestAccess::cacheMesh(context, object, false),
           "failed to seed error-only physics debug cache entry");
    expect(VulkanContextTestAccess::probeMesh(context, object) == nullptr,
           "error-only cache entry was exposed as successful diagnostics");
    expect(!VulkanContextTestAccess::shouldAcquire(object, false, false),
           "hidden error-only cache entry requested acquisition");
}

void testHiddenSelectionDoesNotAcquire() {
    TestScene scene;
    auto object = std::make_unique<GameObject>();
    configurePhysicsObject(*object);
    GameObject* selected = object.get();
    expect(static_cast<bool>(scene.addGameObject(std::move(object))),
           "failed to add physics object for selection policy test");

    EditorSession editorSession;
    editorSession.select(&scene, selected);
    VulkanContext context;
    VulkanContextTestAccess::prepareSelectedDiagnostics(
        context, &scene, editorSession);
    expect(VulkanContextTestAccess::cacheSize(context) == 0,
           "hidden selection acquired a physics debug shape");
}

}  // namespace

int main() {
    JPH::RegisterDefaultAllocator();
    testVisibilityAcquisitionPolicy();
    testScaleAcquisitionDeferralPolicy();
    testMatchingProbeAndRetention();
    testScalePreviewCompatibility();
    testConvexScalePreview();
    testRelativeScaleMath();
    testScalePreviewMembership();
    testScalePreviewComCompensation();
    testMatchingErrorIsNotProbed();
    testHiddenSelectionDoesNotAcquire();
    return EXIT_SUCCESS;
}
