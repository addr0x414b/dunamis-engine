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

    Result result = vulkanContext.createDescriptorPool(
        totalMeshInstances * 2);
    if (!result) {
        return result;
    }

    result = vulkanContext.createDirectionalShadowResources();
    if (!result) {
        return result;
    }

    result = vulkanContext.createAmbientOcclusionResources();
    if (!result) {
        return result;
    }

    result = vulkanContext.createLightsUBO();
    if (!result) {
        return result;
    }

    return loadSceneResources(currentScene);
}

Result VisualServer::loadSceneResources(Scene* scene) {
    if (!scene) {
        return Result::failure(
            "Cannot load rendering resources for a null scene");
    }

    Result result = vulkanContext.beginSceneResourceLoad(scene);
    if (!result) {
        return Result::failure(
            "Failed to begin scene resource loading: " + result.error());
    }

    spdlog::info("Initializing scene game object visual data...");
    for (const auto& gameObject : scene->gameObjects()) {
        result = vulkanContext.createTextureImages(gameObject);
        if (!result) {
            break;
        }
        result = vulkanContext.createTextureImageViews(gameObject);
        if (!result) {
            break;
        }
        result = vulkanContext.createTextureSamplers(gameObject);
        if (!result) {
            break;
        }
        result = vulkanContext.createVertexBuffers(gameObject);
        if (!result) {
            break;
        }
        result = vulkanContext.createIndexBuffers(gameObject);
        if (!result) {
            break;
        }
        result = vulkanContext.createUniformBuffers(gameObject);
        if (!result) {
            break;
        }
        result = vulkanContext.createDescriptorSets(gameObject);
        if (!result) {
            break;
        }
    }
    if (!result) {
        const Result cleanupResult = vulkanContext.cancelSceneResourceLoad();
        if (!cleanupResult) {
            return Result::failure(
                "Scene rendering resource creation failed: " +
                result.error() + "; partial-resource cleanup failed: " +
                cleanupResult.error());
        }
        return Result::failure(
            "Scene rendering resource creation failed: " + result.error());
    }

    result = vulkanContext.commitSceneResourceLoad(scene);
    if (!result) {
        const Result cleanupResult = vulkanContext.cancelSceneResourceLoad();
        if (!cleanupResult) {
            return Result::failure(
                "Failed to commit scene rendering resources: " +
                result.error() + "; partial-resource cleanup failed: " +
                cleanupResult.error());
        }
        return Result::failure(
            "Failed to commit scene rendering resources: " +
            result.error());
    }
    spdlog::info("Successfully initialized all game object visual data");
    return Result::success();
}

Result VisualServer::unloadSceneResources(Scene* scene) {
    if (!initialized) {
        return Result::failure("Visual Server is not initialized");
    }
    Result result = vulkanContext.unloadSceneResources(scene);
    if (!result) {
        return Result::failure(
            "Failed to unload scene rendering resources: " +
            result.error());
    }
    return Result::success();
}

Result VisualServer::switchScene(Scene* scene) {
    if (!initialized || !currentScene) {
        return Result::failure("Visual Server is not initialized");
    }
    if (!scene) {
        return Result::failure("Cannot switch rendering to a null scene");
    }
    if (scene == currentScene) {
        return Result::success();
    }

    clearEditorSelection();
    Result result = scene->activate();
    if (!result) {
        return Result::failure(
            "Incoming scene cannot be activated: " + result.error());
    }
    result = vulkanContext.switchScene(scene);
    if (!result) {
        scene->deactivate();
        return Result::failure(
            "Vulkan scene switch failed: " + result.error());
    }

    currentScene->deactivate();
    currentScene = scene;
    return Result::success();
}

Scene* VisualServer::renderScene() const noexcept { return currentScene; }

Result VisualServer::run(Scene* scene, const Camera& renderCamera,
                         SceneRunState runState) {
    if (!initialized || !currentScene) {
        return Result::failure("Visual Server is not initialized");
    }
    if (!scene || scene != currentScene) {
        return Result::failure(
            "Requested scene does not match the active render scene");
    }
    return vulkanContext.drawFrame(scene, renderCamera, runState);
}

void VisualServer::processEvent(const SDL_Event& event) noexcept {
    vulkanContext.processEvent(event);
}

void VisualServer::setImGuiInputEnabled(bool enabled) noexcept {
    vulkanContext.setImGuiInputEnabled(enabled);
}

void VisualServer::clearEditorSelection() noexcept {
    vulkanContext.clearEditorSelection();
}

EditorCommand VisualServer::consumeEditorCommand() noexcept {
    return vulkanContext.consumeEditorCommand();
}

bool VisualServer::sceneInteractionAreaHovered() const noexcept {
    return vulkanContext.sceneInteractionAreaHovered();
}

void VisualServer::setCurrentScenePath(const std::string& path) {
    vulkanContext.setCurrentScenePath(path);
}

std::string VisualServer::requestedScenePath() const {
    return vulkanContext.requestedScenePath();
}

void VisualServer::requestLoadConfirmation() {
    vulkanContext.requestLoadConfirmation();
}

void VisualServer::requestQuitConfirmation() {
    vulkanContext.requestQuitConfirmation();
}

void VisualServer::setPersistenceStatus(std::string status, bool error) {
    vulkanContext.setPersistenceStatus(std::move(status), error);
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
