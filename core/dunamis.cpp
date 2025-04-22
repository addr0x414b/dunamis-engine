#include "dunamis.h"

Dunamis::Dunamis() {
    spdlog::info("Dunamis Engine v0.2");

    // Initialize the scene. Eventually replace with a scene manager
    level1.name = "Level 1";
    level1.init();

    inputManager = std::make_shared<InputManager>();
    level1.inputManager = inputManager;

    visualServer.init(&level1);
    inputManager->window = visualServer.window;

    run();
}

void Dunamis::run() {
    spdlog::info("Begin running Dunamis Engine...");
    bool running = true;

    level1.start();

    while (running) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_EVENT_QUIT) {
                running = false;
            } else if (e.type == SDL_EVENT_KEY_DOWN) {
                if (e.key.key == SDLK_ESCAPE) {
                    running = false;
                }
            }
            inputManager->handleEvent(e);
        }
        level1.update();
        visualServer.run();
        inputManager->clearKeys();
    }
    spdlog::info("Shutting down Dunamis Engine...");
}

Dunamis::~Dunamis() {}