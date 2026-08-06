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
    clearTransientInput();
}

bool InputManager::isKeyDown(SDL_Keycode key) const {
    if (!gameplayInputEnabled()) {
        return false;
    }
    auto it = currentKeys.find(key);
    return it != currentKeys.end() && it->second;
}

bool InputManager::isKeyPressed(SDL_Keycode key) const {
    if (!gameplayInputEnabled()) {
        return false;
    }
    auto it = pressedKeys.find(key);
    return it != pressedKeys.end() && it->second;
}

bool InputManager::isKeyReleased(SDL_Keycode key) const {
    if (!gameplayInputEnabled()) {
        return false;
    }
    auto it = releasedKeys.find(key);
    return it != releasedKeys.end() && it->second;
}

int InputManager::getMouseRelX() const {
    if (!gameplayInputEnabled()) {
        return 0;
    }
    return relMouseX;
}

int InputManager::getMouseRelY() const {
    if (!gameplayInputEnabled()) {
        return 0;
    }
    return relMouseY;
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
    return inputMode_ == InputMode::GameplayCaptured;
}

bool InputManager::editorInputEnabled() const noexcept {
    return inputMode_ == InputMode::EditorInteractive;
}

Result InputManager::setInputMode(InputMode mode) {
    bool relativeMouseEnabled = false;
    if (mode == InputMode::GameplayCaptured) {
        relativeMouseEnabled = true;
    } else if (mode != InputMode::EditorInteractive) {
        return Result::failure("Unknown input mode");
    }

    Result result = setRelativeMouseMode(relativeMouseEnabled);
    if (!result) {
        return result;
    }

    inputMode_ = mode;
    clearTransientInput();
    return Result::success();
}

void InputManager::toggleInputMode() {
    const InputMode nextMode = gameplayInputEnabled()
                                   ? InputMode::EditorInteractive
                                   : InputMode::GameplayCaptured;
    Result result = setInputMode(nextMode);
    if (!result) {
        spdlog::error("Failed to toggle input mode: {}", result.error());
    }
}
