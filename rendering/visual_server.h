#ifndef VISUAL_SERVER_H
#define VISUAL_SERVER_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "spdlog/spdlog.h"
#include "../core/result.h"
#include "../scene/scene.h"

#include "vulkan_context.h"

class VisualServer {
public:
    ~VisualServer() noexcept;

    [[nodiscard]] Result initialize(SDL_Window* window, Scene* scene);
    [[nodiscard]] Result run();
    bool shutdown() noexcept;

private:
    VulkanContext vulkanContext;
    [[nodiscard]] Result initGameObjects();
    Scene* currentScene = nullptr;
    bool initializationAttempted = false;
    bool initialized = false;

};

#endif
