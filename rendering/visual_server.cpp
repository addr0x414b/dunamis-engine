#include "visual_server.h"

void VisualServer::init(Scene* scene) {
    spdlog::info("Initializing Visual Server...");
    currentScene = scene;

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        spdlog::error("Failed to initialize SDL3 video: {}", SDL_GetError());
    } else {
        spdlog::info("Successfully initialized SDL3 video");
    }

    window = SDL_CreateWindow("Dunamis Engine", 1920, 1080, SDL_WINDOW_VULKAN);

    if (!window) {
        spdlog::error("Failed to create SDL3 window: {}", SDL_GetError());
    } else {
        spdlog::info("Successfully created SDL3 window");
    }

    vulkanContext.init(window);
    initGameObjects();
    spdlog::info("Successfully initialized Vulkan Context");

    spdlog::info("Successfully initialized Visual Server");

}

void VisualServer::initGameObjects() {
    uint32_t totalMeshInstances = 0;
    for (auto& o : currentScene->gameObjects) {
        totalMeshInstances += static_cast<uint32_t>(o->meshInstances.size());
    }
    //vulkanContext.createDescriptorPool(static_cast<uint32_t>(currentScene->gameObjects.size()));
    vulkanContext.createDescriptorPool(totalMeshInstances);
    spdlog::info("Initializing scene game object visual data...");
    for (auto& gameObject : currentScene->gameObjects) {
        vulkanContext.createTextureImages(gameObject); 
        vulkanContext.createTextureImageViews(gameObject);
        vulkanContext.createTextureSamplers(gameObject);
        vulkanContext.createVertexBuffers(gameObject);
        vulkanContext.createIndexBuffers(gameObject);
        vulkanContext.createUniformBuffers(gameObject);
        vulkanContext.createDescriptorSets(gameObject);
    }
    spdlog::info("Successfully initialized all game object visual data");
}

void VisualServer::run() {
    vulkanContext.drawFrame(currentScene);
}

VisualServer::~VisualServer() {
    spdlog::info("Shutting down Visual Server...");
    vulkanContext.cleanup(currentScene);
    if (window) {
        SDL_DestroyWindowSurface(window);
        SDL_DestroyWindow(window);
        window = nullptr;
        SDL_Quit();
    }
    spdlog::info("Successfully shut down Visual Server");
    spdlog::info("Successfully shut down Dunamis Engine");
}