#include "assets/model_import_policy.h"
#include "physics/collision_shapes.h"
#include "scene/game_object.h"
#include "scene/model_renderable.h"

#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Core/Factory.h>
#include <Jolt/RegisterTypes.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool near(float actual, float expected, float tolerance = 0.0001f) {
    return std::fabs(actual - expected) <= tolerance;
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

std::filesystem::path makeFixtureDirectory() {
    const auto suffix = std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count();
    const auto directory = std::filesystem::temp_directory_path() /
                           ("dunamis-model-import-units-" +
                            std::to_string(suffix));
    std::filesystem::create_directories(directory);
    return directory;
}

void writeObj(const std::filesystem::path& path) {
    std::ofstream file(path);
    file << "v 0 0 0\n"
            "v 2 0 0\n"
            "v 0 1 0\n"
            "f 1 2 3\n";
}

void writeGltf(const std::filesystem::path& path) {
    std::ofstream binary(path.parent_path() / "meter-triangle.bin",
                         std::ios::binary);
    const std::array<float, 9> positions = {
        0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<std::uint16_t, 3> indices = {0, 1, 2};
    binary.write(reinterpret_cast<const char*>(positions.data()),
                 sizeof(positions));
    binary.write(reinterpret_cast<const char*>(indices.data()),
                 sizeof(indices));

    std::ofstream model(path);
    model << R"json({
"asset":{"version":"2.0"},
"buffers":[{"uri":"meter-triangle.bin","byteLength":42}],
"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],
"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],
"materials":[{}],
"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],
"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0
})json";
}

void appendUint32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
    bytes.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
}

void writeGlb(const std::filesystem::path& path) {
    const std::array<float, 9> positions = {
        0.0f, 0.0f, 0.0f, 2.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    const std::array<std::uint16_t, 3> indices = {0, 1, 2};
    std::vector<std::uint8_t> binary;
    binary.reserve(44);
    for (const float value : positions) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        binary.insert(binary.end(), bytes, bytes + sizeof(value));
    }
    for (const std::uint16_t value : indices) {
        const auto* bytes = reinterpret_cast<const std::uint8_t*>(&value);
        binary.insert(binary.end(), bytes, bytes + sizeof(value));
    }
    while (binary.size() % 4U != 0U) binary.push_back(0U);

    std::string json = R"json({"asset":{"version":"2.0"},"scene":0,"scenes":[{"nodes":[0]}],"nodes":[{"mesh":0}],"meshes":[{"primitives":[{"attributes":{"POSITION":0},"indices":1,"material":0}]}],"materials":[{}],"buffers":[{"byteLength":42}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36},{"buffer":0,"byteOffset":36,"byteLength":6}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[0,0,0],"max":[2,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}]} )json";
    while (json.size() % 4U != 0U) json.push_back(' ');

    std::vector<std::uint8_t> file;
    file.reserve(12U + 8U + json.size() + 8U + binary.size());
    appendUint32(file, 0x46546c67U);  // glTF
    appendUint32(file, 2U);
    appendUint32(file, static_cast<std::uint32_t>(12U + 8U + json.size() +
                                                   8U + binary.size()));
    appendUint32(file, static_cast<std::uint32_t>(json.size()));
    appendUint32(file, 0x4e4f534aU);  // JSON
    file.insert(file.end(), json.begin(), json.end());
    appendUint32(file, static_cast<std::uint32_t>(binary.size()));
    appendUint32(file, 0x004e4942U);  // BIN
    file.insert(file.end(), binary.begin(), binary.end());

    std::ofstream output(path, std::ios::binary);
    output.write(reinterpret_cast<const char*>(file.data()),
                 static_cast<std::streamsize>(file.size()));
}

void writeFbx(const std::filesystem::path& path, float unitScaleFactor) {
    std::ofstream file(path);
    file << "; FBX 7.4.0 project file\n"
            "FBXHeaderExtension:  {\n"
            "\tFBXHeaderVersion: 1003\n"
            "\tFBXVersion: 7400\n"
            "\tCreator: \"Dunamis test fixture\"\n"
            "}\n"
            "GlobalSettings:  {\n"
            "\tVersion: 1000\n"
            "\tProperties70:  {\n"
            "\t\tP: \"UpAxis\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"UpAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"FrontAxis\", \"int\", \"Integer\", \"\",2\n"
            "\t\tP: \"FrontAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"CoordAxis\", \"int\", \"Integer\", \"\",0\n"
            "\t\tP: \"CoordAxisSign\", \"int\", \"Integer\", \"\",1\n"
            "\t\tP: \"UnitScaleFactor\", \"double\", \"Number\", \"\","
         << unitScaleFactor
         << "\n"
            "\t\tP: \"OriginalUnitScaleFactor\", \"double\", \"Number\", \"\","
         << unitScaleFactor
         << "\n"
            "\t}\n"
            "}\n"
            "Definitions:  {\n"
            "\tVersion: 100\n"
            "\tCount: 3\n"
            "\tObjectType: \"Model\" { Count: 1 }\n"
            "\tObjectType: \"Geometry\" { Count: 1 }\n"
            "\tObjectType: \"Material\" { Count: 1 }\n"
            "}\n"
            "Objects:  {\n"
            "\tGeometry: 1, \"Geometry::Triangle\", \"Mesh\" {\n"
            "\t\tGeometryVersion: 124\n"
            "\t\tVertices: *9 { a: 0,0,0,100,0,0,0,100,0 }\n"
            "\t\tPolygonVertexIndex: *3 { a: 0,1,-3 }\n"
            "\t}\n"
            "\tModel: 2, \"Model::Triangle\", \"Mesh\" { Version: 232 }\n"
            "\tMaterial: 3, \"Material::Material\", \"\" {\n"
            "\t\tVersion: 102\n"
            "\t\tShadingModel: \"phong\"\n"
            "\t}\n"
            "}\n"
            "Connections:  {\n"
            "\tC: \"OO\",1,2\n"
            "\tC: \"OO\",2,0\n"
            "\tC: \"OO\",3,2\n"
            "}\n";
}

struct Bounds {
    glm::vec3 minimum{0.0f};
    glm::vec3 maximum{0.0f};
};

bool loadModelBounds(const std::filesystem::path& path, Bounds& output) {
    GameObject object;
    const std::string modelPath = path.string();
    object.modelPath = modelPath.c_str();
    const Result result = object.loadModel();
    if (!result) {
        std::cerr << "Model load failed for " << path << ": " << result.error()
                  << '\n';
        return false;
    }
    bool found = false;
    for (const MeshInstance& instance : object.modelRenderable().meshInstances()) {
        if (!instance.mesh.bounds.valid) continue;
        if (!found) {
            output.minimum = instance.mesh.bounds.minimum;
            output.maximum = instance.mesh.bounds.maximum;
            found = true;
        } else {
            output.minimum = glm::min(output.minimum, instance.mesh.bounds.minimum);
            output.maximum = glm::max(output.maximum, instance.mesh.bounds.maximum);
        }
    }
    return found;
}

bool testImportNormalization(const std::filesystem::path& directory) {
    const auto objPath = directory / "meter-triangle.obj";
    const auto gltfPath = directory / "meter-triangle.gltf";
    const auto glbPath = directory / "meter-triangle.glb";
    const auto fbxU1Path = directory / "meter-triangle-u1.fbx";
    const auto fbxU100Path = directory / "meter-triangle-u100.fbx";
    writeObj(objPath);
    writeGltf(gltfPath);
    writeGlb(glbPath);
    writeFbx(fbxU1Path, 1.0f);
    writeFbx(fbxU100Path, 100.0f);

    bool passed = true;
    for (const auto& path : {objPath, gltfPath, glbPath}) {
        Bounds bounds;
        passed &= expect(loadModelBounds(path, bounds),
                         "meter-native model did not load");
        passed &= expect(bounds.minimum == glm::vec3(0.0f) &&
                             near(bounds.maximum.x, 2.0f) &&
                             near(bounds.maximum.y, 1.0f),
                         "meter-native glTF/GLB/OBJ coordinates were rescaled");
    }

    Bounds fbxU1Bounds;
    Bounds fbxU100Bounds;
    passed &= expect(loadModelBounds(fbxU1Path, fbxU1Bounds) &&
                         loadModelBounds(fbxU100Path, fbxU100Bounds),
                     "FBX unit fixtures did not load");
    passed &= expect(near(fbxU1Bounds.maximum.x, 1.0f) &&
                         near(fbxU1Bounds.maximum.y, 1.0f) &&
                         near(fbxU100Bounds.maximum.x, 100.0f) &&
                         near(fbxU100Bounds.maximum.y, 100.0f),
                     "FBX source units were not converted exactly once");

    const auto u1Migration =
        model_loading::inspectLegacyAssetScaleMigration(fbxU1Path);
    const auto u100Migration =
        model_loading::inspectLegacyAssetScaleMigration(fbxU100Path);
    const auto objMigration =
        model_loading::inspectLegacyAssetScaleMigration(objPath);
    passed &= expect(u1Migration.known && near(u1Migration.scaleFactor, 1.0f) &&
                         u100Migration.known &&
                         near(u100Migration.scaleFactor, 0.01f) &&
                         objMigration.known &&
                         near(objMigration.scaleFactor, 0.01f),
                     "legacy scale compatibility did not use format metadata");
    return passed;
}

bool testRenderPhysicsAgreement(const std::filesystem::path& path) {
    GameObject object;
    const std::string modelPath = path.string();
    object.modelPath = modelPath.c_str();
    object.scale = {2.0f, 3.0f, 1.0f};
    object.physics.enabled = true;
    object.physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    const Result load = object.loadModel();
    if (!expect(static_cast<bool>(load), "physics agreement model did not load")) {
        return false;
    }

    const Mesh::Bounds& renderBounds =
        object.modelRenderable().meshInstances().front().mesh.bounds;
    physics::CookedShape cooked;
    const Result cook = physics::buildGameObjectShape(object, cooked);
    if (!expect(static_cast<bool>(cook), "normalized mesh could not be cooked")) {
        return false;
    }
    const JPH::AABox physicsBounds = cooked.shape->GetLocalBounds();
    return expect(renderBounds.valid && near(renderBounds.maximum.x, 2.0f) &&
                      near(renderBounds.maximum.y, 1.0f) &&
                      near(physicsBounds.mMax.GetX(), 4.0f) &&
                      near(physicsBounds.mMax.GetY(), 3.0f),
                  "render and physics did not use the same meter geometry");
}

}  // namespace

int main() {
    CurrentPathGuard sourceDirectory(DUNAMIS_SOURCE_DIR);
    const std::filesystem::path directory = makeFixtureDirectory();
    bool passed = testImportNormalization(directory);
    JPH::RegisterDefaultAllocator();
    JPH::Factory::sInstance = new JPH::Factory();
    JPH::RegisterTypes();
    passed &= testRenderPhysicsAgreement(directory / "meter-triangle.gltf");
    JPH::UnregisterTypes();
    delete JPH::Factory::sInstance;
    JPH::Factory::sInstance = nullptr;
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return passed ? 0 : 1;
}
