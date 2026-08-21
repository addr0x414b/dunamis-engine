#include "scene/game_object.h"
#include "scene/model_renderable.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>

class GameObjectModelCacheTestAccess {
public:
    static std::weak_ptr<const model_loading::CachedCpuModel> asset(
        const GameObject& object) {
        return object.modelRenderable().loadedModelAsset_;
    }
};

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

class CurrentPathGuard {
public:
    explicit CurrentPathGuard(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~CurrentPathGuard() { std::filesystem::current_path(original_); }

private:
    std::filesystem::path original_;
};

std::filesystem::path makeFixtureDirectory() {
    const auto directory = std::filesystem::temp_directory_path() /
                           ("dunamis-model-cache-" + std::to_string(
                               std::chrono::steady_clock::now()
                                   .time_since_epoch()
                                   .count()));
    std::filesystem::create_directories(directory);
    const auto writeModel = [&](const char* name, float offset) {
        std::ofstream file(directory / name);
        file << "v " << offset << " 0 0\n"
             << "v " << offset + 1.0f << " 0 0\n"
             << "v " << offset << " 1 0\n"
             << "vt 0 0\nvt 1 0\nvt 0 1\n"
             << "vn 0 0 1\n"
             << "f 1/1/1 2/2/1 3/3/1\n";
    };
    writeModel("triangle.obj", 0.0f);
    writeModel("other.obj", 10.0f);
    for (const char* name : {"override-a.ppm", "override-b.ppm"}) {
        std::ofstream image(directory / name, std::ios::binary);
        image << "P6\n1 1\n255\n";
        const unsigned char pixel[] = {32, 96, 160};
        image.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
    }
    return directory;
}

bool cleanInstance(const MeshInstance& instance) {
    const auto& material = instance.material;
    return instance.mesh.vertexBuffer == VK_NULL_HANDLE &&
           instance.mesh.vertexBufferMemory == VK_NULL_HANDLE &&
           instance.mesh.indexBuffer == VK_NULL_HANDLE &&
           instance.mesh.indexBufferMemory == VK_NULL_HANDLE &&
           material.textureImage == VK_NULL_HANDLE &&
           material.textureImageMemory == VK_NULL_HANDLE &&
           material.textureImageView == VK_NULL_HANDLE &&
           material.textureSampler == VK_NULL_HANDLE &&
           material.normalMapImage == VK_NULL_HANDLE &&
           material.normalMapImageMemory == VK_NULL_HANDLE &&
           material.normalMapImageView == VK_NULL_HANDLE &&
           material.normalMapSampler == VK_NULL_HANDLE &&
           material.metallicRoughnessMapImage == VK_NULL_HANDLE &&
           material.metallicRoughnessMapImageMemory == VK_NULL_HANDLE &&
           material.metallicRoughnessMapImageView == VK_NULL_HANDLE &&
           material.metallicRoughnessMapSampler == VK_NULL_HANDLE &&
           instance.renderData.uniformBuffers.empty() &&
           instance.renderData.uniformBuffersMemory.empty() &&
           instance.renderData.uniformBuffersMapped.empty() &&
           instance.renderData.descriptorSets.empty() &&
           material.pixels == material.pixelsOwner.get() &&
           material.normalMapPixels == material.normalMapPixelsOwner.get() &&
           material.metallicRoughnessMapPixels ==
               material.metallicRoughnessMapPixelsOwner.get() &&
           instance.mesh.bounds.valid;
}

bool testReuseIndependenceAndLifetime(const std::filesystem::path& directory) {
    const std::string model = (directory / "triangle.obj").string();
    const std::string equivalent =
        (directory / "nested" / ".." / "triangle.obj").string();
    bool passed = true;
    std::weak_ptr<const model_loading::CachedCpuModel> originalAsset;
    {
        GameObject editing;
        editing.modelPath = model.c_str();
        passed &= expect(static_cast<bool>(editing.loadModel()),
                         "Initial cache fixture load failed");
        originalAsset = GameObjectModelCacheTestAccess::asset(editing);
        passed &= expect(!originalAsset.expired(),
                         "Initial object did not retain its cached asset");

        {
            GameObject runtime;
            runtime.modelPath = equivalent.c_str();
            passed &= expect(static_cast<bool>(runtime.loadModel()),
                             "Equivalent-path cache load failed");
            passed &= expect(GameObjectModelCacheTestAccess::asset(runtime).lock() ==
                                 originalAsset.lock(),
                             "Equivalent normalized path did not reuse the asset");
            const MeshInstance& a =
                editing.modelRenderable().meshInstances().front();
            MeshInstance& b = const_cast<MeshInstance&>(
                runtime.modelRenderable().meshInstances().front());
            passed &= expect(a.mesh.vertices == b.mesh.vertices &&
                                 a.mesh.indices == b.mesh.indices &&
                                 a.material.baseColorFactor == b.material.baseColorFactor,
                             "Cache-hit mesh data differs from the source load");
            passed &= expect(cleanInstance(b),
                             "Cache-hit instance contains renderer state or invalid views");
            passed &= expect(a.mesh.vertices.data() != b.mesh.vertices.data() &&
                                 a.mesh.indices.data() != b.mesh.indices.data(),
                             "Cache-hit geometry vectors were shared");
            passed &= expect(a.material.pixelsOwner == b.material.pixelsOwner &&
                                 a.material.pixels != nullptr,
                             "Decoded base-color pixels were not shared");
            passed &= expect(a.material.texturePath != b.material.texturePath &&
                                 std::string(a.material.texturePath) ==
                                     std::string(b.material.texturePath),
                             "Base-color paths were not rebuilt into object storage");
            const glm::vec4 originalFactor = a.material.baseColorFactor;
            const glm::vec3 originalVertex = a.mesh.vertices.front().pos;
            b.material.baseColorFactor.r = 0.125f;
            b.mesh.vertices.front().pos.x += 3.0f;
            passed &= expect(a.material.baseColorFactor == originalFactor &&
                                 a.mesh.vertices.front().pos == originalVertex,
                             "Mutable CPU state leaked between cached objects");
        }
        passed &= expect(!originalAsset.expired(),
                         "Runtime destruction evicted the editing asset");
        GameObject runtimeAgain;
        runtimeAgain.modelPath = model.c_str();
        passed &= expect(static_cast<bool>(runtimeAgain.loadModel()) &&
                             GameObjectModelCacheTestAccess::asset(runtimeAgain).lock() ==
                                 originalAsset.lock(),
                         "Later runtime object did not reuse editing asset");
    }
    passed &= expect(originalAsset.expired(),
                     "Asset survived after its final GameObject was destroyed");
    GameObject reloaded;
    reloaded.modelPath = model.c_str();
    passed &= expect(static_cast<bool>(reloaded.loadModel()) &&
                         GameObjectModelCacheTestAccess::asset(reloaded).lock() !=
                             originalAsset.lock(),
                     "Expired weak cache entry was not replaced by a new asset");
    return passed;
}

bool testKeyAndFailureBehavior(const std::filesystem::path& directory) {
    const std::string model = (directory / "triangle.obj").string();
    const std::string otherModel = (directory / "other.obj").string();
    const std::string overrideA = (directory / "override-a.ppm").string();
    const std::string overrideB = (directory / "override-b.ppm").string();
    bool passed = true;
    GameObject nullFallback;
    nullFallback.modelPath = model.c_str();
    passed &= expect(static_cast<bool>(nullFallback.loadModel()),
                     "Null-fallback fixture load failed");
    GameObject explicitA;
    explicitA.modelPath = model.c_str();
    explicitA.texturePath = overrideA.c_str();
    passed &= expect(static_cast<bool>(explicitA.loadModel()),
                     "Explicit fallback fixture load failed");
    GameObject explicitB;
    explicitB.modelPath = model.c_str();
    explicitB.texturePath = overrideB.c_str();
    passed &= expect(static_cast<bool>(explicitB.loadModel()),
                     "Second explicit fallback fixture load failed");
    GameObject other;
    other.modelPath = otherModel.c_str();
    passed &= expect(static_cast<bool>(other.loadModel()),
                     "Different-model fixture load failed");
    passed &= expect(GameObjectModelCacheTestAccess::asset(nullFallback).lock() !=
                         GameObjectModelCacheTestAccess::asset(explicitA).lock() &&
                         GameObjectModelCacheTestAccess::asset(explicitA).lock() !=
                             GameObjectModelCacheTestAccess::asset(explicitB).lock() &&
                         GameObjectModelCacheTestAccess::asset(nullFallback).lock() !=
                             GameObjectModelCacheTestAccess::asset(other).lock(),
                     "Model path or fallback override was omitted from cache key");

    GameObject failed;
    const std::string missing = (directory / "missing.obj").string();
    failed.modelPath = missing.c_str();
        passed &= expect(!failed.loadModel() &&
                             failed.modelRenderable().meshInstances().empty() &&
                         GameObjectModelCacheTestAccess::asset(failed).expired(),
                     "Failed source load published or retained a cached asset");
    failed.modelPath = model.c_str();
    passed &= expect(static_cast<bool>(failed.loadModel()),
                     "Clean object could not retry after a failed load");
    passed &= expect(!failed.loadModel(),
                     "Repeated loading on an already-populated object succeeded");

    const std::filesystem::path noFallback = directory / "no-fallback";
    std::filesystem::create_directories(noFallback);
    GameObject hardTextureFailure;
    hardTextureFailure.modelPath = model.c_str();
    hardTextureFailure.texturePath = "missing-override.png";
    {
        CurrentPathGuard noFallbackDirectory(noFallback);
        passed &= expect(!hardTextureFailure.loadModel() &&
                             hardTextureFailure.modelRenderable().meshInstances().empty() &&
                             GameObjectModelCacheTestAccess::asset(
                                 hardTextureFailure).expired(),
                         "Hard texture/fallback failure entered the cache");
    }
    hardTextureFailure.texturePath = nullptr;
    passed &= expect(static_cast<bool>(hardTextureFailure.loadModel()),
                     "Object could not retry after hard texture failure");
    return passed;
}

}  // namespace

int main() {
    const CurrentPathGuard sourceDirectory(DUNAMIS_SOURCE_DIR);
    const std::filesystem::path directory = makeFixtureDirectory();
    const bool passed = testReuseIndependenceAndLifetime(directory) &&
                        testKeyAndFailureBehavior(directory);
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return passed ? 0 : 1;
}
