#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

#include <filesystem>

enum class SceneRunState {
    Editing,
    Playing,
    Simulating,
};

enum class TransformTool {
    Translate,
    Rotate,
    Scale,
};

enum class TransformSpace {
    World,
    Local,
};

enum class SelectionOperation {
    ReplaceExact,
    ToggleExact,
    ReplaceSubtree,
    AddSubtree,
};

[[nodiscard]] constexpr SelectionOperation selectionOperationForModifiers(
    bool ctrl, bool shift) noexcept {
    if (ctrl && shift) return SelectionOperation::AddSubtree;
    if (shift) return SelectionOperation::ReplaceSubtree;
    if (ctrl) return SelectionOperation::ToggleExact;
    return SelectionOperation::ReplaceExact;
}

enum class EmptyWorldSelectionOperation {
    Clear,
    Preserve,
};

[[nodiscard]] constexpr EmptyWorldSelectionOperation
emptyWorldSelectionOperationForModifiers(bool ctrl, bool shift) noexcept {
    return ctrl ? EmptyWorldSelectionOperation::Preserve
                : EmptyWorldSelectionOperation::Clear;
}

enum class EditorCommand {
    None,
    DuplicateGameObject,
    ParentSelectionToActive,
    GroupSelection,
    Play,
    Simulate,
    Stop,
    SaveScene,
    SaveSceneAs,
    ConfirmSaveSceneAsOverwrite,
    CancelSaveSceneAs,
    LoadScene,
    SaveAndLoad,
    DiscardAndLoad,
    SaveAndQuit,
    DiscardAndQuit,
    Cancel,
};

struct EditorAction {
    EditorCommand command = EditorCommand::None;
    std::filesystem::path path;
};

[[nodiscard]] constexpr bool editorToolsEnabled(
    SceneRunState state) noexcept {
    return state == SceneRunState::Editing ||
           state == SceneRunState::Simulating;
}

[[nodiscard]] constexpr bool runtimeSceneRunning(
    SceneRunState state) noexcept {
    return state == SceneRunState::Playing ||
           state == SceneRunState::Simulating;
}

[[nodiscard]] constexpr bool usesGameplayCamera(
    SceneRunState state) noexcept {
    return state == SceneRunState::Playing;
}

#endif
