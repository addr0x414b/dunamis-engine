#ifndef EDITOR_H
#define EDITOR_H

#include "../core/debugger/debugger.h"

#include <imgui.h>
#include <imgui_impl_sdl2.h>
#include <imgui_impl_vulkan.h>


class Editor {
public:
    void processEvent(const SDL_Event* event);
    void showMenuBar();
    bool quit = false;

private:
    void ShowExampleMenuFile();
};


#endif  // EDITOR_H