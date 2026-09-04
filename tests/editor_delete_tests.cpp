#include "editor/editor_duplication.h"
#include "editor/editor_session.h"
#include "scene/camera.h"
#include "scene/directional_light.h"
#include "scene/group.h"
#include "scene/model_renderable.h"
#include "scene/point_light.h"
#include "scene/scene.h"

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
};

class GameObjectTestAccess {
public:
    static Result attach(GameObject& object) {
        return object.modelRenderable().markRenderResourcesAttached();
    }
    static void detach(GameObject& object) {
        object.modelRenderable().markRenderResourcesDetached();
    }
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
                             session.activeGameObject() == second,
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
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        auto ownerObject = std::make_unique<AttachedCameraObject>();
        ownerObject->persistentId = "attached-owner";
        AttachedCameraObject* owner = ownerObject.get();
        if (!scene.addGameObject(std::move(ownerObject)) ||
            !scene.setActiveCameraReference(owner->attachedCamera()) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare attached-camera Delete Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {owner}, owner)) return false;
        const Result result = editor::EditorObjectCoordinator::deleteSelection(
            scene, session);
        passed &= expect(static_cast<bool>(result) && scene.activeCamera() == nullptr,
                         "Deleting an active attached-camera owner left a dangling reference");
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

}  // namespace

int main() {
    bool passed = true;
    passed &= runLeafAndPromotionTests();
    passed &= runGroupAndFailureTests();
    passed &= runSceneBookkeepingTests();
    passed &= runCameraAndColliderTests();
    return passed ? 0 : 1;
}
