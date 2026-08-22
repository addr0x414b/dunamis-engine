#include "editor_session.h"

#include "../scene/game_object.h"
#include "../scene/scene.h"

#include <utility>

SceneRunState EditorSession::runState() const noexcept {
    return runState_;
}

void EditorSession::setRunState(SceneRunState state) noexcept {
    runState_ = state;
}

TransformTool EditorSession::transformTool() const noexcept {
    return transformTool_;
}

void EditorSession::setTransformTool(TransformTool tool) noexcept {
    transformTool_ = tool;
}

void EditorSession::select(Scene* scene, GameObject* object) noexcept {
    if (scene == nullptr) {
        selectionScene_ = nullptr;
        clearSelection();
        return;
    }

    selectionScene_ = scene;
    if (object == nullptr) {
        clearSelection();
        return;
    }

    for (const auto& owner : scene->gameObjects()) {
        if (owner.get() == object) {
            if (selectedGameObject_ != object) {
                transformTool_ = TransformTool::Translate;
            }
            selectedGameObject_ = object;
            return;
        }
    }
    clearSelection();
}

void EditorSession::clearSelection() noexcept {
    selectedGameObject_ = nullptr;
    transformTool_ = TransformTool::Translate;
}

void EditorSession::synchronizeSelection(Scene* scene) noexcept {
    if (scene != selectionScene_) {
        selectionScene_ = scene;
        clearSelection();
    }

    if (scene == nullptr) {
        selectionScene_ = nullptr;
        clearSelection();
        return;
    }

    if (selectedGameObject_ == nullptr) {
        return;
    }

    for (const auto& object : scene->gameObjects()) {
        if (object.get() == selectedGameObject_) {
            return;
        }
    }
    clearSelection();
}

Scene* EditorSession::selectionScene() const noexcept {
    return selectionScene_;
}

GameObject* EditorSession::selectedGameObject() const noexcept {
    return selectedGameObject_;
}

const GameObject* EditorSession::selectedGameObjectForScene(
    const Scene* scene) const noexcept {
    if (scene == nullptr || scene != selectionScene_ ||
        selectedGameObject_ == nullptr) {
        return nullptr;
    }

    for (const auto& object : scene->gameObjects()) {
        if (object.get() == selectedGameObject_) {
            return selectedGameObject_;
        }
    }
    return nullptr;
}

void EditorSession::submitEditorAction(EditorAction action) {
    pendingEditorAction_ = std::move(action);
}

const EditorAction& EditorSession::pendingEditorAction() const noexcept {
    return pendingEditorAction_;
}

EditorAction EditorSession::consumeEditorAction() {
    EditorAction action = std::move(pendingEditorAction_);
    pendingEditorAction_ = {};
    return action;
}

void EditorSession::setPendingLoadPath(std::filesystem::path path) {
    pendingLoadPath_ = std::move(path);
}

const std::filesystem::path& EditorSession::pendingLoadPath() const noexcept {
    return pendingLoadPath_;
}

void EditorSession::clearPendingLoadPath() noexcept {
    pendingLoadPath_.clear();
}

void EditorSession::setPendingSaveAsPath(std::filesystem::path path) {
    pendingSaveAsPath_ = std::move(path);
}

const std::filesystem::path&
EditorSession::pendingSaveAsPath() const noexcept {
    return pendingSaveAsPath_;
}

void EditorSession::clearPendingSaveAsPath() noexcept {
    pendingSaveAsPath_.clear();
}

bool EditorSession::quitConfirmationPending() const noexcept {
    return quitConfirmationPending_;
}

void EditorSession::setQuitConfirmationPending(bool pending) noexcept {
    quitConfirmationPending_ = pending;
}

void EditorSession::submitRuntimeTransformEdit(
    const RuntimeTransformEdit& edit) noexcept {
    pendingRuntimeTransformEdit_ = edit;
}

std::optional<RuntimeTransformEdit>
EditorSession::consumeRuntimeTransformEdit() noexcept {
    std::optional<RuntimeTransformEdit> edit =
        std::move(pendingRuntimeTransformEdit_);
    pendingRuntimeTransformEdit_.reset();
    return edit;
}

void EditorSession::setRenderColliderIds(
    const std::vector<std::string>& ids) {
    renderColliderIds_.clear();
    for (const std::string& id : ids) {
        if (!id.empty()) {
            renderColliderIds_.insert(id);
        }
    }
}

std::vector<std::string> EditorSession::renderColliderIds() const {
    return {renderColliderIds_.begin(), renderColliderIds_.end()};
}

bool EditorSession::renderColliderEnabled(
    const GameObject& object) const noexcept {
    return renderColliderIds_.count(object.persistentId) != 0;
}

void EditorSession::setRenderColliderEnabled(const GameObject& object,
                                             bool enabled) {
    if (object.persistentId.empty()) {
        return;
    }
    if (enabled) {
        renderColliderIds_.insert(object.persistentId);
    } else {
        renderColliderIds_.erase(object.persistentId);
    }
}
