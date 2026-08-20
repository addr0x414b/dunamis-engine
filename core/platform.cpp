#include "platform.h"

#include <string>

namespace {

constexpr int windowWidth = 3440;
constexpr int windowHeight = 1440;

std::string sdlError(const std::string& message) {
    const char* error = SDL_GetError();
    if (!error || error[0] == '\0') {
        return message;
    }
    return message + ": " + error;
}

}  // namespace

Platform::~Platform() noexcept {
    shutdown();
}

Result Platform::initialize() {
    if (window_) {
        return Result::success();
    }

    shutdown();

    if (!SDL_Init(SDL_INIT_VIDEO)) {
        return Result::failure(sdlError("Failed to initialize SDL3 video"));
    }
    sdlInitialized_ = true;

    window_ = SDL_CreateWindow(
        "Dunamis Engine", windowWidth, windowHeight, SDL_WINDOW_VULKAN);
    if (!window_) {
        const std::string error =
            sdlError("Failed to create the Dunamis Engine window");
        shutdown();
        return Result::failure(error);
    }

    return Result::success();
}

void Platform::shutdown() noexcept {
    if (window_) {
        SDL_DestroyWindow(window_);
        window_ = nullptr;
    }

    if (sdlInitialized_) {
        SDL_Quit();
        sdlInitialized_ = false;
    }
}

void Platform::abandon() noexcept {
    // A live Vulkan surface must outlive its SDL window. If catastrophic
    // device cleanup cannot establish that submitted work is finished, leave
    // both SDL and the window for process teardown rather than destroy them in
    // the wrong order.
    window_ = nullptr;
    sdlInitialized_ = false;
}

SDL_Window* Platform::window() const noexcept {
    return window_;
}
