#include "core/editor_state.h"

#include <iostream>

namespace {

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

    return passed ? 0 : 1;
}
