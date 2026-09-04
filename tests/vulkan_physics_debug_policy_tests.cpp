#include "rendering/vulkan_context.h"
#include "editor/editor_session.h"
#include "scene/character.h"
#include "scene/scene.h"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <utility>

class VulkanContextTestAccess {
public:
    [[nodiscard]] static bool shouldAcquire(
        const GameObject& object, bool character, bool renderColliderEnabled) {
        return VulkanContext::shouldAcquirePhysicsDebugShape(
            object, character, renderColliderEnabled);
    }

    [[nodiscard]] static const physics::CookedShape* probeMesh(
        const VulkanContext& context, const GameObject& object) {
        return context.probePhysicsDebugShape(
            object, false, VulkanContext::makePhysicsDebugShapeSignature(object));
    }

    [[nodiscard]] static bool cacheMesh(
        VulkanContext& context, const GameObject& object, bool cooked) {
        VulkanContext::PhysicsDebugShapeCacheEntry entry;
        entry.signature = VulkanContext::makePhysicsDebugShapeSignature(object);
        if (cooked) {
            entry.cooked.emplace();
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
    expect(!VulkanContextTestAccess::shouldAcquire(object, false, false),
           "a warm hidden cache should not request shape acquisition");
    expect(VulkanContextTestAccess::cacheSize(context) == 1,
           "hidden debug visualization unexpectedly evicted its cache entry");

    object.scale.x = 2.0f;
    expect(VulkanContextTestAccess::probeMesh(context, object) == nullptr,
           "stale debug diagnostics were accepted for a changed signature");
    expect(!VulkanContextTestAccess::shouldAcquire(object, false, false),
           "stale hidden diagnostics requested replacement acquisition");
    expect(VulkanContextTestAccess::cacheSize(context) == 1,
           "probing a stale hidden cache entry mutated cache state");
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
    testMatchingProbeAndRetention();
    testMatchingErrorIsNotProbed();
    testHiddenSelectionDoesNotAcquire();
    return EXIT_SUCCESS;
}
