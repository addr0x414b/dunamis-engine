#ifndef VISUAL_SERVER_H
#define VISUAL_SERVER_H

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include "spdlog/spdlog.h"
#include "../scene/scene.h"

#include "vulkan_context.h"

class VisualServer {
public:
    ~VisualServer();
    void init(Scene* scene);
    void run();

private:
    SDL_Window *window;
    VulkanContext vulkanContext;
    void initGameObjects();
    Scene* currentScene;

};

#endif