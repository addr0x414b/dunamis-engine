#ifndef INPUT_MANAGER_H
#define INPUT_MANAGER_H

#include <SDL3/SDL.h>
#include "../core/result.h"
#include "spdlog/spdlog.h"
#include <unordered_map>

enum class InputMode {
    GameplayCaptured,
    EditorInteractive,
};

class InputManager {
public:
    void handleEvent(const SDL_Event& event);
    void clearTransientInput() noexcept;
    void clearKeys() noexcept;

    bool isKeyDown(SDL_Keycode key) const;
    bool isKeyPressed(SDL_Keycode key) const;
    bool isKeyReleased(SDL_Keycode key) const;

    int getMouseRelX() const;
    int getMouseRelY() const;

    [[nodiscard]] Result setRelativeMouseMode(bool enabled);
    [[nodiscard]] InputMode inputMode() const noexcept;
    [[nodiscard]] bool gameplayInputEnabled() const noexcept;
    [[nodiscard]] bool editorInputEnabled() const noexcept;
    [[nodiscard]] Result setInputMode(InputMode mode);
    void toggleInputMode();

    SDL_Window* window = nullptr;

private:
    std::unordered_map<SDL_Keycode, bool> currentKeys;
    std::unordered_map<SDL_Keycode, bool> pressedKeys;
    std::unordered_map<SDL_Keycode, bool> releasedKeys;

    double relMouseX = 0;
    double relMouseY = 0;
    InputMode inputMode_ = InputMode::GameplayCaptured;

};

#endif
