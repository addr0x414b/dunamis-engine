#include "editor/editor_picking.h"
#include "physics/collision_shapes.h"
#include "physics/physics_server.h"
#include "physics/physics_shape_cache.h"
#include "physics/physics_transforms.h"
#include "scene/loading_cache_key.h"
#include "scene/model_importer.h"
#include "scene/model_renderable.h"
#include "scene/scene_serializer.h"
#include "scene/type_registry.h"
#include "math/transform_math.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool near(float first, float second, float tolerance = 1.0e-4f) {
    return std::fabs(first - second) <= tolerance;
}

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

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

class JoltGuard final {
public:
    JoltGuard() {
        JPH::RegisterDefaultAllocator();
        JPH::Factory::sInstance = new JPH::Factory();
        JPH::RegisterTypes();
    }
    ~JoltGuard() {
        JPH::UnregisterTypes();
        delete JPH::Factory::sInstance;
        JPH::Factory::sInstance = nullptr;
    }
};

std::filesystem::path makeFixtureDirectory() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    const auto directory = std::filesystem::temp_directory_path() /
        ("dunamis-model-hierarchy-" + std::to_string(suffix));
    std::filesystem::create_directories(directory);
    return directory;
}

std::vector<std::uint8_t> makeBinaryFixture() {
    const std::array<float, 18> positions = {
        0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.25f, 0.0f, 1.0f, 0.5f,
        1.0f, 0.0f, 0.25f, 2.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.75f};
    std::vector<std::uint8_t> bytes;
    bytes.reserve(88);
    const auto* positionBytes =
        reinterpret_cast<const std::uint8_t*>(positions.data());
    bytes.insert(bytes.end(), positionBytes,
                 positionBytes + sizeof(positions));
    const std::array<std::uint16_t, 3> first = {0, 1, 2};
    const auto* firstBytes = reinterpret_cast<const std::uint8_t*>(first.data());
    bytes.insert(bytes.end(), firstBytes, firstBytes + sizeof(first));
    bytes.push_back(0);
    bytes.push_back(0);
    const std::array<std::uint16_t, 3> second = {3, 4, 5};
    const auto* secondBytes =
        reinterpret_cast<const std::uint8_t*>(second.data());
    bytes.insert(bytes.end(), secondBytes, secondBytes + sizeof(second));
    while (bytes.size() % 4 != 0) bytes.push_back(0);
    return bytes;
}

std::string makeGltfDocument(bool embeddedBuffer) {
    std::string json = R"json({
"asset":{"version":"2.0"},
"scene":0,
"scenes":[{"nodes":[0]}],
"nodes":[
 {"name":"Root","children":[1,2,3,5,6,7]},
 {"name":"Container","translation":[10,0,0],"scale":[2,1,1],"children":[4]},
 {"name":"Part","mesh":0},
 {"name":"Part","mesh":0,"translation":[4,0,0],"rotation":[0,0,0.70710678,0.70710678],"scale":[-1,2,1]},
 {"name":"NestedPart","mesh":0,"translation":[0,3,0]},
 {"name":"Part","mesh":0,"translation":[8,0,0]},
 {"name":"ShearedPart","mesh":0,"matrix":[1,0,0,0,0.5,1,0,0,0,0,1,0,12,0,0,1]},
 {"translation":[5,0,0],"children":[8]},
 {"translation":[0,2,0],"children":[9]},
 {"name":"WrappedPart","mesh":0,"translation":[0,0,1]}
],
"meshes":[{"primitives":[
 {"attributes":{"POSITION":0},"indices":1,"material":0},
 {"attributes":{"POSITION":0},"indices":2,"material":1}
]}],
"materials":[
 {"pbrMetallicRoughness":{"baseColorFactor":[1,0,0,1]}},
 {"pbrMetallicRoughness":{"baseColorFactor":[0,1,0,1]}}
],
"buffers":[{"byteLength":86}],
"bufferViews":[
 {"buffer":0,"byteOffset":0,"byteLength":72},
 {"buffer":0,"byteOffset":72,"byteLength":6},
 {"buffer":0,"byteOffset":80,"byteLength":6}
],
"accessors":[
 {"bufferView":0,"componentType":5126,"count":6,"type":"VEC3","min":[0,0,0],"max":[2,1,0.75]},
 {"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR","min":[0],"max":[2]},
 {"bufferView":2,"componentType":5123,"count":3,"type":"SCALAR","min":[3],"max":[5]}
]
})json";
    if (!embeddedBuffer) {
        const std::string marker = "\"buffers\":[{\"byteLength\":86}]";
        const auto offset = json.find(marker);
        json.replace(offset, marker.size(),
                     "\"buffers\":[{\"uri\":\"hierarchy.bin\",\"byteLength\":86}]");
    }
    return json;
}

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void writeGltf(const std::filesystem::path& path) {
    const std::vector<std::uint8_t> binary = makeBinaryFixture();
    std::ofstream binaryFile(path.parent_path() / "hierarchy.bin",
                             std::ios::binary);
    binaryFile.write(reinterpret_cast<const char*>(binary.data()),
                     static_cast<std::streamsize>(binary.size()));
    std::ofstream jsonFile(path);
    jsonFile << makeGltfDocument(false);
}

void writeMeshBearingAncestorGltf(const std::filesystem::path& path) {
    writeGltf(path);
    std::string document = makeGltfDocument(false);
    const std::string emptyContainer =
        "\"name\":\"Container\",\"translation\":[10,0,0],"
        "\"scale\":[2,1,1],\"children\":[4]";
    const std::string meshContainer =
        "\"name\":\"Container\",\"mesh\":0,\"translation\":[10,0,0],"
        "\"scale\":[2,1,1],\"children\":[4]";
    const std::size_t offset = document.find(emptyContainer);
    if (offset != std::string::npos) {
        document.replace(offset, emptyContainer.size(), meshContainer);
    }
    std::ofstream jsonFile(path);
    jsonFile << document;
}

void writeGlb(const std::filesystem::path& path) {
    std::string json = makeGltfDocument(true);
    while (json.size() % 4 != 0) json.push_back(' ');
    const std::vector<std::uint8_t> binary = makeBinaryFixture();
    std::vector<std::uint8_t> file;
    appendUint32(file, 0x46546c67U);
    appendUint32(file, 2U);
    appendUint32(file, static_cast<std::uint32_t>(12U + 8U + json.size() +
                                                  8U + binary.size()));
    appendUint32(file, static_cast<std::uint32_t>(json.size()));
    appendUint32(file, 0x4e4f534aU);
    file.insert(file.end(), json.begin(), json.end());
    appendUint32(file, static_cast<std::uint32_t>(binary.size()));
    appendUint32(file, 0x004e4942U);
    file.insert(file.end(), binary.begin(), binary.end());
    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file.data()),
                 static_cast<std::streamsize>(file.size()));
}

void writeObj(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "o SingleObject\n"
            "v 0 0 0\n"
            "v 2 0 0\n"
            "v 0 1 0\n"
            "f 1 2 3\n";
}

void writeGroupedObj(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "o FirstObject\n"
            "v 0 0 0\n"
            "v 1 0 0\n"
            "v 0 1 0\n"
            "f 1 2 3\n"
            "o SecondObject\n"
            "v 3 0 0\n"
            "v 4 0 0\n"
            "v 3 1 0\n"
            "f 4 5 6\n";
}

std::vector<GameObject*> objectsNamed(Scene& scene, const std::string& name) {
    std::vector<GameObject*> found;
    for (const auto& object : scene.gameObjects()) {
        if (object && object->name == name) found.push_back(object.get());
    }
    return found;
}

std::vector<glm::vec3> importedWorldPoints(const Scene& scene) {
    std::vector<glm::vec3> points;
    for (const auto& object : scene.gameObjects()) {
        if (!object || object->importedAssetReference().empty()) continue;
        const glm::mat4 world = object->worldTransformMatrix();
        for (const MeshInstance& instance :
             object->modelRenderable().meshInstances()) {
            for (const Vertex& vertex : instance.mesh.vertexData()) {
                points.emplace_back(world * glm::vec4(vertex.pos, 1.0f));
            }
        }
    }
    std::sort(points.begin(), points.end(), [](const glm::vec3& first,
                                               const glm::vec3& second) {
        return std::tie(first.x, first.y, first.z) <
               std::tie(second.x, second.y, second.z);
    });
    return points;
}

std::vector<glm::vec3> flattenedWorldPoints(
    const GameObject& flat, const glm::mat4& placement) {
    std::vector<glm::vec3> points;
    for (const MeshInstance& instance : flat.modelRenderable().meshInstances()) {
        for (const Vertex& vertex : instance.mesh.vertices) {
            points.emplace_back(placement * glm::vec4(vertex.pos, 1.0f));
        }
    }
    std::sort(points.begin(), points.end(), [](const glm::vec3& first,
                                               const glm::vec3& second) {
        return std::tie(first.x, first.y, first.z) <
               std::tie(second.x, second.y, second.z);
    });
    return points;
}

bool samePoints(const std::vector<glm::vec3>& first,
                const std::vector<glm::vec3>& second) {
    if (first.size() != second.size()) {
        std::cerr << "World vertex count mismatch: " << first.size() << " vs "
                  << second.size() << '\n';
        return false;
    }
    std::vector<bool> matched(second.size(), false);
    for (const glm::vec3& point : first) {
        std::size_t nearestIndex = second.size();
        float nearestDistance = std::numeric_limits<float>::infinity();
        for (std::size_t index = 0; index < second.size(); ++index) {
            if (matched[index]) continue;
            const float distance = glm::length(point - second[index]);
            if (distance < nearestDistance) {
                nearestDistance = distance;
                nearestIndex = index;
            }
        }
        if (nearestIndex == second.size() || nearestDistance > 2.0e-4f) {
            return false;
        }
        matched[nearestIndex] = true;
    }
    return true;
}

bool importedNormalsMatchWinding(const Scene& scene) {
    for (const auto& object : scene.gameObjects()) {
        if (!object || object->importedAssetReference().empty()) continue;
        const glm::mat4 world = object->worldTransformMatrix();
        const glm::mat3 normalTransform =
            glm::transpose(glm::inverse(glm::mat3(world)));
        for (const MeshInstance& instance :
             object->modelRenderable().meshInstances()) {
            const auto& vertices = instance.mesh.vertexData();
            const auto& indices = instance.mesh.indexData();
            if (indices.size() % 3 != 0) return false;
            for (std::size_t index = 0; index < indices.size(); index += 3) {
                if (indices[index] >= vertices.size() ||
                    indices[index + 1] >= vertices.size() ||
                    indices[index + 2] >= vertices.size()) {
                    return false;
                }
                const Vertex& first = vertices[indices[index]];
                const Vertex& second = vertices[indices[index + 1]];
                const Vertex& third = vertices[indices[index + 2]];
                const glm::vec3 firstPosition = glm::vec3(
                    world * glm::vec4(first.pos, 1.0f));
                const glm::vec3 secondPosition = glm::vec3(
                    world * glm::vec4(second.pos, 1.0f));
                const glm::vec3 thirdPosition = glm::vec3(
                    world * glm::vec4(third.pos, 1.0f));
                glm::vec3 faceNormal = glm::cross(
                    secondPosition - firstPosition,
                    thirdPosition - firstPosition);
                glm::vec3 vertexNormal = normalTransform *
                    (first.normal + second.normal + third.normal);
                if (glm::dot(faceNormal, faceNormal) <= 1.0e-12f ||
                    glm::dot(vertexNormal, vertexNormal) <= 1.0e-12f) {
                    return false;
                }
                faceNormal = glm::normalize(faceNormal);
                vertexNormal = glm::normalize(vertexNormal);
                if (glm::dot(faceNormal, vertexNormal) < 0.9f) return false;
            }
        }
    }
    return true;
}

bool testHierarchyImport(const std::filesystem::path& directory) {
    const auto modelPath = directory / "hierarchy.gltf";
    writeGltf(modelPath);
    TestScene scene;
    model_import::Options options;
    options.rootName = "ImportedFixture";
    options.position = {3.0f, -2.0f, 1.0f};
    options.rotation = {0.0f, 0.0f, 90.0f};
    options.generateStaticColliders = true;
    model_import::ImportResult imported;
    const Result importResult =
        model_import::importHierarchy(scene, modelPath, options, imported);
    if (!expect(static_cast<bool>(importResult),
                "hierarchical glTF import failed: " + importResult.error())) {
        return false;
    }

    bool passed = true;
    passed &= expect(imported.root != nullptr &&
                         imported.root->name == "ImportedFixture" &&
                         imported.objectCount == scene.gameObjects().size() &&
                         imported.sourceNodeCount == 10 &&
                         imported.objectCount == 8,
                     "import result did not expose the source hierarchy root and nodes");
    const auto parts = objectsNamed(scene, "Part");
    const auto containers = objectsNamed(scene, "Container");
    const auto nestedParts = objectsNamed(scene, "NestedPart");
    const auto shearedParts = objectsNamed(scene, "ShearedPart");
    const auto wrappedParts = objectsNamed(scene, "WrappedPart");
    passed &= expect(parts.size() == 3 && containers.size() == 1 &&
                         nestedParts.size() == 1 && shearedParts.size() == 1 &&
                         wrappedParts.size() == 1 &&
                         objectsNamed(scene, "Node_7").empty() &&
                         objectsNamed(scene, "Node_8").empty(),
                     "source node names or duplicate-name objects were lost");
    if (parts.size() != 3 || containers.size() != 1 || nestedParts.size() != 1) {
        return false;
    }
    passed &= expect(parts[0]->modelRenderable().meshInstances().size() == 2 &&
                         parts[1]->modelRenderable().meshInstances().size() == 2 &&
                         parts[0]->physics.enabled && parts[1]->physics.enabled &&
                         !containers[0]->physics.enabled &&
                         containers[0]->modelRenderable().meshInstances().empty(),
                     "multi-primitive node ownership or empty-node policy is incorrect");
    const auto firstReference = parts[0]->importedAssetReference();
    const auto secondReference = parts[1]->importedAssetReference();
    const auto thirdReference = parts[2]->importedAssetReference();
    passed &= expect(firstReference.nodeIdentity != secondReference.nodeIdentity &&
                         secondReference.nodeIdentity != thirdReference.nodeIdentity,
                     "duplicate source names did not receive stable distinct identities");
    passed &= expect(parts[0]->modelRenderable().meshInstances()[0]
                             .mesh.sharedGeometry ==
                         parts[2]->modelRenderable().meshInstances()[0]
                             .mesh.sharedGeometry &&
                         parts[0]->modelRenderable().meshInstances()[0]
                             .mesh.sharedGeometry !=
                         parts[1]->modelRenderable().meshInstances()[0]
                             .mesh.sharedGeometry,
                     "shared and derived immutable mesh geometry identity is incorrect");
    passed &= expect(parts[0]->modelRenderable().meshInstances()[0]
                             .mesh.sharedGeometry->collisionFingerprint != 0,
                     "immutable imported geometry did not cache its collision fingerprint");
    const auto& firstMaterials = parts[0]->modelRenderable().meshInstances();
    passed &= expect(firstMaterials[0].material.baseColorFactor.r > 0.9f &&
                         firstMaterials[0].material.baseColorFactor.g < 0.1f &&
                         firstMaterials[1].material.baseColorFactor.g > 0.9f &&
                         firstMaterials[1].material.baseColorFactor.r < 0.1f,
                     "source primitive material assignments were not preserved");
    passed &= expect(nestedParts[0]->parent() == containers[0],
                     "nested source-node parent relationship was not preserved");
    passed &= expect(wrappedParts[0]->parent() == imported.root &&
                         near(wrappedParts[0]->position.x, 5.0f) &&
                         near(wrappedParts[0]->position.y, 2.0f) &&
                         near(wrappedParts[0]->position.z, 1.0f),
                     "pruned unnamed wrappers did not preserve child pivots and transforms");
    passed &= expect(near(containers[0]->scale.x, 1.0f) &&
                         near(nestedParts[0]->position.y, 3.0f),
                     "source scale was not normalized into reusable mesh geometry");

    GameObject flat;
    const std::string modelPathString = modelPath.string();
    flat.modelPath = modelPathString.c_str();
    const Result flatLoad = flat.loadModel();
    passed &= expect(static_cast<bool>(flatLoad),
                     "legacy whole-model path failed for the same glTF fixture");
    const glm::mat4 placement = transform_math::makeModelMatrix(
        options.position, options.rotation, glm::vec3(1.0f));
    const std::vector<glm::vec3> hierarchyPoints =
        importedWorldPoints(scene);
    passed &= expect(samePoints(hierarchyPoints,
                                flattenedWorldPoints(flat, placement)),
                     "hierarchical world geometry differs from flattened legacy geometry");
    const glm::vec3 expectedShearedPoint =
        placement * glm::vec4(12.5f, 1.0f, 0.5f, 1.0f);
    passed &= expect(std::any_of(
                         hierarchyPoints.begin(), hierarchyPoints.end(),
                         [&](const glm::vec3& point) {
                             return glm::length(point - expectedShearedPoint) <
                                    2.0e-4f;
                         }),
                     "source-node shear was not preserved in derived geometry");
    passed &= expect(importedNormalsMatchWinding(scene),
                     "transformed vertex normals disagree with imported face winding");

    for (const auto& object : scene.gameObjects()) {
        if (object && object->physics.enabled) {
            const Result hierarchy = physics::validatePhysicsHierarchy(*object);
            passed &= expect(static_cast<bool>(hierarchy),
                             "imported static body violates Jolt ancestry: " +
                                 hierarchy.error());
            physics::CookedShape shape;
            const Result cooked = physics::buildGameObjectShape(*object, shape);
            passed &= expect(static_cast<bool>(cooked) && shape.shape != nullptr,
                             "imported static collider could not be cooked: " +
                                 cooked.error());
        }
    }

    const std::string sourceIdentity =
        model_loading::normalizedFilesystemIdentity(modelPathString);
    const auto partShapeKey = physics::makeStaticMeshShapeCacheKey(
        sourceIdentity, parts[0]->modelRenderable().meshInstances(),
        parts[0]->scale, 0);
    const auto sameShapeKey = physics::makeStaticMeshShapeCacheKey(
        sourceIdentity, parts[2]->modelRenderable().meshInstances(),
        parts[2]->scale, 0);
    const auto derivedShapeKey = physics::makeStaticMeshShapeCacheKey(
        sourceIdentity, nestedParts[0]->modelRenderable().meshInstances(),
        nestedParts[0]->scale, 0);
    const std::uint64_t firstRamGeometryIdentity =
        physics::makeStaticMeshInMemoryGeometryIdentity(
            parts[0]->modelRenderable().meshInstances());
    const std::uint64_t sharedRamGeometryIdentity =
        physics::makeStaticMeshInMemoryGeometryIdentity(
            parts[2]->modelRenderable().meshInstances());
    const std::uint64_t derivedRamGeometryIdentity =
        physics::makeStaticMeshInMemoryGeometryIdentity(
            nestedParts[0]->modelRenderable().meshInstances());
    const auto firstGpuKey =
        parts[0]->modelRenderable().gpuModelCacheKey();
    const auto sharedGpuKey =
        parts[2]->modelRenderable().gpuModelCacheKey();
    const auto reflectedGpuKey =
        parts[1]->modelRenderable().gpuModelCacheKey();
    passed &= expect(partShapeKey.geometryFingerprint ==
                         sameShapeKey.geometryFingerprint &&
                         partShapeKey.geometryFingerprint !=
                             derivedShapeKey.geometryFingerprint &&
                         firstRamGeometryIdentity ==
                             sharedRamGeometryIdentity &&
                         firstRamGeometryIdentity !=
                             derivedRamGeometryIdentity &&
                         firstGpuKey && sharedGpuKey && reflectedGpuKey &&
                         *firstGpuKey == *sharedGpuKey &&
                         !(*firstGpuKey == *reflectedGpuKey),
                     "CPU/GPU/physics cache identities do not follow derived geometry");

    bool allPrimitivesPickToOwner = true;
    for (const MeshInstance& instance :
         parts[0]->modelRenderable().meshInstances()) {
        const auto& vertices = instance.mesh.vertexData();
        const auto& indices = instance.mesh.indexData();
        if (indices.size() < 3) {
            allPrimitivesPickToOwner = false;
            continue;
        }
        const glm::vec3 localCenter =
            (vertices[indices[0]].pos + vertices[indices[1]].pos +
             vertices[indices[2]].pos) / 3.0f;
        const glm::vec3 worldCenter = glm::vec3(
            parts[0]->worldTransformMatrix() * glm::vec4(localCenter, 1.0f));
        allPrimitivesPickToOwner = allPrimitivesPickToOwner &&
            editor_picking::pickClosestObject(
                scene, {worldCenter + glm::vec3(0, 0, 5),
                        glm::vec3(0, 0, -1)}) == parts[0];
    }
    passed &= expect(allPrimitivesPickToOwner,
                     "viewport picking did not resolve every source primitive to its owning object");

    parts[0]->position += glm::vec3(0.25f, 0.5f, 1.0f);
    parts[0]->rotation = {3.0f, 7.0f, 13.0f};
    parts[0]->scale = {1.0f, 1.5f, 0.75f};
    parts[0]->physics.sphereRadius = 0.75f;

    TypeRegistry registry;
    passed &= expect(static_cast<bool>(registerEngineTypes(registry)),
                     "engine types did not register for hierarchy persistence");
    std::string duplicateId;
    auto duplicate = std::make_unique<GameObject>();
    const Result copied = registry.copyAuthoredProperties(*parts[0], *duplicate);
    passed &= expect(static_cast<bool>(copied),
                     "imported object authored references could not be duplicated");
    if (copied) {
        duplicate->position.x += 6.0f;
        GameObject* duplicateRaw = duplicate.get();
        const Result inserted = scene.addGameObject(std::move(duplicate));
        passed &= expect(static_cast<bool>(inserted),
                         "duplicated imported object could not enter the scene");
        if (inserted) {
            const Result parented = scene.reparentGameObject(
                *duplicateRaw, containers[0], Scene::ReparentMode::PreserveLocal);
            duplicateId = duplicateRaw->persistentId;
            passed &= expect(static_cast<bool>(parented) &&
                                 duplicateId != parts[0]->persistentId &&
                                 duplicateRaw->modelRenderable().meshInstances()[0]
                                         .mesh.sharedGeometry ==
                                     parts[0]->modelRenderable().meshInstances()[0]
                                         .mesh.sharedGeometry,
                             "duplicate lost identity, parent placement, or shared geometry");
        }
    }

    PhysicsServer physicsServer;
    const Result physicsInitialization = physicsServer.initialize();
    passed &= expect(static_cast<bool>(physicsInitialization),
                     "PhysicsServer could not initialize for imported hierarchy");
    if (physicsInitialization) {
        const Result runtime = physicsServer.beginRuntimeSession(scene);
        passed &= expect(static_cast<bool>(runtime),
                         "Play/Simulate could not create imported static bodies: " +
                             runtime.error());
        if (runtime) physicsServer.endRuntimeSession();
        physicsServer.shutdown();
    }

    TestScene runtimeCopy;
    const Result copiedScene = SceneSerializer::copyAuthoredState(
        scene, runtimeCopy, registry);
    passed &= expect(static_cast<bool>(copiedScene) &&
                         runtimeCopy.gameObjects().size() ==
                             scene.gameObjects().size(),
                     "runtime scene reconstruction lost imported objects");

    nlohmann::json document;
    const Result serialized =
        SceneSerializer::serializeAuthored(scene, registry, document);
    passed &= expect(static_cast<bool>(serialized),
                     "imported hierarchy could not be serialized");
    if (serialized) {
        const std::string removedIdentity = secondReference.nodeIdentity;
        const std::string editedId = parts[0]->persistentId;
        auto& records = document["objects"];
        records.erase(std::remove_if(records.begin(), records.end(),
                                    [&](const nlohmann::json& record) {
            return record.at("id") == parts[1]->persistentId;
        }), records.end());
        TestScene reloaded;
        SceneLoadData loadData;
        const Result loaded = SceneSerializer::applyDocument(
            document, reloaded, registry, loadData);
        passed &= expect(static_cast<bool>(loaded),
                         "saved imported hierarchy could not be reloaded: " +
                             loaded.error());
        if (loaded) {
            bool deletedNodeReturned = false;
            const GameObject* restoredEdited =
                reloaded.findGameObject(editedId);
            const GameObject* restoredDuplicate =
                reloaded.findGameObject(duplicateId);
            for (const auto& object : reloaded.gameObjects()) {
                if (object && object->importedAssetReference().nodeIdentity ==
                                  removedIdentity) {
                    deletedNodeReturned = true;
                }
            }
            passed &= expect(!deletedNodeReturned &&
                                 restoredEdited != nullptr &&
                                 restoredDuplicate != nullptr &&
                                 near(restoredEdited->position.x,
                                      parts[0]->position.x) &&
                                 near(restoredEdited->scale.y,
                                      parts[0]->scale.y),
                             "scene load regenerated deleted nodes or lost authored transforms");
        }
    }

    const std::size_t originalObjectCount = scene.gameObjects().size();
    model_import::ImportResult failedImport;
    const Result failed = model_import::importHierarchy(
        scene, directory / "missing.gltf", options, failedImport);
    passed &= expect(!failed && failedImport.root == nullptr &&
                         scene.gameObjects().size() == originalObjectCount,
                     "failed import partially changed the target scene");
    return passed;
}

bool testOtherFormats(const std::filesystem::path& directory) {
    bool passed = true;
    const auto glbPath = directory / "hierarchy.glb";
    writeGlb(glbPath);
    TestScene glbScene;
    model_import::Options options;
    options.generateStaticColliders = true;
    model_import::ImportResult glbResult;
    const Result glbLoad = model_import::importHierarchy(
        glbScene, glbPath, options, glbResult);
    passed &= expect(static_cast<bool>(glbLoad) && glbResult.objectCount > 1,
                     "hierarchy-aware GLB import failed: " + glbLoad.error());

    const auto objPath = directory / "single.obj";
    writeObj(objPath);
    TestScene objScene;
    model_import::ImportResult objResult;
    const Result objLoad = model_import::importHierarchy(
        objScene, objPath, options, objResult);
    bool hasRenderableObject = false;
    for (const auto& object : objScene.gameObjects()) {
        hasRenderableObject = hasRenderableObject ||
            (object && !object->modelRenderable().meshInstances().empty());
    }
    passed &= expect(static_cast<bool>(objLoad) && hasRenderableObject,
                     "single-object OBJ did not import through hierarchy path: " +
                         objLoad.error());

    const auto groupedObjPath = directory / "grouped.obj";
    writeGroupedObj(groupedObjPath);
    TestScene groupedObjScene;
    model_import::ImportResult groupedObjResult;
    const Result groupedObjLoad = model_import::importHierarchy(
        groupedObjScene, groupedObjPath, options, groupedObjResult);
    const auto first = objectsNamed(groupedObjScene, "FirstObject");
    const auto second = objectsNamed(groupedObjScene, "SecondObject");
    passed &= expect(static_cast<bool>(groupedObjLoad) && first.size() == 1 &&
                         second.size() == 1 &&
                         !first[0]->modelRenderable().meshInstances().empty() &&
                         !second[0]->modelRenderable().meshInstances().empty(),
                     "Assimp OBJ object groups did not remain independently addressable: " +
                         groupedObjLoad.error());
    return passed;
}

bool testStaticPhysicsAncestorRejection(
    const std::filesystem::path& directory) {
    const std::filesystem::path fixtureDirectory = directory / "invalid-physics";
    std::filesystem::create_directories(fixtureDirectory);
    const auto modelPath = fixtureDirectory / "mesh-parent.gltf";
    writeMeshBearingAncestorGltf(modelPath);

    TestScene scene;
    model_import::Options options;
    options.generateStaticColliders = true;
    model_import::ImportResult imported;
    const Result result =
        model_import::importHierarchy(scene, modelPath, options, imported);
    return expect(!result && imported.root == nullptr &&
                      imported.objectCount == 0 && scene.gameObjects().empty() &&
                      result.error().find("mesh-bearing descendants") !=
                          std::string::npos,
                  "unsupported static-body ancestry did not fail atomically with a useful diagnostic");
}

}  // namespace

int main() {
    CurrentPathGuard sourceDirectory(DUNAMIS_SOURCE_DIR);
    JoltGuard jolt;
    const std::filesystem::path directory = makeFixtureDirectory();
    bool passed = testHierarchyImport(directory);
    passed &= testOtherFormats(directory);
    passed &= testStaticPhysicsAncestorRejection(directory);
    std::error_code cleanupError;
    std::filesystem::remove_all(directory, cleanupError);
    if (cleanupError) {
        std::cerr << "Failed to remove test fixture directory: "
                  << cleanupError.message() << '\n';
        passed = false;
    }
    if (!passed) return 1;
    std::cout << "Hierarchical model import tests passed\n";
    return 0;
}
