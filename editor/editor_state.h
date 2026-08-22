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

enum class EditorCommand {
    None,
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
