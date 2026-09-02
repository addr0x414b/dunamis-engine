#include "scene/game_object.h"
#include "scene/model_renderable.h"

#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool nearlyEqual(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
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

std::string jsonEscape(std::string_view value) {
    std::string escaped;
    escaped.reserve(value.size());
    for (const char character : value) {
        if (character == '\\' || character == '"') {
            escaped.push_back('\\');
        }
        escaped.push_back(character);
    }
    return escaped;
}

void writePpm(const std::filesystem::path& path, unsigned char red,
              unsigned char green, unsigned char blue) {
    std::ofstream image(path, std::ios::binary);
    image << "P6\n1 1\n255\n";
    const unsigned char pixel[] = {red, green, blue};
    image.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
}

std::filesystem::path createMissingTextureModel(
    const char* baseColorFactor = "0.8,0.7,0.6,0.5",
    const char* metallicFactor = "0.35",
    const char* roughnessFactor = "0.65",
    std::string_view baseColorReference = "missing.png") {
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

    std::ofstream metallicRoughness(directory / "metallic-roughness.ppm",
                                    std::ios::binary);
    metallicRoughness << "P6\n1 1\n255\n";
    const unsigned char metallicRoughnessPixel[] = {0, 255, 128};
    metallicRoughness.write(
        reinterpret_cast<const char*>(metallicRoughnessPixel),
        sizeof(metallicRoughnessPixel));

    std::ofstream model(directory / "model.gltf");
    model << R"json({
"asset":{"version":"2.0"},
"buffers":[{"uri":"mesh.bin","byteLength":42}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
"images":[{"uri":")json"
           << jsonEscape(baseColorReference)
           << R"json("},{"uri":"missing-normal.png"},{"uri":"metallic-roughness.ppm"}],"textures":[{"source":0},{"source":1},{"source":2}],
"materials":[{"pbrMetallicRoughness":{"baseColorFactor":[)json"
           << baseColorFactor
           << R"json(],"metallicFactor":)json"
           << metallicFactor
           << R"json(,"roughnessFactor":)json"
           << roughnessFactor
           << R"json(,"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":2}},"normalTexture":{"index":1}}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
})json";
    return directory / "model.gltf";
}

std::filesystem::path createSharedMaterialModel() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory =
        std::filesystem::temp_directory_path() /
        ("dunamis-shared-material-" + std::to_string(suffix));
    std::filesystem::create_directories(directory);

    std::ofstream binary(directory / "mesh.bin", std::ios::binary);
    const float positions[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f};
    const float texCoords[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const unsigned short indices[] = {0, 1, 2};
    binary.write(reinterpret_cast<const char*>(positions), sizeof(positions));
    binary.write(reinterpret_cast<const char*>(texCoords), sizeof(texCoords));
    binary.write(reinterpret_cast<const char*>(indices), sizeof(indices));

    std::ofstream image(directory / "shared.ppm", std::ios::binary);
    image << "P6\n1 1\n255\n";
    const unsigned char pixel[] = {32, 96, 160};
    image.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));

    std::ofstream model(directory / "model.gltf");
    model << R"json({
"asset":{"version":"2.0"},
"buffers":[{"uri":"mesh.bin","byteLength":66}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},{"buffer":0,"byteOffset":60,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
"images":[{"uri":"shared.ppm"}],"textures":[{"source":0},{"source":0},{"source":0}],
"materials":[{"pbrMetallicRoughness":{"baseColorTexture":{"index":0},"metallicRoughnessTexture":{"index":2}},"normalTexture":{"index":1}}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0,"TEXCOORD_0":1},"indices":2,"material":0}]},{"primitives":[{"attributes":{"POSITION":0},"indices":2,"material":0}]}],
"nodes":[{"mesh":0},{"mesh":1}],"scenes":[{"nodes":[0,1]}],"scene":0
})json";
    return directory / "model.gltf";
}

std::filesystem::path createParallelDecodeModel() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("dunamis-parallel-decode-" +
                            std::to_string(suffix));
    std::filesystem::create_directories(directory);

    std::ofstream binary(directory / "mesh.bin", std::ios::binary);
    const float positions[] = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f,
                               0.0f, 1.0f, 0.0f};
    const float texCoords[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    const unsigned short indices[] = {0, 1, 2};
    binary.write(reinterpret_cast<const char*>(positions), sizeof(positions));
    binary.write(reinterpret_cast<const char*>(texCoords), sizeof(texCoords));
    binary.write(reinterpret_cast<const char*>(indices), sizeof(indices));

    for (int index = 0; index < 8; ++index) {
        std::ofstream image(directory / ("image-" + std::to_string(index) +
                                         ".ppm"),
                            std::ios::binary);
        image << "P6\n1 1\n255\n";
        const unsigned char pixel[] = {static_cast<unsigned char>(20 + index),
                                       static_cast<unsigned char>(80 + index),
                                       static_cast<unsigned char>(140 + index)};
        image.write(reinterpret_cast<const char*>(pixel), sizeof(pixel));
    }

    std::ofstream model(directory / "model.gltf");
    model << R"json({
"asset":{"version":"2.0"},
"buffers":[{"uri":"mesh.bin","byteLength":66}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":24},{"buffer":0,"byteOffset":60,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR"}],
"images":[)json";
    for (int index = 0; index < 8; ++index) {
        if (index != 0) {
            model << ',';
        }
        model << "{\"uri\":\"image-" << index << ".ppm\"}";
    }
    model << "],\n\"textures\":[";
    for (int index = 0; index < 8; ++index) {
        if (index != 0) {
            model << ',';
        }
        model << "{\"source\":" << index << '}';
    }
    model << "],\n\"materials\":[";
    for (int index = 0; index < 9; ++index) {
        if (index != 0) {
            model << ',';
        }
        const int texture = index == 8 ? 0 : index;
        model << "{\"pbrMetallicRoughness\":{\"baseColorTexture\":{\"index\":"
              << texture << "}";
        if (index == 1) {
            model << ",\"metallicRoughnessTexture\":{\"index\":0}";
        }
        model << '}';
        if (index == 0) {
            model << ",\"normalTexture\":{\"index\":0}";
        }
        model << '}';
    }
    model << "],\n\"meshes\":[{\"primitives\":[";
    for (int index = 0; index < 9; ++index) {
        if (index != 0) {
            model << ',';
        }
        model << "{\"attributes\":{\"POSITION\":0,\"TEXCOORD_0\":1},\"indices\":2,\"material\":"
              << index << '}';
    }
    model << R"json(]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
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
        passed &= expect(object.modelRenderable().meshInstances().size() == 1,
                         "Fallback model did not load one mesh");
        for (const auto& instance : object.modelRenderable().meshInstances()) {
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
            passed &= expect(
                nearlyEqual(instance.material.baseColorFactor.r, 0.8f) &&
                    nearlyEqual(instance.material.baseColorFactor.g, 0.7f) &&
                    nearlyEqual(instance.material.baseColorFactor.b, 0.6f) &&
                    nearlyEqual(instance.material.baseColorFactor.a, 0.5f) &&
                    nearlyEqual(instance.material.metallicFactor, 0.35f) &&
                    nearlyEqual(instance.material.roughnessFactor, 0.65f),
                "glTF material factors were not imported");
            passed &= expect(
                instance.material.hasMetallicRoughnessMap &&
                    instance.material.metallicRoughnessMapPixels != nullptr &&
                    instance.material.metallicRoughnessMapWidth == 1 &&
                    instance.material.metallicRoughnessMapHeight == 1 &&
                    instance.material.metallicRoughnessMapPath ==
                        (modelPath.parent_path() /
                         "metallic-roughness.ppm")
                            .string() &&
                    instance.material.metallicRoughnessMapPixels[1] == 255 &&
                    instance.material.metallicRoughnessMapPixels[2] == 128,
                "Combined metallic-roughness texture was not resolved");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testExactReferencedPathSuccess(
    const std::filesystem::path& buildDirectory) {
    const auto modelPath = createMissingTextureModel(
        "0.8,0.7,0.6,0.5", "0.35", "0.65", "direct.ppm");
    const auto directPath = modelPath.parent_path() / "direct.ppm";
    writePpm(directPath, 11, 22, 33);
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "Existing referenced texture did not load");
        if (result && object.modelRenderable().meshInstances().size() == 1) {
            const Material& material =
                object.modelRenderable().meshInstances().front().material;
            passed &= expect(
                material.texturePath &&
                    std::string(material.texturePath) == directPath.string() &&
                    material.pixels != nullptr && material.pixels[0] == 11 &&
                    material.pixels[1] == 22 && material.pixels[2] == 33,
                "Existing referenced texture was not used directly");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testTexturesDirectoryFallback(const std::filesystem::path& buildDirectory) {
    const auto modelPath = createMissingTextureModel(
        "0.8,0.7,0.6,0.5", "0.35", "0.65", "missing/source.ppm");
    const auto fallbackDirectory = modelPath.parent_path() / "textures";
    const auto fallbackPath = fallbackDirectory / "source.ppm";
    std::filesystem::create_directories(fallbackDirectory);
    writePpm(fallbackPath, 44, 55, 66);
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "Textures-directory fallback did not load");
        if (result && object.modelRenderable().meshInstances().size() == 1) {
            const Material& material =
                object.modelRenderable().meshInstances().front().material;
            passed &= expect(
                material.texturePath &&
                    std::string(material.texturePath) == fallbackPath.string() &&
                    material.pixels != nullptr && material.pixels[0] == 44 &&
                    material.pixels[1] == 55 && material.pixels[2] == 66,
                "Textures-directory fallback did not use the exact filename");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testWindowsStyleTextureReference(
    const std::filesystem::path& buildDirectory) {
    const auto modelPath = createMissingTextureModel(
        "0.8,0.7,0.6,0.5", "0.35", "0.65", "nested\\windows.ppm");
    const auto referencedPath = modelPath.parent_path() / "nested" /
                                "windows.ppm";
    std::filesystem::create_directories(referencedPath.parent_path());
    writePpm(referencedPath, 77, 88, 99);
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "Windows-style texture reference did not load");
        if (result && object.modelRenderable().meshInstances().size() == 1) {
            const Material& material =
                object.modelRenderable().meshInstances().front().material;
            passed &= expect(
                material.texturePath &&
                    std::string(material.texturePath) == referencedPath.string() &&
                    material.pixels != nullptr && material.pixels[0] == 77 &&
                    material.pixels[1] == 88 && material.pixels[2] == 99,
                "Windows-style separators were not normalized");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testDecodeFailureDoesNotUseTexturesFallback(
    const std::filesystem::path& buildDirectory) {
    const auto modelPath = createMissingTextureModel(
        "0.8,0.7,0.6,0.5", "0.35", "0.65", "corrupt.ppm");
    const auto corruptPath = modelPath.parent_path() / "corrupt.ppm";
    {
        std::ofstream corrupt(corruptPath, std::ios::binary);
        corrupt << "not an image";
    }
    const auto fallbackDirectory = modelPath.parent_path() / "textures";
    const auto fallbackPath = fallbackDirectory / "corrupt.ppm";
    std::filesystem::create_directories(fallbackDirectory);
    writePpm(fallbackPath, 123, 134, 145);
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "Decode failure did not use the error texture");
        if (result && object.modelRenderable().meshInstances().size() == 1) {
            const Material& material =
                object.modelRenderable().meshInstances().front().material;
            passed &= expect(
                material.texturePath &&
                    std::string(material.texturePath).find(
                        "rendering/default_textures/error.jpg") !=
                        std::string::npos &&
                    std::string(material.texturePath) != fallbackPath.string(),
                "Decode failure incorrectly used the textures fallback");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testFactorValidation(const std::filesystem::path& buildDirectory) {
    const auto modelPath = createMissingTextureModel(
        "-0.2,2.0,0.5,3.0", "2.0", "-1.0");
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "Invalid-factor glTF fixture did not load");
        if (result && !object.modelRenderable().meshInstances().empty()) {
            const Material& material =
                object.modelRenderable().meshInstances().front().material;
            passed &= expect(
                nearlyEqual(material.baseColorFactor.r, 0.0f) &&
                    nearlyEqual(material.baseColorFactor.g, 1.0f) &&
                    nearlyEqual(material.baseColorFactor.b, 0.5f) &&
                    nearlyEqual(material.baseColorFactor.a, 1.0f) &&
                    nearlyEqual(material.metallicFactor, 1.0f) &&
                    nearlyEqual(material.roughnessFactor, 0.0f),
                "Invalid glTF factors were not bounded safely");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testOptionalMetallicRoughnessFailure(
    const std::filesystem::path& buildDirectory) {
    const auto modelPath = createMissingTextureModel();
    std::filesystem::remove(modelPath.parent_path() /
                             "metallic-roughness.ppm");
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "A failed optional metallic-roughness map rejected "
                         "the model");
        if (result && !object.modelRenderable().meshInstances().empty()) {
            const Material& material =
                object.modelRenderable().meshInstances().front().material;
            passed &= expect(
                !material.hasMetallicRoughnessMap &&
                    material.metallicRoughnessMapPixels == nullptr &&
                    nearlyEqual(material.metallicFactor, 0.35f) &&
                    nearlyEqual(material.roughnessFactor, 0.65f),
                "Optional metallic-roughness failure did not preserve factors");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testSharedMaterialReuse(const std::filesystem::path& buildDirectory) {
    const auto modelPath = createSharedMaterialModel();
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject object;
        object.modelPath = modelPath.c_str();
        const Result result = object.loadModel();
        passed &= expect(static_cast<bool>(result),
                         "Shared-material fixture did not load");
        passed &= expect(object.modelRenderable().meshInstances().size() == 2,
                         "Shared-material fixture did not produce two meshes");
        if (result && object.modelRenderable().meshInstances().size() == 2) {
            const MeshInstance& firstInstance =
                object.modelRenderable().meshInstances()[0];
            const MeshInstance& secondInstance =
                object.modelRenderable().meshInstances()[1];
            const Material& first = firstInstance.material;
            const Material& second = secondInstance.material;
            passed &= expect(first.pixels != nullptr &&
                                 first.pixels == second.pixels &&
                                 first.pixelsOwner.use_count() >= 2,
                             "Meshes did not share one base-color allocation");
            passed &= expect(first.hasMetallicRoughnessMap &&
                                 second.hasMetallicRoughnessMap &&
                                 first.metallicRoughnessMapPixels ==
                                     second.metallicRoughnessMapPixels &&
                                 first.metallicRoughnessMapPixelsOwner.use_count() >=
                                     2,
                             "Meshes did not share one metallic-roughness allocation");
            const Material& tangentMaterial =
                first.normalMapEnabled ? first : second;
            passed &= expect(tangentMaterial.normalMapPixels != nullptr &&
                                 tangentMaterial.pixels !=
                                     tangentMaterial.normalMapPixels,
                             "Texture usage did not produce a distinct normal decode");
            passed &= expect(firstInstance.mesh.vertices.size() == 3 &&
                                 secondInstance.mesh.vertices.size() == 3 &&
                                 firstInstance.mesh.indices.size() == 3 &&
                                 secondInstance.mesh.indices.size() == 3,
                             "Shared-material meshes lost independent geometry");
            const bool tangentStateDiffers =
                first.normalMapEnabled != second.normalMapEnabled;
            passed &= expect(tangentStateDiffers,
                             "Normal-map enablement was not mesh-specific");
        }
    }
    std::filesystem::remove_all(modelPath.parent_path());
    return passed;
}

bool testParallelDecodeFixture(const std::filesystem::path& buildDirectory) {
    const auto modelPath = createParallelDecodeModel();
    bool passed = true;
    {
        CurrentPathGuard guard(buildDirectory);
        GameObject first;
        first.modelPath = modelPath.c_str();
        const Result firstResult = first.loadModel();
        passed &= expect(static_cast<bool>(firstResult),
                         "Parallel decode fixture did not load");
        passed &= expect(first.modelRenderable().meshInstances().size() == 9,
                         "Parallel decode fixture did not retain mesh order");
        if (firstResult && first.modelRenderable().meshInstances().size() == 9) {
            const Material& firstMaterial =
                first.modelRenderable().meshInstances()[0].material;
            const Material& repeatedMaterial =
                first.modelRenderable().meshInstances()[8].material;
            const Material& metallicMaterial =
                first.modelRenderable().meshInstances()[1].material;
            passed &= expect(firstMaterial.pixels != nullptr &&
                                 firstMaterial.pixels[0] == 20 &&
                                 firstMaterial.pixels[1] == 80 &&
                                 firstMaterial.pixels[2] == 140,
                             "Parallel fixture base-color pixels differ from source");
            passed &= expect(firstMaterial.pixels == repeatedMaterial.pixels &&
                                 firstMaterial.pixelsOwner ==
                                     repeatedMaterial.pixelsOwner,
                             "Same source and usage did not share one decode");
            passed &= expect(firstMaterial.normalMapPixels != nullptr &&
                                 firstMaterial.normalMapPixels !=
                                     firstMaterial.pixels &&
                                 metallicMaterial.metallicRoughnessMapPixels !=
                                     firstMaterial.pixels,
                             "Same source used by different usages was not decoded separately");
            passed &= expect(firstMaterial.pixels == firstMaterial.pixelsOwner.get() &&
                                 firstMaterial.normalMapPixels ==
                                     firstMaterial.normalMapPixelsOwner.get() &&
                                 metallicMaterial.metallicRoughnessMapPixels ==
                                     metallicMaterial.metallicRoughnessMapPixelsOwner.get(),
                             "Parallel fixture raw pixel views do not match owners");
            GameObject cached;
            cached.modelPath = modelPath.c_str();
            const Result cachedResult = cached.loadModel();
            passed &= expect(static_cast<bool>(cachedResult) &&
                                 cached.modelRenderable().meshInstances().size() == 9 &&
                                 cached.modelRenderable().meshInstances()[0].material.pixels ==
                                     firstMaterial.pixels &&
                                 cached.modelRenderable().meshInstances()[0].mesh.vertices.data() !=
                                     first.modelRenderable().meshInstances()[0].mesh.vertices.data(),
                             "Cache hit did not preserve pixel sharing and geometry independence");
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
           expect(object.modelRenderable().meshInstances().empty(),
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
    bool passed = testFallbackSuccess(argv[1]);
    passed &= testExactReferencedPathSuccess(argv[1]);
    passed &= testTexturesDirectoryFallback(argv[1]);
    passed &= testWindowsStyleTextureReference(argv[1]);
    passed &= testDecodeFailureDoesNotUseTexturesFallback(argv[1]);
    passed &= testFallbackFailure(modelPath, noFallbackDirectory);
    passed &= testFactorValidation(argv[1]);
    passed &= testOptionalMetallicRoughnessFailure(argv[1]);
    passed &= testSharedMaterialReuse(argv[1]);
    passed &= testParallelDecodeFixture(argv[1]);
    std::filesystem::remove_all(modelPath.parent_path());
    return passed ? 0 : 1;
}
