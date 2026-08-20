#include "rendering/vulkan_context.h"
#include "scene/model_renderable.h"

#include <cstdlib>
#include <iostream>
#include <memory>

class VulkanContextTestAccess {
public:
    static Result validateTextureData(
        VulkanContext& context, const std::unique_ptr<GameObject>& object) {
        return context.validateTextureData(object);
    }
};

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    auto object = std::make_unique<GameObject>();
    MeshInstance instance{};
    instance.material.texWidth = 1;
    instance.material.texHeight = 1;
    const Result addResult = object->modelRenderable().addMeshInstance(
        std::move(instance));
    if (!addResult) {
        std::cerr << addResult.error() << '\n';
        return 1;
    }

    VulkanContext context;
    const Result result = VulkanContextTestAccess::validateTextureData(
        context, object);
    const bool passed =
        expect(!result, "Null texture pixels were accepted") &&
        expect(result.error().find("missing texture pixels") !=
                   std::string::npos,
               "Texture preflight error did not identify missing pixels");

    auto normalMapObject = std::make_unique<GameObject>();
    MeshInstance normalMapInstance{};
    normalMapInstance.material.pixels =
        static_cast<stbi_uc*>(std::malloc(4));
    normalMapInstance.material.texWidth = 1;
    normalMapInstance.material.texHeight = 1;
    normalMapInstance.material.mipLevels = 1;
    normalMapInstance.material.normalMapEnabled = true;
    const Result normalAddResult = normalMapObject->modelRenderable().addMeshInstance(
        std::move(normalMapInstance));
    if (!normalAddResult) {
        std::cerr << normalAddResult.error() << '\n';
        return 1;
    }
    const Result normalResult = VulkanContextTestAccess::validateTextureData(
        context, normalMapObject);
    const bool normalPassed =
        expect(!normalResult, "Null normal-map pixels were accepted") &&
        expect(normalResult.error().find("missing normal-map pixels") !=
                   std::string::npos,
               "Normal-map preflight error did not identify missing pixels");
    stbi_image_free(normalMapObject->modelRenderable().meshInstances().front().material.pixels);

    auto metallicRoughnessObject = std::make_unique<GameObject>();
    MeshInstance metallicRoughnessInstance{};
    metallicRoughnessInstance.material.pixels =
        static_cast<stbi_uc*>(std::malloc(4));
    metallicRoughnessInstance.material.texWidth = 1;
    metallicRoughnessInstance.material.texHeight = 1;
    metallicRoughnessInstance.material.mipLevels = 1;
    metallicRoughnessInstance.material.hasMetallicRoughnessMap = true;
    const Result metallicRoughnessAddResult =
        metallicRoughnessObject->modelRenderable().addMeshInstance(
            std::move(metallicRoughnessInstance));
    if (!metallicRoughnessAddResult) {
        std::cerr << metallicRoughnessAddResult.error() << '\n';
        return 1;
    }
    const Result metallicRoughnessResult =
        VulkanContextTestAccess::validateTextureData(
            context, metallicRoughnessObject);
    const bool metallicRoughnessPassed =
        expect(!metallicRoughnessResult,
               "Null metallic-roughness pixels were accepted") &&
        expect(metallicRoughnessResult.error().find(
                   "missing metallic-roughness pixels") != std::string::npos,
               "Metallic-roughness preflight error did not identify missing "
               "pixels");
    stbi_image_free(
        metallicRoughnessObject->modelRenderable().meshInstances().front().material.pixels);
    context.cleanup();
    context.cleanup();
    return passed && normalPassed && metallicRoughnessPassed ? 0 : 1;
}
