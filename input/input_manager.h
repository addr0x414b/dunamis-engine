#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <SDL3/SDL.h>
#include "spdlog/spdlog.h"
#include <unordered_map>

class InputManager {
public:
    void handleEvent(const SDL_Event& event);
    void clearKeys();

    bool isKeyDown(SDL_Keycode key) const;
    bool isKeyPressed(SDL_Keycode key) const;
    bool isKeyReleased(SDL_Keycode key) const;

    int getMouseRelX() const;
    int getMouseRelY() const;

    void setRelativeMouseMode(bool enabled);

    SDL_Window* window = nullptr;
    double relMouseX = 0;
    double relMouseY = 0;

private:
    std::unordered_map<SDL_Keycode, bool> currentKeys;
    std::unordered_map<SDL_Keycode, bool> pressedKeys;
    std::unordered_map<SDL_Keycode, bool> releasedKeys;



};

#endif