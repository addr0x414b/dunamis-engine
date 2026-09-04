#include "editor/editor_duplication.h"
#include "editor/editor_session.h"
#include "scene/group.h"
#include "scene/model_renderable.h"
#include "scene/scene.h"
#include "scene/scene_serializer.h"
#include "scene/type_registry.h"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

class SceneTestAccess {
public:
    static Result activate(Scene& scene) { return scene.activate(); }
};

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool sameVector(const glm::vec3& left, const glm::vec3& right,
                float epsilon = 1.0e-5f) {
    return std::fabs(left.x - right.x) <= epsilon &&
           std::fabs(left.y - right.y) <= epsilon &&
           std::fabs(left.z - right.z) <= epsilon;
}

bool sameMatrix(const glm::mat4& left, const glm::mat4& right,
                float epsilon = 2.0e-4f) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::fabs(left[column][row] - right[column][row]) > epsilon) {
                return false;
            }
        }
    }
    return true;
}

GameObject* addObject(Scene& scene, const char* id, const char* name = "") {
    auto object = std::make_unique<GameObject>();
    object->persistentId = id;
    object->name = name;
    GameObject* pointer = object.get();
    return scene.addGameObject(std::move(object)) ? pointer : nullptr;
}

Group* addGroup(Scene& scene, const char* id, const char* name) {
    auto group = std::make_unique<Group>();
    group->persistentId = id;
    group->name = name;
    Group* pointer = group.get();
    return scene.addGameObject(std::move(group)) ? pointer : nullptr;
}

bool rootOrder(const Scene& scene,
               std::initializer_list<GameObject*> expected) {
    const auto& roots = scene.rootObjects();
    return roots.size() == expected.size() &&
           std::equal(roots.begin(), roots.end(), expected.begin());
}

bool childOrder(const GameObject& parent,
                std::initializer_list<GameObject*> expected) {
    const auto& children = parent.children();
    return children.size() == expected.size() &&
           std::equal(children.begin(), children.end(), expected.begin());
}

bool hasExactly(const EditorSession& session,
                std::initializer_list<GameObject*> expected) {
    const auto& selected = session.selectedGameObjects();
    if (selected.size() != expected.size()) return false;
    for (GameObject* object : expected) {
        if (!session.isSelected(object)) return false;
    }
    return true;
}

TypeRegistry makeRegistry(bool& passed) {
    TypeRegistry registry;
    passed &= expect(static_cast<bool>(registerEngineTypes(registry)),
                     "engine type registration failed");
    return registry;
}

bool runOrderingTests() {
    TestScene scene;
    GameObject* a = addObject(scene, "order-a", "A");
    GameObject* b = addObject(scene, "order-b", "B");
    GameObject* c = addObject(scene, "order-c", "C");
    GameObject* parent = addObject(scene, "order-parent", "P");
    GameObject* childA = addObject(scene, "order-child-a", "A");
    GameObject* childB = addObject(scene, "order-child-b", "B");
    GameObject* childC = addObject(scene, "order-child-c", "C");
    bool passed = expect(a && b && c && parent && childA && childB && childC,
                         "could not create ordering fixture");
    if (!passed) return false;

    const glm::mat4 rootWorld = c->worldTransformMatrix();
    const glm::mat4 childWorld = childC->worldTransformMatrix();
    passed &= expect(rootOrder(scene, {a, b, c, parent, childA, childB, childC}),
                     "new roots were not appended in ownership-independent root order");
    passed &= expect(static_cast<bool>(scene.reorderGameObject(*c, nullptr, 0)) &&
                         rootOrder(scene, {c, a, b, parent, childA, childB, childC}) &&
                         sameMatrix(c->worldTransformMatrix(), rootWorld),
                     "root reorder changed order or transform incorrectly");

    passed &= expect(static_cast<bool>(scene.reparentGameObject(
                             *childA, parent, Scene::ReparentMode::PreserveLocal)) &&
                         static_cast<bool>(scene.reparentGameObject(
                             *childB, parent, Scene::ReparentMode::PreserveLocal)) &&
                         static_cast<bool>(scene.reparentGameObject(
                             *childC, parent, Scene::ReparentMode::PreserveLocal)) &&
                         childOrder(*parent, {childA, childB, childC}),
                     "child ordering fixture could not be parented");
    passed &= expect(static_cast<bool>(scene.reorderGameObject(*childC, parent, 0)) &&
                         childOrder(*parent, {childC, childA, childB}) &&
                         childC->parent() == parent &&
                         sameMatrix(childC->worldTransformMatrix(), childWorld),
                     "child reorder changed parent or transform incorrectly");
    passed &= expect(!scene.reorderGameObject(*childC, nullptr, 0) &&
                         childOrder(*parent, {childC, childA, childB}) &&
                         childC->parent() == parent,
                     "cross-parent sibling reorder was accepted");
    passed &= expect(static_cast<bool>(scene.validateAuthoredState()),
                     "ordered hierarchy fixture failed Scene validation");
    return passed;
}

bool runPersistenceOrderTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    TestScene source;
    GameObject* a = addObject(source, "persist-a", "A");
    GameObject* b = addObject(source, "persist-b", "B");
    GameObject* c = addObject(source, "persist-c", "C");
    GameObject* parent = addObject(source, "persist-parent", "P");
    GameObject* childA = addObject(source, "persist-child-a", "A");
    GameObject* childB = addObject(source, "persist-child-b", "B");
    GameObject* childC = addObject(source, "persist-child-c", "C");
    passed &= expect(a && b && c && parent && childA && childB && childC,
                     "could not create persistence-order fixture");
    if (!passed) return false;
    passed &= expect(static_cast<bool>(source.reparentGameObject(
                             *childA, parent, Scene::ReparentMode::PreserveLocal)) &&
                         static_cast<bool>(source.reparentGameObject(
                             *childB, parent, Scene::ReparentMode::PreserveLocal)) &&
                         static_cast<bool>(source.reparentGameObject(
                             *childC, parent, Scene::ReparentMode::PreserveLocal)) &&
                         static_cast<bool>(source.reorderGameObject(*c, nullptr, 0)) &&
                         static_cast<bool>(source.reorderGameObject(*childC, parent, 0)),
                     "could not create deliberate persistence order");

    nlohmann::json document;
    Result result = SceneSerializer::serializeAuthored(source, registry, document);
    passed &= expect(static_cast<bool>(result),
                     "ordered hierarchy serialization failed: " + result.error());
    TestScene loaded;
    SceneLoadData loadData;
    result = result ? SceneSerializer::applyDocument(document, loaded, registry,
                                                      loadData)
                    : Result::failure("serialization failed");
    GameObject* loadedParent = loaded.findGameObject("persist-parent");
    passed &= expect(static_cast<bool>(result) && loadedParent &&
                         rootOrder(loaded,
                                   {loaded.findGameObject("persist-c"),
                                    loaded.findGameObject("persist-a"),
                                    loaded.findGameObject("persist-b"),
                                    loadedParent}) &&
                         childOrder(*loadedParent,
                                    {loaded.findGameObject("persist-child-c"),
                                     loaded.findGameObject("persist-child-a"),
                                     loaded.findGameObject("persist-child-b")}),
                     "root/child presentation order did not survive save/load");

    nlohmann::json legacy = document;
    for (auto& record : legacy["objects"]) record.erase("siblingIndex");
    TestScene legacyLoaded;
    SceneLoadData legacyData;
    result = SceneSerializer::applyDocument(legacy, legacyLoaded, registry,
                                            legacyData);
    passed &= expect(static_cast<bool>(result),
                     "scene without hierarchy-order metadata did not load");
    return passed;
}

bool runGroupTypeTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    const TypeDescriptor* groupType = registry.find("Group");
    passed &= expect(groupType && groupType->parentName == "GameObject",
                     "Group was not registered as a GameObject subtype");
    TestScene source;
    Group* group = addGroup(source, "group-id", "Group");
    GameObject* child = addObject(source, "group-child", "Child");
    passed &= expect(group && child &&
                         static_cast<bool>(source.reparentGameObject(
                             *child, group, Scene::ReparentMode::PreserveLocal)) &&
                         group->parent() == nullptr && child->parent() == group &&
                         group->modelRenderable().meshInstances().empty() &&
                         !group->physics.enabled,
                     "Group did not behave as an empty authored hierarchy object");
    nlohmann::json document;
    Result result = SceneSerializer::serializeAuthored(source, registry, document);
    TestScene loaded;
    SceneLoadData loadData;
    result = result ? SceneSerializer::applyDocument(document, loaded, registry,
                                                      loadData)
                    : Result::failure("Group serialization failed");
    passed &= expect(static_cast<bool>(result) &&
                         dynamic_cast<Group*>(loaded.findGameObject("group-id")) !=
                             nullptr,
                     "Group did not round-trip as its dedicated subtype");
    return passed;
}

bool runParentCommandTests() {
    bool passed = true;
    {
        TestScene scene;
        GameObject* a = addObject(scene, "parent-a", "A");
        GameObject* b = addObject(scene, "parent-b", "B");
        GameObject* c = addObject(scene, "parent-c", "C");
        a->position = {10.0f, 1.0f, 0.0f};
        b->position = {-2.0f, 3.0f, 1.0f};
        c->position = {7.0f, -4.0f, 2.0f};
        const glm::mat4 bWorld = b->worldTransformMatrix();
        const glm::mat4 cWorld = c->worldTransformMatrix();
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, b);
        session.select(&scene, c, SelectionOperation::ToggleExact);
        session.select(&scene, a, SelectionOperation::ToggleExact);
        TypeRegistry registry;
        bool registrationPassed = static_cast<bool>(registerEngineTypes(registry));
        (void)registry;
        Result result = editor::EditorObjectCoordinator::parentSelectionToActive(
            scene, session);
        passed &= expect(registrationPassed && static_cast<bool>(result) &&
                             b->parent() == a && c->parent() == a &&
                             childOrder(*a, {b, c}) &&
                             sameMatrix(b->worldTransformMatrix(), bWorld) &&
                             sameMatrix(c->worldTransformMatrix(), cWorld) &&
                             hasExactly(session, {b, c, a}) &&
                             session.activeGameObject() == a,
                         "Ctrl+P basic transaction changed hierarchy, worlds, or selection");
    }

    {
        TestScene scene;
        GameObject* a = addObject(scene, "subtree-a");
        GameObject* b = addObject(scene, "subtree-b");
        GameObject* c = addObject(scene, "subtree-c");
        (void)scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal);
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, b);
        session.select(&scene, c, SelectionOperation::ToggleExact);
        session.select(&scene, a, SelectionOperation::ToggleExact);
        Result result = editor::EditorObjectCoordinator::parentSelectionToActive(
            scene, session);
        passed &= expect(static_cast<bool>(result) && b->parent() == a &&
                             c->parent() == b && childOrder(*a, {b}),
                         "Ctrl+P moved a selected descendant independently");
    }

    {
        TestScene scene;
        GameObject* a = addObject(scene, "cycle-a");
        GameObject* b = addObject(scene, "cycle-b");
        GameObject* c = addObject(scene, "cycle-c");
        (void)scene.reparentGameObject(*b, a, Scene::ReparentMode::PreserveLocal);
        (void)scene.reparentGameObject(*c, b, Scene::ReparentMode::PreserveLocal);
        const glm::mat4 aWorld = a->worldTransformMatrix();
        const std::vector<GameObject*> rootsBefore = scene.rootObjects();
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, a);
        session.select(&scene, c, SelectionOperation::ToggleExact);
        Result result = editor::EditorObjectCoordinator::parentSelectionToActive(
            scene, session);
        passed &= expect(!result && a->parent() == nullptr && b->parent() == a &&
                             c->parent() == b && scene.rootObjects() == rootsBefore &&
                             sameMatrix(a->worldTransformMatrix(), aWorld) &&
                             hasExactly(session, {a, c}) &&
                             session.activeGameObject() == c,
                         "Ctrl+P cycle rejection was not atomic");
    }

    {
        TestScene scene;
        GameObject* oldParent = addObject(scene, "strict-old");
        GameObject* active = addObject(scene, "strict-active");
        GameObject* moving = addObject(scene, "strict-moving");
        oldParent->rotation = {0.0f, 0.0f, 10.0f};
        oldParent->scale = {1.5f, 2.0f, 1.0f};
        active->rotation = {0.0f, 0.0f, 45.0f};
        active->scale = {2.0f, 1.0f, 1.0f};
        moving->position = {1.0f, 2.0f, 3.0f};
        moving->rotation = {10.0f, 20.0f, 30.0f};
        (void)scene.reparentGameObject(
            *moving, oldParent, Scene::ReparentMode::PreserveLocal);
        const std::vector<GameObject*> rootsBefore = scene.rootObjects();
        const glm::vec3 localBefore = moving->position;
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, moving);
        session.select(&scene, active, SelectionOperation::ToggleExact);
        Result result = editor::EditorObjectCoordinator::parentSelectionToActive(
            scene, session);
        passed &= expect(!result && moving->parent() == oldParent &&
                             moving->position == localBefore &&
                             scene.rootObjects() == rootsBefore &&
                             hasExactly(session, {moving, active}) &&
                             session.activeGameObject() == active,
                         "Ctrl+P strict-TRS failure partially mutated the batch");
    }
    return passed;
}

bool runGroupCommandTests() {
    bool passed = true;
    bool registrationPassed = true;
    TypeRegistry registry = makeRegistry(registrationPassed);

    {
        TestScene scene;
        GameObject* a = addObject(scene, "basic-a", "A");
        GameObject* b = addObject(scene, "basic-b", "B");
        GameObject* c = addObject(scene, "basic-c", "C");
        a->position = {0.0f, 0.0f, 0.0f};
        b->position = {10.0f, 0.0f, 0.0f};
        c->position = {2.0f, 3.0f, 4.0f};
        const glm::mat4 aWorld = a->worldTransformMatrix();
        const glm::mat4 bWorld = b->worldTransformMatrix();
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, a);
        session.select(&scene, b, SelectionOperation::ToggleExact);
        GameObject* group = nullptr;
        Result result = editor::EditorObjectCoordinator::groupSelection(
            scene, session, registry, group);
        passed &= expect(registrationPassed && static_cast<bool>(result) && group &&
                             scene.gameObjects().size() == 4 &&
                             group->parent() == nullptr &&
                             childOrder(*group, {a, b}) &&
                             sameVector(glm::vec3(group->worldTransformMatrix()[3]),
                                        {5.0f, 0.0f, 0.0f}) &&
                             sameVector(group->rotation, glm::vec3(0.0f)) &&
                             sameVector(group->scale, glm::vec3(1.0f)) &&
                             sameMatrix(a->worldTransformMatrix(), aWorld) &&
                             sameMatrix(b->worldTransformMatrix(), bWorld) &&
                             c->parent() == nullptr &&
                             hasExactly(session, {a, b, group}) &&
                             session.activeGameObject() == group,
                         "Ctrl+G basic transaction did not preserve roots, worlds, or selection");
    }

    {
        TestScene scene;
        GameObject* parent = addObject(scene, "slot-parent", "Parent");
        GameObject* x = addObject(scene, "slot-x", "X");
        GameObject* a = addObject(scene, "slot-a", "A");
        GameObject* y = addObject(scene, "slot-y", "Y");
        GameObject* otherParent = addObject(scene, "slot-other", "Other");
        GameObject* b = addObject(scene, "slot-b", "B");
        (void)scene.reparentGameObject(
            *x, parent, Scene::ReparentMode::PreserveLocal);
        (void)scene.reparentGameObject(
            *a, parent, Scene::ReparentMode::PreserveLocal);
        (void)scene.reparentGameObject(
            *y, parent, Scene::ReparentMode::PreserveLocal);
        (void)scene.reparentGameObject(
            *b, otherParent, Scene::ReparentMode::PreserveLocal);
        const glm::mat4 aWorld = a->worldTransformMatrix();
        const glm::mat4 bWorld = b->worldTransformMatrix();
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, b);
        session.select(&scene, a, SelectionOperation::ToggleExact);
        GameObject* group = nullptr;
        Result result = editor::EditorObjectCoordinator::groupSelection(
            scene, session, registry, group);
        passed &= expect(static_cast<bool>(result) && group &&
                             group->parent() == parent &&
                             childOrder(*parent, {x, group, y}) &&
                             otherParent->children().empty() &&
                             childOrder(*group, {a, b}) &&
                             sameMatrix(a->worldTransformMatrix(), aWorld) &&
                             sameMatrix(b->worldTransformMatrix(), bWorld),
                         "Ctrl+G did not occupy the Active selected root's structural slot");
    }

    {
        TestScene scene;
        GameObject* a = addObject(scene, "exact-a", "A");
        GameObject* b = addObject(scene, "exact-b", "B");
        GameObject* c = addObject(scene, "exact-c", "C");
        (void)scene.reparentGameObject(
            *c, a, Scene::ReparentMode::PreserveLocal);
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, a);
        session.select(&scene, b, SelectionOperation::ToggleExact);
        GameObject* group = nullptr;
        Result result = editor::EditorObjectCoordinator::groupSelection(
            scene, session, registry, group);
        passed &= expect(static_cast<bool>(result) && group &&
                             hasExactly(session, {a, b, group}) &&
                             !session.isSelected(c) && c->parent() == a,
                         "Ctrl+G expanded selection to an unselected descendant");
    }

    {
        TestScene scene;
        addObject(scene, "name-base", "Group");
        addObject(scene, "name-one", "Group (1)");
        addObject(scene, "name-three", "Group (3)");
        GameObject* a = addObject(scene, "name-a", "A");
        GameObject* b = addObject(scene, "name-b", "B");
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, a);
        session.select(&scene, b, SelectionOperation::ToggleExact);
        GameObject* group = nullptr;
        Result result = editor::EditorObjectCoordinator::groupSelection(
            scene, session, registry, group);
        passed &= expect(static_cast<bool>(result) && group &&
                             group->name == "Group (2)",
                         "Ctrl+G did not choose the smallest deterministic Group suffix");
    }

    {
        TestScene scene;
        GameObject* parent = addObject(scene, "group-strict-parent");
        GameObject* a = addObject(scene, "group-strict-a");
        GameObject* b = addObject(scene, "group-strict-b");
        parent->rotation = {0.0f, 0.0f, 45.0f};
        parent->scale = {2.0f, 1.0f, 1.0f};
        (void)scene.reparentGameObject(
            *a, parent, Scene::ReparentMode::PreserveLocal);
        const std::vector<GameObject*> rootsBefore = scene.rootObjects();
        const std::vector<GameObject*> parentChildrenBefore = parent->children();
        const glm::vec3 aLocal = a->position;
        const glm::vec3 bLocal = b->position;
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, b);
        session.select(&scene, a, SelectionOperation::ToggleExact);
        const std::size_t countBefore = scene.gameObjects().size();
        GameObject* group = nullptr;
        Result result = editor::EditorObjectCoordinator::groupSelection(
            scene, session, registry, group);
        passed &= expect(!result && group == nullptr &&
                             scene.gameObjects().size() == countBefore &&
                             scene.rootObjects() == rootsBefore &&
                             parent->children() == parentChildrenBefore &&
                             a->parent() == parent && b->parent() == nullptr &&
                             a->position == aLocal && b->position == bLocal &&
                             hasExactly(session, {b, a}) &&
                             session.activeGameObject() == a,
                         "Ctrl+G strict-TRS failure left a partial Group transaction");
    }

    {
        TestScene scene;
        GameObject* parent = addObject(scene, "group-child-strict-parent");
        GameObject* a = addObject(scene, "group-child-strict-a");
        GameObject* b = addObject(scene, "group-child-strict-b");
        parent->rotation = {0.0f, 0.0f, 45.0f};
        parent->scale = {2.0f, 1.0f, 1.0f};
        a->rotation = {0.0f, 30.0f, 0.0f};
        (void)scene.reparentGameObject(
            *a, parent, Scene::ReparentMode::PreserveLocal);
        const std::vector<GameObject*> rootsBefore = scene.rootObjects();
        const std::vector<GameObject*> parentChildrenBefore = parent->children();
        const glm::mat4 aWorld = a->worldTransformMatrix();
        SceneTestAccess::activate(scene);
        EditorSession session;
        session.select(&scene, a);
        session.select(&scene, b, SelectionOperation::ToggleExact);
        const std::size_t countBefore = scene.gameObjects().size();
        GameObject* group = nullptr;
        Result result = editor::EditorObjectCoordinator::groupSelection(
            scene, session, registry, group);
        passed &= expect(!result && group == nullptr &&
                             scene.gameObjects().size() == countBefore &&
                             scene.rootObjects() == rootsBefore &&
                             parent->children() == parentChildrenBefore &&
                             a->parent() == parent && b->parent() == nullptr &&
                             sameMatrix(a->worldTransformMatrix(), aWorld) &&
                             hasExactly(session, {a, b}) &&
                             session.activeGameObject() == b,
                         "Ctrl+G child PreserveWorld failure was not atomic");
    }
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= runOrderingTests();
    passed &= runPersistenceOrderTests();
    passed &= runGroupTypeTests();
    passed &= runParentCommandTests();
    passed &= runGroupCommandTests();
    return passed ? 0 : 1;
}
