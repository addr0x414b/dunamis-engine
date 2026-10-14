#include "editor/editor_mutation.h"
#include "editor/transform_undo_history.h"
#include "scene/camera.h"
#include "scene/game_object.h"
#include "scene/scene.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>
#include <vector>

namespace {

class UndoTestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

class AttachedCameraOwner final : public GameObject {
public:
    Camera camera;

    Camera* attachedCamera() noexcept override { return &camera; }
    const Camera* attachedCamera() const noexcept override {
        return &camera;
    }
};

class SwappableCameraOwner final : public GameObject {
public:
    std::unique_ptr<Camera> camera = std::make_unique<Camera>();

    Camera* attachedCamera() noexcept override { return camera.get(); }
    const Camera* attachedCamera() const noexcept override {
        return camera.get();
    }
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second,
                float tolerance = 1.0e-5f) {
    return glm::length(first - second) <= tolerance;
}

template <typename ObjectType = GameObject>
ObjectType* addObject(UndoTestScene& scene, const char* id) {
    auto object = std::make_unique<ObjectType>();
    object->persistentId = id;
    ObjectType* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) {
        return nullptr;
    }
    return pointer;
}

bool beginAction(editor_undo::TransformUndoHistory& history,
                 const Scene& scene, GameObject* object,
                 editor_undo::TransformUndoSnapshot& snapshot) {
    return static_cast<bool>(history.capture(
        scene, std::vector<GameObject*>{object}, snapshot));
}

bool runBasicAndSequentialUndoTests() {
    UndoTestScene scene;
    GameObject* object = addObject(scene, "undo-sequential");
    if (object == nullptr) {
        return expect(false, "Could not create transform undo fixture");
    }

    editor_undo::TransformUndoHistory history;
    editor_undo::TransformUndoSnapshot snapshot;
    bool passed = beginAction(history, scene, object, snapshot) &&
        static_cast<bool>(editor_mutation::applyPosition(*object, {4, 5, 6})) &&
        static_cast<bool>(history.commit(scene, snapshot));
    passed &= expect(history.actionCount() == 1,
                     "Translation did not create one undo action");

    passed &= beginAction(history, scene, object, snapshot) &&
        static_cast<bool>(editor_mutation::applyRotation(*object, {10, 20, 30})) &&
        static_cast<bool>(history.commit(scene, snapshot));
    passed &= beginAction(history, scene, object, snapshot) &&
        static_cast<bool>(editor_mutation::applyScale(*object, {2, 3, 4})) &&
        static_cast<bool>(history.commit(scene, snapshot));

    passed &= expect(history.actionCount() == 3,
                     "Move, rotation, and scale were not separate actions");
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(object->position, {4, 5, 6}) &&
                         sameVector(object->rotation, {10, 20, 30}) &&
                         sameVector(object->scale, {1, 1, 1}),
                     "Scale undo did not restore the preceding local state");
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(object->position, {4, 5, 6}) &&
                         sameVector(object->rotation, {0, 0, 0}) &&
                         sameVector(object->scale, {1, 1, 1}),
                     "Rotation undo did not restore the preceding local state");
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(object->position, {0, 0, 0}) &&
                         sameVector(object->rotation, {0, 0, 0}) &&
                         sameVector(object->scale, {1, 1, 1}) &&
                         history.actionCount() == 0,
                     "Translation undo did not restore the initial state");
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(object->position, {0, 0, 0}) &&
                         history.actionCount() == 0,
                     "Undo on empty history was not a harmless no-op");
    return passed;
}

bool runGestureAndNoOpTests() {
    UndoTestScene scene;
    GameObject* object = addObject(scene, "undo-gesture");
    if (object == nullptr) {
        return expect(false, "Could not create gesture undo fixture");
    }
    editor_undo::TransformUndoHistory history;
    editor_undo::TransformUndoSnapshot snapshot;

    bool passed = beginAction(history, scene, object, snapshot);
    object->position = {1, 0, 0};
    object->position = {2, 0, 0};
    object->position = {3, 0, 0};
    passed &= static_cast<bool>(history.commit(scene, snapshot));
    passed &= expect(history.actionCount() == 1,
                     "Intermediate drag updates created multiple actions");
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(object->position, {0, 0, 0}),
                     "Completed gesture did not restore its original state");

    passed &= beginAction(history, scene, object, snapshot);
    passed &= static_cast<bool>(history.commit(scene, snapshot));
    passed &= expect(history.actionCount() == 0,
                     "A no-op gesture created an undo action");

    passed &= beginAction(history, scene, object, snapshot);
    object->position = {9, 0, 0};
    object->position = {0, 0, 0};  // The caller rolled back a canceled drag.
    passed &= expect(history.actionCount() == 0,
                     "A canceled gesture created an undo action");

    passed &= beginAction(history, scene, object, snapshot);
    // A rejected transform never commits the captured pending transaction.
    passed &= expect(history.actionCount() == 0,
                     "A rejected gesture created an undo action");
    return passed;
}

bool runMultiObjectAndHierarchyTests() {
    UndoTestScene scene;
    GameObject* parent = addObject(scene, "undo-parent");
    GameObject* child = addObject(scene, "undo-child");
    GameObject* peer = addObject(scene, "undo-peer");
    if (parent == nullptr || child == nullptr || peer == nullptr ||
        !scene.reparentGameObject(*child, parent,
                                  Scene::ReparentMode::PreserveLocal)) {
        return expect(false, "Could not create hierarchy undo fixture");
    }
    child->position = {2, 1, 0};
    const glm::mat4 childWorldBefore = child->worldTransformMatrix();

    editor_undo::TransformUndoHistory history;
    editor_undo::TransformUndoSnapshot snapshot;
    bool passed = static_cast<bool>(history.capture(
        scene, std::vector<GameObject*>{parent, peer}, snapshot));
    parent->scale = {3, 3, 3};
    peer->position = {5, 0, 0};
    passed &= static_cast<bool>(history.commit(scene, snapshot));
    passed &= expect(history.actionCount() == 1,
                     "A multi-object transform created more than one action");
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(parent->scale, {1, 1, 1}) &&
                         sameVector(peer->position, {0, 0, 0}) &&
                         glm::length(child->worldTransformMatrix()[3] -
                                     childWorldBefore[3]) < 1.0e-5f,
                     "Multi-object or inherited parent undo was incorrect");

    passed &= static_cast<bool>(history.capture(
        scene, std::vector<GameObject*>{parent, child}, snapshot));
    parent->scale = {2, 2, 2};
    child->position = {4, 2, 0};
    passed &= static_cast<bool>(history.commit(scene, snapshot));
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(parent->scale, {1, 1, 1}) &&
                         sameVector(child->position, {2, 1, 0}) &&
                         glm::length(child->worldTransformMatrix()[3] -
                                     childWorldBefore[3]) < 1.0e-5f,
                     "Parent and selected child locals were double-transformed");
    return passed;
}

bool runCameraTests() {
    UndoTestScene scene;
    AttachedCameraOwner* attached =
        addObject<AttachedCameraOwner>(scene, "undo-attached-camera");
    Camera* standalone = addObject<Camera>(scene, "undo-standalone-camera");
    if (attached == nullptr || standalone == nullptr) {
        return expect(false, "Could not create Camera undo fixtures");
    }
    attached->position = {1, 2, 3};
    attached->camera.position = {2, 2, 3};
    attached->camera.front = {0, 0, -1};
    attached->camera.up = {0, 1, 0};
    standalone->position = {7, 8, 9};
    standalone->front = {1, 0, 0};
    standalone->up = {0, 1, 0};

    editor_undo::TransformUndoHistory history;
    editor_undo::TransformUndoSnapshot snapshot;
    bool passed = beginAction(history, scene, attached, snapshot);
    passed &= static_cast<bool>(editor_mutation::applyPosition(
        *attached, {11, 2, 3}));
    passed &= static_cast<bool>(history.commit(scene, snapshot));
    const glm::vec3 attachedCameraAfter = attached->camera.position;
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(attached->position, {1, 2, 3}) &&
                         sameVector(attached->camera.position, {2, 2, 3}) &&
                         !sameVector(attached->camera.position,
                                     attachedCameraAfter),
                     "Attached Camera auxiliary pose was not restored");

    passed &= beginAction(history, scene, standalone, snapshot);
    passed &= static_cast<bool>(editor_mutation::applyRotation(
        *standalone, {0, 90, 0}));
    passed &= static_cast<bool>(history.commit(scene, snapshot));
    passed &= expect(static_cast<bool>(history.undo(scene)) &&
                         sameVector(standalone->position, {7, 8, 9}) &&
                         sameVector(standalone->rotation, {0, 0, 0}) &&
                         sameVector(standalone->front, {1, 0, 0}) &&
                         sameVector(standalone->up, {0, 1, 0}),
                     "Standalone Camera orientation was not restored");
    return passed;
}

bool runCapacityTests() {
    UndoTestScene scene;
    GameObject* object = addObject(scene, "undo-capacity");
    if (object == nullptr) {
        return expect(false, "Could not create capacity undo fixture");
    }
    editor_undo::TransformUndoHistory history;
    editor_undo::TransformUndoSnapshot snapshot;
    bool passed = true;
    for (std::size_t index = 0; index <= maxTransformUndoActions; ++index) {
        passed &= beginAction(history, scene, object, snapshot);
        object->position.x += 1.0f;
        passed &= static_cast<bool>(history.commit(scene, snapshot));
    }
    passed &= expect(history.actionCount() == maxTransformUndoActions,
                     "Undo history did not retain exactly 100 actions");
    for (std::size_t index = 0; index < maxTransformUndoActions; ++index) {
        passed &= static_cast<bool>(history.undo(scene));
    }
    passed &= expect(sameVector(object->position, {1, 0, 0}) &&
                         history.actionCount() == 0,
                     "Capacity eviction did not discard the oldest action");
    return passed;
}

bool runInvalidationAndAtomicityTests() {
    UndoTestScene scene;
    GameObject* first = addObject(scene, "undo-atomic-a");
    GameObject* second = addObject(scene, "undo-atomic-b");
    if (first == nullptr || second == nullptr) {
        return expect(false, "Could not create atomic undo fixtures");
    }
    editor_undo::TransformUndoHistory history;
    editor_undo::TransformUndoSnapshot snapshot;
    bool passed = static_cast<bool>(history.capture(
        scene, std::vector<GameObject*>{first, second}, snapshot));
    first->position = {1, 0, 0};
    second->position = {2, 0, 0};
    passed &= static_cast<bool>(history.commit(scene, snapshot));
    second->position.x = std::numeric_limits<float>::quiet_NaN();
    const Result invalidUndo = history.undo(scene);
    passed &= expect(!invalidUndo && history.actionCount() == 0 &&
                         sameVector(first->position, {1, 0, 0}) &&
                         std::isnan(second->position.x),
                     "Invalid multi-object undo partially changed the Scene");

    UndoTestScene sceneA;
    UndoTestScene sceneB;
    GameObject* objectA = addObject(sceneA, "same-persistent-id");
    GameObject* objectB = addObject(sceneB, "same-persistent-id");
    if (objectA == nullptr || objectB == nullptr) {
        return expect(false, "Could not create scene replacement fixtures");
    }
    passed &= beginAction(history, sceneA, objectA, snapshot);
    objectA->position = {8, 0, 0};
    passed &= static_cast<bool>(history.commit(sceneA, snapshot));
    const Result replacedSceneUndo = history.undo(sceneB);
    passed &= expect(!replacedSceneUndo && history.actionCount() == 0 &&
                         sameVector(objectB->position, {0, 0, 0}),
                     "Scene replacement allowed an old action to affect a new Scene");

    SwappableCameraOwner* cameraOwner =
        addObject<SwappableCameraOwner>(sceneA, "swappable-camera-owner");
    if (cameraOwner == nullptr) {
        return expect(false, "Could not create Camera association fixture");
    }
    passed &= beginAction(history, sceneA, cameraOwner, snapshot);
    cameraOwner->position = {3, 0, 0};
    passed &= static_cast<bool>(history.commit(sceneA, snapshot));
    cameraOwner->camera = std::make_unique<Camera>();
    const Result changedCameraUndo = history.undo(sceneA);
    passed &= expect(!changedCameraUndo && history.actionCount() == 0 &&
                         sameVector(cameraOwner->position, {3, 0, 0}),
                     "Changed Camera association was restored unsafely");

    passed &= beginAction(history, sceneA, objectA, snapshot);
    objectA->position = {9, 0, 0};
    passed &= static_cast<bool>(history.commit(sceneA, snapshot));
    passed &= static_cast<bool>(sceneA.reparentGameObject(
        *objectA, cameraOwner, Scene::ReparentMode::PreserveLocal));
    const Result changedParentUndo = history.undo(sceneA);
    passed &= expect(!changedParentUndo && history.actionCount() == 0 &&
                         sameVector(objectA->position, {9, 0, 0}),
                     "Changed parent relationship was overwritten by undo");
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= runBasicAndSequentialUndoTests();
    passed &= runGestureAndNoOpTests();
    passed &= runMultiObjectAndHierarchyTests();
    passed &= runCameraTests();
    passed &= runCapacityTests();
    passed &= runInvalidationAndAtomicityTests();
    return passed ? 0 : 1;
}
