#include "editor_session.h"

#include "../scene/game_object.h"
#include "../scene/scene.h"

#include <algorithm>
#include <unordered_set>
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

void EditorSession::select(Scene* scene, GameObject* object) {
    applySelection(scene, object, SelectionOperation::ReplaceExact);
}

void EditorSession::select(Scene* scene, GameObject* object,
                           SelectionOperation operation) {
    applySelection(scene, object, operation);
}

void EditorSession::applySelection(Scene* scene, GameObject* object,
                                   SelectionOperation operation) {
    if (scene == nullptr) {
        selectionScene_ = nullptr;
        clearSelection();
        return;
    }

    if (scene != selectionScene_) {
        selectionScene_ = scene;
        clearSelection();
    }

    // A null target represents an empty-space click. Only an unmodified
    // click replaces the selection; modified empty-space clicks are no-ops.
    if (object == nullptr) {
        if (operation == SelectionOperation::ReplaceExact) {
            clearSelection();
        }
        return;
    }

    if (!ownsGameObject(scene, object)) {
        clearSelection();
        return;
    }

    const GameObject* previousActive = activeGameObject_;
    switch (operation) {
    case SelectionOperation::ReplaceExact:
        selectedGameObjects_.reserve(1);
        selectionRecency_.reserve(1);
        selectedGameObjects_.clear();
        selectionRecency_.clear();
        addSelectedGameObject(object);
        activeGameObject_ = object;
        break;
    case SelectionOperation::ToggleExact:
        if (selectedGameObjects_.count(object) != 0) {
            removeSelectedGameObject(object);
            if (activeGameObject_ == object) {
                activeGameObject_ = mostRecentlySelected();
            }
        } else {
            selectedGameObjects_.reserve(selectedGameObjects_.size() + 1);
            selectionRecency_.reserve(selectionRecency_.size() + 1);
            addSelectedGameObject(object);
            activeGameObject_ = object;
        }
        break;
    case SelectionOperation::ReplaceSubtree: {
        const std::vector<GameObject*> subtree = collectSubtree(object);
        selectedGameObjects_.reserve(subtree.size());
        selectionRecency_.reserve(subtree.size());
        selectedGameObjects_.clear();
        selectionRecency_.clear();
        for (GameObject* descendant : subtree) {
            addSelectedGameObject(descendant);
        }
        // The clicked node is the positive selection that establishes the
        // Active Object, even when the operation selected its descendants.
        makeMostRecentlySelected(object);
        activeGameObject_ = object;
        break;
    }
    case SelectionOperation::AddSubtree: {
        const std::vector<GameObject*> subtree = collectSubtree(object);
        selectedGameObjects_.reserve(selectedGameObjects_.size() +
                                     subtree.size());
        selectionRecency_.reserve(selectionRecency_.size() + subtree.size());
        for (GameObject* descendant : subtree) {
            addSelectedGameObject(descendant);
        }
        makeMostRecentlySelected(object);
        activeGameObject_ = object;
        break;
    }
    }

    if (selectedGameObjects_.empty()) {
        activeGameObject_ = nullptr;
    }
    if (activeGameObject_ != previousActive) {
        transformTool_ = TransformTool::Translate;
    }
}

void EditorSession::clearSelection() noexcept {
    selectedGameObjects_.clear();
    selectionRecency_.clear();
    activeGameObject_ = nullptr;
    transformTool_ = TransformTool::Translate;
}

void EditorSession::synchronizeSelection(Scene* scene) {
    if (scene != selectionScene_) {
        selectionScene_ = scene;
        clearSelection();
    }

    if (scene == nullptr) {
        selectionScene_ = nullptr;
        clearSelection();
        return;
    }

    if (selectedGameObjects_.empty()) {
        selectionRecency_.clear();
        if (activeGameObject_ != nullptr) {
            activeGameObject_ = nullptr;
            transformTool_ = TransformTool::Translate;
        }
        return;
    }

    std::unordered_set<const GameObject*> ownedObjects;
    ownedObjects.reserve(scene->gameObjects().size());
    for (const auto& object : scene->gameObjects()) {
        if (object != nullptr) {
            ownedObjects.insert(object.get());
        }
    }

    for (auto iterator = selectedGameObjects_.begin();
         iterator != selectedGameObjects_.end();) {
        if (ownedObjects.count(*iterator) == 0) {
            iterator = selectedGameObjects_.erase(iterator);
        } else {
            ++iterator;
        }
    }

    selectionRecency_.erase(
        std::remove_if(
            selectionRecency_.begin(), selectionRecency_.end(),
            [this, &ownedObjects](GameObject* object) {
                return ownedObjects.count(object) == 0 ||
                       selectedGameObjects_.count(object) == 0;
            }),
        selectionRecency_.end());

    const GameObject* previousActive = activeGameObject_;
    if (activeGameObject_ == nullptr ||
        selectedGameObjects_.count(activeGameObject_) == 0) {
        activeGameObject_ = mostRecentlySelected();
    }
    if (selectedGameObjects_.empty()) {
        activeGameObject_ = nullptr;
    }
    if (activeGameObject_ != previousActive) {
        transformTool_ = TransformTool::Translate;
    }
}

Scene* EditorSession::selectionScene() const noexcept {
    return selectionScene_;
}

GameObject* EditorSession::selectedGameObject() const noexcept {
    return activeGameObject_;
}

GameObject* EditorSession::activeGameObject() const noexcept {
    return activeGameObject_;
}

const GameObject* EditorSession::activeGameObjectForScene(
    const Scene* scene) const noexcept {
    if (scene == nullptr || scene != selectionScene_ ||
        activeGameObject_ == nullptr ||
        !ownsGameObject(scene, activeGameObject_)) {
        return nullptr;
    }
    return activeGameObject_;
}

const std::vector<GameObject*>&
EditorSession::selectedGameObjects() const noexcept {
    return selectionRecency_;
}

bool EditorSession::isSelected(const GameObject* object) const noexcept {
    return object != nullptr && selectedGameObjects_.count(object) != 0;
}

bool EditorSession::isSelectedForScene(
    const Scene* scene, const GameObject* object) const noexcept {
    return scene != nullptr && scene == selectionScene_ &&
           isSelected(object);
}

std::vector<GameObject*> EditorSession::topLevelSelectedRoots() const {
    std::vector<GameObject*> roots;
    if (selectionScene_ == nullptr || selectedGameObjects_.empty()) {
        return roots;
    }

    std::unordered_set<const GameObject*> ownedObjects;
    ownedObjects.reserve(selectionScene_->gameObjects().size());
    for (const auto& object : selectionScene_->gameObjects()) {
        if (object != nullptr) {
            ownedObjects.insert(object.get());
        }
    }

    for (const auto& owner : selectionScene_->gameObjects()) {
        GameObject* object = owner.get();
        if (object == nullptr || selectedGameObjects_.count(object) == 0) {
            continue;
        }

        bool hasSelectedAncestor = false;
        std::unordered_set<GameObject*> visitedAncestors;
        for (GameObject* ancestor = object->parent(); ancestor != nullptr;) {
            if (ownedObjects.count(ancestor) == 0 ||
                !visitedAncestors.insert(ancestor).second) {
                break;
            }
            if (selectedGameObjects_.count(ancestor) != 0) {
                hasSelectedAncestor = true;
                break;
            }
            ancestor = ancestor->parent();
        }
        if (!hasSelectedAncestor) {
            roots.push_back(object);
        }
    }
    return roots;
}

std::vector<GameObject*> EditorSession::collectSubtree(GameObject* root) {
    std::vector<GameObject*> subtree;
    if (root == nullptr) {
        return subtree;
    }

    std::vector<GameObject*> pending{root};
    std::unordered_set<GameObject*> visited;
    visited.reserve(1);
    while (!pending.empty()) {
        GameObject* object = pending.back();
        pending.pop_back();
        if (object == nullptr || !visited.insert(object).second) {
            continue;
        }

        subtree.push_back(object);
        const std::vector<GameObject*>& children = object->children();
        for (auto iterator = children.rbegin(); iterator != children.rend();
             ++iterator) {
            pending.push_back(*iterator);
        }
    }
    return subtree;
}

const GameObject* EditorSession::selectedGameObjectForScene(
    const Scene* scene) const noexcept {
    return activeGameObjectForScene(scene);
}

bool EditorSession::ownsGameObject(const Scene* scene,
                                   const GameObject* object) noexcept {
    if (scene == nullptr || object == nullptr) {
        return false;
    }
    for (const auto& owner : scene->gameObjects()) {
        if (owner.get() == object) {
            return true;
        }
    }
    return false;
}

void EditorSession::addSelectedGameObject(GameObject* object) {
    if (object == nullptr) {
        return;
    }
    const auto [iterator, inserted] = selectedGameObjects_.insert(object);
    if (!inserted) {
        return;
    }
    try {
        selectionRecency_.push_back(object);
    } catch (...) {
        selectedGameObjects_.erase(iterator);
        throw;
    }
}

void EditorSession::removeSelectedGameObject(GameObject* object) noexcept {
    if (object == nullptr) {
        return;
    }
    selectedGameObjects_.erase(object);
    selectionRecency_.erase(
        std::remove(selectionRecency_.begin(), selectionRecency_.end(), object),
        selectionRecency_.end());
}

void EditorSession::makeMostRecentlySelected(GameObject* object) noexcept {
    if (object == nullptr || selectedGameObjects_.count(object) == 0) {
        return;
    }
    const auto iterator = std::find(selectionRecency_.begin(),
                                     selectionRecency_.end(), object);
    if (iterator != selectionRecency_.end() &&
        iterator + 1 != selectionRecency_.end()) {
        std::rotate(iterator, iterator + 1, selectionRecency_.end());
    }
}

GameObject* EditorSession::mostRecentlySelected() const noexcept {
    for (auto iterator = selectionRecency_.rbegin();
         iterator != selectionRecency_.rend(); ++iterator) {
        if (*iterator != nullptr && selectedGameObjects_.count(*iterator) != 0) {
            return *iterator;
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
