#ifndef PLATFORM_H
#define PLATFORM_H

#include "result.h"

#include <SDL3/SDL.h>

class Platform final {
public:
    Platform() noexcept = default;
    ~Platform() noexcept;

    Platform(const Platform&) = delete;
    Platform& operator=(const Platform&) = delete;
    Platform(Platform&&) = delete;
    Platform& operator=(Platform&&) = delete;

    [[nodiscard]] Result initialize();
    void shutdown() noexcept;
    void abandon() noexcept;

    [[nodiscard]] SDL_Window* window() const noexcept;

private:
    SDL_Window* window_ = nullptr;
    bool sdlInitialized_ = false;
};

#endif
