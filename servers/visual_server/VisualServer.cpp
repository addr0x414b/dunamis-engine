
#include "VisualServer.h"

void VisualServer::init() {
    Debugger::section("Initialize Visual Server\n");

    Debugger::subSection("Initialize SDL2");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        Debugger::print("Failed to initialize SDL2 video!");
        Debugger::print(SDL_GetError(), true);
    } else {
        Debugger::print("Successfully initialized SDL2 video");
    }

    window = SDL_CreateWindow("Dunamis Engine", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 800, 600, SDL_WINDOW_VULKAN);

    if (!window) {
        Debugger::print("Failed to create SDL2 window!");
        Debugger::print(SDL_GetError(), true);
    } else {
        Debugger::print("Successfully created SDL2 window");
    }
    Debugger::subSection("Initialize SDL2\n");
    vulkanContext.setWindow(window);
    vulkanContext.initVulkan();

    Debugger::section("Initialize Visual Server\n");

    run();
}

void VisualServer::run() {
    Debugger::print("Begin running display server...\n");
    SDL_Event e;
    bool bQuit = false;
    while (!bQuit) {
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT || e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                bQuit = true;
            }
        }
        vulkanContext.drawFrame();
    }
    Debugger::section("Shutdown Initiated\n");
    vulkanContext.cleanup();
    cleanup();
    Debugger::section("Shutdown Initiated\n");
    Debugger::print("Successfully shutdown...");
}

void VisualServer::cleanup() {
    Debugger::subSection("Clean up SDL2");
    SDL_DestroyWindowSurface(window);
    Debugger::print("Destroyed SDL window surface");
    SDL_DestroyWindow(window);
    Debugger::print("Destroyed SDL window");
    SDL_Quit();
    Debugger::print("Quit SDL");
    Debugger::subSection("Clean up SDL2");
}

