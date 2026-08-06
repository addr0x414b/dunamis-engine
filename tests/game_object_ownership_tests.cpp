#include "scene/game_object.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <string>
#include <utility>

class GameObjectTestAccess {
public:
    static Result attach(GameObject& object) {
        return object.markRenderResourcesAttached();
    }

    static void detach(GameObject& object) {
        object.markRenderResourcesDetached();
    }

    static bool mutableTopology(const GameObject& object) {
        return object.renderTopologyMutable();
    }

    static std::size_t texturePathCount(const GameObject& object) {
        return object.texturePathStorage_.size();
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

MeshInstance cleanMeshInstance() {
    MeshInstance instance{};
    instance.mesh.vertices = {
        Vertex{{-1.0f, -2.0f, 3.0f}}, Vertex{{4.0f, 5.0f, -6.0f}},
        Vertex{{7.0f, -8.0f, 9.0f}}};
    instance.mesh.indices = {0, 1, 2};
    instance.mesh.bounds.valid = true;
    instance.mesh.bounds.minimum = {100.0f, 100.0f, 100.0f};
    instance.mesh.bounds.maximum = {101.0f, 101.0f, 101.0f};
    return instance;
}

#if VK_USE_64_BIT_PTR_DEFINES
VkBuffer foreignBuffer() {
    return reinterpret_cast<VkBuffer>(std::uintptr_t{1});
}
VkDeviceMemory foreignMemory() {
    return reinterpret_cast<VkDeviceMemory>(std::uintptr_t{1});
}
VkImage foreignImage() {
    return reinterpret_cast<VkImage>(std::uintptr_t{1});
}
VkImageView foreignImageView() {
    return reinterpret_cast<VkImageView>(std::uintptr_t{1});
}
VkSampler foreignSampler() {
    return reinterpret_cast<VkSampler>(std::uintptr_t{1});
}
#else
VkBuffer foreignBuffer() { return static_cast<VkBuffer>(1); }
VkDeviceMemory foreignMemory() { return static_cast<VkDeviceMemory>(1); }
VkImage foreignImage() { return static_cast<VkImage>(1); }
VkImageView foreignImageView() { return static_cast<VkImageView>(1); }
VkSampler foreignSampler() { return static_cast<VkSampler>(1); }
#endif

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

std::filesystem::path writeTriangleModel() {
    const auto path = std::filesystem::temp_directory_path() /
                      ("dunamis-game-object-ownership-" +
                       std::to_string(
                           std::chrono::steady_clock::now()
                               .time_since_epoch()
                               .count()) +
                       ".obj");
    std::ofstream file(path);
    file << "v 0 0 0\n"
            "v 1 0 0\n"
            "v 0 1 0\n"
            "vt 0 0\n"
            "vt 1 0\n"
            "vt 0 1\n"
            "vn 0 0 1\n"
            "f 1/1/1 2/2/1 3/3/1\n";
    return path;
}

bool testIncomingStateAndBounds() {
    bool passed = true;
    GameObject object;
    passed &= expect(static_cast<bool>(object.addMeshInstance(cleanMeshInstance())),
                     "A clean CPU MeshInstance was rejected");
    const Mesh& accepted = object.meshInstances().front().mesh;
    passed &= expect(accepted.bounds.valid &&
                         accepted.bounds.minimum == glm::vec3(-1.0f, -8.0f, -6.0f) &&
                         accepted.bounds.maximum == glm::vec3(7.0f, 5.0f, 9.0f),
                     "Accepted MeshInstance bounds were not recomputed");

    GameObject emptyObject;
    MeshInstance emptyInstance{};
    passed &= expect(static_cast<bool>(emptyObject.addMeshInstance(
                         std::move(emptyInstance))) &&
                         !emptyObject.meshInstances().front().mesh.bounds.valid,
                     "Empty CPU geometry did not retain invalid bounds");
    MeshInstance nonFiniteInstance = cleanMeshInstance();
    nonFiniteInstance.mesh.vertices[0].pos.x =
        std::numeric_limits<float>::infinity();
    const Result nonFiniteResult = object.addMeshInstance(
        std::move(nonFiniteInstance));
    passed &= expect(!nonFiniteResult &&
                         nonFiniteResult.error().find("non-finite") !=
                             std::string::npos,
                     "Non-finite vertex positions were accepted");

    const auto expectRejected = [&](const char* category,
                                    const auto& configure) {
        GameObject candidate;
        MeshInstance instance = cleanMeshInstance();
        configure(instance);
        const Result result = candidate.addMeshInstance(std::move(instance));
        return expect(!result && result.error().find(category) != std::string::npos &&
                          candidate.meshInstances().empty(),
                      std::string("Incoming Vulkan state was not rejected: ") +
                          category);
    };

    passed &= expectRejected("mesh Vulkan state", [](MeshInstance& instance) {
        instance.mesh.vertexBuffer = foreignBuffer();
    });
    passed &= expectRejected("mesh Vulkan state", [](MeshInstance& instance) {
        instance.mesh.vertexBufferMemory = foreignMemory();
    });
    passed &= expectRejected("mesh Vulkan state", [](MeshInstance& instance) {
        instance.mesh.indexBuffer = foreignBuffer();
    });
    passed &= expectRejected("mesh Vulkan state", [](MeshInstance& instance) {
        instance.mesh.indexBufferMemory = foreignMemory();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.textureImage = foreignImage();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.textureImageMemory = foreignMemory();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.textureImageView = foreignImageView();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.textureSampler = foreignSampler();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.normalMapImage = foreignImage();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.normalMapImageMemory = foreignMemory();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.normalMapImageView = foreignImageView();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.normalMapSampler = foreignSampler();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.metallicRoughnessMapImage = foreignImage();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.metallicRoughnessMapImageMemory = foreignMemory();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.metallicRoughnessMapImageView = foreignImageView();
    });
    passed &= expectRejected("material Vulkan state", [](MeshInstance& instance) {
        instance.material.metallicRoughnessMapSampler = foreignSampler();
    });
    passed &= expectRejected("RenderData Vulkan state", [](MeshInstance& instance) {
        instance.renderData.uniformBuffers.push_back(VK_NULL_HANDLE);
    });
    passed &= expectRejected("RenderData Vulkan state", [](MeshInstance& instance) {
        instance.renderData.uniformBuffersMemory.push_back(VK_NULL_HANDLE);
    });
    passed &= expectRejected("RenderData Vulkan state", [](MeshInstance& instance) {
        instance.renderData.uniformBuffersMapped.push_back(nullptr);
    });
    passed &= expectRejected("RenderData Vulkan state", [](MeshInstance& instance) {
        instance.renderData.uniformBuffersMapped.push_back(
            reinterpret_cast<void*>(std::uintptr_t{1}));
    });
    passed &= expectRejected("RenderData Vulkan state", [](MeshInstance& instance) {
        instance.renderData.descriptorSets.push_back(VK_NULL_HANDLE);
    });
    return passed;
}

bool testTopologyLock(const std::filesystem::path& modelPath) {
    GameObject object;
    if (!object.addMeshInstance(cleanMeshInstance())) {
        return false;
    }
    const std::size_t sizeBefore = object.meshInstances().size();
    const std::size_t capacityBefore = object.meshInstances().capacity();
    object.modelPath = modelPath.c_str();
    const Result attach = GameObjectTestAccess::attach(object);
    const Result add = object.addMeshInstance(cleanMeshInstance());
    const Result load = object.loadModel();
    bool passed = expect(static_cast<bool>(attach), "Could not attach topology test object") &&
                  expect(!add && add.error().find("mesh topology") != std::string::npos,
                         "Attached object accepted addMeshInstance") &&
                  expect(!load && load.error().find("mesh topology") != std::string::npos,
                         "Attached object accepted loadModel") &&
                  expect(object.meshInstances().size() == sizeBefore &&
                             object.meshInstances().capacity() == capacityBefore,
                         "Rejected topology mutation changed the mesh vector");
    GameObjectTestAccess::detach(object);
    passed &= expect(GameObjectTestAccess::mutableTopology(object),
                     "Detached topology did not become mutable") &&
              expect(static_cast<bool>(object.addMeshInstance(cleanMeshInstance())),
                     "Detached object rejected mesh mutation");
    return passed;
}

bool testModelLoading(const std::filesystem::path& modelPath) {
    bool passed = true;
    const std::string validPath = modelPath.string();
    GameObject object;
    object.modelPath = validPath.c_str();
    const Result first = object.loadModel();
    passed &= expect(static_cast<bool>(first), "First model load failed: " + first.error());
    if (!first || object.meshInstances().empty()) {
        return false;
    }
    const MeshInstance& firstInstance = object.meshInstances().front();
    const std::size_t meshCount = object.meshInstances().size();
    const std::size_t vertexCount = firstInstance.mesh.vertices.size();
    const std::size_t indexCount = firstInstance.mesh.indices.size();
    const Mesh::Bounds bounds = firstInstance.mesh.bounds;
    const glm::vec4 baseColor = firstInstance.material.baseColorFactor;
    const std::string texturePath = firstInstance.material.texturePath
                                        ? firstInstance.material.texturePath
                                        : "";
    const Result second = object.loadModel();
    passed &= expect(!second && second.error().find("repeated model loading") !=
                                  std::string::npos,
                     "Repeated model loading was accepted") &&
              expect(object.meshInstances().size() == meshCount &&
                         object.meshInstances().front().mesh.vertices.size() == vertexCount &&
                         object.meshInstances().front().mesh.indices.size() == indexCount &&
                         object.meshInstances().front().mesh.bounds.minimum == bounds.minimum &&
                         object.meshInstances().front().mesh.bounds.maximum == bounds.maximum &&
                         object.meshInstances().front().material.baseColorFactor == baseColor &&
                         std::string(object.meshInstances().front().material.texturePath) == texturePath,
                     "Repeated model loading modified imported data") &&
              expect(object.meshInstances().front().material.pixels != nullptr &&
                         object.meshInstances().front().material.texturePath != nullptr,
                     "Imported data did not outlive the local Assimp importer");

    GameObject retry;
    const std::string missingPath = modelPath.string() + ".missing";
    retry.modelPath = missingPath.c_str();
    const Result missing = retry.loadModel();
    passed &= expect(!missing && retry.meshInstances().empty() &&
                         GameObjectTestAccess::texturePathCount(retry) == 0,
                     "Failed first load left partial GameObject state");
    retry.modelPath = validPath.c_str();
    const Result retried = retry.loadModel();
    passed &= expect(static_cast<bool>(retried) && !retry.meshInstances().empty(),
                     "A clean failed load could not be retried");
    return passed;
}

}  // namespace

int main() {
    const CurrentPathGuard sourceDirectory(DUNAMIS_SOURCE_DIR);
    const std::filesystem::path modelPath = writeTriangleModel();
    bool passed = testIncomingStateAndBounds();
    passed &= testTopologyLock(modelPath);
    passed &= testModelLoading(modelPath);
    std::error_code error;
    std::filesystem::remove(modelPath, error);
    return passed ? 0 : 1;
}
