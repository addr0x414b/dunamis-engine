#include "rendering/vulkan_context.h"

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
    const Result addResult = object->addMeshInstance(std::move(instance));
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
    context.cleanup();
    context.cleanup();
    return passed ? 0 : 1;
}
