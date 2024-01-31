#ifndef VISUAL_SERVER_H
#define VISUAL_SERVER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include "../../drivers/vulkan_context/vulkan_context.h"
#include "../../core/debugger/debugger.h"
#include "../../editor/editor.h"


class VisualServer {
public:
    void init();

private:
    VulkanContext vulkanContext;
    Editor editor;
    SDL_Window *window;
    void run();
    void cleanup();

};


#endif //VISUAL_SERVER_H