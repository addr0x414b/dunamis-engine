#ifndef EDITOR_SESSION_H
#define EDITOR_SESSION_H

#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

#include "editor_state.h"
#include "runtime_transform_edit.h"

class Scene;
class GameObject;

class EditorSession final {
public:
    EditorSession() = default;

    [[nodiscard]] SceneRunState runState() const noexcept;
    void setRunState(SceneRunState state) noexcept;

    [[nodiscard]] TransformTool transformTool() const noexcept;
    void setTransformTool(TransformTool tool) noexcept;

    void select(Scene* scene, GameObject* object);
    void select(Scene* scene, GameObject* object,
                SelectionOperation operation);
    void applySelection(Scene* scene, GameObject* object,
                        SelectionOperation operation);
    void clearSelection() noexcept;
    void synchronizeSelection(Scene* scene);
    [[nodiscard]] Scene* selectionScene() const noexcept;
    // The Active Object is always either selected or null.
    [[nodiscard]] GameObject* activeGameObject() const noexcept;
    [[nodiscard]] const GameObject* activeGameObjectForScene(
        const Scene* scene) const noexcept;
    [[nodiscard]] const std::vector<GameObject*>&
    selectedGameObjects() const noexcept;
    [[nodiscard]] bool isSelected(const GameObject* object) const noexcept;
    [[nodiscard]] bool isSelectedForScene(
        const Scene* scene, const GameObject* object) const noexcept;
    [[nodiscard]] std::vector<GameObject*> topLevelSelectedRoots() const;
    [[nodiscard]] static std::vector<GameObject*> collectSubtree(
        GameObject* root);
    // Compatibility accessors for single-target editor consumers. They return
    // the Active Object, never an arbitrary selected object.
    [[nodiscard]] GameObject* selectedGameObject() const noexcept;
    [[nodiscard]] const GameObject* selectedGameObjectForScene(
        const Scene* scene) const noexcept;

    void submitEditorAction(EditorAction action);
    [[nodiscard]] const EditorAction& pendingEditorAction() const noexcept;
    [[nodiscard]] EditorAction consumeEditorAction();

    void setPendingLoadPath(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path& pendingLoadPath() const noexcept;
    void clearPendingLoadPath() noexcept;
    void setPendingSaveAsPath(std::filesystem::path path);
    [[nodiscard]] const std::filesystem::path&
    pendingSaveAsPath() const noexcept;
    void clearPendingSaveAsPath() noexcept;
    [[nodiscard]] bool quitConfirmationPending() const noexcept;
    void setQuitConfirmationPending(bool pending) noexcept;

    void submitRuntimeTransformEdit(
        const RuntimeTransformEdit& edit) noexcept;
    [[nodiscard]] std::optional<RuntimeTransformEdit>
    consumeRuntimeTransformEdit() noexcept;

    void setRenderColliderIds(const std::vector<std::string>& ids);
    [[nodiscard]] std::vector<std::string> renderColliderIds() const;
    [[nodiscard]] bool renderColliderEnabled(
        const GameObject& object) const noexcept;
    void setRenderColliderEnabled(const GameObject& object,
                                  bool enabled);

private:
    [[nodiscard]] static bool ownsGameObject(
        const Scene* scene, const GameObject* object) noexcept;
    void addSelectedGameObject(GameObject* object);
    void removeSelectedGameObject(GameObject* object) noexcept;
    void makeMostRecentlySelected(GameObject* object) noexcept;
    [[nodiscard]] GameObject* mostRecentlySelected() const noexcept;

    SceneRunState runState_ = SceneRunState::Editing;
    TransformTool transformTool_ = TransformTool::Translate;
    Scene* selectionScene_ = nullptr;
    std::unordered_set<const GameObject*> selectedGameObjects_;
    std::vector<GameObject*> selectionRecency_;
    GameObject* activeGameObject_ = nullptr;
    EditorAction pendingEditorAction_;
    std::filesystem::path pendingLoadPath_;
    std::filesystem::path pendingSaveAsPath_;
    bool quitConfirmationPending_ = false;
    std::optional<RuntimeTransformEdit> pendingRuntimeTransformEdit_;
    std::unordered_set<std::string> renderColliderIds_;
};

#endif
