#include "editor/editor_mutation.h"
#include "scene/scene_manager.h"
#include "scene/scene_serializer.h"
#include "scene/type_registry.h"
#include "scene/character.h"
#include "scene/directional_light.h"
#include "input/input_manager.h"
#include "math/transform_math.h"

#include <chrono>
#include <algorithm>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_set>

namespace {

class CustomObject final : public GameObject {
public:
    inline static constexpr float transferFailureSentinel = -9876.5f;

    float spinSpeed = 0.0f;
    int value = 0;
    unsigned count = 0;
    bool enabled = true;
    double precision = 0.0;
    glm::vec2 range{0.0f};
    glm::vec4 tint{1.0f};
    std::string tag;
    float runtimeOnly = 0.0f;
    float transferOnlyValue = 1.0f;
};

class AttachedCameraObject final : public GameObject {
public:
    AttachedCameraObject() : camera(std::make_unique<Camera>()) {}

    Camera* attachedCamera() noexcept override { return camera.get(); }
    const Camera* attachedCamera() const noexcept override {
        return camera.get();
    }

    std::unique_ptr<Camera> camera;
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
    result = registry.registerProperty(
        "CustomObject", "runtimeOnly", &CustomObject::runtimeOnly,
        PropertyLifecycle::Transient);
    if (!result) return result;
    return registry.registerAccessor<CustomObject, float>(
        "CustomObject", "transferOnlyValue",
        [](const CustomObject& object) { return object.transferOnlyValue; },
        [](CustomObject& object, const float value) {
            if (value == CustomObject::transferFailureSentinel) {
                return Result::failure("intentional transfer failure");
            }
            object.transferOnlyValue = value;
            return Result::success();
        },
        PropertyLifecycle::RuntimeTransferOnly);
}

Result registerAttachedCamera(TypeRegistry& registry) {
    return registry.registerType<AttachedCameraObject>(
        "AttachedCameraObject", "GameObject",
        [] { return std::make_unique<AttachedCameraObject>(); });
}

class PersistenceScene final : public Scene {
public:
    static Result registerTypes(TypeRegistry& registry) {
        return registerCustom(registry);
    }

    void buildDefaults() override {
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
        custom->transferOnlyValue = 17.0f;
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
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

bool expect(bool condition, const std::string& message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool sameVector(const glm::vec3& left, const glm::vec3& right,
                float epsilon = 1.0e-5f) {
    return std::fabs(left.x - right.x) <= epsilon &&
           std::fabs(left.y - right.y) <= epsilon &&
           std::fabs(left.z - right.z) <= epsilon;
}

bool sameMatrix(const glm::mat4& left, const glm::mat4& right,
                float epsilon = 1.0e-5f) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::fabs(left[column][row] - right[column][row]) > epsilon) {
                return false;
            }
        }
    }
    return true;
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

GameObject* addPlainObject(Scene& scene, const std::string& persistentId) {
    auto object = std::make_unique<GameObject>();
    object->persistentId = persistentId;
    GameObject* pointer = object.get();
    return scene.addGameObject(std::move(object)) ? pointer : nullptr;
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
    const auto contains = [](const auto& properties, const std::string& name) {
        return std::any_of(
            properties.begin(), properties.end(),
            [&name](const PropertyDescriptor* property) {
                return property->name == name;
            });
    };
    const auto persisted = registry.persistedProperties(*custom);
    const auto runtimeTransfer = registry.runtimeTransferProperties(*custom);
    const PropertyDescriptor* spin =
        registry.findProperty(*custom, "spinSpeed");
    const PropertyDescriptor* runtimeOnly =
        registry.findProperty(*custom, "runtimeOnly");
    const PropertyDescriptor* transferOnly =
        registry.findProperty(*custom, "transferOnlyValue");
    const TypeDescriptor* gameObject = registry.find("GameObject");
    passed &= expect(contains(persisted, "position") &&
                         contains(persisted, "spinSpeed") &&
                         !contains(persisted, "runtimeOnly") &&
                         !contains(persisted, "transferOnlyValue") &&
                         contains(runtimeTransfer, "position") &&
                         contains(runtimeTransfer, "spinSpeed") &&
                         contains(runtimeTransfer, "physics") &&
                         contains(runtimeTransfer, "transferOnlyValue") &&
                         !contains(runtimeTransfer, "runtimeOnly") &&
                         spin &&
                         spin->lifecycle == PropertyLifecycle::Persisted &&
                         runtimeOnly &&
                         runtimeOnly->lifecycle == PropertyLifecycle::Transient &&
                         !runtimeOnly->read && !runtimeOnly->write &&
                         !runtimeOnly->copy && transferOnly &&
                         transferOnly->lifecycle ==
                             PropertyLifecycle::RuntimeTransferOnly &&
                         !transferOnly->read && !transferOnly->write &&
                         transferOnly->copy && gameObject &&
                         registry.findProperty(*gameObject, "parentId") == nullptr,
                     "property lifecycle filtering or inheritance failed");

    const TypeDescriptor* cameraType = registry.find("Camera");
    const auto cameraPersisted = cameraType
        ? registry.persistedProperties(*cameraType)
        : std::vector<const PropertyDescriptor*>{};
    const auto cameraRuntimeTransfer = cameraType
        ? registry.runtimeTransferProperties(*cameraType)
        : std::vector<const PropertyDescriptor*>{};
    const PropertyDescriptor* fov = cameraType
        ? registry.findProperty(*cameraType, "fov")
        : nullptr;
    const PropertyDescriptor* physics = cameraType
        ? registry.findProperty(*cameraType, "physics")
        : nullptr;
    passed &= expect(cameraType && contains(cameraPersisted, "position") &&
                         contains(cameraPersisted, "front") &&
                         contains(cameraPersisted, "physics") &&
                         contains(cameraPersisted, "fov") &&
                         contains(cameraRuntimeTransfer, "position") &&
                         contains(cameraRuntimeTransfer, "physics") &&
                         contains(cameraRuntimeTransfer, "fov") && fov &&
                         fov->lifecycle == PropertyLifecycle::Persisted &&
                         fov->read && fov->write && fov->copy && physics &&
                         physics->lifecycle == PropertyLifecycle::Persisted &&
                         physics->read && physics->write && physics->copy,
                     "camera lifecycle registration or inherited transfer metadata failed");

    Camera sourceCamera;
    Camera destinationCamera;
    const bool sourceFovConfigured = sourceCamera.setFov(90.0f);
    Result fovCopy = Result::failure("FOV descriptor was not registered");
    if (fov && fov->copy) fovCopy = fov->copy(sourceCamera, destinationCamera);
    passed &= expect(sourceFovConfigured && fovCopy &&
                         destinationCamera.fov() == 90.0f,
                     "camera FOV was not copied through its typed descriptor");
    const float validFov = destinationCamera.fov();
    passed &= expect(!destinationCamera.setFov(0.0f) &&
                         destinationCamera.fov() == validFov,
                     "camera FOV descriptor did not preserve setter validation semantics");

    GameObject sourcePhysics;
    GameObject destinationPhysics;
    sourcePhysics.physics.enabled = true;
    sourcePhysics.physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    sourcePhysics.physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    sourcePhysics.physics.sphereRadius = 4.25f;
    destinationPhysics.physics.enabled = false;
    destinationPhysics.physics.motionType = GameObject::PhysicsMotionType::Static;
    destinationPhysics.physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    destinationPhysics.physics.sphereRadius = 0.5f;
    Result physicsCopy = Result::failure("physics descriptor was not registered");
    if (physics && physics->copy) {
        physicsCopy = physics->copy(sourcePhysics, destinationPhysics);
    }
    passed &= expect(physicsCopy && destinationPhysics.physics.enabled &&
                         destinationPhysics.physics.motionType ==
                             GameObject::PhysicsMotionType::Dynamic &&
                         destinationPhysics.physics.colliderType ==
                             GameObject::PhysicsColliderType::Sphere &&
                         destinationPhysics.physics.sphereRadius == 4.25f,
                     "physics settings were not copied without JSON encoding");
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
    source.buildDefaults();
    auto* sourceCamera = static_cast<Camera*>(source.findGameObject("camera"));
    auto* sourceCustom = static_cast<CustomObject*>(
        source.findGameObject("custom"));
    const Result sourceParenting = source.reparentGameObject(
        *sourceCustom, source.findGameObject("base"),
        Scene::ReparentMode::PreserveLocal);
    passed &= expect(static_cast<bool>(sourceParenting),
                     "could not create serializer hierarchy fixture");
    sourceCustom->physics.enabled = true;
    sourceCustom->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    sourceCustom->physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    sourceCustom->physics.sphereRadius = 4.25f;
    const bool configuredSourceFov = sourceCamera->setFov(90.0f);
    Camera editor;
    editor.position = {11.0f, 12.0f, 13.0f};
    editor.front = {-1.0f, 0.0f, 0.0f};

    nlohmann::json document;
    Result result = SceneSerializer::serializeFull(source, registry, editor, document);
    passed &= expect(static_cast<bool>(result), "full scene serialization failed: " + result.error());
    passed &= expect(document["formatVersion"] == SceneSerializer::formatVersion &&
                         document["editor"].contains("camera"),
                     "version/editor camera were not serialized");
    const auto customRecord = *std::find_if(
        document["objects"].begin(), document["objects"].end(),
        [](const auto& object) { return object["id"] == "custom"; });
    const auto cameraRecord = *std::find_if(
        document["objects"].begin(), document["objects"].end(),
        [](const auto& object) { return object["id"] == "camera"; });
    const nlohmann::json* baseRecord = findObjectRecord(document, "base");
    passed &= expect(customRecord["parentId"] == "base" && baseRecord &&
                         baseRecord->at("parentId").is_null() &&
                         customRecord["properties"].contains("position") &&
                         customRecord["properties"].contains("spinSpeed") &&
                         !customRecord["properties"].contains("runtimeOnly") &&
                         !customRecord["properties"].contains("transferOnlyValue") &&
                         customRecord["properties"].contains("physics") &&
                         cameraRecord["properties"].contains("fov") &&
                         cameraRecord["properties"].contains("physics"),
                     "generic custom/inherited property serialization failed");
    passed &= expect(document["activeCamera"]["ownerId"] == "camera" &&
                         document["activeCamera"]["kind"] == "object",
                     "active camera logical reference was not serialized");

    EmptyScene candidate;
    SceneLoadData load;
    result = SceneSerializer::applyDocument(document, candidate, registry, load);
    passed &= expect(static_cast<bool>(result), "scene round trip failed: " + result.error());
    const auto* restored = dynamic_cast<const CustomObject*>(candidate.findGameObject("custom"));
    passed &= expect(restored && restored->spinSpeed == 180.0f &&
                         restored->value == 42 && restored->count == 9 &&
                         restored->enabled && restored->precision == 0.125 &&
                         restored->range.y == 8.0f && restored->tint.w == 0.4f &&
                         restored->tag == "metadata-only" &&
                         restored->runtimeOnly == 0.0f &&
                         restored->transferOnlyValue == 1.0f &&
                         restored->position.x == 1.0f,
                     "custom object did not round trip through metadata");
    const auto* point = dynamic_cast<const PointLight*>(candidate.findGameObject("point"));
    const auto* sun = dynamic_cast<const DirectionalLight*>(candidate.findGameObject("sun"));
    const auto* camera = dynamic_cast<const Camera*>(candidate.findGameObject("camera"));
    std::size_t directionalLightCount = 0;
    for (const auto& object : candidate.gameObjects()) {
        if (dynamic_cast<const DirectionalLight*>(object.get()) != nullptr) {
            ++directionalLightCount;
        }
    }
    passed &= expect(point && point->intensity == 7.0f &&
                         sun && directionalLightCount == 1 &&
                         sun->shadow.halfExtent == 321.0f &&
                         camera && candidate.activeCamera() == camera &&
                         configuredSourceFov && camera->fov() == 90.0f &&
                         candidate.ambientIntensity() == 0.7f,
                     "built-in object/camera/scene state did not round trip");
    passed &= expect(
        restored && restored->physics.enabled &&
            restored->physics.motionType == GameObject::PhysicsMotionType::Dynamic &&
            restored->physics.colliderType == GameObject::PhysicsColliderType::Sphere &&
            restored->physics.sphereRadius == 4.25f,
        "authored physics configuration did not survive serialize/load");
    passed &= expect(restored && restored->parent() == candidate.findGameObject("base"),
                     "object hierarchy did not survive serialize/load");
    passed &= expect(candidate.findGameObject("base")->authoredTexturePath() ==
                         "textures/override.png",
                     "logical texture override path did not round trip");
    passed &= expect(load.editorCamera && load.editorCamera->position.x == 11.0f,
                     "editor camera did not round trip");

    nlohmann::json v1 = document;
    v1["formatVersion"] = 1;
    for (auto& object : v1["objects"]) {
        object.erase("parentId");
        if (object.at("id") == "custom") object["parentId"] = "base";
    }
    EmptyScene v1Candidate;
    SceneLoadData v1Load;
    result = SceneSerializer::applyDocument(v1, v1Candidate, registry, v1Load);
    const GameObject* v1Custom = v1Candidate.findGameObject("custom");
    const Camera* v1Camera =
        dynamic_cast<const Camera*>(v1Candidate.findGameObject("camera"));
    passed &= expect(result && v1Custom && v1Custom->parent() == nullptr &&
                         v1Camera && v1Candidate.activeCamera() == v1Camera,
                     "v1 scene did not load as roots or restore active camera");

    nlohmann::json unknownProperty = document;
    unknownProperty["objects"][0]["properties"]["futureProperty"] = 5;
    EmptyScene unknownCandidate;
    SceneLoadData unknownLoad;
    result = SceneSerializer::applyDocument(
        unknownProperty, unknownCandidate, registry, unknownLoad);
    passed &= expect(result && !unknownLoad.warnings.empty(),
                     "unknown property was not warned and ignored");

    nlohmann::json noEditor = document;
    noEditor.erase("editor");
    EmptyScene noEditorCandidate;
    SceneLoadData noEditorLoad;
    result = SceneSerializer::applyDocument(
        noEditor, noEditorCandidate, registry, noEditorLoad);
    passed &= expect(result && !noEditorLoad.editorCamera,
                     "missing optional editor camera rejected the scene");

    nlohmann::json invalidEditor = document;
    invalidEditor["editor"]["camera"]["front"] = {0.0f, 0.0f, 0.0f};
    EmptyScene invalidEditorCandidate;
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
    EmptyScene dynamicCandidate;
    SceneLoadData dynamicLoad;
    result = SceneSerializer::applyDocument(
        dynamicObject, dynamicCandidate, registry, dynamicLoad);
    passed &= expect(result &&
                         dynamic_cast<CustomObject*>(
                             dynamicCandidate.findGameObject("factory_extra")),
                     "registered factory did not construct a saved dynamic object");

    nlohmann::json unknownType = document;
    unknownType["objects"][0]["type"] = "MissingType";
    EmptyScene unknownTypeCandidate;
    SceneLoadData ignored;
    result = SceneSerializer::applyDocument(
        unknownType, unknownTypeCandidate, registry, ignored);
    passed &= expect(!result, "unknown required object type was accepted");

    nlohmann::json wrongType = document;
    wrongType["objects"][0]["properties"]["position"] = "bad";
    EmptyScene untouched;
    result = SceneSerializer::applyDocument(wrongType, untouched, registry, ignored);
    passed &= expect(!result,
                     "wrong property type was accepted");

    nlohmann::json unsupported = document;
    unsupported["formatVersion"] = SceneSerializer::formatVersion + 1;
    EmptyScene versionCandidate;
    result = SceneSerializer::applyDocument(unsupported, versionCandidate, registry, ignored);
    passed &= expect(!result, "unsupported format version was accepted");

    nlohmann::json duplicate = document;
    duplicate["objects"].push_back(duplicate["objects"][0]);
    EmptyScene duplicateCandidate;
    result = SceneSerializer::applyDocument(duplicate, duplicateCandidate, registry, ignored);
    passed &= expect(!result, "duplicate persistent ID was accepted");

    EmptyScene generatedIdScene;
    auto generatedObject = std::make_unique<GameObject>();
    generatedObject->name = "Pot";
    GameObject* generatedObjectPointer = generatedObject.get();
    const Result generatedAddResult =
        generatedIdScene.addGameObject(std::move(generatedObject));
    const std::string generatedId = generatedAddResult
                                        ? generatedObjectPointer->persistentId
                                        : std::string{};
    const Result generatedRenameResult = generatedAddResult
        ? editor_mutation::applyName(*generatedObjectPointer, "Flower Pot")
        : Result::failure("generated-ID object could not be added");
    nlohmann::json generatedDocument;
    result = generatedRenameResult
        ? SceneSerializer::serializeFull(
              generatedIdScene, registry, editor, generatedDocument)
        : Result::failure("generated-ID object could not be renamed");
    const nlohmann::json* generatedRecord = result
        ? findObjectRecord(generatedDocument, generatedId)
        : nullptr;
    passed &= expect(generatedAddResult && generatedRenameResult &&
                         !generatedId.empty() &&
                         generatedObjectPointer->persistentId == generatedId &&
                         result &&
                         generatedRecord && generatedRecord->at("id") == generatedId &&
                         generatedRecord->at("properties").at("name") ==
                             "Flower Pot",
                     "Generated persistent ID or authored name was not serialized");

    EmptyScene generatedCandidate;
    SceneLoadData generatedLoad;
    result = result
        ? SceneSerializer::applyDocument(
              generatedDocument, generatedCandidate, registry, generatedLoad)
        : Result::failure("generated-ID object could not be serialized");
    const GameObject* restoredGenerated = result
        ? generatedCandidate.findGameObject(generatedId)
        : nullptr;
    passed &= expect(result && restoredGenerated &&
                         restoredGenerated->persistentId == generatedId &&
                         restoredGenerated->name == "Flower Pot",
                     "Generated identity or renamed name did not survive load");
    return passed;
}

bool hierarchyPersistenceTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    EmptyScene source;
    GameObject* root = addPlainObject(source, "root");
    GameObject* child = addPlainObject(source, "child");
    GameObject* grandchild = addPlainObject(source, "grandchild");
    if (!root || !child || !grandchild) {
        return expect(false, "could not create hierarchy persistence fixture");
    }

    root->position = {20.0f, -3.0f, 11.0f};
    root->rotation = {15.0f, 25.0f, -35.0f};
    root->scale = {2.0f, 1.5f, 0.75f};
    child->position = {-4.0f, 7.0f, 2.5f};
    child->rotation = {-10.0f, 35.0f, 12.0f};
    child->scale = {0.5f, 2.0f, 1.25f};
    grandchild->position = {3.0f, -6.0f, 1.0f};
    grandchild->rotation = {5.0f, -15.0f, 22.0f};
    grandchild->scale = {1.1f, 0.8f, 1.4f};
    const Result parenting =
        source.reparentGameObject(*child, root, Scene::ReparentMode::PreserveLocal);
    const Result grandparenting = source.reparentGameObject(
        *grandchild, child, Scene::ReparentMode::PreserveLocal);
    passed &= expect(parenting && grandparenting,
                     "could not establish multi-level hierarchy fixture");

    const glm::vec3 childLocalPosition = child->position;
    const glm::vec3 childLocalRotation = child->rotation;
    const glm::vec3 childLocalScale = child->scale;
    const glm::vec3 grandchildLocalPosition = grandchild->position;
    const glm::vec3 grandchildLocalRotation = grandchild->rotation;
    const glm::vec3 grandchildLocalScale = grandchild->scale;
    const glm::mat4 childWorld = child->worldTransformMatrix();
    const glm::mat4 grandchildWorld = grandchild->worldTransformMatrix();

    nlohmann::json document;
    Result result = SceneSerializer::serializeAuthored(source, registry, document);
    const nlohmann::json* rootRecord = result
        ? findObjectRecord(document, "root")
        : nullptr;
    const nlohmann::json* childRecord = result
        ? findObjectRecord(document, "child")
        : nullptr;
    const nlohmann::json* grandchildRecord = result
        ? findObjectRecord(document, "grandchild")
        : nullptr;
    passed &= expect(
        result && document.at("formatVersion") == SceneSerializer::formatVersion && rootRecord && childRecord &&
            grandchildRecord && rootRecord->at("parentId").is_null() &&
            childRecord->at("parentId") == "root" &&
            grandchildRecord->at("parentId") == "child",
        "v3 hierarchy records did not serialize structural parentId values");

    EmptyScene candidate;
    SceneLoadData load;
    result = result
        ? SceneSerializer::applyDocument(document, candidate, registry, load)
        : Result::failure("hierarchy document could not be serialized");
    const GameObject* loadedRoot = candidate.findGameObject("root");
    const GameObject* loadedChild = candidate.findGameObject("child");
    const GameObject* loadedGrandchild = candidate.findGameObject("grandchild");
    passed &= expect(
        result && loadedRoot && loadedChild && loadedGrandchild &&
            loadedRoot->parent() == nullptr && loadedChild->parent() == loadedRoot &&
            loadedGrandchild->parent() == loadedChild &&
            sameVector(loadedChild->position, childLocalPosition) &&
            sameVector(loadedChild->rotation, childLocalRotation) &&
            sameVector(loadedChild->scale, childLocalScale) &&
            sameVector(loadedGrandchild->position, grandchildLocalPosition) &&
            sameVector(loadedGrandchild->rotation, grandchildLocalRotation) &&
            sameVector(loadedGrandchild->scale, grandchildLocalScale) &&
            sameMatrix(loadedChild->worldTransformMatrix(), childWorld) &&
            sameMatrix(loadedGrandchild->worldTransformMatrix(), grandchildWorld) &&
            std::find(loadedRoot->children().begin(), loadedRoot->children().end(),
                      loadedChild) != loadedRoot->children().end() &&
            std::find(loadedChild->children().begin(), loadedChild->children().end(),
                      loadedGrandchild) != loadedChild->children().end(),
        "multi-level hierarchy did not preserve local or world transforms");

    nlohmann::json reordered = document;
    EmptyScene reorderedCandidate;
    SceneLoadData reorderedLoad;
    if (rootRecord && childRecord && grandchildRecord) {
        reordered["objects"] = nlohmann::json::array();
        reordered["objects"].push_back(*grandchildRecord);
        reordered["objects"].push_back(*rootRecord);
        reordered["objects"].push_back(*childRecord);
        result = SceneSerializer::applyDocument(
            reordered, reorderedCandidate, registry, reorderedLoad);
    } else {
        result = Result::failure("hierarchy records were not available for ordering test");
    }
    const GameObject* reorderedChild =
        reorderedCandidate.findGameObject("child");
    const GameObject* reorderedGrandchild =
        reorderedCandidate.findGameObject("grandchild");
    passed &= expect(
        result && reorderedChild && reorderedGrandchild &&
            reorderedChild->parent() == reorderedCandidate.findGameObject("root") &&
            reorderedGrandchild->parent() == reorderedChild,
        "child-before-parent JSON ordering was not supported");

    nlohmann::json legacyDocument = document;
    legacyDocument["formatVersion"] = 1;
    for (auto& object : legacyDocument["objects"]) object.erase("parentId");
    EmptyScene legacyCandidate;
    GameObject* legacyRoot = addPlainObject(legacyCandidate, "root");
    GameObject* legacyChild = addPlainObject(legacyCandidate, "child");
    GameObject* legacyGrandchild = addPlainObject(legacyCandidate, "grandchild");
    const Result legacyParenting = legacyRoot && legacyChild && legacyGrandchild
        ? legacyCandidate.reparentGameObject(
              *legacyChild, legacyRoot, Scene::ReparentMode::PreserveLocal)
        : Result::failure("legacy candidate hierarchy fixture was incomplete");
    const Result legacyGrandparenting = legacyParenting
        ? legacyCandidate.reparentGameObject(
              *legacyGrandchild, legacyChild, Scene::ReparentMode::PreserveLocal)
        : Result::failure("legacy candidate hierarchy fixture was incomplete");
    SceneLoadData legacyLoad;
    result = legacyGrandparenting
        ? SceneSerializer::applyDocument(
              legacyDocument, legacyCandidate, registry, legacyLoad)
        : legacyGrandparenting;
    bool legacyObjectsAreRoots = static_cast<bool>(result);
    if (result) {
        for (const auto& object : legacyCandidate.gameObjects()) {
            legacyObjectsAreRoots &= object->parent() == nullptr;
        }
    }
    passed &= expect(result && legacyObjectsAreRoots,
                     "v1 load did not clear a candidate hierarchy");

    nlohmann::json extensible = document;
    extensible["futureMetadata"] = {{"example", true}};
    EmptyScene extensibleCandidate;
    SceneLoadData extensibleLoad;
    result = SceneSerializer::applyDocument(
        extensible, extensibleCandidate, registry, extensibleLoad);
    passed &= expect(static_cast<bool>(result),
                     "unknown top-level scene data was rejected");

    const auto setParentId = [](nlohmann::json& value,
                                const std::string& objectId,
                                nlohmann::json parentId) {
        for (auto& object : value["objects"]) {
            if (object.at("id") == objectId) {
                object["parentId"] = std::move(parentId);
                return;
            }
        }
    };
    const auto expectRejected = [&](const nlohmann::json& malformed,
                                    const std::string& message) {
        EmptyScene malformedCandidate;
        SceneLoadData malformedLoad;
        const Result malformedResult = SceneSerializer::applyDocument(
            malformed, malformedCandidate, registry, malformedLoad);
        return expect(!malformedResult, message);
    };

    nlohmann::json missingParent = document;
    for (auto& object : missingParent["objects"]) {
        if (object.at("id") == "child") object.erase("parentId");
    }
    passed &= expectRejected(missingParent,
        "v3 object without parentId was accepted");

    const std::vector<nlohmann::json> invalidParentIds = {
        nlohmann::json(""), 123, nlohmann::json::array(),
        nlohmann::json::object(), true};
    for (const nlohmann::json& invalidParentId : invalidParentIds) {
        nlohmann::json malformed = document;
        setParentId(malformed, "child", invalidParentId);
        passed &= expectRejected(malformed,
                                 "invalid v3 parentId type was accepted");
    }

    nlohmann::json unknownParent = document;
    setParentId(unknownParent, "child", "missing-parent");
    EmptyScene unknownParentCandidate;
    SceneLoadData unknownParentLoad;
    result = SceneSerializer::applyDocument(
        unknownParent, unknownParentCandidate, registry, unknownParentLoad);
    passed &= expect(!result &&
                         result.error().find("child") != std::string::npos &&
                         result.error().find("missing-parent") != std::string::npos,
                     "unknown v3 parent reference was accepted or lacked context");

    nlohmann::json unsavedParent = document;
    setParentId(unsavedParent, "child", "candidate-only-parent");
    EmptyScene unsavedParentCandidate;
    passed &= expect(addPlainObject(unsavedParentCandidate,
                                    "candidate-only-parent") != nullptr,
                     "could not create unsaved candidate parent fixture");
    SceneLoadData unsavedParentLoad;
    result = SceneSerializer::applyDocument(
        unsavedParent, unsavedParentCandidate, registry, unsavedParentLoad);
    passed &= expect(!result,
                     "hierarchy reference resolved to an unsaved candidate object");

    nlohmann::json selfParent = document;
    setParentId(selfParent, "root", "root");
    passed &= expectRejected(selfParent, "self-parenting v3 document was accepted");

    nlohmann::json cycle = document;
    setParentId(cycle, "root", "grandchild");
    setParentId(cycle, "child", "root");
    setParentId(cycle, "grandchild", "child");
    passed &= expectRejected(cycle, "cyclic v3 document was accepted");

    return passed;
}

bool runtimeHierarchyTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    EmptyScene source;
    GameObject* root = addPlainObject(source, "runtime-root");
    auto customObject = std::make_unique<CustomObject>();
    customObject->persistentId = "runtime-child";
    customObject->spinSpeed = 240.0f;
    customObject->runtimeOnly = 91.0f;
    customObject->transferOnlyValue = 37.0f;
    CustomObject* child = customObject.get();
    const Result addChildResult = source.addGameObject(std::move(customObject));
    GameObject* grandchild = addPlainObject(source, "runtime-grandchild");
    if (!root || !child || !grandchild || !addChildResult) {
        return expect(false, "could not create runtime hierarchy fixture");
    }
    root->position = {4.0f, 5.0f, 6.0f};
    root->rotation = {10.0f, 20.0f, 30.0f};
    root->scale = {2.0f, 1.0f, 3.0f};
    child->position = {-2.0f, 7.0f, 1.0f};
    child->rotation = {5.0f, -8.0f, 12.0f};
    child->scale = {0.75f, 1.25f, 0.5f};
    grandchild->position = {8.0f, -1.0f, 3.0f};
    grandchild->rotation = {-4.0f, 6.0f, 9.0f};
    grandchild->scale = {1.1f, 0.9f, 1.3f};
    passed &= expect(
        source.reparentGameObject(*child, root, Scene::ReparentMode::PreserveLocal) &&
            source.reparentGameObject(*grandchild, child,
                                       Scene::ReparentMode::PreserveLocal),
        "could not establish runtime hierarchy fixture");

    const glm::mat4 sourceChildWorld = child->worldTransformMatrix();
    const glm::mat4 sourceGrandchildWorld = grandchild->worldTransformMatrix();
    EmptyScene destination;
    Result result = SceneSerializer::copyAuthoredState(source, destination, registry);
    const GameObject* runtimeRoot = destination.findGameObject("runtime-root");
    const auto* runtimeChild = dynamic_cast<const CustomObject*>(
        destination.findGameObject("runtime-child"));
    const GameObject* runtimeGrandchild =
        destination.findGameObject("runtime-grandchild");
    const std::unordered_set<const GameObject*> sourceObjects = {
        root, child, grandchild};
    bool destinationPointersAreIsolated = true;
    for (const auto& object : destination.gameObjects()) {
        destinationPointersAreIsolated &=
            sourceObjects.count(object.get()) == 0 &&
            (object->parent() == nullptr ||
             sourceObjects.count(object->parent()) == 0);
        for (const GameObject* descendant : object->children()) {
            destinationPointersAreIsolated &= sourceObjects.count(descendant) == 0;
        }
    }
    const TypeDescriptor* sourceChildType = registry.find(*child);
    const TypeDescriptor* runtimeChildType = runtimeChild
        ? registry.find(*runtimeChild)
        : nullptr;
    passed &= expect(
        result && runtimeRoot && runtimeChild && runtimeGrandchild &&
            runtimeRoot != root && runtimeChild != child &&
            runtimeGrandchild != grandchild && runtimeRoot->parent() == nullptr &&
            runtimeChild->parent() == runtimeRoot &&
            runtimeGrandchild->parent() == runtimeChild &&
            std::find(runtimeRoot->children().begin(), runtimeRoot->children().end(),
                      runtimeChild) != runtimeRoot->children().end() &&
            std::find(runtimeChild->children().begin(), runtimeChild->children().end(),
                      runtimeGrandchild) != runtimeChild->children().end() &&
            sameVector(runtimeChild->position, child->position) &&
            sameVector(runtimeChild->rotation, child->rotation) &&
            sameVector(runtimeChild->scale, child->scale) &&
            sameVector(runtimeGrandchild->position, grandchild->position) &&
            sameVector(runtimeGrandchild->rotation, grandchild->rotation) &&
            sameVector(runtimeGrandchild->scale, grandchild->scale) &&
            sameMatrix(runtimeChild->worldTransformMatrix(), sourceChildWorld) &&
            sameMatrix(runtimeGrandchild->worldTransformMatrix(), sourceGrandchildWorld) &&
            sourceChildType && runtimeChildType &&
            sourceChildType->type == runtimeChildType->type &&
            runtimeChild->spinSpeed == 240.0f &&
            runtimeChild->runtimeOnly == 0.0f &&
            runtimeChild->transferOnlyValue == 37.0f &&
            destinationPointersAreIsolated,
        "runtime authored copy did not isolate or reconstruct hierarchy");
    return passed;
}

bool characterPersistenceTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    EmptyScene source;
    auto character = std::make_unique<Character>();
    character->persistentId = "character";
    character->capsuleHeight = 211.0f;
    character->capsuleRadius = 41.0f;
    passed &= expect(static_cast<bool>(source.addGameObject(std::move(character))),
                     "could not create Character persistence fixture");

    Camera editor;
    nlohmann::json document;
    Result result = SceneSerializer::serializeFull(source, registry, editor, document);
    const nlohmann::json* record = result
        ? findObjectRecord(document, "character")
        : nullptr;
    passed &= expect(
        result && document.at("formatVersion") == SceneSerializer::formatVersion && record &&
            record->at("properties").contains("capsuleHeight") &&
            record->at("properties").contains("capsuleRadius"),
        "Character capsule dimensions were not persisted");

    EmptyScene candidate;
    SceneLoadData load;
    result = result
        ? SceneSerializer::applyDocument(document, candidate, registry, load)
        : Result::failure("Character persistence serialization failed");
    nlohmann::json roundTrip;
    const Result roundTripResult = result
        ? SceneSerializer::serializeAuthored(candidate, registry, roundTrip)
        : Result::failure("Character persistence round trip could not be loaded");
    const nlohmann::json* roundTripRecord = roundTripResult
        ? findObjectRecord(roundTrip, "character")
        : nullptr;
    const auto* restoredCharacter = result
        ? dynamic_cast<const Character*>(
              candidate.findGameObject("character"))
        : nullptr;
    passed &= expect(
        result && roundTripResult && roundTrip.at("formatVersion") == SceneSerializer::formatVersion &&
            roundTripRecord &&
            roundTripRecord->at("properties").contains("capsuleHeight") &&
            roundTripRecord->at("properties").contains("capsuleRadius") &&
            restoredCharacter && restoredCharacter->capsuleHeight == 211.0f &&
            restoredCharacter->capsuleRadius == 41.0f,
        "Character capsule dimensions did not survive serialize/load");
    return passed;
}

bool sceneUnitMigrationTests() {
    bool passed = true;
    TypeRegistry registry = makeRegistry(passed);
    passed &= expect(static_cast<bool>(registerAttachedCamera(registry)),
                     "attached-camera migration type registration failed");

    const auto fixtureDirectory =
        std::filesystem::temp_directory_path() /
        ("dunamis-scene-asset-migration-" +
         std::to_string(std::chrono::steady_clock::now()
                            .time_since_epoch()
                            .count()));
    std::filesystem::create_directories(fixtureDirectory);
    const auto modelFixture = fixtureDirectory / "meter-triangle.obj";
    {
        std::ofstream model(modelFixture);
        model << "v 0 0 0\n"
                 "v 2 0 0\n"
                 "v 0 1 0\n"
                 "f 1 2 3\n";
    }
    const std::string fallbackTexture =
        (std::filesystem::path(DUNAMIS_SOURCE_DIR) /
         "rendering/default_textures/error.jpg")
            .string();

    nlohmann::json legacyDocument = nlohmann::json::object();
    legacyDocument["formatVersion"] = 2;
    legacyDocument["scene"] = nlohmann::json::object();
    legacyDocument["scene"]["name"] = "Legacy Units";
    legacyDocument["scene"]["backgroundColor"] = {0.1f, 0.2f, 0.3f, 1.0f};
    legacyDocument["scene"]["ambientColor"] = {0.4f, 0.5f, 0.6f};
    legacyDocument["scene"]["ambientIntensity"] = 0.7f;
    legacyDocument["activeCamera"] = {
        {"ownerId", "camera-owner"}, {"kind", "attached"}};
    legacyDocument["editor"] = nlohmann::json::object();
    legacyDocument["editor"]["camera"] = nlohmann::json::object();
    legacyDocument["editor"]["camera"]["position"] = {300.0f, 400.0f, 500.0f};
    legacyDocument["editor"]["camera"]["front"] = {0.0f, 0.0f, -1.0f};
    legacyDocument["editor"]["camera"]["up"] = {0.0f, 1.0f, 0.0f};
    legacyDocument["editor"]["renderColliders"] = nlohmann::json::array();
    legacyDocument["objects"] = nlohmann::json::array();

    nlohmann::json rootRecord = nlohmann::json::object();
    rootRecord["id"] = "root";
    rootRecord["parentId"] = nullptr;
    rootRecord["siblingIndex"] = std::size_t{0};
    rootRecord["type"] = "GameObject";
    rootRecord["properties"] = nlohmann::json::object();
    rootRecord["properties"]["name"] = "Legacy Root";
    rootRecord["properties"]["position"] = {1000.0f, 200.0f, 300.0f};
    rootRecord["properties"]["rotation"] = {10.0f, 20.0f, 30.0f};
    rootRecord["properties"]["scale"] = {2.0f, 3.0f, 4.0f};
    rootRecord["properties"]["physics"] = {
        {"enabled", false},
        {"motionType", "static"},
        {"colliderType", "sphere"},
        {"sphereRadius", 0.5f}};
    legacyDocument["objects"].push_back(rootRecord);

    nlohmann::json modelRecord = nlohmann::json::object();
    modelRecord["id"] = "model";
    modelRecord["parentId"] = nullptr;
    modelRecord["siblingIndex"] = std::size_t{3};
    modelRecord["type"] = "GameObject";
    modelRecord["properties"] = nlohmann::json::object();
    modelRecord["properties"]["name"] = "Legacy Model";
    modelRecord["properties"]["modelPath"] = modelFixture.string();
    modelRecord["properties"]["texturePath"] = fallbackTexture;
    modelRecord["properties"]["position"] = {100.0f, 0.0f, 0.0f};
    modelRecord["properties"]["rotation"] = {0.0f, 0.0f, 0.0f};
    modelRecord["properties"]["scale"] = {2.0f, 2.0f, 2.0f};
    modelRecord["properties"]["physics"] = {
        {"enabled", false},
        {"motionType", "static"},
        {"colliderType", "mesh"},
        {"sphereRadius", 0.5f}};
    legacyDocument["objects"].push_back(modelRecord);

    nlohmann::json characterRecord = nlohmann::json::object();
    characterRecord["id"] = "character";
    characterRecord["parentId"] = "root";
    characterRecord["siblingIndex"] = std::size_t{0};
    characterRecord["type"] = "Character";
    characterRecord["properties"] = nlohmann::json::object();
    characterRecord["properties"]["name"] = "Legacy Character";
    characterRecord["properties"]["position"] = {50.0f, 0.0f, -25.0f};
    characterRecord["properties"]["rotation"] = {-5.0f, 15.0f, 25.0f};
    characterRecord["properties"]["scale"] = {1.0f, 1.0f, 1.0f};
    characterRecord["properties"]["capsuleHeight"] = 180.0f;
    characterRecord["properties"]["capsuleRadius"] = 35.0f;
    characterRecord["properties"]["physics"] = {
        {"enabled", false},
        {"motionType", "static"},
        {"colliderType", "sphere"},
        {"sphereRadius", 0.5f}};
    legacyDocument["objects"].push_back(characterRecord);

    nlohmann::json cameraOwnerRecord = nlohmann::json::object();
    cameraOwnerRecord["id"] = "camera-owner";
    cameraOwnerRecord["parentId"] = nullptr;
    cameraOwnerRecord["siblingIndex"] = std::size_t{1};
    cameraOwnerRecord["type"] = "AttachedCameraObject";
    cameraOwnerRecord["properties"] = nlohmann::json::object();
    cameraOwnerRecord["properties"]["name"] = "Legacy Camera Owner";
    cameraOwnerRecord["properties"]["position"] = {0.0f, 0.0f, 0.0f};
    cameraOwnerRecord["properties"]["rotation"] = {0.0f, 0.0f, 0.0f};
    cameraOwnerRecord["properties"]["scale"] = {1.0f, 1.0f, 1.0f};
    cameraOwnerRecord["attachedCamera"] = nlohmann::json::object();
    cameraOwnerRecord["attachedCamera"]["position"] = {0.0f, 150.0f, 0.0f};
    cameraOwnerRecord["attachedCamera"]["front"] = {0.0f, 0.0f, -1.0f};
    cameraOwnerRecord["attachedCamera"]["up"] = {0.0f, 1.0f, 0.0f};
    cameraOwnerRecord["attachedCamera"]["fov"] = 75.0f;
    legacyDocument["objects"].push_back(cameraOwnerRecord);

    nlohmann::json sunRecord = nlohmann::json::object();
    sunRecord["id"] = "sun";
    sunRecord["parentId"] = nullptr;
    sunRecord["siblingIndex"] = std::size_t{2};
    sunRecord["type"] = "DirectionalLight";
    sunRecord["properties"] = nlohmann::json::object();
    sunRecord["properties"]["name"] = "Legacy Sun";
    sunRecord["properties"]["position"] = {0.0f, 0.0f, 0.0f};
    sunRecord["properties"]["rotation"] = {0.0f, 0.0f, 0.0f};
    sunRecord["properties"]["scale"] = {1.0f, 1.0f, 1.0f};
    sunRecord["properties"]["color"] = {0.2f, 0.4f, 0.6f};
    sunRecord["properties"]["intensity"] = 12.0f;
    sunRecord["properties"]["shadowFocus"] = {100.0f, 200.0f, 300.0f};
    sunRecord["properties"]["shadowHalfExtent"] = 500.0f;
    sunRecord["properties"]["shadowLightDistance"] = 500.0f;
    sunRecord["properties"]["shadowNearPlane"] = 1.0f;
    sunRecord["properties"]["shadowFarPlane"] = 1000.0f;
    legacyDocument["objects"].push_back(sunRecord);
    nlohmann::json document = legacyDocument;

    EmptyScene candidate;
    SceneLoadData load;
    Result result = SceneSerializer::applyDocument(
        document, candidate, registry, load);
    const auto* root = result
        ? candidate.findGameObject("root")
        : nullptr;
    const auto* model = result
        ? candidate.findGameObject("model")
        : nullptr;
    const auto* character = result
        ? dynamic_cast<const Character*>(candidate.findGameObject("character"))
        : nullptr;
    const auto* cameraOwner = result
        ? dynamic_cast<const AttachedCameraObject*>(
              candidate.findGameObject("camera-owner"))
        : nullptr;
    const auto* sun = result
        ? dynamic_cast<const DirectionalLight*>(candidate.findGameObject("sun"))
        : nullptr;
    passed &= expect(
        result && root && model && character && cameraOwner && sun &&
            root->position == glm::vec3(10.0f, 2.0f, 3.0f) &&
            model->position == glm::vec3(1.0f, 0.0f, 0.0f) &&
            model->scale == glm::vec3(0.02f) &&
            character->position == glm::vec3(0.5f, 0.0f, -0.25f) &&
            character->parent() == root &&
            character->capsuleHeight == 1.8f &&
            character->capsuleRadius == 0.35f &&
            root->scale == glm::vec3(2.0f, 3.0f, 4.0f) &&
            root->rotation == glm::vec3(10.0f, 20.0f, 30.0f),
        "v2 world positions or Character dimensions did not migrate to meters");

    nlohmann::json v1Document = legacyDocument;
    v1Document["formatVersion"] = 1;
    EmptyScene v1Candidate;
    SceneLoadData v1Load;
    const Result v1Result = SceneSerializer::applyDocument(
        v1Document, v1Candidate, registry, v1Load);
    const GameObject* v1Model = v1Candidate.findGameObject("model");
    const GameObject* v1Root = v1Candidate.findGameObject("root");
    passed &= expect(
        v1Result && v1Root && v1Model &&
            v1Root->position == glm::vec3(10.0f, 2.0f, 3.0f) &&
            v1Model->position == glm::vec3(1.0f, 0.0f, 0.0f) &&
            v1Model->scale == glm::vec3(0.02f),
        "v1 loading did not apply both world and asset-basis migrations");
    if (root && character) {
        const glm::mat4 expectedRoot = transform_math::makeModelMatrix(
            {10.0f, 2.0f, 3.0f}, {10.0f, 20.0f, 30.0f},
            {2.0f, 3.0f, 4.0f});
        const glm::mat4 expectedCharacter = expectedRoot *
            transform_math::makeModelMatrix(
                {0.5f, 0.0f, -0.25f}, {-5.0f, 15.0f, 25.0f},
                {1.0f, 1.0f, 1.0f});
        passed &= expect(sameMatrix(root->worldTransformMatrix(), expectedRoot) &&
                             sameMatrix(character->worldTransformMatrix(),
                                        expectedCharacter),
                         "v2 child local position did not preserve hierarchy layout");
    }
    passed &= expect(
        cameraOwner && cameraOwner->camera &&
            cameraOwner->camera->position == glm::vec3(0.0f, 1.5f, 0.0f) &&
            cameraOwner->camera->front == glm::vec3(0.0f, 0.0f, -1.0f) &&
            cameraOwner->camera->up == glm::vec3(0.0f, 1.0f, 0.0f) &&
            cameraOwner->camera->fov() == 75.0f && load.editorCamera &&
            load.editorCamera->position == glm::vec3(3.0f, 4.0f, 5.0f) &&
            load.editorCamera->front == glm::vec3(0.0f, 0.0f, -1.0f) &&
            load.editorCamera->up == glm::vec3(0.0f, 1.0f, 0.0f),
        "v2 camera positions did not migrate without changing camera semantics");
    passed &= expect(
        sun && sun->color == glm::vec3(0.2f, 0.4f, 0.6f) &&
            sun->intensity == 12.0f &&
            sun->shadow.focus == glm::vec3(1.0f, 2.0f, 3.0f) &&
            sun->shadow.halfExtent == 5.0f &&
            sun->shadow.lightDistance == 5.0f &&
            sun->shadow.nearPlane == 0.01f &&
            sun->shadow.farPlane == 10.0f && root &&
            root->physics.sphereRadius == 0.5f && character &&
            character->physics.sphereRadius == 0.5f,
        "v2 mixed-unit exceptions or non-distance fields were changed incorrectly");
    passed &= expect(
        result && cameraOwner && cameraOwner->camera &&
            candidate.activeCamera() == cameraOwner->camera.get() &&
            load.authoredBaseline.at("formatVersion") == SceneSerializer::formatVersion,
        "migrated scene did not establish the current authored baseline");

    Camera migratedEditor;
    if (load.editorCamera) {
        migratedEditor.position = load.editorCamera->position;
        migratedEditor.front = load.editorCamera->front;
        migratedEditor.up = load.editorCamera->up;
    }
    nlohmann::json saved;
    const Result saveResult = result
        ? SceneSerializer::serializeFull(candidate, registry, migratedEditor, saved)
        : Result::failure("migrated scene could not be saved");
    passed &= expect(saveResult && saved.at("formatVersion") == SceneSerializer::formatVersion,
                     "saving a migrated scene did not write format v4");

    EmptyScene reloaded;
    SceneLoadData reloadData;
    const Result reloadResult = saveResult
        ? SceneSerializer::applyDocument(saved, reloaded, registry, reloadData)
        : Result::failure("migrated scene was not serializable");
    const GameObject* reloadedRoot = reloaded.findGameObject("root");
    const GameObject* reloadedModel = reloaded.findGameObject("model");
    const Character* reloadedCharacter = dynamic_cast<const Character*>(
        reloaded.findGameObject("character"));
    passed &= expect(
        reloadResult && reloadedRoot && reloadedModel && reloadedCharacter &&
            reloadedRoot->position == glm::vec3(10.0f, 2.0f, 3.0f) &&
            reloadedModel->position == glm::vec3(1.0f, 0.0f, 0.0f) &&
            reloadedModel->scale == glm::vec3(0.02f) &&
            reloadedCharacter->position == glm::vec3(0.5f, 0.0f, -0.25f) &&
            reloadedCharacter->capsuleHeight == 1.8f &&
            reloadData.editorCamera &&
            reloadData.editorCamera->position == glm::vec3(3.0f, 4.0f, 5.0f),
        "loading a saved v4 scene applied a second legacy conversion");

    nlohmann::json v3Document = load.authoredBaseline;
    v3Document["formatVersion"] = 3;
    const auto v3ModelRecord = std::find_if(
        v3Document.at("objects").begin(), v3Document.at("objects").end(),
        [](const auto& object) { return object.at("id") == "model"; });
    if (v3ModelRecord != v3Document.at("objects").end()) {
        v3ModelRecord->at("properties")["scale"] = {2.0f, 2.0f, 2.0f};
    }
    EmptyScene v3Candidate;
    SceneLoadData v3Load;
    const Result v3Result = SceneSerializer::applyDocument(
        v3Document, v3Candidate, registry, v3Load);
    const GameObject* v3Model = v3Candidate.findGameObject("model");
    passed &= expect(
        v3Result && v3Model &&
            v3Model->position == glm::vec3(1.0f, 0.0f, 0.0f) &&
            v3Model->scale == glm::vec3(0.02f),
        "v3 migration did not apply asset-basis compatibility without redoing world migration");

    nlohmann::json v4Document = load.authoredBaseline;
    const auto v4ModelRecord = std::find_if(
        v4Document.at("objects").begin(), v4Document.at("objects").end(),
        [](const auto& object) { return object.at("id") == "model"; });
    if (v4ModelRecord != v4Document.at("objects").end()) {
        v4ModelRecord->at("properties")["scale"] = {2.0f, 2.0f, 2.0f};
    }
    EmptyScene v4Candidate;
    SceneLoadData v4Load;
    const Result v4Result = SceneSerializer::applyDocument(
        v4Document, v4Candidate, registry, v4Load);
    const GameObject* v4Model = v4Candidate.findGameObject("model");
    passed &= expect(
        v4Result && v4Model &&
            v4Model->position == glm::vec3(1.0f, 0.0f, 0.0f) &&
            v4Model->scale == glm::vec3(2.0f),
        "current v4 scene was unexpectedly asset-migrated");

    std::error_code fixtureError;
    std::filesystem::remove_all(fixtureDirectory, fixtureError);
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
    editingCustom->runtimeOnly = 777.0f;
    editingCustom->transferOnlyValue = 123.0f;
    const bool configuredCameraFov = editingCamera->setFov(90.0f);
    passed &= expect(configuredCameraFov && manager.hasUnsavedChanges(),
                     "authored camera FOV did not become persisted dirty state");
    editingCustom->spinSpeed = 270.0f;
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
                         runtimeCustom->runtimeOnly == 0.0f &&
                         runtimeCustom->transferOnlyValue == 123.0f &&
                         configuredCameraFov && runtimeCamera &&
                         runtimeCamera->fov() == 90.0f,
                     "runtime clone omitted authored custom state or copied transient state");
    manager.cancelPreparedRuntimeScene();

    editingCustom->transferOnlyValue = CustomObject::transferFailureSentinel;
    const Result failedTransfer = manager.prepareRuntimeScene();
    passed &= expect(
        !failedTransfer && manager.runtimeScene() == nullptr &&
            failedTransfer.error().find("transferOnlyValue") != std::string::npos &&
            failedTransfer.error().find("custom") != std::string::npos &&
            failedTransfer.error().find("CustomObject") != std::string::npos &&
            failedTransfer.error().find("intentional transfer failure") !=
                std::string::npos,
        "runtime-only descriptor failure was not surfaced by Scene Manager");
    editingCustom->transferOnlyValue = 123.0f;

    result = manager.saveEditingScene(editor);
    passed &= expect(result && !manager.hasUnsavedChanges(),
                     "successful save did not establish a clean baseline");
    const nlohmann::json savedDocument = readSceneDocument(path);
    const nlohmann::json* savedCustom =
        findObjectRecord(savedDocument, "custom");
    const nlohmann::json* savedCamera =
        findObjectRecord(savedDocument, "camera");
    passed &= expect(savedDocument.at("formatVersion") == SceneSerializer::formatVersion && savedCustom &&
                         savedCamera &&
                         savedCustom->at("properties").contains("physics") &&
                         !savedCustom->at("properties").contains("transferOnlyValue") &&
                         savedCamera->at("properties").contains("fov"),
                     "authored property state was not persisted");
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
    passed &= expect(static_cast<bool>(result), "scene document without a custom object was rejected");
    result = manager.commitPreparedEditingSceneLoad();
    manager.finishEditingSceneLoad();
    passed &= expect(result && manager.editingScene()->findGameObject("custom") == nullptr &&
                         !manager.hasUnsavedChanges(),
                     "JSON topology was not authoritative during scene load");

    manager.shutdown();
    std::error_code error;
    std::filesystem::remove_all(directory, error);
    return passed;
}

bool managerV1MigrationTests() {
    bool passed = true;
    SceneManager manager;
    Result result = manager.initialize<PersistenceScene>(
        "Persistence v1 Migration Test", std::make_shared<InputManager>());
    if (!result) {
        return expect(false, "v1 migration manager initialization failed");
    }

    const auto directory = std::filesystem::temp_directory_path() /
        ("dunamis-v1-migration-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "migration.scene.json";
    manager.setCurrentScenePath(path);
    GameObject* base = manager.editingScene()->findGameObject("base");
    GameObject* custom = manager.editingScene()->findGameObject("custom");
    passed &= expect(base && custom && manager.editingScene()->reparentGameObject(
                             *custom, base, Scene::ReparentMode::PreserveLocal),
                     "could not create v1 migration hierarchy fixture");
    Camera editor;
    result = manager.saveEditingScene(editor);
    nlohmann::json v2Document = result
        ? readSceneDocument(path)
        : nlohmann::json{};
    passed &= expect(result && v2Document.at("formatVersion") == SceneSerializer::formatVersion,
                     "migration fixture was not written as v4");

    nlohmann::json v1Document = v2Document;
    if (v1Document.is_object()) {
        v1Document["formatVersion"] = 1;
        for (auto& object : v1Document["objects"]) {
            object.erase("parentId");
            if (object.at("id") == "custom") object["parentId"] = "base";
        }
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << v1Document.dump(2) << '\n';
    }

    const Scene* stableScene = manager.editingScene();
    result = SceneSerializer::formatVersion == 4
        ? manager.prepareEditingSceneLoad(path)
        : Result::failure("current scene format is not v4");
    const Scene* prepared = manager.preparedEditingScene();
    const GameObject* preparedCustom = prepared
        ? prepared->findGameObject("custom")
        : nullptr;
    const Camera* preparedCamera = prepared
        ? dynamic_cast<const Camera*>(prepared->findGameObject("camera"))
        : nullptr;
    bool preparedObjectsAreRoots = prepared != nullptr;
    if (prepared) {
        for (const auto& object : prepared->gameObjects()) {
            preparedObjectsAreRoots &= object->parent() == nullptr;
        }
    }
    passed &= expect(
        result && preparedCustom && preparedCustom->parent() == nullptr &&
            preparedCamera && prepared->activeCamera() == preparedCamera &&
            preparedObjectsAreRoots,
        "v1 document did not load all objects as roots or restore its camera");

    result = result ? manager.commitPreparedEditingSceneLoad() : result;
    manager.finishEditingSceneLoad();
    const GameObject* loadedCustom = manager.editingScene()
        ? manager.editingScene()->findGameObject("custom")
        : nullptr;
    passed &= expect(
        result && manager.editingScene() != stableScene && loadedCustom &&
            loadedCustom->parent() == nullptr && !manager.hasUnsavedChanges(),
        "v1 load did not commit cleanly with a v4 authored baseline");

    result = manager.saveEditingScene(editor);
    const nlohmann::json upgraded = result
        ? readSceneDocument(path)
        : nlohmann::json{};
    bool everyObjectHasParentId = result && upgraded.at("formatVersion") == SceneSerializer::formatVersion;
    if (result) {
        for (const auto& object : upgraded.at("objects")) {
            everyObjectHasParentId &= object.contains("parentId");
        }
    }
    passed &= expect(result && everyObjectHasParentId,
                     "saving a v1 scene did not upgrade every object to v4");

    nlohmann::json malformed = upgraded;
    if (malformed.is_object()) {
        for (auto& object : malformed["objects"]) {
            if (object.at("id") == "custom") object["parentId"] = "missing";
        }
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << malformed.dump(2) << '\n';
    }
    const Scene* sceneBeforeMalformedLoad = manager.editingScene();
    result = manager.prepareEditingSceneLoad(path);
    passed &= expect(
        !result && manager.editingScene() == sceneBeforeMalformedLoad,
        "malformed hierarchy load replaced the editing scene");

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
    passed &= hierarchyPersistenceTests();
    passed &= runtimeHierarchyTests();
    passed &= characterPersistenceTests();
    passed &= sceneUnitMigrationTests();
    passed &= managerDirtyTests();
    passed &= managerV1MigrationTests();
    passed &= managerSavePathTests();
    return passed ? 0 : 1;
}
