#include "input_manager.h"

void InputManager::handleEvent(const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN) {
        currentKeys[event.key.key] = true;
        pressedKeys[event.key.key] = true;
    } else if (event.type == SDL_EVENT_KEY_UP) {
        currentKeys[event.key.key] = false;
        releasedKeys[event.key.key] = true;
    }

    if (event.type == SDL_EVENT_MOUSE_MOTION) {
        relMouseX = event.motion.xrel;
        relMouseY = event.motion.yrel;
    }
}

void InputManager::clearKeys() {
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
    return relMouseX;
}

int InputManager::getMouseRelY() const {
    return relMouseY;
}

void InputManager::setRelativeMouseMode(bool enabled) {
    SDL_SetWindowRelativeMouseMode(window, enabled);
}