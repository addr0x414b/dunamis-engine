#include "editor/editor_duplication.h"
#include "editor/editor_session.h"
#include "scene/camera.h"
#include "scene/character.h"
#include "scene/directional_light.h"
#include "scene/group.h"
#include "scene/point_light.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/scene_limits.h"
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
};

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

class AuthoredObject final : public GameObject {
public:
    float authoredValue = 0.0f;
    int runtimeTransferValue = 7;
    int transientValue = 9;
    Camera camera;

    Camera* attachedCamera() noexcept override { return &camera; }
    const Camera* attachedCamera() const noexcept override { return &camera; }
};

class TopologyObject final : public GameObject {
public:
    inline static bool factoryHasCamera = true;

    TopologyObject() {
        if (factoryHasCamera) {
            camera = std::make_unique<Camera>();
        }
    }

    Camera* attachedCamera() noexcept override { return camera.get(); }
    const Camera* attachedCamera() const noexcept override {
        return camera.get();
    }

private:
    std::unique_ptr<Camera> camera;
};

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second) {
    return glm::length(first - second) <= 1.0e-5f;
}

Result registerTestTypes(TypeRegistry& registry) {
    Result result = registerEngineTypes(registry);
    if (!result) return result;
    result = registry.registerType<AuthoredObject>(
        "AuthoredObject", "GameObject",
        [] { return std::make_unique<AuthoredObject>(); });
    if (!result) return result;
    result = registry.registerProperty(
        "AuthoredObject", "authoredValue", &AuthoredObject::authoredValue);
    if (!result) return result;
    result = registry.registerProperty(
        "AuthoredObject", "runtimeTransferValue",
        &AuthoredObject::runtimeTransferValue,
        PropertyLifecycle::RuntimeTransferOnly);
    if (!result) return result;
    result = registry.registerProperty(
        "AuthoredObject", "transientValue", &AuthoredObject::transientValue,
        PropertyLifecycle::Transient);
    if (!result) return result;
    return registry.registerType<TopologyObject>(
        "TopologyObject", "GameObject",
        [] { return std::make_unique<TopologyObject>(); });
}

std::unique_ptr<AuthoredObject> makeAuthoredObject(const char* id,
                                                   const char* name) {
    auto object = std::make_unique<AuthoredObject>();
    object->persistentId = id;
    object->name = name;
    object->position = {1.0f, 2.0f, 3.0f};
    object->rotation = {4.0f, 5.0f, 6.0f};
    object->scale = {2.0f, 3.0f, 4.0f};
    object->physics.enabled = true;
    object->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    object->physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    object->physics.sphereRadius = 2.5f;
    object->authoredValue = 42.0f;
    object->runtimeTransferValue = 84;
    object->transientValue = 126;
    object->camera.position = {10.0f, 11.0f, 12.0f};
    object->camera.front = {0.0f, 0.0f, -1.0f};
    object->camera.up = {0.0f, 1.0f, 0.0f};
    (void)object->camera.setFov(73.0f);
    return object;
}

bool runNameTests() {
    bool passed = true;
    passed &= expect(editor::makeDuplicateName("Pot", {"Pot"}) == "Pot (1)",
                     "Pot did not receive the first duplicate suffix");
    passed &= expect(editor::makeDuplicateName(
                         "Pot", {"Pot", "Pot (1)"}) == "Pot (2)",
                     "Pot did not skip its existing duplicate");
    passed &= expect(editor::makeDuplicateName(
                         "Pot", {"Pot", "Pot (1)", "Pot (3)"}) == "Pot (2)",
                     "Duplicate naming did not choose the smallest available index");
    passed &= expect(editor::makeDuplicateName(
                         "Pot (1)", {"Pot", "Pot (1)"}) == "Pot (2)",
                     "Existing duplicate suffix was not normalized");
    passed &= expect(editor::makeDuplicateName("Room (North)",
                                               {"Room (North)"}) ==
                         "Room (North) (1)",
                     "Parenthesized text was parsed as a duplicate suffix");
    passed &= expect(editor::makeDuplicateName("", {""}).empty(),
                     "Empty names did not remain empty when duplicated");
    return passed;
}

bool runDetachedCopyTests(const TypeRegistry& registry) {
    auto source = makeAuthoredObject("source-id", "Pot");
    std::unique_ptr<GameObject> duplicate;
    Result result = editor::duplicateAuthoredGameObject(
        *source, registry, duplicate);
    bool passed = expect(static_cast<bool>(result),
                         "Authored object duplication failed") &&
                  expect(duplicate != nullptr,
                         "Authored duplication returned no object");
    if (!passed) return false;

    auto* typedDuplicate = dynamic_cast<AuthoredObject*>(duplicate.get());
    passed &= expect(typedDuplicate != nullptr,
                     "Duplication did not preserve the exact concrete type");
    passed &= expect(duplicate.get() != source.get(),
                     "Duplication reused the source object");
    passed &= expect(duplicate->persistentId.empty(),
                     "Detached duplication inherited persistent identity");
    passed &= expect(duplicate->name == source->name &&
                         sameVector(duplicate->position, source->position) &&
                         sameVector(duplicate->rotation, source->rotation) &&
                         sameVector(duplicate->scale, source->scale),
                     "Inherited authored properties were not copied");
    passed &= expect(typedDuplicate->authoredValue == source->authoredValue &&
                         typedDuplicate->physics.enabled == source->physics.enabled &&
                         typedDuplicate->physics.motionType ==
                             source->physics.motionType &&
                         typedDuplicate->physics.colliderType ==
                             source->physics.colliderType &&
                         typedDuplicate->physics.sphereRadius ==
                             source->physics.sphereRadius,
                     "Authored custom or physics properties were not copied");
    passed &= expect(typedDuplicate->runtimeTransferValue == 7 &&
                         typedDuplicate->transientValue == 9,
                     "Transient or runtime-transfer state was copied as authored state");
    passed &= expect(typedDuplicate->attachedCamera() != source->attachedCamera(),
                     "Attached Camera instance was shared");
    passed &= expect(sameVector(typedDuplicate->camera.position,
                                source->camera.position) &&
                         sameVector(typedDuplicate->camera.front,
                                    source->camera.front) &&
                         sameVector(typedDuplicate->camera.up,
                                    source->camera.up) &&
                         typedDuplicate->camera.fov() == source->camera.fov(),
                     "Attached Camera authored state was not copied");

    source->authoredValue = -1.0f;
    source->camera.position.x = -2.0f;
    typedDuplicate->authoredValue = 3.0f;
    typedDuplicate->camera.position.y = 4.0f;
    passed &= expect(typedDuplicate->authoredValue != source->authoredValue &&
                         typedDuplicate->camera.position.x !=
                             source->camera.position.x &&
                         typedDuplicate->camera.position.y !=
                             source->camera.position.y,
                     "Source and duplicate share mutable authored state");

    Character character;
    character.capsuleHeight = 8.0f;
    character.capsuleRadius = 1.25f;
    duplicate.reset();
    result = editor::duplicateAuthoredGameObject(character, registry, duplicate);
    auto* characterDuplicate =
        duplicate ? dynamic_cast<Character*>(duplicate.get()) : nullptr;
    passed &= expect(static_cast<bool>(result) && characterDuplicate != nullptr &&
                         characterDuplicate->capsuleHeight == 8.0f &&
                         characterDuplicate->capsuleRadius == 1.25f,
                     "Inherited Character authored dimensions were not copied");

    Camera camera;
    camera.position = {2.0f, 3.0f, 4.0f};
    camera.front = {0.0f, 0.0f, -1.0f};
    (void)camera.setFov(81.0f);
    duplicate.reset();
    result = editor::duplicateAuthoredGameObject(camera, registry, duplicate);
    auto* cameraDuplicate = duplicate ? dynamic_cast<Camera*>(duplicate.get())
                                       : nullptr;
    passed &= expect(static_cast<bool>(result) && cameraDuplicate != nullptr &&
                         sameVector(cameraDuplicate->position, camera.position) &&
                         cameraDuplicate->fov() == 81.0f,
                     "Standalone Camera authored state was not copied");

    PointLight point;
    point.color = {0.2f, 0.4f, 0.6f};
    point.intensity = 9.0f;
    duplicate.reset();
    result = editor::duplicateAuthoredGameObject(point, registry, duplicate);
    auto* pointDuplicate = duplicate ? dynamic_cast<PointLight*>(duplicate.get())
                                      : nullptr;
    passed &= expect(static_cast<bool>(result) && pointDuplicate != nullptr &&
                         pointDuplicate->color == point.color &&
                         pointDuplicate->intensity == point.intensity,
                     "PointLight authored fields were not copied");

    DirectionalLight directional;
    directional.color = {0.7f, 0.8f, 0.9f};
    directional.intensity = 4.0f;
    directional.shadow.focus = {1.0f, 2.0f, 3.0f};
    directional.shadow.halfExtent = 321.0f;
    duplicate.reset();
    result = editor::duplicateAuthoredGameObject(
        directional, registry, duplicate);
    auto* directionalDuplicate =
        duplicate ? dynamic_cast<DirectionalLight*>(duplicate.get()) : nullptr;
    passed &= expect(static_cast<bool>(result) && directionalDuplicate != nullptr &&
                         directionalDuplicate->color == directional.color &&
                         directionalDuplicate->intensity == directional.intensity &&
                         directionalDuplicate->shadow.focus ==
                             directional.shadow.focus &&
                         directionalDuplicate->shadow.halfExtent ==
                             directional.shadow.halfExtent,
                     "DirectionalLight authored fields were not copied");
    return passed;
}

bool runAttachedTopologyTest(const TypeRegistry& registry) {
    TopologyObject::factoryHasCamera = true;
    TopologyObject source;
    const bool sourceHasCamera = source.attachedCamera() != nullptr;
    TopologyObject::factoryHasCamera = false;
    std::unique_ptr<GameObject> duplicate;
    const Result result = editor::duplicateAuthoredGameObject(
        source, registry, duplicate);
    TopologyObject::factoryHasCamera = true;
    return expect(sourceHasCamera && !result && duplicate == nullptr,
                  "Attached-camera topology mismatch did not fail cleanly");
}

bool runSceneTransactionTests(const TypeRegistry& registry) {
    TestScene scene;
    auto source = makeAuthoredObject("source-id", "Pot");
    AuthoredObject* sourcePointer = source.get();
    if (!scene.addGameObject(std::move(source))) {
        return expect(false, "Could not create duplication source object");
    }
    if (!scene.setActiveCameraReference(&sourcePointer->camera)) {
        return expect(false, "Could not set the source attached Camera active");
    }
    if (!SceneTestAccess::activate(scene)) {
        return expect(false, "Could not activate the editor test Scene");
    }

    bool passed = true;
    const std::size_t initialCount = scene.gameObjects().size();
    passed &= expect(!scene.addGameObject(std::make_unique<GameObject>()),
                     "Ordinary active-scene insertion was not rejected");

    EditorSession session;
    session.select(&scene, sourcePointer);
    GameObject* duplicate = nullptr;
    bool rendererCalled = false;
    Result result = editor::EditorObjectCoordinator::duplicateIntoScene(
        scene, *sourcePointer, registry,
        [&rendererCalled](Scene&, GameObject&) {
            rendererCalled = true;
            return Result::success();
        },
        duplicate);
    passed &= expect(static_cast<bool>(result) && rendererCalled && duplicate != nullptr,
                     "Controlled active-scene duplication failed");
    passed &= expect(scene.gameObjects().size() == initialCount + 1,
                     "Successful duplication changed Scene topology incorrectly");
    passed &= expect(duplicate->persistentId != sourcePointer->persistentId &&
                         !duplicate->persistentId.empty() &&
                         duplicate->name == "Pot (1)",
                     "Successful duplication did not assign a fresh identity/name");
    passed &= expect(scene.activeCamera() == &sourcePointer->camera,
                     "Duplicating the active-camera owner changed active camera");
    session.select(&scene, duplicate);
    passed &= expect(session.selectedGameObject() == duplicate,
                     "Successful duplication did not support selecting the result");

    const std::size_t countBeforeFailure = scene.gameObjects().size();
    GameObject* failedDuplicate = nullptr;
    result = editor::EditorObjectCoordinator::duplicateIntoScene(
        scene, *sourcePointer, registry,
        [](Scene&, GameObject&) {
            return Result::failure("injected renderer failure");
        },
        failedDuplicate);
    passed &= expect(!result && failedDuplicate == nullptr &&
                         scene.gameObjects().size() == countBeforeFailure &&
                         session.selectedGameObject() == duplicate,
                     "Failed duplication did not roll back topology/selection");

    SceneTestAccess::deactivate(scene);

    TestScene directionalScene;
    auto directional = std::make_unique<DirectionalLight>();
    DirectionalLight* directionalPointer = directional.get();
    if (!directionalScene.addGameObject(std::move(directional)) ||
        !SceneTestAccess::activate(directionalScene)) {
        return expect(false, "Could not prepare directional-light test Scene");
    }
    const std::size_t directionalCount = directionalScene.gameObjects().size();
    bool directionalRendererCalled = false;
    result = editor::EditorObjectCoordinator::duplicateIntoScene(
        directionalScene, *directionalPointer, registry,
        [&directionalRendererCalled](Scene&, GameObject&) {
            directionalRendererCalled = true;
            return Result::success();
        },
        duplicate);
    passed &= expect(!result && !directionalRendererCalled &&
                         directionalScene.gameObjects().size() == directionalCount &&
                         directionalScene.directionalLight() == directionalPointer,
                     "Directional-light uniqueness was bypassed by duplication");
    SceneTestAccess::deactivate(directionalScene);

    TestScene pointScene;
    for (std::size_t index = 0; index < scene_limits::maxPointLights; ++index) {
        if (!pointScene.addGameObject(std::make_unique<PointLight>())) {
            return expect(false, "Could not fill point-light test Scene");
        }
    }
    PointLight* pointSource =
        dynamic_cast<PointLight*>(pointScene.gameObjects().front().get());
    if (!pointSource || !SceneTestAccess::activate(pointScene)) {
        return expect(false, "Could not activate point-light test Scene");
    }
    const std::size_t pointCount = pointScene.gameObjects().size();
    result = editor::EditorObjectCoordinator::duplicateIntoScene(
        pointScene, *pointSource, registry,
        [](Scene&, GameObject&) { return Result::success(); }, duplicate);
    passed &= expect(!result && pointScene.gameObjects().size() == pointCount &&
                         pointScene.pointLightCount() == scene_limits::maxPointLights,
                     "Point-light capacity was bypassed by duplication");
    SceneTestAccess::deactivate(pointScene);

    TestScene idScene;
    auto first = std::make_unique<GameObject>();
    first->persistentId = "same-id";
    passed &= expect(static_cast<bool>(idScene.addGameObject(std::move(first))),
                     "Could not create explicit-ID test object");
    auto second = std::make_unique<GameObject>();
    second->persistentId = "same-id";
    passed &= expect(!idScene.addGameObject(std::move(second)) &&
                         idScene.gameObjects().size() == 1,
                     "Duplicate explicit persistent IDs were accepted");
    return passed;
}

bool runPersistenceTest(TypeRegistry& registry) {
    TestScene sourceScene;
    auto source = makeAuthoredObject("source-id", "Pot");
    AuthoredObject* sourcePointer = source.get();
    if (!sourceScene.addGameObject(std::move(source)) ||
        !sourceScene.setActiveCameraReference(&sourcePointer->camera) ||
        !SceneTestAccess::activate(sourceScene)) {
        return expect(false, "Could not prepare persistence duplication Scene");
    }
    GameObject* duplicate = nullptr;
    const Result duplicateResult =
        editor::EditorObjectCoordinator::duplicateIntoScene(
            sourceScene, *sourcePointer, registry,
            [](Scene&, GameObject&) { return Result::success(); }, duplicate);
    if (!duplicateResult) {
        return expect(false, "Could not duplicate persistence test object");
    }

    nlohmann::json document;
    Result result = SceneSerializer::serializeAuthored(
        sourceScene, registry, document);
    if (!result) return expect(false, "Could not serialize duplicated object");

    TestScene loadedScene;
    SceneLoadData loadData;
    result = SceneSerializer::applyDocument(document, loadedScene, registry,
                                            loadData);
    bool passed = expect(static_cast<bool>(result),
                         "Could not reload duplicated authored object");
    if (passed) {
        passed &= expect(loadedScene.gameObjects().size() == 2,
                         "Reloaded Scene lost the duplicated object");
        const GameObject* loadedSource = loadedScene.findGameObject("source-id");
        const GameObject* loadedDuplicate =
            duplicate ? loadedScene.findGameObject(duplicate->persistentId) : nullptr;
        passed &= expect(loadedSource != nullptr && loadedDuplicate != nullptr &&
                             loadedSource->persistentId !=
                                 loadedDuplicate->persistentId &&
                             loadedDuplicate->name == "Pot (1)",
                         "Reloaded duplicate did not retain identity/name");
        const auto* typedLoadedDuplicate =
            dynamic_cast<const AuthoredObject*>(loadedDuplicate);
        passed &= expect(typedLoadedDuplicate != nullptr &&
                             typedLoadedDuplicate->authoredValue == 42.0f &&
                             typedLoadedDuplicate->physics.enabled &&
                             typedLoadedDuplicate->camera.fov() == 73.0f,
                         "Reloaded duplicate did not retain authored state");
        passed &= expect(loadedScene.activeCamera() ==
                             loadedSource->attachedCamera(),
                         "Reloaded active attached Camera reference changed");
    }
    SceneTestAccess::deactivate(sourceScene);
    return passed;
}

GameObject* addBasicObject(TestScene& scene, const char* id,
                           const char* name) {
    auto object = std::make_unique<GameObject>();
    object->persistentId = id;
    object->name = name;
    object->position = {1.0f, 2.0f, 3.0f};
    GameObject* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) return nullptr;
    return pointer;
}

bool runBatchSelectionTests(const TypeRegistry& registry) {
    bool passed = true;

    {
        TestScene scene;
        GameObject* a = addBasicObject(scene, "a", "Pot");
        GameObject* b = addBasicObject(scene, "b", "Pot");
        GameObject* c = addBasicObject(scene, "c", "Other");
        passed &= expect(a != nullptr && b != nullptr && c != nullptr,
                         "Could not create multi-root duplicate test Scene");
        if (a == nullptr || b == nullptr || c == nullptr) return false;
        a->position.x = 4.0f;
        b->position.x = 8.0f;
        if (!SceneTestAccess::activate(scene)) return false;

        EditorSession session;
        passed &= expect(static_cast<bool>(session.setExactSelection(
                             &scene, {a, b}, b)),
                         "Could not select the multi-root duplicate sources");
        session.setTransformTool(TransformTool::Rotate);
        Result result =
            editor::EditorObjectCoordinator::duplicateSelectionIntoScene(
                scene, session, registry,
                [](Scene&, GameObject&) { return Result::success(); });
        passed &= expect(static_cast<bool>(result),
                         "Multi-root batch duplication failed");
        const auto& roots = scene.rootObjects();
        passed &= expect(roots.size() == 5 && roots[0] == a && roots[2] == b &&
                             roots[4] == c,
                         "Multi-root duplicate order was not deterministic");
        GameObject* aCopy = roots.size() > 1 ? roots[1] : nullptr;
        GameObject* bCopy = roots.size() > 3 ? roots[3] : nullptr;
        passed &= expect(aCopy != nullptr && bCopy != nullptr &&
                             aCopy->name == "Pot (1)" &&
                             bCopy->name == "Pot (2)" &&
                             aCopy->persistentId != a->persistentId &&
                             bCopy->persistentId != b->persistentId &&
                             aCopy->position == a->position &&
                             bCopy->position == b->position,
                         "Batch duplicates did not copy fresh authored state");
        passed &= expect(session.isSelected(aCopy) && session.isSelected(bCopy) &&
                             !session.isSelected(a) && !session.isSelected(b) &&
                             session.activeGameObject() == bCopy &&
                             session.transformTool() == TransformTool::Translate,
                         "Batch duplicate selection/Active policy was incorrect");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* parent = addBasicObject(scene, "parent", "Parent");
        GameObject* child = addBasicObject(scene, "child", "Child");
        GameObject* gap = addBasicObject(scene, "gap", "Gap");
        passed &= expect(parent != nullptr && child != nullptr && gap != nullptr,
                         "Could not create selected-parent duplicate test Scene");
        if (parent == nullptr || child == nullptr || gap == nullptr) return false;
        child->position = {2.0f, 3.0f, 4.0f};
        gap->position = {5.0f, 6.0f, 7.0f};
        if (!scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*gap, child,
                                      Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare hierarchy-hole duplicate Scene");
        }

        EditorSession session;
        passed &= expect(static_cast<bool>(session.setExactSelection(
                             &scene, {parent, gap}, gap)),
                         "Could not select hierarchy-hole duplicate sources");
        Result result =
            editor::EditorObjectCoordinator::duplicateSelectionIntoScene(
                scene, session, registry,
                [](Scene&, GameObject&) { return Result::success(); });
        passed &= expect(static_cast<bool>(result),
                         "Hierarchy-hole batch duplication failed");
        GameObject* parentCopy = nullptr;
        GameObject* gapCopy = nullptr;
        for (GameObject* object : session.selectedGameObjects()) {
            if (object->name == "Parent (1)") parentCopy = object;
            if (object->name == "Gap (1)") gapCopy = object;
        }
        passed &= expect(parentCopy != nullptr && gapCopy != nullptr &&
                             parentCopy->parent() == nullptr &&
                             gapCopy->parent() == child &&
                             gapCopy != parentCopy &&
                             std::find(child->children().begin(),
                                       child->children().end(), gapCopy) !=
                                 child->children().end() &&
                             std::find(parentCopy->children().begin(),
                                       parentCopy->children().end(), gapCopy) ==
                                 parentCopy->children().end(),
                         "Selected-parent mapping crossed an unselected hierarchy gap");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* parent = addBasicObject(scene, "hole-parent", "Parent");
        GameObject* firstGap = addBasicObject(scene, "first-gap", "First");
        GameObject* firstSelected =
            addBasicObject(scene, "first-selected", "Selected A");
        GameObject* secondGap = addBasicObject(scene, "second-gap", "Second");
        GameObject* secondSelected =
            addBasicObject(scene, "second-selected", "Selected B");
        if (parent == nullptr || firstGap == nullptr || firstSelected == nullptr ||
            secondGap == nullptr || secondSelected == nullptr ||
            !scene.reparentGameObject(*firstGap, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*firstSelected, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*secondGap, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*secondSelected, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare duplicate-hole child Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene,
                                       {parent, firstSelected, secondSelected},
                                       secondSelected)) {
            return expect(false, "Could not select duplicate-hole children");
        }
        const Result result =
            editor::EditorObjectCoordinator::duplicateSelectionIntoScene(
                scene, session, registry,
                [](Scene&, GameObject&) { return Result::success(); });
        passed &= expect(static_cast<bool>(result),
                         "Selected children with gaps could not be duplicated");
        GameObject* parentCopy = nullptr;
        for (GameObject* object : session.selectedGameObjects()) {
            if (object->name == "Parent (1)") parentCopy = object;
        }
        passed &= expect(parentCopy != nullptr && parentCopy->children().size() == 2 &&
                             parentCopy->children()[0]->name == "Selected A (1)" &&
                             parentCopy->children()[1]->name == "Selected B (1)" &&
                             parentCopy->children()[0]->parent() == parentCopy &&
                             parentCopy->children()[1]->parent() == parentCopy,
                         "Duplicate child order did not close unselected gaps");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        auto groupObject = std::make_unique<Group>();
        groupObject->persistentId = "duplicate-group";
        groupObject->name = "Group";
        Group* group = groupObject.get();
        auto childObject = std::make_unique<GameObject>();
        childObject->persistentId = "duplicate-group-child";
        childObject->name = "Child";
        GameObject* child = childObject.get();
        if (!scene.addGameObject(std::move(groupObject)) ||
            !scene.addGameObject(std::move(childObject)) ||
            !scene.reparentGameObject(*child, group,
                                      Scene::ReparentMode::PreserveLocal) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare Group duplicate Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {group, child}, child)) return false;
        const Result result =
            editor::EditorObjectCoordinator::duplicateSelectionIntoScene(
                scene, session, registry,
                [](Scene&, GameObject&) { return Result::success(); });
        Group* groupCopy = nullptr;
        GameObject* childCopy = nullptr;
        for (GameObject* object : session.selectedGameObjects()) {
            if (object->name == "Group (1)") {
                groupCopy = dynamic_cast<Group*>(object);
            } else if (object->name == "Child (1)") {
                childCopy = object;
            }
        }
        passed &= expect(static_cast<bool>(result) && groupCopy != nullptr &&
                             childCopy != nullptr && childCopy->parent() == groupCopy,
                         "Batch duplication did not preserve Group concrete type/topology");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        GameObject* first = addBasicObject(scene, "atomic-a", "A");
        GameObject* second = addBasicObject(scene, "atomic-b", "B");
        if (first == nullptr || second == nullptr ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare atomic batch duplicate Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {first, second}, second)) {
            return expect(false, "Could not select atomic batch sources");
        }
        const std::size_t initialCount = scene.gameObjects().size();
        const std::vector<GameObject*> initialRoots = scene.rootObjects();
        std::size_t attachCalls = 0;
        std::size_t detachCalls = 0;
        const Result result =
            editor::EditorObjectCoordinator::duplicateSelectionIntoScene(
                scene, session, registry,
                [&attachCalls](Scene&, GameObject&) {
                    ++attachCalls;
                    return attachCalls == 1
                               ? Result::success()
                               : Result::failure("injected batch failure");
                },
                [&detachCalls](Scene&, const std::vector<GameObject*>& objects) {
                    ++detachCalls;
                    return objects.size() == 1 ? Result::success()
                                               : Result::failure("bad rollback set");
                });
        passed &= expect(!result && scene.gameObjects().size() == initialCount &&
                             scene.rootObjects() == initialRoots &&
                             session.selectedGameObjects().size() == 2 &&
                             session.isSelected(first) && session.isSelected(second) &&
                             session.activeGameObject() == second &&
                             attachCalls == 2 && detachCalls == 1,
                         "Failed duplicate batch did not roll back atomically");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        auto camera = std::make_unique<Camera>();
        camera->persistentId = "active-camera";
        camera->name = "Camera";
        camera->front = {0.0f, 0.0f, -1.0f};
        (void)camera->setFov(79.0f);
        Camera* cameraPointer = camera.get();
        if (!scene.addGameObject(std::move(camera)) ||
            !scene.setActiveCameraReference(cameraPointer) ||
            !SceneTestAccess::activate(scene)) {
            return expect(false, "Could not prepare active-camera duplicate Scene");
        }
        EditorSession session;
        if (!session.setExactSelection(&scene, {cameraPointer}, cameraPointer)) {
            return expect(false, "Could not select active Camera for duplication");
        }
        const Result result =
            editor::EditorObjectCoordinator::duplicateSelectionIntoScene(
                scene, session, registry,
                [](Scene&, GameObject&) { return Result::success(); });
        Camera* duplicate = nullptr;
        for (GameObject* object : session.selectedGameObjects()) {
            duplicate = dynamic_cast<Camera*>(object);
        }
        passed &= expect(static_cast<bool>(result) && duplicate != nullptr &&
                             duplicate != cameraPointer &&
                             duplicate->fov() == cameraPointer->fov() &&
                             scene.activeCamera() == cameraPointer,
                         "Active Camera duplication changed camera bookkeeping");
        SceneTestAccess::deactivate(scene);
    }

    {
        TestScene scene;
        if (!SceneTestAccess::activate(scene)) return false;
        EditorSession session;
        std::size_t attachCalls = 0;
        const Result result =
            editor::EditorObjectCoordinator::duplicateSelectionIntoScene(
                scene, session, registry,
                [&attachCalls](Scene&, GameObject&) {
                    ++attachCalls;
                    return Result::success();
                });
        passed &= expect(static_cast<bool>(result) && attachCalls == 0 &&
                             scene.gameObjects().empty(),
                         "No-selection Ctrl+D was not a silent no-op");
        SceneTestAccess::deactivate(scene);
    }

    return passed;
}

}  // namespace

int main() {
    TypeRegistry registry;
    const Result registration = registerTestTypes(registry);
    if (!registration) {
        std::cerr << "Type registration failed: " << registration.error() << '\n';
        return 1;
    }

    bool passed = true;
    passed &= runNameTests();
    passed &= runDetachedCopyTests(registry);
    passed &= runAttachedTopologyTest(registry);
    passed &= runSceneTransactionTests(registry);
    passed &= runPersistenceTest(registry);
    passed &= runBatchSelectionTests(registry);
    return passed ? 0 : 1;
}
