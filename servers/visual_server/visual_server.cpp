#include "visual_server.h"

void VisualServer::init() {
    Debugger::section("Initialize Visual Server\n");

    Debugger::subSection("Initialize SDL2");

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        Debugger::print("Failed to initialize SDL2 video!");
        Debugger::print(SDL_GetError(), true);
    } else {
        Debugger::print("Successfully initialized SDL2 video");
    }

    window =
        SDL_CreateWindow("Dunamis Engine", SDL_WINDOWPOS_CENTERED,
                         SDL_WINDOWPOS_CENTERED, 1920, 1080, SDL_WINDOW_VULKAN);

    if (!window) {
        Debugger::print("Failed to create SDL2 window!");
        Debugger::print(SDL_GetError(), true);
    } else {
        Debugger::print("Successfully created SDL2 window");
    }
    Debugger::subSection("Initialize SDL2\n");
    vulkanContext.setWindow(window);
    editor.setWindow(window);

    GameObject test("test_models/models/dennis.obj",
                    "test_models/textures/dennis.jpg", "Dennis 1");
    test.scale = glm::vec3(0.01f, 0.01f, 0.01f);
    test.position = glm::vec3(0.0f, -80.0f, -100.0f);
    scene.gameObjects.push_back(&test);

    GameObject test2("test_models/models/dennis.obj",
                    "test_models/textures/dennis.jpg", "Dennis Big Boy");
    test2.scale = glm::vec3(0.01f, 0.01f, 0.01f);
    test2.position = glm::vec3(0.0f, -80.0f, -100.0f);
    test2.rotation = glm::vec3(0.0f, 90.0f, 0.0f);
    scene.gameObjects.push_back(&test2);

    GameObject test3("test_models/models/dennis.obj",
                    "test_models/textures/dennis.jpg");
    test3.scale = glm::vec3(0.01f, 0.01f, 0.01f);
    test3.position = glm::vec3(20.0f, -40.0f, -100.0f);
    //test3.rotation = glm::vec3(0.0f, 90.0f, 0.0f);
    scene.gameObjects.push_back(&test3);

    Camera sceneCam;
    sceneCam.position = glm::vec3(0.0f, 0.0f, 20.0f);
    scene.sceneCamera = sceneCam;
    sceneCam.name = "Main Scene Camera";
    scene.cameras.push_back(&sceneCam);

    vulkanContext.setScene(&scene);
    editor.setScene(&scene);
    vulkanContext.initVulkan();
    vulkanContext.setEditor(&editor);
    vulkanContext.initImgui();


    Debugger::section("Initialize Visual Server\n");

    run();
}

void VisualServer::run() {
    Debugger::print("Begin running display server...\n");
    SDL_Event e;
    bool bQuit = false;
    while (!bQuit) {
        while (SDL_PollEvent(&e)) {
            editor.processEvent(&e);
            if (e.type == SDL_QUIT ||
                e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) {
                bQuit = true;
            }
        }
        if (editor.quit == true) {
            bQuit = true;
        }
        editor.processInput();
        if (scene.simulating) {
            scene.run();
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
