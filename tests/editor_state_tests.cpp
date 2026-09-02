#include "editor/editor_session.h"
#include "scene/game_object.h"
#include "scene/scene.h"

#include <iostream>
#include <filesystem>
#include <memory>
#include <vector>

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

    return passed ? 0 : 1;
}
