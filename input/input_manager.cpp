#include "input_manager.h"

#include <string>

namespace {

std::string sdlError(const char* message) {
    const char* error = SDL_GetError();
    if (!error || error[0] == '\0') {
        return message;
    }
    return std::string(message) + ": " + error;
}

}  // namespace

void InputManager::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
        currentKeys[event.key.key] = true;
        pressedKeys[event.key.key] = true;
    } else if (event.type == SDL_EVENT_KEY_UP) {
        currentKeys[event.key.key] = false;
        releasedKeys[event.key.key] = true;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        relMouseX += event.motion.xrel;
        relMouseY += event.motion.yrel;
    }
}

void InputManager::clearTransientInput() noexcept {
    pressedKeys.clear();
    releasedKeys.clear();
    relMouseX = 0;
    relMouseY = 0;
}

void InputManager::clearKeys() noexcept {
    currentKeys.clear();
    pressedKeys.clear();
    releasedKeys.clear();
    relMouseX = 0;
    relMouseY = 0;
}

bool InputManager::isKeyDown(SDL_Keycode key) const {
    auto it = currentKeys.find(key);
    return it != currentKeys.end() && it->second;
}

bool InputManager::isKeyPressed(SDL_Keycode key) const {
    auto it = pressedKeys.find(key);
    return it != pressedKeys.end() && it->second;
}

bool InputManager::isKeyReleased(SDL_Keycode key) const {
    auto it = releasedKeys.find(key);
    return it != releasedKeys.end() && it->second;
}

int InputManager::getMouseRelX() const {
    return static_cast<int>(relMouseX);
}

int InputManager::getMouseRelY() const {
    return static_cast<int>(relMouseY);
}

Result InputManager::setRelativeMouseMode(bool enabled) {
    if (!window) {
        return Result::failure(
            "Cannot change relative mouse mode without an SDL window");
    }
    if (!SDL_SetWindowRelativeMouseMode(window, enabled)) {
        return Result::failure(sdlError(
            enabled ? "Failed to enable SDL relative mouse mode"
                    : "Failed to disable SDL relative mouse mode"));
    }
    if (!enabled && !SDL_ShowCursor()) {
        return Result::failure(sdlError("Failed to show the SDL cursor"));
    }
    return Result::success();
}

InputMode InputManager::inputMode() const noexcept {
    return inputMode_;
}

bool InputManager::gameplayInputEnabled() const noexcept {
    return inputMode_ == InputMode::GameplayInteractive ||
           inputMode_ == InputMode::GameplayCaptured;
}

bool InputManager::editorCameraInputEnabled() const noexcept {
    return inputMode_ == InputMode::EditorCameraCaptured;
}

bool InputManager::imguiInputEnabled() const noexcept {
    return inputMode_ == InputMode::EditorInteractive ||
           inputMode_ == InputMode::GameplayInteractive ||
           inputMode_ == InputMode::GameplaySuspended;
}

Result InputManager::transitionTo(InputMode mode, bool relativeMouseEnabled) {
    Result result = setRelativeMouseMode(relativeMouseEnabled);
    if (!result) {
        return result;
    }

    inputMode_ = mode;
    clearKeys();
    return Result::success();
}

Result InputManager::enterEditorInteractive() {
    return transitionTo(InputMode::EditorInteractive, false);
}

Result InputManager::beginEditorCameraCapture() {
    if (inputMode_ != InputMode::EditorInteractive) {
        return Result::failure(
            "Editor camera capture requires editor-interactive input");
    }
    return transitionTo(InputMode::EditorCameraCaptured, true);
}

Result InputManager::endEditorCameraCapture() {
    if (inputMode_ != InputMode::EditorCameraCaptured) {
        return Result::failure("Editor camera is not captured");
    }
    return transitionTo(InputMode::EditorInteractive, false);
}

Result InputManager::beginGameplaySession() {
    if (inputMode_ != InputMode::EditorInteractive) {
        return Result::failure(
            "Gameplay can begin only from editor-interactive input");
    }
    return transitionTo(InputMode::GameplayInteractive, false);
}

Result InputManager::requestGameplayMouseCapture() {
    if (inputMode_ != InputMode::GameplayInteractive) {
        return Result::failure(
            "Gameplay mouse capture requires interactive gameplay input");
    }
    return transitionTo(InputMode::GameplayCaptured, true);
}

Result InputManager::releaseGameplayMouseCapture() {
    if (inputMode_ != InputMode::GameplayCaptured) {
        return Result::failure("Gameplay mouse is not captured");
    }
    return transitionTo(InputMode::GameplayInteractive, false);
}

Result InputManager::toggleGameplayMouseRelease() {
    if (inputMode_ == InputMode::GameplayCaptured) {
        return transitionTo(InputMode::GameplaySuspended, false);
    }
    if (inputMode_ == InputMode::GameplaySuspended) {
        return transitionTo(InputMode::GameplayCaptured, true);
    }
    if (inputMode_ == InputMode::GameplayInteractive) {
        return Result::success();
    }
    return Result::failure(
        "Gameplay mouse release cannot be toggled from an editor input mode");
}
