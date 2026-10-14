#include "editor/editor_duplication.h"
#include "editor/editor_mutation.h"
#include "editor/editor_session.h"
#include "scene/camera.h"
#include "scene/directional_light.h"
#include "scene/group.h"
#include "scene/model_renderable.h"
#include "scene/point_light.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/type_registry.h"

#include <algorithm>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

class SceneTestAccess {
public:
    static Result activate(Scene& scene) { return scene.activate(); }
    static void deactivate(Scene& scene) { scene.deactivate(); }
    static Result remove(Scene& scene, GameObject* object,
                         std::unique_ptr<GameObject>& removed) {
        return scene.removeGameObjectForEditor(object, removed);
    }
    static Result addEditor(Scene& scene,
                            std::unique_ptr<GameObject> object,
                            GameObject*& inserted) {
        return scene.addGameObjectForEditor(std::move(object), inserted);
    }
};

class GameObjectTestAccess {
public:
    static Result attach(GameObject& object) {
        return object.modelRenderable().markRenderResourcesAttached();
    }
    static void detach(GameObject& object) {
        object.modelRenderable().markRenderResourcesDetached();
    }
    static bool mutableTopology(const GameObject& object) {
        return object.modelRenderable().renderTopologyMutable();
    }
};

class CountedGameObject final : public GameObject {
public:
    inline static std::size_t destroyedCount = 0;
    ~CountedGameObject() override { ++destroyedCount; }
};

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
    const Camera* attachedCamera() const noexcept override { return &camera; }
};

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

GameObject* addObject(TestScene& scene, const char* id, const char* name) {
    auto object = std::make_unique<GameObject>();
    object->persistentId = id;
    object->name = name;
    GameObject* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) return nullptr;
    return pointer;
}

bool sameMatrix(const glm::mat4& first, const glm::mat4& second) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::fabs(first[column][row] - second[column][row]) >
                1.0e-4f) {
                return false;
            }
        }
    }
    return true;
}

Result undoLast(
    EditorSession& session, Scene& scene,
    const editor::EditorObjectCoordinator::RendererBatchDetachment&
        detachRenderer = {},
    const editor::EditorObjectCoordinator::RendererAttachment&
        attachRenderer = {}) {
    return session.undoHistory().undo(
        scene, [&session, &detachRenderer, &attachRenderer](
                   Scene& target, editor_undo::DeleteUndoAction& action) {
            return editor::EditorObjectCoordinator::restoreDeletedSelection(
                target, session, action, detachRenderer, attachRenderer);
        });
}

bool runLeafAndPromotionTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* a = addObject(scene, "leaf-a", "A");
        GameObject* b = addObject(scene, "leaf-b", "B");
        GameObject* c = addObject(scene, "leaf-c", "C");
        if (a == nullptr || b == nullptr || c == nullptr ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare leaf Delete Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {b}, b)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.gameObjects().size() == 2 &&
                             scene.rootObjects().size() == 2 &&
                             scene.rootObjects()[0] == a &&
                             scene.rootObjects()[1] == c &&
                             session.selectedGameObjects().empty() &&
                             session.activeGameObject() == nullptr,
                         "Leaf Delete did not remove exactly the selection");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* a = addObject(scene, "promote-a", "A");
        GameObject* x = addObject(scene, "promote-x", "X");
        GameObject* b = addObject(scene, "promote-b", "B");
        GameObject* c = addObject(scene, "promote-c", "C");
        GameObject* d = addObject(scene, "promote-d", "D");
        GameObject* y = addObject(scene, "promote-y", "Y");
        if (a == nullptr || x == nullptr || b == nullptr || c == nullptr ||
            d == nullptr || y == nullptr ||
            !scene.reparentGameObject(*x, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*d, b, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*y, a, Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare parent-promotion Delete Scene");
        }
        a->position = {10.0f, 2.0f, -3.0f};
        a->scale = {1.5f, 1.5f, 1.5f};
        b->position = {2.0f, 3.0f, 4.0f};
        c->position = {-1.0f, 5.0f, 0.5f};
        d->position = {3.0f, -2.0f, 1.0f};
        const glm::mat4 cWorld = c->worldTransformMatrix();
        const glm::mat4 dWorld = d->worldTransformMatrix();

        EditorSession session;
        if (!session.setExactSelection(&scene, {b}, b)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && a->children().size() == 4 &&
                             a->children()[0] == x && a->children()[1] == c &&
                             a->children()[2] == d && a->children()[3] == y &&
                             c->parent() == a && d->parent() == a &&
                             sameMatrix(c->worldTransformMatrix(), cWorld) &&
                             sameMatrix(d->worldTransformMatrix(), dWorld),
                         "Deleting a parent did not splice/promote children correctly");
        const Result undoResult = undoLast(session, scene);
        passed &= expect(static_cast<bool>(undoResult) &&
                             scene.findGameObject("promote-b") == b &&
                             a->children().size() == 3 &&
                             a->children()[0] == x && a->children()[1] == b &&
                             a->children()[2] == y && b->children().size() == 2 &&
                             b->children()[0] == c && b->children()[1] == d &&
                             c->parent() == b && d->parent() == b &&
                             c->position == glm::vec3(-1.0f, 5.0f, 0.5f) &&
                             d->position == glm::vec3(3.0f, -2.0f, 1.0f) &&
                             sameMatrix(c->worldTransformMatrix(), cWorld) &&
                             sameMatrix(d->worldTransformMatrix(), dWorld),
                         "Delete undo did not restore promoted children and exact local transforms");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* a = addObject(scene, "chain-a", "A");
        GameObject* b = addObject(scene, "chain-b", "B");
        GameObject* c = addObject(scene, "chain-c", "C");
        GameObject* d = addObject(scene, "chain-d", "D");
        if (a == nullptr || b == nullptr || c == nullptr || d == nullptr ||
            !scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*d, c, Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare selected-chain Delete Scene");
        }
        b->position = {2.0f, 0.0f, 0.0f};
        c->position = {0.0f, 3.0f, 0.0f};
        d->position = {0.0f, 0.0f, 4.0f};
        const glm::mat4 dWorld = d->worldTransformMatrix();
        EditorSession session;
        if (!session.setExactSelection(&scene, {b, c}, c)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && a->children().size() == 1 &&
                             a->children()[0] == d && d->parent() == a &&
                             sameMatrix(d->worldTransformMatrix(), dWorld),
                         "Selected deletion chain did not promote the survivor directly");
        const Result undoResult = undoLast(session, scene);
        passed &= expect(static_cast<bool>(undoResult) &&
                             a->children().size() == 1 && a->children()[0] == b &&
                             b->children().size() == 1 && b->children()[0] == c &&
                             c->children().size() == 1 && c->children()[0] == d &&
                             scene.findGameObject("chain-b") == b &&
                             scene.findGameObject("chain-c") == c &&
                             session.selectedGameObjects() ==
                                 std::vector<GameObject*>{b, c} &&
                             session.activeGameObject() == c &&
                             sameMatrix(d->worldTransformMatrix(), dWorld),
                         "Multi-object Delete undo did not restore the original nested chain");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* a = addObject(scene, "splice-a", "A");
        GameObject* x = addObject(scene, "splice-x", "X");
        GameObject* b = addObject(scene, "splice-b", "B");
        GameObject* c = addObject(scene, "splice-c", "C");
        GameObject* e = addObject(scene, "splice-e", "E");
        GameObject* f = addObject(scene, "splice-f", "F");
        GameObject* d = addObject(scene, "splice-d", "D");
        GameObject* y = addObject(scene, "splice-y", "Y");
        if (a == nullptr || x == nullptr || b == nullptr || c == nullptr ||
            e == nullptr || f == nullptr || d == nullptr || y == nullptr ||
            !scene.reparentGameObject(*x, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*e, c, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*f, c, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*d, b, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*y, a, Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare recursive-splice Delete Scene");
        }
        const glm::mat4 eWorld = e->worldTransformMatrix();
        const glm::mat4 fWorld = f->worldTransformMatrix();
        const glm::mat4 dWorld = d->worldTransformMatrix();
        EditorSession session;
        if (!session.setExactSelection(&scene, {b, c}, c)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && a->children().size() == 5 &&
                             a->children()[0] == x && a->children()[1] == e &&
                             a->children()[2] == f && a->children()[3] == d &&
                             a->children()[4] == y &&
                             sameMatrix(e->worldTransformMatrix(), eWorld) &&
                             sameMatrix(f->worldTransformMatrix(), fWorld) &&
                             sameMatrix(d->worldTransformMatrix(), dWorld),
                         "Recursive Delete promotion order was not deterministic");
        const Result undoResult = undoLast(session, scene);
        passed &= expect(static_cast<bool>(undoResult) &&
                             a->children().size() == 3 &&
                             a->children()[0] == x && a->children()[1] == b &&
                             a->children()[2] == y &&
                             b->children().size() == 2 &&
                             b->children()[0] == c && b->children()[1] == d &&
                             c->children().size() == 2 &&
                             c->children()[0] == e && c->children()[1] == f &&
                             sameMatrix(e->worldTransformMatrix(), eWorld) &&
                             sameMatrix(f->worldTransformMatrix(), fWorld) &&
                             sameMatrix(d->worldTransformMatrix(), dWorld),
                         "Nested mixed Delete undo did not restore exact hierarchy and ordering");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* x = addObject(scene, "root-x", "X");
        GameObject* b = addObject(scene, "root-b", "B");
        GameObject* c = addObject(scene, "root-c", "C");
        GameObject* d = addObject(scene, "root-d", "D");
        GameObject* y = addObject(scene, "root-y", "Y");
        if (x == nullptr || b == nullptr || c == nullptr || d == nullptr ||
            y == nullptr ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*d, b, Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare root-promotion Delete Scene");
        }
        const glm::mat4 cWorld = c->worldTransformMatrix();
        const glm::mat4 dWorld = d->worldTransformMatrix();
        EditorSession session;
        if (!session.setExactSelection(&scene, {b}, b)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.rootObjects().size() == 4 &&
                             scene.rootObjects()[0] == x &&
                             scene.rootObjects()[1] == c &&
                             scene.rootObjects()[2] == d &&
                             scene.rootObjects()[3] == y &&
                             sameMatrix(c->worldTransformMatrix(), cWorld) &&
                             sameMatrix(d->worldTransformMatrix(), dWorld),
                         "Deleted root promotion did not preserve its old slot");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* b = addObject(scene, "mixed-b", "B");
        GameObject* c = addObject(scene, "mixed-c", "C");
        GameObject* d = addObject(scene, "mixed-d", "D");
        GameObject* e = addObject(scene, "mixed-e", "E");
        if (b == nullptr || c == nullptr || d == nullptr || e == nullptr ||
            !scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*d, c, Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*e, d, Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare mixed-selection Delete Scene");
        }
        const glm::mat4 cWorld = c->worldTransformMatrix();
        const glm::mat4 eWorld = e->worldTransformMatrix();
        EditorSession session;
        if (!session.setExactSelection(&scene, {b, d}, d)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.rootObjects().size() == 1 &&
                             scene.rootObjects()[0] == c && c->children().size() == 1 &&
                             c->children()[0] == e && c->parent() == nullptr &&
                             sameMatrix(c->worldTransformMatrix(), cWorld) &&
                             sameMatrix(e->worldTransformMatrix(), eWorld),
                         "Mixed selected descendants were not promoted independently");
        SceneTestAccess::deactivate(scene);
    }

    return passed;
}

bool runGroupAndFailureTests() {
    bool passed = true;

    {
        TestScene scene;
        auto groupObject = std::make_unique<Group>();
        groupObject->persistentId = "group";
        groupObject->name = "Group";
        Group* group = groupObject.get();
        auto firstObject = std::make_unique<GameObject>();
        firstObject->persistentId = "group-a";
        firstObject->name = "A";
        GameObject* first = firstObject.get();
        auto secondObject = std::make_unique<GameObject>();
        secondObject->persistentId = "group-b";
        secondObject->name = "B";
        GameObject* second = secondObject.get();
        if (!scene.addGameObject(std::move(groupObject)) ||
            !scene.addGameObject(std::move(firstObject)) ||
            !scene.addGameObject(std::move(secondObject)) ||
            !scene.reparentGameObject(*first, group,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*second, group,
                                      Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare Group Delete Scene");
        }
        const glm::mat4 firstWorld = first->worldTransformMatrix();
        const glm::mat4 secondWorld = second->worldTransformMatrix();
        EditorSession session;
        if (!session.setExactSelection(&scene, {group}, group)) return false;
        Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.gameObjects().size() == 2 &&
                             scene.rootObjects().size() == 2 &&
                             scene.rootObjects()[0] == first &&
                             scene.rootObjects()[1] == second &&
                             sameMatrix(first->worldTransformMatrix(), firstWorld) &&
                             sameMatrix(second->worldTransformMatrix(), secondWorld),
                         "Group deletion did not use ordinary promotion rules");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        auto groupObject = std::make_unique<Group>();
        groupObject->persistentId = "whole-group";
        Group* group = groupObject.get();
        auto childObject = std::make_unique<GameObject>();
        childObject->persistentId = "whole-child";
        GameObject* child = childObject.get();
        if (!scene.addGameObject(std::move(groupObject)) ||
            !scene.addGameObject(std::move(childObject)) ||
            !scene.reparentGameObject(*child, group,
                                      Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare full Group Delete Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {group, child}, child)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.gameObjects().empty() &&
                             scene.rootObjects().empty(),
                         "Deleting a fully selected Group subtree left survivors");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* parent = addObject(scene, "shear-parent", "Parent");
        GameObject* child = addObject(scene, "shear-child", "Child");
        if (parent == nullptr || child == nullptr ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare strict-TRS Delete Scene");
        }
        parent->scale = {2.0f, 1.0f, 1.0f};
        child->rotation.z = 45.0f;
        EditorSession session;
        if (!session.setExactSelection(&scene, {parent}, parent)) return false;
        session.setRenderColliderEnabled(*parent, true);
        const std::vector<GameObject*> rootsBefore = scene.rootObjects();
        const glm::mat4 childWorld = child->worldTransformMatrix();
        std::size_t detachCalls = 0;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session,
            [&detachCalls](Scene&, const std::vector<GameObject*>&) {
                ++detachCalls;
                return Result::success();
            },
            [](Scene&, GameObject&) { return Result::success(); });
        passed &= expect(!result && scene.gameObjects().size() == 2 &&
                             scene.rootObjects() == rootsBefore &&
                             child->parent() == parent &&
                             sameMatrix(child->worldTransformMatrix(), childWorld) &&
                             session.isSelected(parent) &&
                             session.activeGameObject() == parent &&
                             session.renderColliderEnabled(*parent) &&
                             detachCalls == 0,
                         "Strict-TRS Delete failure was not atomic");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        if (!SceneTestAccess::activate(scene)) return false;
        EditorSession session;
        std::size_t detachCalls = 0;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session,
            [&detachCalls](Scene&, const std::vector<GameObject*>&) {
                ++detachCalls;
                return Result::success();
            },
            [](Scene&, GameObject&) { return Result::success(); });
        passed &= expect(static_cast<bool>(result) && detachCalls == 0,
                         "No-selection Delete was not a silent no-op");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* first = addObject(scene, "attached-preflight-a", "A");
        GameObject* second = addObject(scene, "attached-preflight-b", "B");
        if (first == nullptr || second == nullptr ||
            !GameObjectTestAccess::attach(*second) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare renderer-preflight Delete Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {first, second}, second)) return false;
        const std::vector<GameObject*> rootsBefore = scene.rootObjects();
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(!result && scene.gameObjects().size() == 2 &&
                             scene.rootObjects() == rootsBefore &&
                             session.isSelected(first) && session.isSelected(second) &&
                             session.activeGameObject() == second &&
                             session.undoHistory().actionCount() == 0,
                         "Delete ownership preflight allowed a partial erase");
        GameObjectTestAccess::detach(*second);
        SceneTestAccess::deactivate(scene);
    }

    return passed;
}

bool runSceneBookkeepingTests() {
    bool passed = true;
    TestScene scene;
    GameObject* first = addObject(scene, "book-first", "First");
    auto pointOneObject = std::make_unique<PointLight>();
    pointOneObject->persistentId = "book-point-one";
    PointLight* pointOne = pointOneObject.get();
    auto middleObject = std::make_unique<GameObject>();
    middleObject->persistentId = "book-middle";
    GameObject* middle = middleObject.get();
    auto pointTwoObject = std::make_unique<PointLight>();
    pointTwoObject->persistentId = "book-point-two";
    PointLight* pointTwo = pointTwoObject.get();
    GameObject* later = addObject(scene, "book-later", "Later");
    auto directionalObject = std::make_unique<DirectionalLight>();
    directionalObject->persistentId = "book-directional";
    DirectionalLight* directional = directionalObject.get();
    if (first == nullptr || later == nullptr || pointOne == nullptr ||
        middle == nullptr || pointTwo == nullptr || directional == nullptr ||
        !scene.addGameObject(std::move(pointOneObject)) ||
        !scene.addGameObject(std::move(middleObject)) ||
        !scene.addGameObject(std::move(pointTwoObject)) ||
        !scene.addGameObject(std::move(directionalObject)) ||
        !SceneTestAccess::activate(scene)) {
        return expect(false, "Could not prepare Scene bookkeeping test");
    }

    std::unique_ptr<GameObject> removed;
    Result result = SceneTestAccess::remove(scene, first, removed);
    passed &= expect(static_cast<bool>(result) && scene.pointLightCount() == 2 &&
                         &scene.pointLightAt(0) == pointOne &&
                         &scene.pointLightAt(1) == pointTwo &&
                         scene.directionalLight() == directional,
                     "Removing an early object corrupted light indices");
    removed.reset();

    result = SceneTestAccess::remove(scene, middle, removed);
    passed &= expect(static_cast<bool>(result) && scene.pointLightCount() == 2 &&
                         &scene.pointLightAt(0) == pointOne &&
                         &scene.pointLightAt(1) == pointTwo &&
                         scene.directionalLight() == directional,
                     "Removing a middle object corrupted light indices");
    removed.reset();

    result = SceneTestAccess::remove(scene, pointOne, removed);
    passed &= expect(static_cast<bool>(result) && scene.pointLightCount() == 1 &&
                         &scene.pointLightAt(0) == pointTwo,
                     "Removing a PointLight did not compact its registration");
    removed.reset();

    result = SceneTestAccess::remove(scene, directional, removed);
    passed &= expect(static_cast<bool>(result) && scene.directionalLight() == nullptr,
                     "Removing the DirectionalLight left a stale registration");
    removed.reset();
    SceneTestAccess::deactivate(scene);

    auto replacement = std::make_unique<DirectionalLight>();
    passed &= expect(static_cast<bool>(scene.addGameObject(std::move(replacement))),
                     "A replacement DirectionalLight could not be inserted");
    return passed;
}

bool runCameraAndColliderTests() {
    bool passed = true;

    {
        TestScene scene;
        auto cameraObject = std::make_unique<Camera>();
        cameraObject->persistentId = "standalone-camera";
        Camera* camera = cameraObject.get();
        GameObject* unrelated = addObject(scene, "unrelated", "Unrelated");
        if (!scene.addGameObject(std::move(cameraObject)) || unrelated == nullptr ||
            !scene.setActiveCameraReference(camera) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare standalone-camera Delete Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {unrelated}, unrelated)) return false;
        Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.activeCamera() == camera,
                         "Deleting an unrelated object changed the active Camera");
        auto* remainingCamera = dynamic_cast<Camera*>(scene.gameObjects().front().get());
        if (remainingCamera == nullptr) return false;
        if (!session.setExactSelection(&scene, {remainingCamera}, remainingCamera)) {
            return false;
        }
        result = editor::EditorObjectCoordinator::deleteSelection(scene, session);
        passed &= expect(static_cast<bool>(result) && scene.activeCamera() == nullptr,
                         "Deleting the active standalone Camera left a dangling reference");
        const Result undoResult = undoLast(session, scene);
        passed &= expect(static_cast<bool>(undoResult) &&
                             scene.findGameObject("standalone-camera") == camera &&
                             scene.activeCamera() == camera,
                         "Delete undo did not restore the active standalone Camera");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        auto ownerObject = std::make_unique<AttachedCameraObject>();
        ownerObject->persistentId = "attached-owner";
        AttachedCameraObject* owner = ownerObject.get();
        Camera* attachedCamera = owner->attachedCamera();
        if (!scene.addGameObject(std::move(ownerObject)) ||
            !scene.setActiveCameraReference(attachedCamera) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare attached-camera Delete Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {owner}, owner)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.activeCamera() == nullptr,
                         "Deleting an active attached-camera owner left a dangling reference");
        const Result undoResult = undoLast(session, scene);
        passed &= expect(static_cast<bool>(undoResult) &&
                             scene.findGameObject("attached-owner") == owner &&
                             owner->attachedCamera() == attachedCamera &&
                             scene.activeCamera() == attachedCamera,
                         "Delete undo did not restore the original attached Camera");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* parent = addObject(scene, "collider-parent", "Parent");
        GameObject* survivor = addObject(scene, "collider-survivor", "Survivor");
        if (parent == nullptr || survivor == nullptr ||
            !scene.reparentGameObject(*survivor, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare collider-ID Delete Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {parent}, parent)) return false;
        session.setRenderColliderEnabled(*parent, true);
        session.setRenderColliderEnabled(*survivor, true);
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) &&
                             !session.renderColliderEnabled(*parent) &&
                             session.renderColliderEnabled(*survivor) &&
                             session.selectedGameObjects().empty(),
                         "Delete collider-ID cleanup removed the wrong IDs");
        SceneTestAccess::deactivate(scene);
    }

    return passed;
}

bool runDeleteUndoIdentityAndPersistenceTests() {
    TestScene scene;
    scene.name = "Delete undo authored baseline";
    GameObject* first = addObject(scene, "undo-order-first", "First");
    auto groupOwner = std::make_unique<Group>();
    groupOwner->persistentId = "undo-order-group";
    groupOwner->name = "Building_002";
    groupOwner->position = {4.0f, 2.0f, -6.0f};
    groupOwner->rotation = {0.0f, 35.0f, 0.0f};
    groupOwner->scale = {1.25f, 1.5f, 0.75f};
    groupOwner->physics.enabled = true;
    groupOwner->physics.motionType = GameObject::PhysicsMotionType::Static;
    groupOwner->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    Group* group = groupOwner.get();
    MeshInstance importedLikeMesh;
    importedLikeMesh.mesh.assetIdentity =
        "fixture:City/Building_002/mesh_0";
    importedLikeMesh.mesh.vertices.resize(3);
    importedLikeMesh.mesh.vertices[0].pos = {0.0f, 0.0f, 0.0f};
    importedLikeMesh.mesh.vertices[1].pos = {1.0f, 0.0f, 0.0f};
    importedLikeMesh.mesh.vertices[2].pos = {0.0f, 1.0f, 0.0f};
    importedLikeMesh.mesh.indices = {0, 1, 2};
    const Result meshResult = group->modelRenderable().addMeshInstance(
        std::move(importedLikeMesh));
    if (first == nullptr || !meshResult ||
        !scene.addGameObject(std::move(groupOwner))) {
        return expect(false, "Could not prepare Delete undo persistence fixture");
    }
    GameObject* last = addObject(scene, "undo-order-last", "Last");
    if (last == nullptr ||
        !SceneTestAccess::activate(scene)) {
        return expect(false, "Could not prepare Delete undo persistence fixture");
    }

    TypeRegistry registry;
    nlohmann::json baseline;
    if (!registerEngineTypes(registry) ||
        !SceneSerializer::serializeAuthored(scene, registry, baseline)) {
        return expect(false, "Could not serialize Delete undo baseline");
    }

    const std::uint64_t instanceId = group->instanceId();
    EditorSession session;
    if (!session.setExactSelection(&scene, {group}, group)) return false;
    session.setRenderColliderEnabled(*group, true);
    const Result deleteResult =
        editor::EditorObjectCoordinator::deleteSelection(scene, session);
    nlohmann::json deletedState;
    const Result deletedSerialize =
        SceneSerializer::serializeAuthored(scene, registry, deletedState);
    bool passed = expect(static_cast<bool>(deleteResult) &&
                             scene.findGameObject("undo-order-group") == nullptr &&
                             session.undoHistory().actionCount() == 1 &&
                             static_cast<bool>(deletedSerialize) &&
                             deletedState != baseline,
                         "Delete did not detach the object or record a distinct authored state");

    const Result undoResult = undoLast(session, scene);
    nlohmann::json restoredState;
    const Result restoredSerialize =
        SceneSerializer::serializeAuthored(scene, registry, restoredState);
    passed &= expect(static_cast<bool>(undoResult) &&
                         scene.findGameObject("undo-order-group") == group &&
                         group->instanceId() == instanceId &&
                         dynamic_cast<Group*>(scene.findGameObject(
                             "undo-order-group")) == group &&
                         group->name == "Building_002" &&
                         group->position == glm::vec3(4.0f, 2.0f, -6.0f) &&
                         group->rotation == glm::vec3(0.0f, 35.0f, 0.0f) &&
                         group->scale == glm::vec3(1.25f, 1.5f, 0.75f) &&
                         group->physics.enabled &&
                         group->physics.motionType ==
                             GameObject::PhysicsMotionType::Static &&
                         group->physics.colliderType ==
                             GameObject::PhysicsColliderType::Mesh &&
                         group->modelRenderable().meshInstances().size() == 1 &&
                         group->modelRenderable().meshInstances()[0]
                                 .mesh.assetIdentity ==
                             "fixture:City/Building_002/mesh_0" &&
                         scene.gameObjects().size() == 3 &&
                         scene.gameObjects()[0].get() == first &&
                         scene.gameObjects()[1].get() == group &&
                         scene.gameObjects()[2].get() == last &&
                         scene.rootObjects().size() == 3 &&
                         scene.rootObjects()[0] == first &&
                         scene.rootObjects()[1] == group &&
                         scene.rootObjects()[2] == last &&
                         session.activeGameObject() == group &&
                         session.selectedGameObjects() ==
                             std::vector<GameObject*>{group} &&
                         session.renderColliderEnabled(*group) &&
                         static_cast<bool>(restoredSerialize) &&
                         restoredState == baseline,
                     "Delete undo did not restore exact identity, authored state, ordering, selection, or serialization");
    session.undoHistory().clear();
    passed &= expect(scene.findGameObject("undo-order-group") == group &&
                         session.undoHistory().actionCount() == 0,
                     "Clearing history after restoration disturbed the Scene-owned object");
    SceneTestAccess::deactivate(scene);
    return passed;
}

bool runSharedChronologicalUndoTests() {
    {
        TestScene scene;
        GameObject* object = addObject(scene, "move-delete-same-object", "A");
        if (object == nullptr || !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare move-delete regression fixture");
        }
        EditorSession session;
        editor_undo::TransformUndoSnapshot snapshot;
        bool passed = static_cast<bool>(session.undoHistory().capture(
            scene, {object}, snapshot));
        object->position.x = 10.0f;
        passed &= static_cast<bool>(session.undoHistory().commit(scene, snapshot));
        passed &= static_cast<bool>(session.setExactSelection(
            &scene, {object}, object));
        passed &= static_cast<bool>(
            editor::EditorObjectCoordinator::deleteSelection(scene, session));
        passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                             scene.findGameObject("move-delete-same-object") == object &&
                             object->position.x == 10.0f,
                         "First undo did not restore the moved original object");
        passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                             object->position.x == 0.0f,
                         "Second undo did not undo the transform preceding Delete");
        SceneTestAccess::deactivate(scene);
        if (!passed) return false;
    }

    TestScene scene;
    GameObject* moved = addObject(scene, "history-moved", "Moved");
    GameObject* deleted = addObject(scene, "history-deleted", "Deleted");
    GameObject* scaled = addObject(scene, "history-scaled", "Scaled");
    if (moved == nullptr || deleted == nullptr || scaled == nullptr ||
        !SceneTestAccess::activate(scene)) {
        return expect(false, "Could not prepare shared undo history fixture");
    }
    EditorSession session;
    editor_undo::TransformUndoSnapshot snapshot;
    bool passed = static_cast<bool>(session.undoHistory().capture(
        scene, {moved}, snapshot));
    moved->position = {10.0f, 0.0f, 0.0f};
    passed &= static_cast<bool>(session.undoHistory().commit(scene, snapshot));
    passed &= static_cast<bool>(session.undoHistory().capture(
        scene, {deleted}, snapshot));
    deleted->rotation = {0.0f, 45.0f, 0.0f};
    passed &= static_cast<bool>(session.undoHistory().commit(scene, snapshot));
    if (!session.setExactSelection(&scene, {deleted}, deleted)) return false;
    passed &= static_cast<bool>(
        editor::EditorObjectCoordinator::deleteSelection(scene, session));
    passed &= static_cast<bool>(session.undoHistory().capture(
        scene, {scaled}, snapshot));
    scaled->scale = {2.0f, 2.0f, 2.0f};
    passed &= static_cast<bool>(session.undoHistory().commit(scene, snapshot));
    passed &= expect(session.undoHistory().actionCount() == 4,
                     "Transform and Delete actions did not share one history");

    passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                         scaled->scale == glm::vec3(1.0f) &&
                         scene.findGameObject("history-deleted") == nullptr,
                     "First mixed undo did not undo the newest transform");
    passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                         scene.findGameObject("history-deleted") == deleted &&
                         deleted->position == glm::vec3(0.0f) &&
                         deleted->rotation == glm::vec3(0.0f, 45.0f, 0.0f),
                     "Second mixed undo did not restore the deleted original instance");
    passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                         deleted->rotation == glm::vec3(0.0f),
                     "Earlier transform undo was invalidated by Delete restoration");
    passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                         moved->position == glm::vec3(0.0f),
                     "Mixed undo did not continue in reverse chronological order");
    SceneTestAccess::deactivate(scene);
    return passed;
}

bool runRendererRestoreFailureTests() {
    TestScene scene;
    GameObject* first = addObject(scene, "renderer-retry-first", "RendererRetryFirst");
    GameObject* second = addObject(scene, "renderer-retry-second", "RendererRetrySecond");
    if (first == nullptr || second == nullptr ||
        !GameObjectTestAccess::attach(*first) ||
        !GameObjectTestAccess::attach(*second) ||
        !SceneTestAccess::activate(scene)) {
        return expect(false, "Could not prepare renderer retry fixture");
    }
    EditorSession session;
    if (!session.setExactSelection(&scene, {first, second}, second)) return false;
    const auto detach = [](Scene&, const std::vector<GameObject*>& objects) {
        for (GameObject* candidate : objects) GameObjectTestAccess::detach(*candidate);
        return Result::success();
    };
    bool failSecondAttach = true;
    std::size_t attachCalls = 0;
    const auto attach = [&failSecondAttach, &attachCalls](Scene&,
                                                           GameObject& candidate) {
        ++attachCalls;
        if (failSecondAttach && attachCalls == 2) {
            failSecondAttach = false;
            return Result::failure("injected renderer restoration failure");
        }
        return GameObjectTestAccess::attach(candidate);
    };
    const Result deleteResult = editor::EditorObjectCoordinator::deleteSelection(
        scene, session, detach, attach);
    bool passed = expect(static_cast<bool>(deleteResult) &&
                             scene.findGameObject("renderer-retry-first") == nullptr &&
                             scene.findGameObject("renderer-retry-second") == nullptr &&
                             GameObjectTestAccess::mutableTopology(*first) &&
                             GameObjectTestAccess::mutableTopology(*second),
                         "Renderer resources were not detached for Delete");

    const Result failedUndo = undoLast(session, scene, detach, attach);
    passed &= expect(!failedUndo && session.undoHistory().actionCount() == 1 &&
                         scene.findGameObject("renderer-retry-first") == nullptr &&
                         scene.findGameObject("renderer-retry-second") == nullptr &&
                         first->instanceId() != 0 && second->instanceId() != 0 &&
                         GameObjectTestAccess::mutableTopology(*first) &&
                         GameObjectTestAccess::mutableTopology(*second),
                     "Partial renderer failure did not roll back the whole Delete restoration");
    const Result retryUndo = undoLast(session, scene, detach, attach);
    passed &= expect(static_cast<bool>(retryUndo) && attachCalls == 4 &&
                         scene.findGameObject("renderer-retry-first") == first &&
                         scene.findGameObject("renderer-retry-second") == second &&
                         !GameObjectTestAccess::mutableTopology(*first) &&
                         !GameObjectTestAccess::mutableTopology(*second),
                     "Retry did not restore and attach every original GameObject");
    GameObjectTestAccess::detach(*first);
    GameObjectTestAccess::detach(*second);
    SceneTestAccess::deactivate(scene);
    return passed;
}

bool runDeleteUndoMemoryPreflightTests() {
    TestScene scene;
    GameObject* large = addObject(scene, "too-large-delete", "Large");
    GameObject* transformTarget = addObject(scene, "large-delete-transform", "Other");
    if (large == nullptr || transformTarget == nullptr ||
        !SceneTestAccess::activate(scene)) {
        return expect(false, "Could not prepare Delete memory preflight fixture");
    }
    large->name.assign(70u * 1024u * 1024u, 'L');
    EditorSession session;
    editor_undo::TransformUndoSnapshot snapshot;
    if (!session.undoHistory().capture(scene, {transformTarget}, snapshot)) {
        return false;
    }
    transformTarget->position = {3.0f, 0.0f, 0.0f};
    if (!session.undoHistory().commit(scene, snapshot) ||
        !session.setExactSelection(&scene, {large}, large)) {
        return false;
    }
    const Result deleteResult =
        editor::EditorObjectCoordinator::deleteSelection(scene, session);
    bool passed = expect(!deleteResult &&
                             scene.findGameObject("too-large-delete") == large &&
                             session.isSelected(large) &&
                             session.undoHistory().actionCount() == 1,
                         "Oversized deletion was not rejected before scene mutation");
    passed &= expect(static_cast<bool>(session.undoHistory().undo(scene)) &&
                         transformTarget->position == glm::vec3(0.0f),
                     "Rejected Delete altered older shared undo history");
    SceneTestAccess::deactivate(scene);
    return passed;
}

bool runLightDeleteUndoTests() {
    TestScene scene;
    auto pointOneOwner = std::make_unique<PointLight>();
    pointOneOwner->persistentId = "undo-point-one";
    pointOneOwner->color = {0.2f, 0.4f, 0.8f};
    PointLight* pointOne = pointOneOwner.get();
    GameObject* middle = addObject(scene, "undo-light-middle", "Middle");
    auto pointTwoOwner = std::make_unique<PointLight>();
    pointTwoOwner->persistentId = "undo-point-two";
    PointLight* pointTwo = pointTwoOwner.get();
    auto directionalOwner = std::make_unique<DirectionalLight>();
    directionalOwner->persistentId = "undo-directional";
    DirectionalLight* directional = directionalOwner.get();
    if (!scene.addGameObject(std::move(pointOneOwner)) || middle == nullptr ||
        !scene.addGameObject(std::move(pointTwoOwner)) ||
        !scene.addGameObject(std::move(directionalOwner)) ||
        !SceneTestAccess::activate(scene)) {
        return expect(false, "Could not prepare light Delete undo fixture");
    }

    EditorSession session;
    bool passed = true;
    if (!session.setExactSelection(&scene, {pointOne}, pointOne)) return false;
    passed &= static_cast<bool>(
        editor::EditorObjectCoordinator::deleteSelection(scene, session));
    passed &= expect(scene.pointLightCount() == 1 &&
                         &scene.pointLightAt(0) == pointTwo,
                     "Deleting a PointLight did not update light bookkeeping");
    passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                         scene.pointLightCount() == 2 &&
                         &scene.pointLightAt(0) == pointOne &&
                         &scene.pointLightAt(1) == pointTwo &&
                         pointOne->color == glm::vec3(0.2f, 0.4f, 0.8f),
                     "Delete undo did not restore PointLight identity and ordering");

    if (!session.setExactSelection(&scene, {directional}, directional)) return false;
    passed &= static_cast<bool>(
        editor::EditorObjectCoordinator::deleteSelection(scene, session));
    passed &= expect(scene.directionalLight() == nullptr,
                     "Deleting a DirectionalLight left a registration");
    passed &= expect(static_cast<bool>(undoLast(session, scene)) &&
                         scene.directionalLight() == directional &&
                         scene.findGameObject("undo-directional") == directional,
                     "Delete undo did not restore DirectionalLight registration");
    SceneTestAccess::deactivate(scene);
    return passed;
}

bool runDeleteHistoryLifetimeTests() {
    bool passed = true;
    {
        CountedGameObject::destroyedCount = 0;
        TestScene scene;
        auto countedOwner = std::make_unique<CountedGameObject>();
        countedOwner->persistentId = "clear-delete-history";
        CountedGameObject* counted = countedOwner.get();
        GameObject* survivor = addObject(scene, "clear-delete-survivor", "Survivor");
        if (survivor == nullptr ||
            !scene.addGameObject(std::move(countedOwner)) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare history-clear fixture");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {counted}, counted)) return false;
        passed &= static_cast<bool>(
            editor::EditorObjectCoordinator::deleteSelection(scene, session));
        session.undoHistory().clear();
        passed &= expect(CountedGameObject::destroyedCount == 1 &&
                             scene.findGameObject("clear-delete-survivor") == survivor &&
                             scene.findGameObject("clear-delete-history") == nullptr,
                         "Clearing history did not release only its detached object");
        SceneTestAccess::deactivate(scene);
    }

    {
        CountedGameObject::destroyedCount = 0;
        TestScene scene;
        auto countedOwner = std::make_unique<CountedGameObject>();
        countedOwner->persistentId = "evicted-delete-history";
        CountedGameObject* counted = countedOwner.get();
        GameObject* transformTarget =
            addObject(scene, "evicted-delete-transform", "Transform");
        if (transformTarget == nullptr ||
            !scene.addGameObject(std::move(countedOwner)) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare history-eviction fixture");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {counted}, counted)) return false;
        passed &= static_cast<bool>(
            editor::EditorObjectCoordinator::deleteSelection(scene, session));
        for (std::size_t index = 0; index < maxEditorUndoActions; ++index) {
            editor_undo::TransformUndoSnapshot snapshot;
            passed &= static_cast<bool>(session.undoHistory().capture(
                scene, {transformTarget}, snapshot));
            transformTarget->position.x += 1.0f;
            passed &= static_cast<bool>(session.undoHistory().commit(
                scene, snapshot));
        }
        passed &= expect(session.undoHistory().actionCount() ==
                             maxEditorUndoActions &&
                             CountedGameObject::destroyedCount == 1 &&
                             scene.findGameObject("evicted-delete-history") == nullptr,
                         "Shared 100-action eviction did not release the oldest deleted object");
        for (std::size_t index = 0; index < maxEditorUndoActions; ++index) {
            passed &= static_cast<bool>(session.undoHistory().undo(scene));
        }
        passed &= expect(transformTarget->position == glm::vec3(0.0f) &&
                             scene.findGameObject("evicted-delete-history") == nullptr,
                         "Evicted Delete action reappeared or disturbed newer transforms");
        SceneTestAccess::deactivate(scene);
    }
    return passed;
}

bool runOwnershipPreservingInsertionFailureTests() {
    TestScene scene;
    auto originalOwner = std::make_unique<DirectionalLight>();
    originalOwner->persistentId = "insertion-original-directional";
    DirectionalLight* original = originalOwner.get();
    if (!scene.addGameObject(std::move(originalOwner)) ||
        !SceneTestAccess::activate(scene)) {
        return expect(false, "Could not prepare insertion failure fixture");
    }
    EditorSession session;
    if (!session.setExactSelection(&scene, {original}, original)) return false;
    bool passed = static_cast<bool>(
        editor::EditorObjectCoordinator::deleteSelection(scene, session));

    auto replacementOwner = std::make_unique<DirectionalLight>();
    replacementOwner->persistentId = "insertion-replacement-directional";
    GameObject* replacement = nullptr;
    passed &= static_cast<bool>(SceneTestAccess::addEditor(
        scene, std::move(replacementOwner), replacement));
    const Result failedUndo = undoLast(session, scene);
    passed &= expect(!failedUndo && session.undoHistory().actionCount() == 1 &&
                         scene.findGameObject("insertion-original-directional") == nullptr &&
                         original->instanceId() != 0 &&
                         scene.directionalLight() == replacement,
                     "Failed Scene insertion consumed the retained original object");

    std::unique_ptr<GameObject> removedReplacement;
    passed &= static_cast<bool>(SceneTestAccess::remove(
        scene, replacement, removedReplacement));
    removedReplacement.reset();
    const Result retryUndo = undoLast(session, scene);
    passed &= expect(static_cast<bool>(retryUndo) &&
                         scene.findGameObject("insertion-original-directional") == original &&
                         scene.directionalLight() == original,
                     "Original object could not be restored after insertion became valid");
    SceneTestAccess::deactivate(scene);
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= runLeafAndPromotionTests();
    passed &= runGroupAndFailureTests();
    passed &= runSceneBookkeepingTests();
    passed &= runCameraAndColliderTests();
    passed &= runDeleteUndoIdentityAndPersistenceTests();
    passed &= runSharedChronologicalUndoTests();
    passed &= runRendererRestoreFailureTests();
    passed &= runDeleteUndoMemoryPreflightTests();
    passed &= runLightDeleteUndoTests();
    passed &= runDeleteHistoryLifetimeTests();
    passed &= runOwnershipPreservingInsertionFailureTests();
    return passed ? 0 : 1;
}
