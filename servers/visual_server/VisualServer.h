
#ifndef VISUAL_SERVER_H
#define VISUAL_SERVER_H

#include <SDL2/SDL.h>
#include <SDL2/SDL_vulkan.h>
#include "../../drivers/vulkan/VulkanContext.h"
#include "../../core/debugger/Debugger.h"

#include "../../thirdparty/imgui/imgui.h"
#include "../../thirdparty/imgui/imgui_impl_sdl2.h"
#include "../../thirdparty/imgui/imgui_impl_vulkan.h"

class VisualServer {
public:
    void init();

private:
    VulkanContext vulkanContext;
    SDL_Window *window;
    void run();
    void cleanup();

};


#endif //VISUAL_SERVER_H
