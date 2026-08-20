#include "scene/scene_manager.h"
#include "scene/scene_serializer.h"
#include "scene/type_registry.h"
#include "input/input_manager.h"

#include <chrono>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

namespace {

class CustomObject final : public GameObject {
public:
    float spinSpeed = 0.0f;
    int value = 0;
    unsigned count = 0;
    bool enabled = true;
    double precision = 0.0;
    glm::vec2 range{0.0f};
    glm::vec4 tint{1.0f};
    std::string tag;
    float runtimeOnly = 0.0f;
};

Result registerCustom(TypeRegistry& registry) {
    Result result = registry.registerType<CustomObject>(
        "CustomObject", "GameObject",
        [] { return std::make_unique<CustomObject>(); });
    if (!result) return result;
#define REGISTER_CUSTOM(name) \
    result = registry.registerProperty("CustomObject", #name, &CustomObject::name); \
    if (!result) return result
    REGISTER_CUSTOM(spinSpeed);
    REGISTER_CUSTOM(value);
    REGISTER_CUSTOM(count);
    REGISTER_CUSTOM(enabled);
    REGISTER_CUSTOM(precision);
    REGISTER_CUSTOM(range);
    REGISTER_CUSTOM(tint);
    REGISTER_CUSTOM(tag);
#undef REGISTER_CUSTOM
    return registry.registerProperty(
        "CustomObject", "runtimeOnly", &CustomObject::runtimeOnly, false);
}

class PersistenceScene final : public Scene {
public:
    static Result registerTypes(TypeRegistry& registry) {
        return registerCustom(registry);
    }

    void init() override {
        auto base = std::make_unique<GameObject>();
        base->persistentId = "base";
        base->name = "Base";
        require(base->setAuthoredTexturePath("textures/override.png"));
        require(addGameObject(std::move(base)));

        auto point = std::make_unique<PointLight>();
        point->persistentId = "point";
        point->color = {0.2f, 0.4f, 0.6f};
        point->intensity = 7.0f;
        require(addGameObject(std::move(point)));

        auto directional = std::make_unique<DirectionalLight>();
        directional->persistentId = "sun";
        directional->rotation = {25.0f, -40.0f, 3.0f};
        directional->shadow.halfExtent = 321.0f;
        require(addGameObject(std::move(directional)));

        auto camera = std::make_unique<Camera>();
        camera->persistentId = "camera";
        camera->front = {0.0f, 0.0f, -1.0f};
        Camera* cameraObserver = camera.get();
        require(addGameObject(std::move(camera)));
        require(setActiveCameraReference(cameraObserver));

        auto custom = std::make_unique<CustomObject>();
        custom->persistentId = "custom";
        custom->position = {1.0f, 2.0f, 3.0f};
        custom->spinSpeed = 180.0f;
        custom->value = 42;
        custom->count = 9;
        custom->precision = 0.125;
        custom->range = {2.0f, 8.0f};
        custom->tint = {0.1f, 0.2f, 0.3f, 0.4f};
        custom->tag = "metadata-only";
        custom->runtimeOnly = 99.0f;
        require(addGameObject(std::move(custom)));

        require(setBackgroundColor({0.1f, 0.2f, 0.3f, 1.0f}));
        require(setAmbientLight({0.4f, 0.5f, 0.6f}, 0.7f));
    }
    void start() override {}
    void update() override {}

private:
    static void require(Result result) {
        if (!result) throw std::runtime_error(result.error());
    }
};

class EmptyScene final : public Scene {
public:
    void init() override {}
    void start() override {}
    void update() override {}
};

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

nlohmann::json readSceneDocument(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    nlohmann::json document;
    stream >> document;
    return document;
}

const nlohmann::json* findObjectRecord(
    const nlohmann::json& document, const std::string& persistentId) {
    for (const auto& object : document.at("objects")) {
        if (object.at("id") == persistentId) return &object;
    }
    return nullptr;
}

TypeRegistry makeRegistry(bool& passed) {
    TypeRegistry registry;
    passed &= expect(static_cast<bool>(registerEngineTypes(registry)),
                     "engine type registration failed");
    passed &= expect(static_cast<bool>(registerCustom(registry)),
                     "custom type registration failed");
    return registry;
}

bool registryTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    const TypeDescriptor* custom = registry.find("CustomObject");
    passed &= expect(custom && registry.isA(*custom, "GameObject"),
                     "custom type did not inherit GameObject metadata");
    const auto properties = registry.authoredProperties(*custom);
    bool hasPosition = false;
    bool hasSpin = false;
    bool hasRuntime = false;
    for (const PropertyDescriptor* property : properties) {
        hasPosition |= property->name == "position";
        hasSpin |= property->name == "spinSpeed";
        hasRuntime |= property->name == "runtimeOnly";
    }
    passed &= expect(hasPosition && hasSpin && !hasRuntime,
                     "property inheritance/authored filtering failed");
    passed &= expect(custom->factory &&
                         dynamic_cast<CustomObject*>(custom->factory().get()),
                     "custom factory did not construct the correct type");
    passed &= expect(!registry.registerType<CustomObject>("Again"),
                     "duplicate C++ type registration was accepted");
    passed &= expect(!registry.registerProperty(
                         "CustomObject", "spinSpeed", &CustomObject::spinSpeed),
                     "duplicate property registration was accepted");
    passed &= expect(!registry.registerProperty(
                         "CustomObject", "position", &CustomObject::position),
                     "inherited property collision was accepted");
    return passed;
}

bool serializerTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    PersistenceScene source;
    source.name = "Persistence Test";
    source.init();
    Camera editor;
    editor.position = {11.0f, 12.0f, 13.0f};
    editor.front = {-1.0f, 0.0f, 0.0f};

    nlohmann::json document;
    Result result = SceneSerializer::serializeFull(source, registry, editor, document);
    passed &= expect(static_cast<bool>(result), "full scene serialization failed: " + result.error());
    passed &= expect(document["formatVersion"] == 1 &&
                         document["editor"].contains("camera"),
                     "version/editor camera were not serialized");
    const auto customRecord = *std::find_if(
        document["objects"].begin(), document["objects"].end(),
        [](const auto& object) { return object["id"] == "custom"; });
    passed &= expect(customRecord["properties"].contains("position") &&
                         customRecord["properties"].contains("spinSpeed") &&
                         !customRecord["properties"].contains("runtimeOnly"),
                     "generic custom/inherited property serialization failed");
    passed &= expect(document["activeCamera"]["ownerId"] == "camera" &&
                         document["activeCamera"]["kind"] == "object",
                     "active camera logical reference was not serialized");

    PersistenceScene candidate;
    candidate.name = "Defaults";
    candidate.init();
    static_cast<CustomObject*>(candidate.findGameObject("custom"))->spinSpeed = 1.0f;
    SceneLoadData load;
    result = SceneSerializer::applyDocument(document, candidate, registry, load);
    passed &= expect(static_cast<bool>(result), "scene round trip failed: " + result.error());
    const auto* restored = dynamic_cast<const CustomObject*>(candidate.findGameObject("custom"));
    passed &= expect(restored && restored->spinSpeed == 180.0f &&
                         restored->value == 42 && restored->count == 9 &&
                         restored->enabled && restored->precision == 0.125 &&
                         restored->range.y == 8.0f && restored->tint.w == 0.4f &&
                         restored->tag == "metadata-only" &&
                         restored->runtimeOnly == 99.0f &&
                         restored->position.x == 1.0f,
                     "custom object did not round trip through metadata");
    const auto* point = dynamic_cast<const PointLight*>(candidate.findGameObject("point"));
    const auto* sun = dynamic_cast<const DirectionalLight*>(candidate.findGameObject("sun"));
    const auto* camera = dynamic_cast<const Camera*>(candidate.findGameObject("camera"));
    passed &= expect(point && point->intensity == 7.0f &&
                         sun && sun->shadow.halfExtent == 321.0f &&
                         camera && candidate.activeCamera() == camera &&
                         candidate.ambientIntensity() == 0.7f,
                     "built-in object/camera/scene state did not round trip");
    passed &= expect(candidate.findGameObject("base")->authoredTexturePath() ==
                         "textures/override.png",
                     "logical texture override path did not round trip");
    passed &= expect(load.editorCamera && load.editorCamera->position.x == 11.0f,
                     "editor camera did not round trip");

    nlohmann::json unknownProperty = document;
    unknownProperty["objects"][0]["properties"]["futureProperty"] = 5;
    PersistenceScene unknownCandidate;
    unknownCandidate.init();
    SceneLoadData unknownLoad;
    result = SceneSerializer::applyDocument(
        unknownProperty, unknownCandidate, registry, unknownLoad);
    passed &= expect(result && !unknownLoad.warnings.empty(),
                     "unknown property was not warned and ignored");

    nlohmann::json noEditor = document;
    noEditor.erase("editor");
    PersistenceScene noEditorCandidate;
    noEditorCandidate.init();
    SceneLoadData noEditorLoad;
    result = SceneSerializer::applyDocument(
        noEditor, noEditorCandidate, registry, noEditorLoad);
    passed &= expect(result && !noEditorLoad.editorCamera,
                     "missing optional editor camera rejected the scene");

    nlohmann::json invalidEditor = document;
    invalidEditor["editor"]["camera"]["front"] = {0.0f, 0.0f, 0.0f};
    PersistenceScene invalidEditorCandidate;
    invalidEditorCandidate.init();
    SceneLoadData invalidEditorLoad;
    result = SceneSerializer::applyDocument(
        invalidEditor, invalidEditorCandidate, registry, invalidEditorLoad);
    passed &= expect(result && !invalidEditorLoad.editorCamera &&
                         !invalidEditorLoad.warnings.empty(),
                     "invalid editor camera did not use a safe warning fallback");

    nlohmann::json dynamicObject = document;
    nlohmann::json extra = customRecord;
    extra["id"] = "factory_extra";
    dynamicObject["objects"].push_back(extra);
    PersistenceScene dynamicCandidate;
    dynamicCandidate.init();
    SceneLoadData dynamicLoad;
    result = SceneSerializer::applyDocument(
        dynamicObject, dynamicCandidate, registry, dynamicLoad);
    passed &= expect(result &&
                         dynamic_cast<CustomObject*>(
                             dynamicCandidate.findGameObject("factory_extra")),
                     "registered factory did not construct a saved dynamic object");

    nlohmann::json unknownType = document;
    unknownType["objects"][0]["type"] = "MissingType";
    PersistenceScene unknownTypeCandidate;
    unknownTypeCandidate.init();
    SceneLoadData ignored;
    result = SceneSerializer::applyDocument(
        unknownType, unknownTypeCandidate, registry, ignored);
    passed &= expect(!result, "unknown required object type was accepted");

    nlohmann::json wrongType = document;
    wrongType["objects"][0]["properties"]["position"] = "bad";
    PersistenceScene untouched;
    untouched.init();
    const glm::vec3 originalPosition = untouched.findGameObject("base")->position;
    result = SceneSerializer::applyDocument(wrongType, untouched, registry, ignored);
    passed &= expect(!result && untouched.findGameObject("base")->position == originalPosition,
                     "wrong property type was accepted or mutated its target property");

    nlohmann::json unsupported = document;
    unsupported["formatVersion"] = 2;
    PersistenceScene versionCandidate;
    versionCandidate.init();
    result = SceneSerializer::applyDocument(unsupported, versionCandidate, registry, ignored);
    passed &= expect(!result, "unsupported format version was accepted");

    nlohmann::json duplicate = document;
    duplicate["objects"].push_back(duplicate["objects"][0]);
    PersistenceScene duplicateCandidate;
    duplicateCandidate.init();
    result = SceneSerializer::applyDocument(duplicate, duplicateCandidate, registry, ignored);
    passed &= expect(!result, "duplicate persistent ID was accepted");

    EmptyScene missingIdScene;
    auto missingId = std::make_unique<GameObject>();
    (void)missingIdScene.addGameObject(std::move(missingId));
    nlohmann::json object;
    result = SceneSerializer::serializeFull(
        missingIdScene, registry, editor, object);
    passed &= expect(!result, "missing required persistent ID was accepted");
    return passed;
}

bool managerDirtyTests() {
    bool passed = true;
    SceneManager manager;
    Result result = manager.initialize<PersistenceScene>(
        "Persistence Test", std::make_shared<InputManager>());
    passed &= expect(static_cast<bool>(result), "persistence manager initialization failed");
    const auto directory = std::filesystem::temp_directory_path() /
        ("dunamis-persistence-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "test.scene.json";
    manager.setCurrentScenePath(path);
    Camera editor;
    auto* editingCustom = static_cast<CustomObject*>(
        manager.editingScene()->findGameObject("custom"));
    auto* editingCamera = static_cast<Camera*>(
        manager.editingScene()->findGameObject("camera"));
    editingCustom->spinSpeed = 270.0f;
    editingCustom->runtimeOnly = 777.0f;
    const bool configuredCameraFov = editingCamera->setFov(90.0f);
    result = manager.prepareRuntimeScene();
    const auto* runtimeCustom = result
        ? static_cast<const CustomObject*>(
              manager.runtimeScene()->findGameObject("custom"))
        : nullptr;
    const auto* runtimeCamera = result
        ? static_cast<const Camera*>(
              manager.runtimeScene()->findGameObject("camera"))
        : nullptr;
    passed &= expect(result && runtimeCustom &&
                         runtimeCustom->spinSpeed == 270.0f &&
                         runtimeCustom->runtimeOnly == 99.0f &&
                         configuredCameraFov && runtimeCamera &&
                         runtimeCamera->fov() == 90.0f,
                     "generic runtime transfer omitted authored custom state or copied runtime-only state");
    manager.cancelPreparedRuntimeScene();
    result = manager.saveEditingScene(editor);
    passed &= expect(result && !manager.hasUnsavedChanges(),
                     "successful save did not establish a clean baseline");
    editor.position.x = 50.0f;
    passed &= expect(!manager.hasUnsavedChanges(),
                     "editor camera movement made authored state dirty");
    manager.editingScene()->findGameObject("point")->position.x += 1.0f;
    passed &= expect(manager.hasUnsavedChanges(),
                     "object transform change was not dirty");
    result = manager.saveEditingScene(editor);
    passed &= expect(result && !manager.hasUnsavedChanges(),
                     "second save did not clear dirty state");
    static_cast<PointLight*>(manager.editingScene()->findGameObject("point"))->color.r = 0.9f;
    passed &= expect(manager.hasUnsavedChanges(),
                     "light color change was not dirty");
    static_cast<CustomObject*>(manager.editingScene()->findGameObject("custom"))->spinSpeed = 999.0f;
    passed &= expect(manager.hasUnsavedChanges(),
                     "custom reflected property change was not dirty");

    result = manager.prepareEditingSceneLoad(path);
    passed &= expect(static_cast<bool>(result), "saved scene could not be prepared");
    result = manager.commitPreparedEditingSceneLoad();
    manager.finishEditingSceneLoad();
    passed &= expect(result && !manager.hasUnsavedChanges(),
                     "successful load did not restore a clean baseline");

    const Scene* stableScene = manager.editingScene();
    { std::ofstream stream(path); stream << "{ malformed"; }
    result = manager.prepareEditingSceneLoad(path);
    passed &= expect(!result && manager.editingScene() == stableScene,
                     "malformed JSON replaced or mutated the current scene");
    result = manager.saveEditingScene(editor);
    passed &= expect(static_cast<bool>(result),
                     "scene could not be restored after malformed-load test");

    nlohmann::json oldDocument;
    { std::ifstream stream(path); stream >> oldDocument; }
    auto& objects = oldDocument["objects"];
    objects.erase(std::remove_if(objects.begin(), objects.end(),
                                 [](const auto& object) {
                                     return object["id"] == "custom";
                                 }), objects.end());
    { std::ofstream stream(path); stream << oldDocument.dump(2); }
    result = manager.prepareEditingSceneLoad(path);
    passed &= expect(static_cast<bool>(result), "older scene document was rejected");
    result = manager.commitPreparedEditingSceneLoad();
    manager.finishEditingSceneLoad();
    passed &= expect(result && manager.editingScene()->findGameObject("custom") &&
                         manager.hasUnsavedChanges(),
                     "new C++ default object did not survive older JSON as dirty");

    manager.shutdown();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return passed;
}

bool managerSavePathTests() {
    bool passed = true;
    SceneManager manager;
    Result result = manager.initialize<PersistenceScene>(
        "Persistence Save Path Test", std::make_shared<InputManager>());
    passed &= expect(static_cast<bool>(result),
                     "save-path manager initialization failed");

    const auto directory = std::filesystem::temp_directory_path() /
        ("dunamis-save-path-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto pathA = directory / "a.scene.json";
    const auto pathB = directory / "b.scene.json";
    const auto pathC = directory / "camera.scene.json";
    const auto blockingParent = directory / "blocking-file";
    const auto invalidPath = blockingParent / "failed.scene.json";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    passed &= expect(!error, "save-path test directory could not be created");
    {
        std::ofstream stream(blockingParent, std::ios::binary);
        stream << "not a directory";
    }

    manager.setCurrentScenePath(pathA);
    Camera editor;
    editor.position = {4.0f, 5.0f, 6.0f};
    result = manager.saveEditingScene(editor);
    passed &= expect(result && std::filesystem::exists(pathA) &&
                         manager.currentScenePath() == pathA &&
                         !manager.hasUnsavedChanges(),
                     "normal save did not remain current-path based");

    manager.editingScene()->findGameObject("point")->position.x += 2.0f;
    passed &= expect(manager.hasUnsavedChanges(),
                     "failed Save As precondition was not dirty");
    result = manager.saveEditingSceneAs(invalidPath, editor);
    passed &= expect(!result && manager.currentScenePath() == pathA &&
                         manager.hasUnsavedChanges(),
                     "failed Save As changed the current path or dirty state");
    result = manager.saveEditingSceneAs(directory, editor);
    passed &= expect(!result && manager.currentScenePath() == pathA,
                     "directory Save As destination was not rejected");
    result = manager.saveEditingScene(editor);
    passed &= expect(result && !manager.hasUnsavedChanges(),
                     "scene could not be cleaned before Save As");

    const Scene* editingScene = manager.editingScene();
    result = manager.saveEditingSceneAs(pathB, editor);
    const nlohmann::json documentB = result
        ? readSceneDocument(pathB)
        : nlohmann::json{};
    const nlohmann::json* customB = result
        ? findObjectRecord(documentB, "custom")
        : nullptr;
    passed &= expect(result && std::filesystem::exists(pathB) &&
                         manager.currentScenePath() == pathB &&
                         manager.editingScene() == editingScene &&
                         !manager.hasUnsavedChanges() &&
                         documentB.contains("editor") &&
                         documentB.at("editor").at("camera").at("position")[0] == 4.0f &&
                         customB != nullptr,
                     "Save As did not write the full scene or establish the new current path");

    auto* custom = static_cast<CustomObject*>(
        manager.editingScene()->findGameObject("custom"));
    custom->spinSpeed = 321.0f;
    passed &= expect(manager.hasUnsavedChanges(),
                     "Save As follow-up authored modification was not dirty");
    result = manager.saveEditingScene(editor);
    const nlohmann::json documentAAfterSave = readSceneDocument(pathA);
    const nlohmann::json documentBAfterSave = readSceneDocument(pathB);
    const nlohmann::json* customAAfterSave =
        findObjectRecord(documentAAfterSave, "custom");
    const nlohmann::json* customBAfterSave =
        findObjectRecord(documentBAfterSave, "custom");
    passed &= expect(result && !manager.hasUnsavedChanges() &&
                         customAAfterSave != nullptr &&
                         customBAfterSave != nullptr &&
                         customAAfterSave->at("properties").at("spinSpeed") != 321.0f &&
                         customBAfterSave->at("properties").at("spinSpeed") == 321.0f,
                     "normal save after Save As did not target the new path");

    editor.position.x = 123.0f;
    passed &= expect(!manager.hasUnsavedChanges(),
                     "editor camera movement made authored state dirty before Save As");
    result = manager.saveEditingSceneAs(pathC, editor);
    const nlohmann::json documentC = result
        ? readSceneDocument(pathC)
        : nlohmann::json{};
    passed &= expect(result && manager.currentScenePath() == pathC &&
                         !manager.hasUnsavedChanges() &&
                         documentC.at("editor").at("camera").at("position")[0] == 123.0f,
                     "camera-only Save As did not preserve the editor camera or clean state");

    manager.shutdown();
    std::filesystem::remove_all(directory, error);
    return passed;
}

}  // namespace

int main() {
    bool passed = registryTests();
    passed &= serializerTests();
    passed &= managerDirtyTests();
    passed &= managerSavePathTests();
    return passed ? 0 : 1;
}
