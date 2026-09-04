#include "editor/editor_transform.h"
#include "editor/editor_session.h"
#include "editor/editor_state.h"
#include "rendering/editor_picking.h"
#include "scene/camera.h"
#include "scene/directional_light.h"
#include "scene/game_object.h"
#include "scene/point_light.h"
#include "scene/scene.h"

#include <cmath>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

class AttachedCameraObject final : public GameObject {
public:
    Camera camera;

    Camera* attachedCamera() noexcept override { return &camera; }
    const Camera* attachedCamera() const noexcept override {
        return &camera;
    }
};

template <typename ObjectType>
ObjectType* addObject(TestScene& scene, const char* persistentId) {
    auto object = std::make_unique<ObjectType>();
    object->persistentId = persistentId;
    ObjectType* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) {
        return nullptr;
    }
    return pointer;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second,
                float tolerance = 1.0e-4f) {
    return glm::length(first - second) <= tolerance;
}

bool sameVectorBits(const glm::vec3& first, const glm::vec3& second) {
    return std::memcmp(&first.x, &second.x, sizeof(float)) == 0 &&
           std::memcmp(&first.y, &second.y, sizeof(float)) == 0 &&
           std::memcmp(&first.z, &second.z, sizeof(float)) == 0;
}

bool sameMatrix(const glm::mat4& first, const glm::mat4& second,
                float tolerance = 2.0e-4f) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::fabs(first[column][row] - second[column][row]) >
                tolerance) {
                return false;
            }
        }
    }
    return true;
}

glm::vec3 worldPosition(const GameObject& object) {
    return glm::vec3(object.worldTransformMatrix()[3]);
}

glm::mat4 pivotDelta(const glm::vec3& pivot, const glm::mat4& linearDelta) {
    return transform_math::makeTranslationMatrix(pivot) * linearDelta *
           transform_math::makeTranslationMatrix(-pivot);
}

bool applySharedDelta(
    TestScene& scene, EditorSession& session, const glm::mat4& sharedDelta,
    TransformSpace space = TransformSpace::World) {
    editor_transform::TransformDragSnapshot snapshot;
    Result result = editor_transform::captureTransformDragSnapshot(
        scene, session, space, snapshot);
    if (!result) {
        std::cerr << result.error() << '\n';
        return false;
    }
    const glm::mat4 currentGizmo = sharedDelta * snapshot.originalGizmoWorld;
    std::vector<editor_transform::TransformCandidate> candidates;
    result = editor_transform::solveTransformDrag(
        snapshot, currentGizmo, candidates);
    if (result) {
        result = editor_transform::commitTransformCandidates(snapshot,
                                                              candidates);
    }
    if (!result) {
        std::cerr << result.error() << '\n';
    }
    return static_cast<bool>(result);
}

bool applyRotateDelta(TestScene& scene, EditorSession& session,
                      const glm::mat4& sharedDelta, TransformSpace space) {
    session.setTransformTool(TransformTool::Rotate);
    session.setTransformSpace(space);
    return applySharedDelta(scene, session, sharedDelta, space);
}

bool runRootConversionTests() {
    GameObject object;
    object.position = {2.0f, -3.0f, 4.0f};
    object.rotation = {11.0f, -23.0f, 37.0f};
    object.scale = {-2.0f, 3.0f, -4.0f};

    const glm::vec3 expectedPosition{-7.0f, 8.0f, 9.0f};
    const glm::vec3 expectedRotation{31.0f, -17.0f, 13.0f};
    const glm::vec3 expectedScale{-1.5f, 2.5f, -3.5f};
    const glm::mat4 candidateWorld = transform_math::makeModelMatrix(
        expectedPosition, expectedRotation, expectedScale);

    transform_math::DecomposedTransform local;
    const Result result = editor_transform::deriveLocalTransformFromWorld(
        object, candidateWorld, local);
    bool passed = expect(static_cast<bool>(result),
                         "Root world-to-local gizmo conversion failed");
    passed &= expect(sameVector(local.position, expectedPosition) &&
                         sameVector(local.rotation, expectedRotation) &&
                         sameVector(local.scale, expectedScale),
                     "Root world-to-local conversion changed authored TRS");
    passed &= expect(sameMatrix(
                         transform_math::makeModelMatrix(
                             local.position, local.rotation, local.scale),
                         candidateWorld),
                     "Root world-to-local conversion did not preserve its matrix");

    GameObject zeroScaleObject;
    zeroScaleObject.rotation = {17.0f, -29.0f, 41.0f};
    zeroScaleObject.scale = {-2.0f, 3.0f, 4.0f};
    const glm::mat4 zeroScaleWorld = transform_math::makeModelMatrix(
        {4.0f, 5.0f, 6.0f}, zeroScaleObject.rotation, {0.0f, 3.0f, 4.0f});
    transform_math::DecomposedTransform zeroScaleLocal;
    const Result zeroScaleResult =
        editor_transform::deriveLocalTransformFromWorld(
            zeroScaleObject, zeroScaleWorld, zeroScaleLocal);
    passed &= expect(static_cast<bool>(zeroScaleResult) &&
                         sameVector(zeroScaleLocal.position, {4.0f, 5.0f, 6.0f}) &&
                         sameVector(zeroScaleLocal.scale, {0.0f, 3.0f, 4.0f}) &&
                         sameMatrix(transform_math::makeModelMatrix(
                                         zeroScaleLocal.position,
                                         zeroScaleLocal.rotation,
                                         zeroScaleLocal.scale),
                                     zeroScaleWorld),
                     "Root zero-scale gizmo conversion changed compatibility");
    return passed;
}

bool runChildTranslationTests() {
    TestScene scene;
    GameObject* parent = addObject<GameObject>(scene, "translation-parent");
    GameObject* child = addObject<GameObject>(scene, "translation-child");
    if (!parent || !child ||
        !scene.reparentGameObject(*child, parent,
                                  Scene::ReparentMode::PreserveLocal)) {
        return expect(false, "Could not create child-translation fixture");
    }

    parent->position = {100.0f, 0.0f, 0.0f};
    child->position = {20.0f, 0.0f, 0.0f};
    const glm::vec3 oldParentPosition = parent->position;
    const glm::mat4 candidateWorld = transform_math::makeModelMatrix(
        {150.0f, 0.0f, 0.0f}, child->rotation, child->scale);

    transform_math::DecomposedTransform local;
    const Result result = editor_transform::deriveLocalTransformFromWorld(
        *child, candidateWorld, local);
    bool passed = expect(static_cast<bool>(result),
                         "Child translation world-to-local conversion failed");
    passed &= expect(sameVector(local.position, {50.0f, 0.0f, 0.0f}) &&
                         parent->position == oldParentPosition,
                     "Child translation was not converted into parent space");
    return passed;
}

bool runRotatedAndScaledParentTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "rotated-parent");
        GameObject* child = addObject<GameObject>(scene, "rotated-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create rotated-parent fixture");
        }

        parent->position = {100.0f, 20.0f, -3.0f};
        parent->rotation = {0.0f, 0.0f, 90.0f};
        child->position = {10.0f, 2.0f, 1.0f};
        child->rotation = {15.0f, -20.0f, 25.0f};
        child->scale = {1.5f, 0.75f, 2.0f};

        const glm::vec3 expectedLocalPosition{20.0f, -4.0f, 5.0f};
        const glm::mat4 candidateLocal = transform_math::makeModelMatrix(
            expectedLocalPosition, child->rotation, child->scale);
        const glm::mat4 candidateWorld =
            parent->worldTransformMatrix() * candidateLocal;
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, candidateWorld, local);
        passed &= expect(static_cast<bool>(result) &&
                             sameVector(local.position, expectedLocalPosition),
                         "Rotated parent translation used a naive world delta");
        passed &= expect(sameMatrix(
                             parent->worldTransformMatrix() *
                                 transform_math::makeModelMatrix(
                                     local.position, local.rotation, local.scale),
                             candidateWorld),
                         "Rotated parent conversion did not preserve world matrix");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "scaled-parent");
        GameObject* child = addObject<GameObject>(scene, "scaled-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create scaled-parent fixture");
        }

        parent->position = {5.0f, 6.0f, 7.0f};
        parent->scale = {2.0f, 3.0f, 4.0f};
        child->rotation = {10.0f, 20.0f, 30.0f};
        child->scale = {0.5f, 1.5f, 2.0f};
        const glm::vec3 expectedLocalPosition{-3.0f, 8.0f, 2.5f};
        const glm::vec3 expectedLocalScale{1.25f, 0.75f, 1.5f};
        const glm::mat4 candidateWorld = parent->worldTransformMatrix() *
            transform_math::makeModelMatrix(expectedLocalPosition,
                                             child->rotation,
                                             expectedLocalScale);
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, candidateWorld, local);
        passed &= expect(static_cast<bool>(result) &&
                             sameVector(local.position, expectedLocalPosition) &&
                             sameVector(local.scale, expectedLocalScale),
                         "Scaled parent conversion did not recover local TRS");
    }

    return passed;
}

bool runRotationAndMultiLevelTests() {
    bool passed = true;
    TestScene scene;
    GameObject* root = addObject<GameObject>(scene, "multi-root");
    GameObject* parent = addObject<GameObject>(scene, "multi-parent");
    GameObject* child = addObject<GameObject>(scene, "multi-child");
    if (!root || !parent || !child ||
        !scene.reparentGameObject(*parent, root,
                                  Scene::ReparentMode::PreserveLocal) ||
        !scene.reparentGameObject(*child, parent,
                                  Scene::ReparentMode::PreserveLocal)) {
        return expect(false, "Could not create multi-level conversion fixture");
    }

    root->position = {30.0f, -10.0f, 4.0f};
    root->rotation = {10.0f, 25.0f, 35.0f};
    root->scale = {1.5f, 2.0f, 0.75f};
    parent->position = {-4.0f, 6.0f, 2.0f};
    parent->rotation = {-15.0f, 20.0f, 40.0f};
    parent->scale = {0.75f, 1.25f, 1.5f};
    child->position = {2.0f, 3.0f, 4.0f};
    child->rotation = {15.0f, -20.0f, 25.0f};
    child->scale = {-1.0f, 1.5f, 2.0f};

    const glm::vec3 expectedLocalPosition{-5.0f, 7.0f, 1.0f};
    const glm::vec3 expectedLocalRotation{35.0f, 10.0f, -15.0f};
    const glm::vec3 expectedLocalScale{-1.25f, 0.8f, 1.75f};
    const glm::mat4 candidateLocal = transform_math::makeModelMatrix(
        expectedLocalPosition, expectedLocalRotation, expectedLocalScale);
    const glm::mat4 candidateWorld =
        root->worldTransformMatrix() * parent->localTransformMatrix() *
        candidateLocal;

    transform_math::DecomposedTransform local;
    const Result result = editor_transform::deriveLocalTransformFromWorld(
        *child, candidateWorld, local);
    passed &= expect(static_cast<bool>(result),
                     "Multi-level rotation conversion failed");
    passed &= expect(sameMatrix(
                         parent->worldTransformMatrix() *
                             transform_math::makeModelMatrix(
                                 local.position, local.rotation, local.scale),
                         candidateWorld),
                     "Multi-level conversion did not use the complete parent chain");
    passed &= expect(sameVector(local.position, expectedLocalPosition) &&
                         sameVector(local.rotation, expectedLocalRotation) &&
                         sameVector(local.scale, expectedLocalScale),
                     "Matrix rotation conversion changed the expected local TRS");
    return passed;
}

bool runFailureAndTransactionalTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "shear-parent");
        GameObject* child = addObject<GameObject>(scene, "shear-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create shear fixture");
        }
        parent->rotation = {0.0f, 0.0f, 45.0f};
        parent->scale = {2.0f, 1.0f, 1.0f};
        child->position = {2.0f, 3.0f, 4.0f};
        child->rotation = {10.0f, 20.0f, 30.0f};
        child->scale = {1.0f, 1.5f, 2.0f};

        const glm::vec3 positionBefore = child->position;
        const glm::vec3 rotationBefore = child->rotation;
        const glm::vec3 scaleBefore = child->scale;
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, glm::mat4(1.0f), local);
        passed &= expect(!result,
                         "World-to-local conversion accepted meaningful shear");
        passed &= expect(child->position == positionBefore &&
                             child->rotation == rotationBefore &&
                             child->scale == scaleBefore,
                         "Shear rejection changed authored local state");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "singular-parent");
        auto attachedObject = std::make_unique<AttachedCameraObject>();
        AttachedCameraObject* child = attachedObject.get();
        child->persistentId = "singular-attached-child";
        if (!parent || !scene.addGameObject(std::move(attachedObject)) ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create singular attached fixture");
        }
        parent->scale = {0.0f, 1.0f, 1.0f};
        child->position = {2.0f, 3.0f, 4.0f};
        child->rotation = {5.0f, 6.0f, 7.0f};
        child->scale = {1.0f, 2.0f, 3.0f};
        child->camera.position = {7.0f, 8.0f, 9.0f};
        child->camera.front = {0.0f, 0.0f, -1.0f};
        child->camera.up = {0.0f, 1.0f, 0.0f};

        const glm::vec3 positionBefore = child->position;
        const glm::vec3 rotationBefore = child->rotation;
        const glm::vec3 scaleBefore = child->scale;
        const glm::vec3 cameraPositionBefore = child->camera.position;
        const glm::vec3 cameraFrontBefore = child->camera.front;
        const glm::vec3 cameraUpBefore = child->camera.up;
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, child->worldTransformMatrix(), local);
        passed &= expect(!result,
                         "World-to-local conversion accepted a singular parent");
        passed &= expect(child->position == positionBefore &&
                             child->rotation == rotationBefore &&
                             child->scale == scaleBefore &&
                             child->camera.position == cameraPositionBefore &&
                             child->camera.front == cameraFrontBefore &&
                             child->camera.up == cameraUpBefore,
                         "Failed gizmo conversion changed attached Camera state");
    }

    {
        GameObject object;
        object.position = {1.0f, 2.0f, 3.0f};
        object.rotation = {4.0f, 5.0f, 6.0f};
        object.scale = {2.0f, 3.0f, 4.0f};
        const glm::vec3 positionBefore = object.position;
        const glm::vec3 rotationBefore = object.rotation;
        const glm::vec3 scaleBefore = object.scale;
        glm::mat4 candidateWorld = object.worldTransformMatrix();
        candidateWorld[0][0] = std::numeric_limits<float>::quiet_NaN();
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            object, candidateWorld, local);
        passed &= expect(!result && object.position == positionBefore &&
                             object.rotation == rotationBefore &&
                             object.scale == scaleBefore,
                         "Nonfinite gizmo conversion was not transactional");
    }

    return passed;
}

bool runSelectionPivotAndSpaceTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* a = addObject<GameObject>(scene, "pivot-a");
        GameObject* b = addObject<GameObject>(scene, "pivot-b");
        GameObject* c = addObject<GameObject>(scene, "pivot-c");
        GameObject* d = addObject<GameObject>(scene, "pivot-d");
        GameObject* e = addObject<GameObject>(scene, "pivot-e");
        passed &= expect(a && b && c && d && e &&
                             static_cast<bool>(scene.reparentGameObject(
                                 *b, a, Scene::ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(scene.reparentGameObject(
                                 *c, b, Scene::ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(scene.reparentGameObject(
                                 *e, d, Scene::ReparentMode::PreserveLocal)),
                         "Could not create pivot/root fixture");
        if (!a || !b || !c || !d || !e) return false;

        a->position = {10.0f, 0.0f, 0.0f};
        b->position = {2.0f, 0.0f, 0.0f};
        c->position = {3.0f, 0.0f, 0.0f};
        d->position = {30.0f, 0.0f, 0.0f};
        e->position = {4.0f, 0.0f, 0.0f};

        glm::vec3 singlePivot;
        Result result = editor_transform::calculateSharedPivot({e}, singlePivot);
        passed &= expect(static_cast<bool>(result) &&
                             sameVector(singlePivot, worldPosition(*e)),
                         "Single-object pivot did not use the object world position");

        EditorSession session;
        session.applySelection(&scene, a, SelectionOperation::ReplaceExact);
        session.applySelection(&scene, c, SelectionOperation::ToggleExact);
        editor_transform::TransformDragSnapshot holeSnapshot;
        result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::World, holeSnapshot);
        passed &= expect(static_cast<bool>(result) &&
                             holeSnapshot.topLevelSelectedRoots.size() == 1 &&
                             holeSnapshot.topLevelSelectedRoots[0] == a &&
                             holeSnapshot.activeTransformRoot == a &&
                             sameVector(holeSnapshot.pivot, worldPosition(*a)),
                         "Selection-hole pivot/root resolution was incorrect");

        session.applySelection(&scene, d, SelectionOperation::ToggleExact);
        session.applySelection(&scene, e, SelectionOperation::ToggleExact);
        editor_transform::TransformDragSnapshot multiRootSnapshot;
        result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::World, multiRootSnapshot);
        passed &= expect(static_cast<bool>(result) &&
                             multiRootSnapshot.topLevelSelectedRoots.size() == 2 &&
                             multiRootSnapshot.topLevelSelectedRoots[0] == a &&
                             multiRootSnapshot.topLevelSelectedRoots[1] == d &&
                             multiRootSnapshot.activeTransformRoot == d &&
                             sameVector(multiRootSnapshot.pivot, {20.0f, 0.0f, 0.0f}),
                         "Multi-root pivot included a descendant or wrong root");

        session.setTransformSpace(TransformSpace::Local);
        a->rotation = {0.0f, 0.0f, 35.0f};
        d->rotation = {0.0f, 0.0f, -25.0f};
        editor_transform::TransformDragSnapshot localSnapshot;
        result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::Local, localSnapshot);
        glm::mat4 expectedOrientation;
        const Result orientationResult = editor_transform::calculateWorldOrientation(
            d->worldTransformMatrix(), expectedOrientation);
        passed &= expect(static_cast<bool>(result) &&
                             localSnapshot.activeTransformRoot == d &&
                             static_cast<bool>(orientationResult) &&
                             sameMatrix(localSnapshot.gizmoOrientation,
                                        expectedOrientation),
                         "Local gizmo orientation did not use Active's root");

        editor_transform::TransformDragSnapshot worldSnapshot;
        result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::World, worldSnapshot);
        passed &= expect(static_cast<bool>(result) &&
                             sameMatrix(worldSnapshot.gizmoOrientation,
                                        glm::mat4(1.0f)),
                         "World gizmo orientation was not world-aligned");

        GameObject* resolvedRoot = nullptr;
        result = editor_transform::resolveActiveTransformRoot(
            c, session.selectedGameObjects(), resolvedRoot);
        passed &= expect(static_cast<bool>(result) && resolvedRoot == a,
                         "Active selected descendant did not resolve to its root");
    }

    {
        TestScene scene;
        GameObject* a = addObject<GameObject>(scene, "local-hole-a");
        GameObject* b = addObject<GameObject>(scene, "local-hole-b");
        GameObject* c = addObject<GameObject>(scene, "local-hole-c");
        if (!a || !b || !c ||
            !scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create deep local-root fixture");
        }
        a->position = {8.0f, 3.0f, 0.0f};
        a->rotation = {0.0f, 0.0f, 45.0f};
        EditorSession session;
        session.applySelection(&scene, a, SelectionOperation::ReplaceExact);
        session.applySelection(&scene, c, SelectionOperation::ToggleExact);
        session.setTransformSpace(TransformSpace::Local);
        editor_transform::TransformDragSnapshot snapshot;
        const Result result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::Local, snapshot);
        glm::mat4 expectedOrientation;
        const Result orientationResult = editor_transform::calculateWorldOrientation(
            a->worldTransformMatrix(), expectedOrientation);
        passed &= expect(static_cast<bool>(result) &&
                             snapshot.activeTransformRoot == a &&
                             sameVector(snapshot.pivot, worldPosition(*a)) &&
                             static_cast<bool>(orientationResult) &&
                             sameMatrix(snapshot.gizmoOrientation,
                                        expectedOrientation),
                         "Local selection-hole root did not walk across the gap");
    }

    return passed;
}

bool runMultiObjectTransformTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* object = addObject<GameObject>(scene, "single-transform-object");
        if (!object) {
            return expect(false, "Could not create single-object transform fixture");
        }
        object->position = {2.0f, 3.0f, 4.0f};
        object->rotation = {10.0f, 20.0f, 30.0f};
        object->scale = {1.5f, 0.75f, 2.0f};
        EditorSession session;
        session.select(&scene, object);

        const glm::mat4 translation =
            transform_math::makeTranslationMatrix({5.0f, 0.0f, 0.0f});
        const glm::mat4 beforeTranslation = object->worldTransformMatrix();
        passed &= expect(applySharedDelta(scene, session, translation) &&
                             sameMatrix(object->worldTransformMatrix(),
                                        translation * beforeTranslation),
                         "Single-object translation transaction failed");

        const glm::mat4 rotation = pivotDelta(
            worldPosition(*object),
            transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}));
        const glm::mat4 beforeRotation = object->worldTransformMatrix();
        passed &= expect(applySharedDelta(scene, session, rotation) &&
                             sameMatrix(object->worldTransformMatrix(),
                                        rotation * beforeRotation),
                         "Single-object rotation transaction failed");

        const glm::mat4 scale = pivotDelta(
            worldPosition(*object), transform_math::makeScaleMatrix(
                                        {2.0f, 2.0f, 2.0f}));
        const glm::mat4 beforeScale = object->worldTransformMatrix();
        passed &= expect(applySharedDelta(scene, session, scale) &&
                             sameMatrix(object->worldTransformMatrix(),
                                        scale * beforeScale),
                         "Single-object scale transaction failed");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "translate-parent");
        GameObject* child = addObject<GameObject>(scene, "translate-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create translation boundary fixture");
        }
        parent->position = {10.0f, 0.0f, 0.0f};
        child->position = {2.0f, 3.0f, 0.0f};
        const glm::mat4 parentBefore = parent->worldTransformMatrix();
        const glm::mat4 childBefore = child->worldTransformMatrix();
        const glm::vec3 childLocalBefore = child->position;
        EditorSession session;
        session.select(&scene, parent);
        passed &= expect(applySharedDelta(
                             scene, session,
                             transform_math::makeTranslationMatrix(
                                 {5.0f, 0.0f, 0.0f})),
                         "Selected-parent translation transaction failed");
        passed &= expect(sameMatrix(parent->worldTransformMatrix(),
                                    transform_math::makeTranslationMatrix(
                                        {5.0f, 0.0f, 0.0f}) * parentBefore) &&
                             sameMatrix(child->worldTransformMatrix(), childBefore) &&
                             !sameVector(child->position, childLocalBefore),
                         "Unselected child was not kept world-fixed");
    }

    {
        TestScene scene;
        Camera* camera = addObject<Camera>(scene, "standalone-camera-batch");
        if (!camera) {
            return expect(false, "Could not create standalone Camera batch fixture");
        }
        camera->position = {2.0f, 3.0f, 4.0f};
        camera->front = glm::normalize(glm::vec3(0.2f, 0.3f, -0.9f));
        camera->up = {0.0f, 1.0f, 0.0f};
        const glm::vec3 frontBefore = camera->front;
        const glm::vec3 upBefore = camera->up;
        EditorSession session;
        session.select(&scene, camera);
        passed &= expect(applySharedDelta(
                             scene, session,
                             transform_math::makeTranslationMatrix(
                                 {5.0f, 0.0f, 0.0f})),
                         "Standalone Camera batch translation failed");
        passed &= expect(camera->position == glm::vec3(7.0f, 3.0f, 4.0f) &&
                             camera->front == frontBefore &&
                             camera->up == upBefore,
                         "Standalone Camera batch translation corrupted auxiliary state");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(
            scene, "standalone-camera-compensation-parent");
        Camera* camera = addObject<Camera>(
            scene, "standalone-camera-compensation-child");
        if (!parent || !camera ||
            !scene.reparentGameObject(*camera, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create standalone Camera compensation fixture");
        }
        camera->position = {2.0f, 3.0f, 4.0f};
        camera->front = glm::normalize(glm::vec3(0.6f, 0.0f, -0.8f));
        camera->up = {0.0f, 1.0f, 0.0f};
        parent->scale = {2.0f, 1.0f, 1.0f};
        CameraWorldPose poseBefore;
        const bool poseBeforeResult = camera->calculateWorldPose(poseBefore);
        EditorSession session;
        session.select(&scene, parent);
        const bool transactionResult = applySharedDelta(
            scene, session, pivotDelta(worldPosition(*parent),
                                       transform_math::makeScaleMatrix(
                                           {2.0f, 1.0f, 1.0f})));
        CameraWorldPose poseAfter;
        const bool poseAfterResult = camera->calculateWorldPose(poseAfter);
        passed &= expect(transactionResult && poseBeforeResult && poseAfterResult &&
                             sameVector(poseAfter.position, poseBefore.position) &&
                             sameVector(poseAfter.front, poseBefore.front) &&
                             sameVector(poseAfter.up, poseBefore.up),
                         "Unselected standalone Camera was not kept world-coherent");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "branch-parent");
        GameObject* child = addObject<GameObject>(scene, "branch-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create complete-branch fixture");
        }
        parent->position = {3.0f, 0.0f, 0.0f};
        child->position = {2.0f, 1.0f, 0.0f};
        const glm::mat4 parentBefore = parent->worldTransformMatrix();
        const glm::mat4 childBefore = child->worldTransformMatrix();
        const glm::vec3 childLocalBefore = child->position;
        EditorSession session;
        session.applySelection(&scene, parent, SelectionOperation::ReplaceExact);
        session.applySelection(&scene, child, SelectionOperation::ToggleExact);
        const glm::mat4 delta = transform_math::makeTranslationMatrix(
            {5.0f, 0.0f, 0.0f});
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Complete selected branch translation failed");
        passed &= expect(sameMatrix(parent->worldTransformMatrix(),
                                    delta * parentBefore) &&
                             sameMatrix(child->worldTransformMatrix(),
                                        delta * childBefore) &&
                             child->position == childLocalBefore,
                         "Selected branch translated cumulatively or changed local");
    }

    {
        TestScene scene;
        GameObject* a = addObject<GameObject>(scene, "hole-translate-a");
        GameObject* b = addObject<GameObject>(scene, "hole-translate-b");
        GameObject* c = addObject<GameObject>(scene, "hole-translate-c");
        if (!a || !b || !c ||
            !scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create translation-hole fixture");
        }
        a->position = {4.0f, 0.0f, 0.0f};
        b->position = {2.0f, 0.0f, 0.0f};
        c->position = {3.0f, 0.0f, 0.0f};
        const glm::mat4 aBefore = a->worldTransformMatrix();
        const glm::mat4 bBefore = b->worldTransformMatrix();
        const glm::mat4 cBefore = c->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, a);
        session.applySelection(&scene, c, SelectionOperation::ToggleExact);
        const glm::mat4 delta = transform_math::makeTranslationMatrix(
            {5.0f, 0.0f, 0.0f});
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Selection-hole translation failed");
        passed &= expect(sameMatrix(a->worldTransformMatrix(), delta * aBefore) &&
                             sameMatrix(b->worldTransformMatrix(), bBefore) &&
                             sameMatrix(c->worldTransformMatrix(), delta * cBefore),
                         "Selection-hole translation double-transformed a descendant");
    }

    {
        TestScene scene;
        GameObject* root = addObject<GameObject>(scene, "scale-root");
        GameObject* child = addObject<GameObject>(scene, "scale-child");
        GameObject* grandchild = addObject<GameObject>(scene, "scale-grandchild");
        if (!root || !child || !grandchild ||
            !scene.reparentGameObject(*child, root,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*grandchild, child,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create deep scale fixture");
        }
        root->position = {10.0f, 2.0f, 0.0f};
        root->rotation = {0.0f, 0.0f, 20.0f};
        child->position = {3.0f, 1.0f, 0.0f};
        grandchild->position = {2.0f, -1.0f, 0.0f};
        const glm::mat4 rootBefore = root->worldTransformMatrix();
        const glm::mat4 childBefore = child->worldTransformMatrix();
        const glm::mat4 grandchildBefore = grandchild->worldTransformMatrix();
        const glm::vec3 childLocalBefore = child->position;
        const glm::vec3 grandchildLocalBefore = grandchild->position;
        EditorSession session;
        session.select(&scene, root);
        session.applySelection(&scene, child, SelectionOperation::ToggleExact);
        session.applySelection(&scene, grandchild,
                               SelectionOperation::ToggleExact);
        editor_transform::TransformDragSnapshot snapshot;
        Result result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::World, snapshot);
        const glm::mat4 scaleTwo = pivotDelta(
            snapshot.pivot, transform_math::makeScaleMatrix({2.0f, 2.0f, 2.0f}));
        std::vector<editor_transform::TransformCandidate> candidates;
        result = result ? editor_transform::solveTransformDrag(
                              snapshot, scaleTwo * snapshot.originalGizmoWorld,
                              candidates)
                        : result;
        result = result ? editor_transform::commitTransformCandidates(
                              snapshot, candidates)
                        : result;
        passed &= expect(static_cast<bool>(result),
                         "Deep selected-branch scale transaction failed");
        passed &= expect(sameMatrix(root->worldTransformMatrix(),
                                    scaleTwo * rootBefore) &&
                             sameMatrix(child->worldTransformMatrix(),
                                        scaleTwo * childBefore) &&
                             sameMatrix(grandchild->worldTransformMatrix(),
                                        scaleTwo * grandchildBefore) &&
                             child->position == childLocalBefore &&
                             grandchild->position == grandchildLocalBefore,
                         "Deep selected hierarchy received compounded scale");

        const glm::mat4 scaleThree = pivotDelta(
            snapshot.pivot, transform_math::makeScaleMatrix({3.0f, 3.0f, 3.0f}));
        result = editor_transform::solveTransformDrag(
            snapshot, scaleThree * snapshot.originalGizmoWorld, candidates);
        result = result ? editor_transform::commitTransformCandidates(
                              snapshot, candidates)
                        : result;
        passed &= expect(static_cast<bool>(result) &&
                             sameMatrix(root->worldTransformMatrix(),
                                        scaleThree * rootBefore) &&
                             sameMatrix(child->worldTransformMatrix(),
                                        scaleThree * childBefore) &&
                             sameMatrix(grandchild->worldTransformMatrix(),
                                        scaleThree * grandchildBefore),
                         "Drag candidates accumulated the previous scale frame");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "rotate-parent");
        GameObject* child = addObject<GameObject>(scene, "rotate-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create rotation boundary fixture");
        }
        parent->position = {5.0f, 0.0f, 0.0f};
        child->position = {2.0f, 1.0f, 0.0f};
        const glm::mat4 parentBefore = parent->worldTransformMatrix();
        const glm::mat4 childBefore = child->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, parent);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*parent),
            transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}));
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Selected-parent rotation transaction failed");
        passed &= expect(sameMatrix(parent->worldTransformMatrix(),
                                    delta * parentBefore) &&
                             sameMatrix(child->worldTransformMatrix(), childBefore),
                         "Unselected child moved during parent rotation");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "rotate-branch-parent");
        GameObject* child = addObject<GameObject>(scene, "rotate-branch-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create selected rotation branch");
        }
        parent->position = {2.0f, 4.0f, 0.0f};
        child->position = {3.0f, 0.0f, 0.0f};
        const glm::mat4 parentBefore = parent->worldTransformMatrix();
        const glm::mat4 childBefore = child->worldTransformMatrix();
        const glm::vec3 childLocalBefore = child->position;
        EditorSession session;
        session.select(&scene, parent);
        session.applySelection(&scene, child, SelectionOperation::ToggleExact);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*parent),
            transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}));
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Complete selected rotation branch failed");
        passed &= expect(sameMatrix(parent->worldTransformMatrix(),
                                    delta * parentBefore) &&
                             sameMatrix(child->worldTransformMatrix(),
                                        delta * childBefore) &&
                             child->position == childLocalBefore,
                         "Selected rotation branch changed descendant local");
    }

    {
        TestScene scene;
        GameObject* a = addObject<GameObject>(scene, "hole-rotate-a");
        GameObject* b = addObject<GameObject>(scene, "hole-rotate-b");
        GameObject* c = addObject<GameObject>(scene, "hole-rotate-c");
        if (!a || !b || !c ||
            !scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create rotation-hole fixture");
        }
        a->position = {4.0f, 0.0f, 0.0f};
        b->position = {2.0f, 0.0f, 0.0f};
        c->position = {3.0f, 0.0f, 0.0f};
        const glm::mat4 aBefore = a->worldTransformMatrix();
        const glm::mat4 bBefore = b->worldTransformMatrix();
        const glm::mat4 cBefore = c->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, a);
        session.applySelection(&scene, c, SelectionOperation::ToggleExact);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*a),
            transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}));
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Selection-hole rotation failed");
        passed &= expect(sameMatrix(a->worldTransformMatrix(), delta * aBefore) &&
                             sameMatrix(b->worldTransformMatrix(), bBefore) &&
                             sameMatrix(c->worldTransformMatrix(), delta * cBefore),
                         "Selection-hole rotation target was incorrect");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "scale-boundary-parent");
        GameObject* child = addObject<GameObject>(scene, "scale-boundary-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create scale boundary fixture");
        }
        parent->position = {3.0f, 0.0f, 0.0f};
        child->position = {2.0f, 1.0f, 0.0f};
        const glm::mat4 parentBefore = parent->worldTransformMatrix();
        const glm::mat4 childBefore = child->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, parent);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*parent), transform_math::makeScaleMatrix(
                                         {2.0f, 2.0f, 2.0f}));
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Selected-parent scale transaction failed");
        passed &= expect(sameMatrix(parent->worldTransformMatrix(), delta * parentBefore) &&
                             sameMatrix(child->worldTransformMatrix(), childBefore),
                         "Unselected child moved during parent scale");
    }

    {
        TestScene scene;
        GameObject* a = addObject<GameObject>(scene, "hole-scale-a");
        GameObject* b = addObject<GameObject>(scene, "hole-scale-b");
        GameObject* c = addObject<GameObject>(scene, "hole-scale-c");
        if (!a || !b || !c ||
            !scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create scale-hole fixture");
        }
        a->position = {4.0f, 0.0f, 0.0f};
        b->position = {2.0f, 0.0f, 0.0f};
        c->position = {3.0f, 0.0f, 0.0f};
        const glm::mat4 aBefore = a->worldTransformMatrix();
        const glm::mat4 bBefore = b->worldTransformMatrix();
        const glm::mat4 cBefore = c->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, a);
        session.applySelection(&scene, c, SelectionOperation::ToggleExact);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*a), transform_math::makeScaleMatrix({2.0f, 2.0f, 2.0f}));
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Selection-hole scale failed");
        passed &= expect(sameMatrix(a->worldTransformMatrix(), delta * aBefore) &&
                             sameMatrix(b->worldTransformMatrix(), bBefore) &&
                             sameMatrix(c->worldTransformMatrix(), delta * cBefore),
                         "Selection-hole scale target was incorrect");
    }

    {
        TestScene scene;
        GameObject* a = addObject<GameObject>(scene, "multi-root-a");
        GameObject* d = addObject<GameObject>(scene, "multi-root-d");
        GameObject* e = addObject<GameObject>(scene, "multi-root-e");
        if (!a || !d || !e ||
            !scene.reparentGameObject(*e, d, Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create multi-root transform fixture");
        }
        a->position = {10.0f, 0.0f, 0.0f};
        d->position = {30.0f, 0.0f, 0.0f};
        e->position = {2.0f, 1.0f, 0.0f};
        const glm::mat4 aBefore = a->worldTransformMatrix();
        const glm::mat4 dBefore = d->worldTransformMatrix();
        const glm::mat4 eBefore = e->worldTransformMatrix();
        const glm::vec3 eLocalBefore = e->position;
        EditorSession session;
        session.select(&scene, a);
        session.applySelection(&scene, d, SelectionOperation::ToggleExact);
        session.applySelection(&scene, e, SelectionOperation::ToggleExact);
        const glm::mat4 delta = pivotDelta(
            {20.0f, 0.0f, 0.0f}, transform_math::makeScaleMatrix(
                                     {2.0f, 2.0f, 2.0f}));
        passed &= expect(applySharedDelta(scene, session, delta),
                         "Multi-root scale transaction failed");
        passed &= expect(sameMatrix(a->worldTransformMatrix(), delta * aBefore) &&
                             sameMatrix(d->worldTransformMatrix(), delta * dBefore) &&
                             sameMatrix(e->worldTransformMatrix(), delta * eBefore) &&
                             e->position == eLocalBefore,
                         "Multi-root transform did not apply one shared scale");
    }

    {
        TestScene scene;
        GameObject* a = addObject<GameObject>(scene, "multi-root-translate-a");
        GameObject* d = addObject<GameObject>(scene, "multi-root-translate-d");
        GameObject* e = addObject<GameObject>(scene, "multi-root-translate-e");
        if (!a || !d || !e ||
            !scene.reparentGameObject(*e, d, Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create multi-root translation fixture");
        }
        a->position = {10.0f, 0.0f, 0.0f};
        d->position = {30.0f, 0.0f, 0.0f};
        e->position = {2.0f, 1.0f, 0.0f};
        const glm::vec3 eLocalBefore = e->position;
        EditorSession session;
        session.select(&scene, a);
        session.applySelection(&scene, d, SelectionOperation::ToggleExact);
        session.applySelection(&scene, e, SelectionOperation::ToggleExact);
        const glm::mat4 translation =
            transform_math::makeTranslationMatrix({5.0f, 0.0f, 0.0f});
        const glm::mat4 aBeforeTranslation = a->worldTransformMatrix();
        const glm::mat4 dBeforeTranslation = d->worldTransformMatrix();
        const glm::mat4 eBeforeTranslation = e->worldTransformMatrix();
        passed &= expect(applySharedDelta(scene, session, translation) &&
                             sameMatrix(a->worldTransformMatrix(),
                                        translation * aBeforeTranslation) &&
                             sameMatrix(d->worldTransformMatrix(),
                                        translation * dBeforeTranslation) &&
                             sameMatrix(e->worldTransformMatrix(),
                                        translation * eBeforeTranslation) &&
                             e->position == eLocalBefore,
                         "Multiple selected roots did not share translation");

        const glm::mat4 rotation = pivotDelta(
            {25.0f, 0.0f, 0.0f},
            transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}));
        const glm::mat4 aBeforeRotation = a->worldTransformMatrix();
        const glm::mat4 dBeforeRotation = d->worldTransformMatrix();
        const glm::mat4 eBeforeRotation = e->worldTransformMatrix();
        passed &= expect(applySharedDelta(scene, session, rotation) &&
                             sameMatrix(a->worldTransformMatrix(),
                                        rotation * aBeforeRotation) &&
                             sameMatrix(d->worldTransformMatrix(),
                                        rotation * dBeforeRotation) &&
                             sameMatrix(e->worldTransformMatrix(),
                                        rotation * eBeforeRotation) &&
                             e->position == eLocalBefore,
                         "Multiple selected roots did not share rotation");
    }

    return passed;
}

bool runRotateScaleStabilizationTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* object = addObject<GameObject>(scene, "rotate-scale-root-world");
        if (!object) {
            return expect(false, "Could not create world-rotate scale fixture");
        }
        object->position = {2.0f, -3.0f, 4.0f};
        object->rotation = {11.0f, -23.0f, 37.0f};
        object->scale = {1.0f, 1.0f, 1.0f};
        const glm::vec3 originalScale = object->scale;
        const glm::mat4 beforeWorld = object->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, object);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*object),
            transform_math::makeRotationMatrix({17.0f, -29.0f, 41.0f}));
        passed &= expect(
            applyRotateDelta(scene, session, delta, TransformSpace::World) &&
                sameMatrix(object->worldTransformMatrix(), delta * beforeWorld) &&
                sameVectorBits(object->scale, originalScale),
            "World Rotate changed a root object's authored unit scale");
    }

    {
        TestScene scene;
        GameObject* object = addObject<GameObject>(scene, "rotate-scale-root-local");
        if (!object) {
            return expect(false, "Could not create local-rotate scale fixture");
        }
        object->position = {-5.0f, 2.0f, 7.0f};
        object->rotation = {-14.0f, 19.0f, 28.0f};
        object->scale = {1.0f, 1.0f, 1.0f};
        const glm::vec3 originalScale = object->scale;
        const glm::mat4 beforeWorld = object->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, object);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*object),
            transform_math::makeRotationMatrix({-31.0f, 13.0f, 23.0f}));
        passed &= expect(
            applyRotateDelta(scene, session, delta, TransformSpace::Local) &&
                sameMatrix(object->worldTransformMatrix(), delta * beforeWorld) &&
                sameVectorBits(object->scale, originalScale),
            "Local Rotate changed a root object's authored unit scale");
    }

    {
        TestScene scene;
        GameObject* object = addObject<GameObject>(scene, "rotate-scale-nonuniform");
        if (!object) {
            return expect(false, "Could not create non-uniform rotate fixture");
        }
        object->rotation = {8.0f, 27.0f, -16.0f};
        object->scale = {2.0f, 3.0f, 4.0f};
        const glm::vec3 originalScale = object->scale;
        const glm::mat4 beforeWorld = object->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, object);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*object),
            transform_math::makeRotationMatrix({23.0f, 11.0f, -37.0f}));
        passed &= expect(
            applyRotateDelta(scene, session, delta, TransformSpace::World) &&
                sameMatrix(object->worldTransformMatrix(), delta * beforeWorld) &&
                sameVectorBits(object->scale, originalScale),
            "Rotate changed a root object's non-uniform authored scale");
    }

    {
        TestScene scene;
        GameObject* object = addObject<GameObject>(scene, "rotate-scale-reflected");
        if (!object) {
            return expect(false, "Could not create reflected rotate fixture");
        }
        object->rotation = {-12.0f, 21.0f, 34.0f};
        object->scale = {-2.0f, 3.0f, -4.0f};
        const glm::vec3 originalScale = object->scale;
        const glm::mat4 beforeWorld = object->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, object);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*object),
            transform_math::makeRotationMatrix({-19.0f, 33.0f, 7.0f}));
        passed &= expect(
            applyRotateDelta(scene, session, delta, TransformSpace::World) &&
                sameMatrix(object->worldTransformMatrix(), delta * beforeWorld) &&
                sameVectorBits(object->scale, originalScale),
            "Rotate changed a root object's reflected authored scale");
    }

    {
        TestScene scene;
        GameObject* first = addObject<GameObject>(scene, "rotate-scale-shared-a");
        GameObject* second = addObject<GameObject>(scene, "rotate-scale-shared-b");
        if (!first || !second) {
            return expect(false, "Could not create shared-pivot rotate fixture");
        }
        first->position = {10.0f, 0.0f, 0.0f};
        first->rotation = {7.0f, -11.0f, 19.0f};
        first->scale = {2.0f, 3.0f, 4.0f};
        second->position = {30.0f, 0.0f, 0.0f};
        second->rotation = {-13.0f, 17.0f, -23.0f};
        second->scale = {-2.0f, 3.0f, -4.0f};
        const glm::vec3 firstScale = first->scale;
        const glm::vec3 secondScale = second->scale;
        const glm::mat4 firstBefore = first->worldTransformMatrix();
        const glm::mat4 secondBefore = second->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, first);
        session.applySelection(&scene, second, SelectionOperation::ToggleExact);
        const glm::mat4 delta = pivotDelta(
            {20.0f, 0.0f, 0.0f},
            transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}));
        passed &= expect(
            applyRotateDelta(scene, session, delta, TransformSpace::World) &&
                sameMatrix(first->worldTransformMatrix(), delta * firstBefore) &&
                sameMatrix(second->worldTransformMatrix(), delta * secondBefore) &&
                sameVectorBits(first->scale, firstScale) &&
                sameVectorBits(second->scale, secondScale),
            "Shared-pivot Rotate changed a selected root's authored scale");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "rotate-scale-hierarchy-parent");
        GameObject* child = addObject<GameObject>(scene, "rotate-scale-hierarchy-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create hierarchy rotate fixture");
        }
        parent->scale = {2.0f, 1.0f, 1.0f};
        child->position = {3.0f, 0.0f, 0.0f};
        child->scale = {1.0f, 1.0f, 1.0f};
        const glm::vec3 originalScale = child->scale;
        const glm::mat4 beforeWorld = child->worldTransformMatrix();
        EditorSession session;
        session.select(&scene, child);
        const glm::mat4 delta = pivotDelta(
            worldPosition(*child),
            transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}));
        passed &= expect(
            applyRotateDelta(scene, session, delta, TransformSpace::World) &&
                sameMatrix(child->worldTransformMatrix(), delta * beforeWorld) &&
                !sameVectorBits(child->scale, originalScale) &&
                sameVector(child->scale, {2.0f, 0.5f, 1.0f}),
            "Rotate incorrectly forced authored scale through a scaled parent");
    }

    return passed;
}

bool runTransactionalBatchFailureTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* validRoot = addObject<GameObject>(scene, "valid-branch");
        auto invalidOwner = std::make_unique<AttachedCameraObject>();
        AttachedCameraObject* invalidRoot = invalidOwner.get();
        invalidRoot->persistentId = "invalid-branch";
        GameObject* invalidChild = addObject<GameObject>(
            scene, "invalid-compensated-child");
        if (!validRoot || !invalidRoot || !invalidChild ||
            !scene.addGameObject(std::move(invalidOwner)) ||
            !scene.reparentGameObject(*invalidChild, invalidRoot,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create transactional failure fixture");
        }
        validRoot->position = {20.0f, 0.0f, 0.0f};
        invalidRoot->position = {0.0f, 0.0f, 0.0f};
        invalidChild->position = {2.0f, 3.0f, 0.0f};
        invalidChild->rotation = {10.0f, 20.0f, 30.0f};
        invalidChild->scale = {1.0f, 1.5f, 2.0f};
        invalidRoot->camera.position = {7.0f, 8.0f, 9.0f};
        invalidRoot->camera.front = {0.0f, 0.0f, -1.0f};
        invalidRoot->camera.up = {0.0f, 1.0f, 0.0f};

        const glm::vec3 validPositionBefore = validRoot->position;
        const glm::vec3 invalidPositionBefore = invalidRoot->position;
        const glm::vec3 childPositionBefore = invalidChild->position;
        const glm::vec3 childRotationBefore = invalidChild->rotation;
        const glm::vec3 childScaleBefore = invalidChild->scale;
        const glm::vec3 cameraPositionBefore = invalidRoot->camera.position;
        const glm::vec3 cameraFrontBefore = invalidRoot->camera.front;
        const glm::vec3 cameraUpBefore = invalidRoot->camera.up;

        EditorSession session;
        session.select(&scene, validRoot);
        session.applySelection(&scene, invalidRoot,
                               SelectionOperation::ToggleExact);
        editor_transform::TransformDragSnapshot snapshot;
        Result result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::World, snapshot);
        std::vector<editor_transform::TransformCandidate> candidates;
        const glm::mat4 validDelta = transform_math::makeTranslationMatrix(
            {5.0f, 0.0f, 0.0f});
        result = result ? editor_transform::solveTransformDrag(
                              snapshot, validDelta * snapshot.originalGizmoWorld,
                              candidates)
                        : result;
        result = result ? editor_transform::commitTransformCandidates(
                              snapshot, candidates)
                        : result;
        passed &= expect(static_cast<bool>(result) &&
                             !sameVector(invalidRoot->camera.position,
                                         cameraPositionBefore),
                         "Valid pre-failure transaction did not update Camera");

        const glm::mat4 invalidDelta = pivotDelta(
            snapshot.pivot, transform_math::makeScaleMatrix({2.0f, 1.0f, 1.0f}));
        result = editor_transform::solveTransformDrag(
            snapshot, invalidDelta * snapshot.originalGizmoWorld, candidates);
        passed &= expect(!result,
                         "Meaningful compensation shear was accepted in a batch");
        passed &= expect(validRoot->position != validPositionBefore &&
                             invalidRoot->position != invalidPositionBefore,
                         "Failure fixture did not have a committed valid prefix");
        passed &= expect(editor_transform::restoreTransformDragSnapshot(snapshot),
                         "Failed to restore the whole transform transaction");
        passed &= expect(validRoot->position == validPositionBefore &&
                             invalidRoot->position == invalidPositionBefore &&
                             invalidChild->position == childPositionBefore &&
                             invalidChild->rotation == childRotationBefore &&
                             invalidChild->scale == childScaleBefore &&
                             invalidRoot->camera.position == cameraPositionBefore &&
                             invalidRoot->camera.front == cameraFrontBefore &&
                             invalidRoot->camera.up == cameraUpBefore,
                         "Transactional failure left partial GameObject/Camera state");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "singular-batch-parent");
        GameObject* child = addObject<GameObject>(scene, "singular-batch-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create singular batch fixture");
        }
        parent->position = {4.0f, 0.0f, 0.0f};
        child->position = {2.0f, 1.0f, 0.0f};
        const glm::vec3 parentPositionBefore = parent->position;
        const glm::vec3 childPositionBefore = child->position;
        EditorSession session;
        session.select(&scene, parent);
        editor_transform::TransformDragSnapshot snapshot;
        Result result = editor_transform::captureTransformDragSnapshot(
            scene, session, TransformSpace::World, snapshot);
        const glm::mat4 singularDelta = pivotDelta(
            snapshot.pivot, transform_math::makeScaleMatrix({0.0f, 1.0f, 1.0f}));
        std::vector<editor_transform::TransformCandidate> candidates;
        result = result ? editor_transform::solveTransformDrag(
                              snapshot,
                              singularDelta * snapshot.originalGizmoWorld,
                              candidates)
                        : result;
        passed &= expect(!result && parent->position == parentPositionBefore &&
                             child->position == childPositionBefore,
                         "Singular compensation did not reject atomically");
    }

    return passed;
}

bool runEditorWorldDataTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "light-parent");
        PointLight* point = addObject<PointLight>(scene, "point-child");
        DirectionalLight* directional =
            addObject<DirectionalLight>(scene, "directional-child");
        if (!parent || !point || !directional ||
            !scene.reparentGameObject(*point, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*directional, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create world-light helper fixture");
        }
        parent->position = {100.0f, 50.0f, 0.0f};
        parent->rotation = {0.0f, 0.0f, 90.0f};
        parent->scale = {2.0f, 3.0f, 1.0f};
        point->position = {10.0f, 5.0f, 0.0f};
        directional->position = {-4.0f, 8.0f, 2.0f};
        directional->rotation = {30.0f, -10.0f, 40.0f};

        glm::vec3 pointWorldPosition;
        glm::vec3 directionalWorldPosition;
        glm::vec3 worldDirection;
        passed &= expect(
            editor_picking::deriveWorldPosition(*point, pointWorldPosition) &&
                sameVector(pointWorldPosition, {85.0f, 70.0f, 0.0f}) &&
                editor_picking::deriveWorldPosition(
                    *directional, directionalWorldPosition) &&
                sameVector(directionalWorldPosition,
                           glm::vec3(directional->worldTransformMatrix()[3])) &&
                directional->calculateWorldDirection(worldDirection),
            "Light editor helper world data did not use hierarchy transforms");
        passed &= expect(
            sameVector(worldDirection,
                       glm::normalize(glm::vec3(
                           directional->worldTransformMatrix() *
                           glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)))),
            "Directional helper did not use the hierarchy-aware direction");

        PointLight rootPoint;
        rootPoint.position = {1.0f, 2.0f, 3.0f};
        glm::vec3 rootPosition;
        passed &= expect(editor_picking::deriveWorldPosition(
                             rootPoint, rootPosition) &&
                             sameVector(rootPosition, rootPoint.position),
                         "Root PointLight helper changed position semantics");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "camera-parent");
        Camera* camera = addObject<Camera>(scene, "camera-child");
        if (!parent || !camera ||
            !scene.reparentGameObject(*camera, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create camera helper fixture");
        }
        parent->position = {100.0f, 20.0f, -5.0f};
        parent->rotation = {10.0f, 20.0f, 30.0f};
        parent->scale = {2.0f, 3.0f, 1.5f};
        camera->position = {4.0f, 5.0f, 6.0f};
        camera->front = {0.2f, 0.7f, -1.0f};
        camera->up = {0.1f, 1.0f, 0.2f};

        CameraWorldPose expected;
        CameraWorldPose visualization;
        const bool expectedResult = camera->calculateWorldPose(expected);
        const bool visualizationResult =
            editor_picking::calculateCameraVisualizationPose(
                *camera, camera, visualization);
        passed &= expect(expectedResult && visualizationResult &&
                             sameVector(visualization.position,
                                        expected.position) &&
                             sameVector(visualization.front, expected.front) &&
                             sameVector(visualization.up, expected.up),
                         "Standalone Camera helper did not use Camera world pose");

        AttachedCameraObject attached;
        attached.camera.position = {7.0f, 8.0f, 9.0f};
        attached.camera.front = {0.0f, 0.0f, -1.0f};
        attached.camera.up = {0.0f, 1.0f, 0.0f};
        CameraWorldPose attachedPose;
        passed &= expect(
            editor_picking::calculateCameraVisualizationPose(
                attached.camera, &attached, attachedPose) &&
                sameVector(attachedPose.position, attached.camera.position) &&
                sameVector(attachedPose.front, attached.camera.front) &&
                sameVector(attachedPose.up, attached.camera.up),
            "Attached Camera helper was transformed a second time");

        Camera rootCamera;
        rootCamera.position = {12.0f, 13.0f, 14.0f};
        CameraWorldPose rootPose;
        passed &= expect(
            editor_picking::calculateCameraVisualizationPose(
                rootCamera, &rootCamera, rootPose) &&
                sameVector(rootPose.position, rootCamera.position),
            "Root Camera helper changed world position semantics");
    }

    return passed;
}

}  // namespace

int main() {
    return runRootConversionTests() && runChildTranslationTests() &&
                   runRotatedAndScaledParentTests() &&
                   runRotationAndMultiLevelTests() &&
                   runFailureAndTransactionalTests() &&
                   runSelectionPivotAndSpaceTests() &&
                   runMultiObjectTransformTests() &&
                   runRotateScaleStabilizationTests() &&
                   runTransactionalBatchFailureTests() &&
                   runEditorWorldDataTests()
               ? 0
               : 1;
}
