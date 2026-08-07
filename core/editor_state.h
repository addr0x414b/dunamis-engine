#ifndef EDITOR_STATE_H
#define EDITOR_STATE_H

enum class SceneRunState {
    Editing,
    Playing,
    Simulating,
};

enum class EditorCommand {
    None,
    Play,
    Simulate,
    Stop,
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
