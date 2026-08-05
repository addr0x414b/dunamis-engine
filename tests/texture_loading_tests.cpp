#include "scene/game_object.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

class CurrentPathGuard final {
public:
    explicit CurrentPathGuard(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }
    ~CurrentPathGuard() { std::filesystem::current_path(original_); }

private:
    std::filesystem::path original_;
};

std::filesystem::path createMissingTextureModel() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("dunamis-texture-loading-" + std::to_string(suffix));
    std::filesystem::create_directories(directory);

    std::ofstream binary(directory / "mesh.bin", std::ios::binary);
    const float positions[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f};
    const unsigned short indices[] = {0, 1, 2};
    binary.write(reinterpret_cast<const char*>(positions), sizeof(positions));
    binary.write(reinterpret_cast<const char*>(indices), sizeof(indices));

    std::ofstream model(directory / "model.gltf");
    model << R"json({
"asset":{"version":"2.0"},
"buffers":[{"uri":"mesh.bin","byteLength":42}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
"images":[{"uri":"missing.png"},{"uri":"missing-normal.png"}],"textures":[{"source":0},{"source":1}],
"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0}},"normalTexture":{"index":1}}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
})json";
    return directory / "model.gltf";
}

bool testFallbackSuccess(const std::filesystem::path& buildDirectory) {
    const auto modelPath = createMissingTextureModel();
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "Missing intended texture did not use fallback");
        passed &= expect(object.meshInstances().size() == 1,
                         "Fallback model did not load one mesh");
        for (const auto& instance : object.meshInstances()) {
            passed &= expect(instance.material.pixels != nullptr,
                             "Fallback mesh has null pixels");
            passed &= expect(instance.material.texWidth > 0 &&
                                 instance.material.texHeight > 0,
                             "Fallback mesh has invalid dimensions");
            passed &= expect(instance.material.mipLevels > 0,
                             "Fallback mesh has invalid mip levels");
            passed &= expect(instance.material.texturePath &&
                                 std::string(instance.material.texturePath).find(
                                     "rendering/default_textures/error.jpg") !=
                                     std::string::npos,
                             "Fallback path was not recorded");
            passed &= expect(!instance.material.normalMapEnabled &&
                                 instance.material.normalMapPixels == nullptr &&
                                 instance.material.normalMapPath.find(
                                     "missing-normal.png") != std::string::npos,
                             "Missing optional normal map was not disabled safely");
            stbi_image_free(instance.material.pixels);
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testFallbackFailure(const std::filesystem::path& modelPath,
                         const std::filesystem::path& noFallbackDirectory) {
    CurrentPathGuard guard(noFallbackDirectory);
    GameObject object;
    object.modelPath = modelPath.c_str();
    const Result result = object.loadModel();
    return expect(!result, "Missing fallback texture unexpectedly succeeded") &&
           expect(result.error().find("intended texture") != std::string::npos &&
                      result.error().find("fallback texture") != std::string::npos,
                  "Fallback failure did not provide texture context") &&
           expect(object.meshInstances().empty(),
                  "Failed model load committed a mesh");
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "Expected the engine build directory argument\n";
        return 1;
    }
    const auto modelPath = createMissingTextureModel();
    const auto noFallbackDirectory = modelPath.parent_path() / "no-fallback";
    std::filesystem::create_directories(noFallbackDirectory);
    const bool passed =
        testFallbackSuccess(argv[1]) &&
        testFallbackFailure(modelPath, noFallbackDirectory);
    std::filesystem::remove_all(modelPath.parent_path());
    return passed ? 0 : 1;
}
