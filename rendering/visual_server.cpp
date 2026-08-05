#include "visual_server.h"

Result VisualServer::initialize(SDL_Window* window, Scene* scene) {
    spdlog::info("Initializing Visual Server...");

    if (initialized) {
        return Result::failure("Visual Server is already initialized");
    }
    if (initializationAttempted) {
        return Result::failure(
            "Visual Server initialization cannot be retried after failure");
    }

    if (!window) {
        return Result::failure(
            "Cannot initialize Visual Server with a null SDL window");
    }
    if (!scene) {
        return Result::failure(
            "Cannot initialize Visual Server with a null scene");
    }

    Result result = scene->activate();
    if (!result) {
        return Result::failure(
            "Failed to activate scene for rendering: " + result.error());
    }

    initializationAttempted = true;
    initialized = false;
    currentScene = scene;

    result = vulkanContext.init(window, currentScene);
    if (!result) {
        if (vulkanContext.cleanup()) {
            currentScene->deactivate();
            currentScene = nullptr;
        }
        return Result::failure("Failed to initialize Vulkan Context: " +
                               result.error());
    }

    result = initGameObjects();
    if (!result) {
        if (vulkanContext.cleanup()) {
            currentScene->deactivate();
            currentScene = nullptr;
        }
        return Result::failure(
            "Failed to initialize scene rendering resources: " +
            result.error());
    }

    initialized = true;

    spdlog::info("Successfully initialized Vulkan Context");
    spdlog::info("Successfully initialized Visual Server");
    return Result::success();
}

Result VisualServer::initGameObjects() {
    uint32_t totalMeshInstances = 0;
    for (const auto& o : currentScene->gameObjects()) {
        totalMeshInstances +=
            static_cast<uint32_t>(o->meshInstances().size());
    }

    Result result = vulkanContext.createDescriptorPool(totalMeshInstances);
    if (!result) {
        return result;
    }

    result = vulkanContext.createLightsUBO();
    if (!result) {
        return result;
    }

    spdlog::info("Initializing scene game object visual data...");
    for (const auto& gameObject : currentScene->gameObjects()) {
        result = vulkanContext.createTextureImages(gameObject);
        if (!result) {
            return result;
        }
        result = vulkanContext.createTextureImageViews(gameObject);
        if (!result) {
            return result;
        }
        result = vulkanContext.createTextureSamplers(gameObject);
        if (!result) {
            return result;
        }
        result = vulkanContext.createVertexBuffers(gameObject);
        if (!result) {
            return result;
        }
        result = vulkanContext.createIndexBuffers(gameObject);
        if (!result) {
            return result;
        }
        result = vulkanContext.createUniformBuffers(gameObject);
        if (!result) {
            return result;
        }
        result = vulkanContext.createDescriptorSets(gameObject);
        if (!result) {
            return result;
        }
    }
    spdlog::info("Successfully initialized all game object visual data");
    return Result::success();
}

Result VisualServer::run() {
    if (!initialized || !currentScene) {
        return Result::failure("Visual Server is not initialized");
    }
    return vulkanContext.drawFrame(currentScene);
}

bool VisualServer::shutdown() noexcept {
    if (!initialized && !currentScene) {
        return vulkanContext.cleanup();
    }

    spdlog::info("Shutting down Visual Server...");
    if (!vulkanContext.cleanup()) {
        spdlog::error(
            "Visual Server shutdown is waiting for Vulkan work to become "
            "idle");
        return false;
    }
    currentScene->deactivate();
    initialized = false;
    currentScene = nullptr;
    spdlog::info("Successfully shut down Visual Server");
    return true;
}

VisualServer::~VisualServer() noexcept {
    (void)shutdown();
    if (currentScene) {
        // No user code can run between this destructor body and the final
        // VulkanContext cleanup attempt. Release the topology lock so a scene
        // that outlives a failed renderer shutdown is not sealed forever.
        currentScene->deactivate();
        currentScene = nullptr;
    }
}
