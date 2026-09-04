#include "editor/editor_session.h"
#include "scene/game_object.h"
#include "scene/scene.h"

#include <algorithm>
#include <iostream>
#include <filesystem>
#include <initializer_list>
#include <memory>
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

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

GameObject* addObject(TestScene& scene, const char* persistentId) {
    auto object = std::make_unique<GameObject>();
    object->persistentId = persistentId;
    GameObject* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) {
        return nullptr;
    }
    return pointer;
}

bool hasExactly(const EditorSession& session,
                std::initializer_list<GameObject*> expected) {
    const auto& selected = session.selectedGameObjects();
    if (selected.size() != expected.size()) {
        return false;
    }
    return std::all_of(expected.begin(), expected.end(),
                       [&session](GameObject* object) {
                           return session.isSelected(object);
                       });
}

}  // namespace

int main() {
    bool passed = true;

    passed &= expect(editorToolsEnabled(SceneRunState::Editing),
                     "Editing must enable editor tools");
    passed &= expect(editorToolsEnabled(SceneRunState::Simulating),
                     "Simulating must enable editor tools");
    passed &= expect(!editorToolsEnabled(SceneRunState::Playing),
                     "Playing must disable editor tools");

    passed &= expect(!runtimeSceneRunning(SceneRunState::Editing),
                     "Editing must not run a runtime scene");
    passed &= expect(runtimeSceneRunning(SceneRunState::Simulating),
                     "Simulating must run a runtime scene");
    passed &= expect(runtimeSceneRunning(SceneRunState::Playing),
                     "Playing must run a runtime scene");

    passed &= expect(!usesGameplayCamera(SceneRunState::Editing),
                     "Editing must not use the gameplay camera");
    passed &= expect(!usesGameplayCamera(SceneRunState::Simulating),
                     "Simulating must not use the gameplay camera");
    passed &= expect(usesGameplayCamera(SceneRunState::Playing),
                     "Playing must use the gameplay camera");

    passed &= expect(
        selectionOperationForModifiers(false, false) ==
            SelectionOperation::ReplaceExact,
        "Plain click did not map to exact replacement");
    passed &= expect(
        selectionOperationForModifiers(true, false) ==
            SelectionOperation::ToggleExact,
        "Ctrl-click did not map to exact toggle");
    passed &= expect(
        selectionOperationForModifiers(false, true) ==
            SelectionOperation::ReplaceSubtree,
        "Shift-click did not map to subtree replacement");
    passed &= expect(
        selectionOperationForModifiers(true, true) ==
            SelectionOperation::AddSubtree,
        "Ctrl+Shift-click did not map to additive subtree selection");

    passed &= expect(
        emptyWorldSelectionOperationForModifiers(false, false) ==
            EmptyWorldSelectionOperation::Clear,
        "Plain empty-world click did not clear selection");
    passed &= expect(
        emptyWorldSelectionOperationForModifiers(true, false) ==
            EmptyWorldSelectionOperation::Preserve,
        "Ctrl empty-world click did not preserve selection");
    passed &= expect(
        emptyWorldSelectionOperationForModifiers(false, true) ==
            EmptyWorldSelectionOperation::Clear,
        "Shift empty-world click did not clear selection");
    passed &= expect(
        emptyWorldSelectionOperationForModifiers(true, true) ==
            EmptyWorldSelectionOperation::Preserve,
        "Ctrl+Shift empty-world click did not preserve selection");

    EditorSession session;
    passed &= expect(session.runState() == SceneRunState::Editing,
                     "EditorSession must default to Editing");
    session.setRunState(SceneRunState::Playing);
    passed &= expect(session.runState() == SceneRunState::Playing,
                     "EditorSession did not retain Playing state");
    session.setRunState(SceneRunState::Simulating);
    passed &= expect(session.runState() == SceneRunState::Simulating,
                     "EditorSession did not retain Simulating state");

    passed &= expect(session.transformTool() == TransformTool::Translate,
                     "EditorSession must default to Translate");
    session.setTransformTool(TransformTool::Rotate);
    passed &= expect(session.transformTool() == TransformTool::Rotate,
                     "Rotate transform tool did not round-trip");
    session.setTransformTool(TransformTool::Scale);
    passed &= expect(session.transformTool() == TransformTool::Scale,
                     "Scale transform tool did not round-trip");
    passed &= expect(session.transformSpace() == TransformSpace::World,
                     "EditorSession must default to World transform space");
    session.setTransformSpace(TransformSpace::Local);
    passed &= expect(session.transformSpace() == TransformSpace::Local,
                     "Local transform space did not round-trip");
    session.setTransformTool(TransformTool::Rotate);
    passed &= expect(session.transformSpace() == TransformSpace::Local,
                     "Changing the transform tool reset transform space");
    session.clearSelection();
    passed &= expect(session.transformSpace() == TransformSpace::Local,
                     "Clearing selection reset transform space");
    session.setTransformSpace(TransformSpace::World);

    session.submitEditorAction({EditorCommand::DuplicateGameObject, {}});
    const EditorAction consumedDuplicate = session.consumeEditorAction();
    passed &= expect(
        consumedDuplicate.command == EditorCommand::DuplicateGameObject,
        "Duplicate action was not preserved by the editor command session");

    const std::filesystem::path loadPath =
        std::filesystem::path("scenes") / "exact-load.scene.json";
    session.submitEditorAction({EditorCommand::LoadScene, loadPath});
    passed &= expect(
        session.pendingEditorAction().command == EditorCommand::LoadScene,
        "Submitted load action was not pending");
    passed &= expect(session.pendingEditorAction().path == loadPath,
                     "Load action path changed before consumption");
    const EditorAction consumedLoad = session.consumeEditorAction();
    passed &= expect(consumedLoad.command == EditorCommand::LoadScene,
                     "Load action consumption returned the wrong command");
    passed &= expect(consumedLoad.path == loadPath,
                     "Load action consumption returned the wrong path");
    passed &= expect(
        session.pendingEditorAction().command == EditorCommand::None &&
            session.pendingEditorAction().path.empty(),
        "Consumed load action remained pending");

    const std::filesystem::path saveAsPath =
        std::filesystem::path("scenes") / "exact-save-as.scene.json";
    session.submitEditorAction({EditorCommand::SaveSceneAs, saveAsPath});
    const EditorAction consumedSaveAs = session.consumeEditorAction();
    passed &= expect(consumedSaveAs.command == EditorCommand::SaveSceneAs,
                     "Save As action consumption returned the wrong command");
    passed &= expect(consumedSaveAs.path == saveAsPath,
                     "Save As action path changed during consumption");

    session.setPendingLoadPath(loadPath);
    passed &= expect(session.pendingLoadPath() == loadPath,
                     "Pending load workflow path did not round-trip");
    session.clearPendingLoadPath();
    passed &= expect(session.pendingLoadPath().empty(),
                     "Pending load workflow path did not clear");
    session.setPendingSaveAsPath(saveAsPath);
    passed &= expect(session.pendingSaveAsPath() == saveAsPath,
                     "Pending Save As workflow path did not round-trip");
    session.clearPendingSaveAsPath();
    passed &= expect(session.pendingSaveAsPath().empty(),
                     "Pending Save As workflow path did not clear");
    session.setQuitConfirmationPending(true);
    passed &= expect(session.quitConfirmationPending(),
                     "Quit confirmation state did not set");
    session.setQuitConfirmationPending(false);
    passed &= expect(!session.quitConfirmationPending(),
                     "Quit confirmation state did not clear");

    TestScene scene;
    auto objectOwner = std::make_unique<GameObject>();
    objectOwner->persistentId = "collider-object";
    GameObject* object = objectOwner.get();
    passed &= expect(static_cast<bool>(scene.addGameObject(std::move(objectOwner))),
                     "Could not add the editor-session test object");

    RuntimeTransformEdit edit{object, {1.0f, 2.0f, 3.0f},
                              {4.0f, 5.0f, 6.0f}, true};
    session.submitRuntimeTransformEdit(edit);
    const auto consumedEdit = session.consumeRuntimeTransformEdit();
    passed &= expect(consumedEdit.has_value(),
                     "Submitted runtime transform edit was not pending");
    if (consumedEdit.has_value()) {
        passed &= expect(consumedEdit->object == edit.object,
                         "Runtime edit object changed during handoff");
        passed &= expect(consumedEdit->position == edit.position,
                         "Runtime edit position changed during handoff");
        passed &= expect(consumedEdit->rotation == edit.rotation,
                         "Runtime edit rotation changed during handoff");
        passed &= expect(consumedEdit->manipulating == edit.manipulating,
                         "Runtime edit manipulation flag changed during handoff");
    }
    passed &= expect(!session.consumeRuntimeTransformEdit().has_value(),
                     "Consumed runtime transform edit remained pending");

    session.setRenderColliderIds(
        {"collider-object", "collider-object", "", "other-object"});
    const std::vector<std::string> colliderIds = session.renderColliderIds();
    passed &= expect(colliderIds.size() == 2,
                     "Render collider IDs were not kept unique");
    passed &= expect(session.renderColliderEnabled(*object),
                     "Matching render collider ID was not enabled");
    GameObject otherObject;
    otherObject.persistentId = "unselected-object";
    passed &= expect(!session.renderColliderEnabled(otherObject),
                     "Non-matching render collider ID was enabled");

    TestScene otherScene;
    session.select(&scene, object);
    passed &= expect(session.selectedGameObjectForScene(&scene) == object,
                     "Selection was not set for its scene");
    passed &= expect(session.selectedGameObjectForScene(&otherScene) == nullptr,
                     "Selection leaked into another scene");
    session.synchronizeSelection(&scene);
    passed &= expect(session.selectedGameObjectForScene(&scene) == object,
                     "Synchronizing a compatible scene cleared selection");
    session.synchronizeSelection(&otherScene);
    passed &= expect(session.selectedGameObjectForScene(&otherScene) == nullptr,
                     "Scene switch retained stale selection");
    session.select(&scene, object);
    session.clearSelection();
    passed &= expect(session.selectedGameObjectForScene(&scene) == nullptr,
                     "clearSelection did not clear selection");

    {
        TestScene exactScene;
        GameObject* first = addObject(exactScene, "exact-first");
        GameObject* second = addObject(exactScene, "exact-second");
        passed &= expect(first != nullptr && second != nullptr,
                         "Could not create exact-selection fixture");

        session.applySelection(&exactScene, first,
                               SelectionOperation::ReplaceExact);
        passed &= expect(hasExactly(session, {first}) &&
                             session.activeGameObject() == first,
                         "Exact selection did not select its object");
        session.applySelection(&exactScene, second,
                               SelectionOperation::ReplaceExact);
        passed &= expect(hasExactly(session, {second}) &&
                             session.activeGameObject() == second,
                         "Exact replacement retained an old object");
        session.setTransformSpace(TransformSpace::Local);
        session.applySelection(&exactScene, first,
                               SelectionOperation::ReplaceExact);
        passed &= expect(session.transformSpace() == TransformSpace::Local,
                         "Changing the selection reset transform space");
        session.setTransformSpace(TransformSpace::World);

        session.applySelection(&exactScene, first,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&exactScene, second,
                               SelectionOperation::ToggleExact);
        passed &= expect(hasExactly(session, {first, second}) &&
                             session.activeGameObject() == second,
                         "Exact toggle did not add and activate its object");
        session.applySelection(&exactScene, first,
                               SelectionOperation::ToggleExact);
        passed &= expect(hasExactly(session, {second}) &&
                             session.activeGameObject() == second,
                         "Removing a non-active object changed Active");
        session.applySelection(&exactScene, second,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.selectedGameObjects().empty() &&
                             session.activeGameObject() == nullptr,
                         "Removing the final object did not clear selection");

        session.applySelection(&exactScene, first,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&exactScene, nullptr,
                               SelectionOperation::ToggleExact);
        passed &= expect(hasExactly(session, {first}),
                         "Modified empty-space selection changed membership");
        session.applySelection(&exactScene, nullptr,
                               SelectionOperation::ReplaceExact);
        passed &= expect(session.selectedGameObjects().empty(),
                         "Exact empty-space selection did not clear");
    }

    {
        TestScene subtreeScene;
        GameObject* root = addObject(subtreeScene, "subtree-root");
        GameObject* branch = addObject(subtreeScene, "subtree-branch");
        GameObject* child = addObject(subtreeScene, "subtree-child");
        GameObject* sibling = addObject(subtreeScene, "subtree-sibling");
        passed &= expect(root != nullptr && branch != nullptr &&
                             child != nullptr && sibling != nullptr &&
                             static_cast<bool>(subtreeScene.reparentGameObject(
                                 *branch, root, ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(subtreeScene.reparentGameObject(
                                 *child, branch, ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(subtreeScene.reparentGameObject(
                                 *sibling, branch, ReparentMode::PreserveLocal)),
                         "Could not create subtree-selection fixture");

        session.applySelection(&subtreeScene, root,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&subtreeScene, branch,
                               SelectionOperation::ReplaceSubtree);
        passed &= expect(hasExactly(session, {branch, child, sibling}) &&
                             session.activeGameObject() == branch &&
                             !session.isSelected(root),
                         "Subtree replacement did not select exactly the subtree");

        TestScene deepScene;
        GameObject* deepA = addObject(deepScene, "deep-a");
        GameObject* deepB = addObject(deepScene, "deep-b");
        GameObject* deepC = addObject(deepScene, "deep-c");
        GameObject* deepD = addObject(deepScene, "deep-d");
        GameObject* deepE = addObject(deepScene, "deep-e");
        passed &= expect(deepA != nullptr && deepB != nullptr &&
                             deepC != nullptr && deepD != nullptr &&
                             deepE != nullptr &&
                             static_cast<bool>(deepScene.reparentGameObject(
                                 *deepB, deepA, ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(deepScene.reparentGameObject(
                                 *deepC, deepB, ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(deepScene.reparentGameObject(
                                 *deepD, deepC, ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(deepScene.reparentGameObject(
                                 *deepE, deepD, ReparentMode::PreserveLocal)),
                         "Could not create deep subtree fixture");
        session.applySelection(&deepScene, deepB,
                               SelectionOperation::ReplaceSubtree);
        passed &= expect(hasExactly(session, {deepB, deepC, deepD, deepE}) &&
                             !session.isSelected(deepA) &&
                             session.activeGameObject() == deepB,
                         "Deep subtree traversal stopped before the leaf");

        TestScene additiveScene;
        GameObject* additiveA = addObject(additiveScene, "additive-a");
        GameObject* additiveB = addObject(additiveScene, "additive-b");
        GameObject* additiveC = addObject(additiveScene, "additive-c");
        GameObject* additiveD = addObject(additiveScene, "additive-d");
        passed &= expect(additiveA != nullptr && additiveB != nullptr &&
                             additiveC != nullptr && additiveD != nullptr &&
                             static_cast<bool>(additiveScene.reparentGameObject(
                                 *additiveC, additiveB,
                                 ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(additiveScene.reparentGameObject(
                                 *additiveD, additiveC,
                                 ReparentMode::PreserveLocal)),
                         "Could not create additive-subtree fixture");
        session.applySelection(&additiveScene, additiveA,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&additiveScene, additiveB,
                               SelectionOperation::AddSubtree);
        passed &= expect(hasExactly(session, {additiveA, additiveB,
                                              additiveC, additiveD}) &&
                             session.activeGameObject() == additiveB,
                         "Additive subtree selection was not strictly additive");
    }

    {
        TestScene holeScene;
        GameObject* holeA = addObject(holeScene, "hole-a");
        GameObject* holeB = addObject(holeScene, "hole-b");
        GameObject* holeC = addObject(holeScene, "hole-c");
        GameObject* holeD = addObject(holeScene, "hole-d");
        passed &= expect(holeA != nullptr && holeB != nullptr &&
                             holeC != nullptr && holeD != nullptr &&
                             static_cast<bool>(holeScene.reparentGameObject(
                                 *holeB, holeA, ReparentMode::PreserveLocal)) &&
                             static_cast<bool>(holeScene.reparentGameObject(
                                 *holeC, holeB, ReparentMode::PreserveLocal)),
                         "Could not create selection-hole fixture");

        session.applySelection(&holeScene, holeA,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&holeScene, holeC,
                               SelectionOperation::ToggleExact);
        session.applySelection(&holeScene, holeD,
                               SelectionOperation::ToggleExact);
        passed &= expect(hasExactly(session, {holeA, holeC, holeD}) &&
                             !session.isSelected(holeB),
                         "Selection-hole membership was normalized incorrectly");
        const std::vector<GameObject*> firstRoots =
            session.topLevelSelectedRoots();
        passed &= expect(firstRoots.size() == 2 && firstRoots[0] == holeA &&
                             firstRoots[1] == holeD,
                         "Top-level selected roots were not hierarchy ordered");

        session.applySelection(&holeScene, holeB,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&holeScene, holeC,
                               SelectionOperation::ToggleExact);
        const std::vector<GameObject*> secondRoots =
            session.topLevelSelectedRoots();
        passed &= expect(hasExactly(session, {holeB, holeC}) &&
                             secondRoots.size() == 1 && secondRoots[0] == holeB,
                         "Top-level roots did not collapse through a selected ancestor");
    }

    {
        TestScene recencyScene;
        GameObject* recencyA = addObject(recencyScene, "recency-a");
        GameObject* recencyB = addObject(recencyScene, "recency-b");
        GameObject* recencyC = addObject(recencyScene, "recency-c");
        passed &= expect(recencyA != nullptr && recencyB != nullptr &&
                             recencyC != nullptr,
                         "Could not create Active fallback fixture");
        session.applySelection(&recencyScene, recencyA,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&recencyScene, recencyB,
                               SelectionOperation::ToggleExact);
        session.applySelection(&recencyScene, recencyC,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.activeGameObject() == recencyC,
                         "Positive selection did not update Active");
        session.applySelection(&recencyScene, recencyC,
                               SelectionOperation::ToggleExact);
        passed &= expect(hasExactly(session, {recencyA, recencyB}) &&
                             session.activeGameObject() == recencyB,
                         "Active removal did not fall back by recency");
        session.applySelection(&recencyScene, recencyA,
                               SelectionOperation::ToggleExact);
        passed &= expect(hasExactly(session, {recencyB}) &&
                             session.activeGameObject() == recencyB,
                         "Removing a non-active object changed fallback Active");
        session.applySelection(&recencyScene, recencyB,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.selectedGameObjects().empty() &&
                             session.activeGameObject() == nullptr,
                         "Active fallback did not clear the final selection");
    }

    {
        TestScene transformScene;
        GameObject* transformA = addObject(transformScene, "transform-a");
        GameObject* transformB = addObject(transformScene, "transform-b");
        passed &= expect(transformA != nullptr && transformB != nullptr,
                         "Could not create transform-tool fixture");
        session.applySelection(&transformScene, transformA,
                               SelectionOperation::ReplaceExact);
        session.setTransformTool(TransformTool::Rotate);
        session.applySelection(&transformScene, transformB,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.transformTool() == TransformTool::Translate,
                         "Active change did not reset the transform tool");
        session.setTransformTool(TransformTool::Rotate);
        session.applySelection(&transformScene, transformA,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.activeGameObject() == transformB &&
                             session.transformTool() == TransformTool::Rotate,
                         "Non-active removal did not preserve the transform tool");
        session.applySelection(&transformScene, transformA,
                               SelectionOperation::ToggleExact);
        session.setTransformTool(TransformTool::Rotate);
        session.applySelection(&transformScene, transformA,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.activeGameObject() == transformB &&
                             session.transformTool() == TransformTool::Translate,
                         "Active fallback did not reset the transform tool");
        session.setTransformTool(TransformTool::Rotate);
        session.clearSelection();
        passed &= expect(session.transformTool() == TransformTool::Translate,
                         "Clearing selection did not reset the transform tool");
    }

    {
        TestScene sceneOne;
        TestScene sceneTwo;
        GameObject* sceneOneObject = addObject(sceneOne, "scene-one-object");
        GameObject* sceneTwoObject = addObject(sceneTwo, "scene-two-object");
        passed &= expect(sceneOneObject != nullptr && sceneTwoObject != nullptr,
                         "Could not create foreign-scene fixture");
        session.applySelection(&sceneOne, sceneOneObject,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&sceneOne, sceneTwoObject,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.selectionScene() == &sceneOne &&
                             session.selectedGameObjects().empty(),
                         "Foreign object was accepted into selection");
        session.applySelection(&sceneOne, sceneOneObject,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&sceneTwo, sceneTwoObject,
                               SelectionOperation::ReplaceExact);
        passed &= expect(session.selectionScene() == &sceneTwo &&
                             hasExactly(session, {sceneTwoObject}) &&
                             !session.isSelected(sceneOneObject),
                         "Scene switch retained a cross-scene selection");
        session.synchronizeSelection(&sceneTwo);
        passed &= expect(hasExactly(session, {sceneTwoObject}),
                         "Same-scene synchronization cleared valid selection");
        session.synchronizeSelection(nullptr);
        passed &= expect(session.selectionScene() == nullptr &&
                             session.selectedGameObjects().empty() &&
                             session.activeGameObject() == nullptr,
                         "Null-scene synchronization was not safe");
    }

    {
        TestScene staleScene;
        GameObject* staleSurvivor = addObject(staleScene, "stale-survivor");
        GameObject* staleRemoved = addObject(staleScene, "stale-removed");
        passed &= expect(staleSurvivor != nullptr && staleRemoved != nullptr &&
                             static_cast<bool>(SceneTestAccess::activate(
                                 staleScene)),
                         "Could not activate stale-selection fixture");
        session.applySelection(&staleScene, staleSurvivor,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&staleScene, staleRemoved,
                               SelectionOperation::ToggleExact);
        std::unique_ptr<GameObject> removedObject;
        passed &= expect(static_cast<bool>(SceneTestAccess::remove(
                                 staleScene, staleRemoved, removedObject)),
                         "Could not remove stale-selection fixture object");
        session.synchronizeSelection(&staleScene);
        passed &= expect(hasExactly(session, {staleSurvivor}) &&
                             session.activeGameObject() == staleSurvivor &&
                             session.transformTool() == TransformTool::Translate,
                         "Synchronization did not remove stale Active safely");
        std::unique_ptr<GameObject> removedSurvivor;
        passed &= expect(static_cast<bool>(SceneTestAccess::remove(
                                 staleScene, staleSurvivor, removedSurvivor)),
                         "Could not remove final stale-selection object");
        session.synchronizeSelection(&staleScene);
        passed &= expect(session.selectedGameObjects().empty() &&
                             session.activeGameObject() == nullptr,
                         "Synchronization retained a stale selected pointer");
        SceneTestAccess::deactivate(staleScene);
    }

    {
        TestScene compatibilityScene;
        GameObject* compatibilityA =
            addObject(compatibilityScene, "compatibility-a");
        GameObject* compatibilityB =
            addObject(compatibilityScene, "compatibility-b");
        passed &= expect(compatibilityA != nullptr && compatibilityB != nullptr,
                         "Could not create compatibility getter fixture");
        session.applySelection(&compatibilityScene, compatibilityA,
                               SelectionOperation::ReplaceExact);
        session.applySelection(&compatibilityScene, compatibilityB,
                               SelectionOperation::ToggleExact);
        passed &= expect(session.selectedGameObject() == compatibilityB &&
                             session.selectedGameObjectForScene(
                                 &compatibilityScene) == compatibilityB,
                         "Legacy selection getter did not return Active");
    }

    return passed ? 0 : 1;
}
