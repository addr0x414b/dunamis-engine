#include "core/platform.h"
#include "rendering/vulkan_context.h"
#include "scene/scene.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>

class VulkanContextTestAccess {
public:
    [[nodiscard]] static Result createUniformBuffers(
        VulkanContext& context,
        const std::unique_ptr<GameObject>& gameObject) {
        return context.createUniformBuffers(gameObject);
    }
};

namespace {

constexpr int skipped = 77;

class EmptyScene final : public Scene {
public:
    void init() override {}
    void start() override {}
    void update() override {}
};

VkSampler foreignSamplerHandle() {
#if VK_USE_64_BIT_PTR_DEFINES
    return reinterpret_cast<VkSampler>(std::uintptr_t{1});
#else
    return static_cast<VkSampler>(1);
#endif
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Expected the engine build directory argument\n";
        return 1;
    }

    Platform platform;
    const Result platformResult = platform.initialize();
    if (!platformResult) {
        std::cout << "Skipping Vulkan partial-cleanup test: "
                  << platformResult.error() << '\n';
        return skipped;
    }

    {
        EmptyScene dirtyScene;
        auto object = std::make_unique<GameObject>();
        MeshInstance dirtyInstance{};
        dirtyInstance.material.textureSampler = foreignSamplerHandle();
        const Result addMeshResult =
            object->addMeshInstance(std::move(dirtyInstance));
        if (!addMeshResult) {
            std::cerr << "Failed to build dirty test mesh: "
                      << addMeshResult.error() << '\n';
            return 1;
        }
        const Result addResult = dirtyScene.addGameObject(std::move(object));
        if (!addResult) {
            std::cerr << "Failed to build dirty test scene: "
                      << addResult.error() << '\n';
            return 1;
        }

        VulkanContext context;
        const Result dirtySceneResult =
            context.init(platform.window(), &dirtyScene);
        const VkSampler retainedSampler =
            dirtyScene.gameObjects().front()
                ->meshInstances().front()
                .material.textureSampler;

        const bool firstCleanup = context.cleanup();
        const bool secondCleanup = context.cleanup();

        if (dirtySceneResult ||
            dirtySceneResult.error().find(
                "already contains Vulkan render state") ==
                std::string::npos ||
            retainedSampler != foreignSamplerHandle() ||
            !firstCleanup || !secondCleanup) {
            std::cerr << "VulkanContext did not reject and preserve "
                         "pre-existing scene resources\n";
            return 1;
        }

    }

    EmptyScene scene;
    VulkanContext context;
    const Result initializationResult =
        context.init(platform.window(), &scene);

    // The test's working directory deliberately has no shader files, so a
    // Vulkan-capable machine reaches a deep, deterministic initialization
    // failure after creating the instance, device, swapchain, and render pass.
    const bool reachedExpectedFailure =
        !initializationResult &&
        initializationResult.error().find(
            "Failed to open file: rendering/shaders/vert.spv") !=
            std::string::npos;

    const bool firstPartialCleanup = context.cleanup();
    const bool secondPartialCleanup = context.cleanup();

    if (!firstPartialCleanup || !secondPartialCleanup) {
        std::cerr << "Repeated partial Vulkan cleanup did not complete\n";
        return 1;
    }

    if (!reachedExpectedFailure) {
        if (!initializationResult) {
            std::cout << "Skipping Vulkan partial-cleanup test because Vulkan "
                         "startup stopped before shader loading: "
                      << initializationResult.error() << '\n';
            return skipped;
        }

        std::cerr << "Vulkan initialization unexpectedly succeeded without "
                     "shader files\n";
        return 1;
    }

    std::filesystem::current_path(argv[1]);

    EmptyScene resourceScene;
    auto resourceObject = std::make_unique<GameObject>();
    const Result addMeshResult =
        resourceObject->addMeshInstance(MeshInstance{});
    if (!addMeshResult) {
        std::cerr << "Failed to build resource test mesh: "
                  << addMeshResult.error() << '\n';
        return 1;
    }
    const Result addResourceResult =
        resourceScene.addGameObject(std::move(resourceObject));
    if (!addResourceResult) {
        std::cerr << "Failed to build resource test scene: "
                  << addResourceResult.error() << '\n';
        return 1;
    }

    {
        VulkanContext initializedContext;
        const Result fullInitializationResult =
            initializedContext.init(platform.window(), &resourceScene);
        if (!fullInitializationResult) {
            std::cerr << "Full Vulkan initialization failed after the partial "
                         "path succeeded: "
                      << fullInitializationResult.error() << '\n';
            return 1;
        }

        const Result bufferResult =
            VulkanContextTestAccess::createUniformBuffers(
                initializedContext,
                resourceScene.gameObjects().front());
        if (!bufferResult) {
            std::cerr << "Failed to create tracked scene buffers: "
                      << bufferResult.error() << '\n';
            return 1;
        }

        if (!initializedContext.cleanup() ||
            !initializedContext.cleanup()) {
            std::cerr << "Repeated full Vulkan cleanup did not complete\n";
            return 1;
        }
    }

    const RenderData& renderData =
        resourceScene.gameObjects().front()
            ->meshInstances().front()
            .renderData;
    if (!renderData.uniformBuffers.empty() ||
        !renderData.uniformBuffersMemory.empty() ||
        !renderData.uniformBuffersMapped.empty()) {
        std::cerr << "Vulkan cleanup retained tracked scene-buffer state\n";
        return 1;
    }

    platform.shutdown();
    platform.shutdown();
    return 0;
}
