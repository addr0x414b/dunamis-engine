#include "scene/scene.h"
#include "scene/scene_manager.h"
#include "scene/model_renderable.h"
#include "game/level_1.h"
#include "input/input_manager.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

class PlayerTestAccess {
public:
    static double yaw(const Player& player) { return player.yaw; }
    static double pitch(const Player& player) { return player.pitch; }
    static void applyMovementDelta(Player& player, const glm::vec3& delta) {
        player.applyMovementDelta(delta);
    }
};

namespace {

class TestScene final : public Scene {
public:
    void init() override {}
    void start() override {}
    void update() override {}
};

class DerivedGameObject final : public GameObject {};

class ManagedObject final : public GameObject {
public:
    ManagedObject() : instanceId(++nextInstanceId) { ++liveCount; }
    ~ManagedObject() override { --liveCount; }

    inline static int liveCount = 0;
    inline static int nextInstanceId = 0;
    const int instanceId;
};

class ManagedScene final : public Scene {
public:
    ManagedScene() { ++constructionCount; }

    void init() override {
        Result result = addGameObject(std::make_unique<ManagedObject>());
        if (!result) {
            throw std::runtime_error(result.error());
        }
    }
    void start() override {}
    void update() override {}

    inline static int constructionCount = 0;
};

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

bool sameVector(const glm::vec3& first, const glm::vec3& second) {
    return glm::length(first - second) < 1.0e-5f;
}

bool hasDirectionalLightError(const Result& result) {
    return result.error().find("Directional light") != std::string::npos;
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

bool runLevel1FailurePropagationTest() {
    const auto missingAssetDirectory = std::filesystem::temp_directory_path() /
        ("dunamis-level1-missing-assets-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(missingAssetDirectory);
    Result result = Result::failure("Level1 initialization was not attempted");
    bool initialized = true;
    {
        CurrentPathGuard guard(missingAssetDirectory);
        SceneManager manager;
        result = manager.initialize<Level1>(
            "Level 1", std::make_shared<InputManager>());
        initialized = manager.initialized();
        manager.shutdown();
    }
    std::error_code error;
    std::filesystem::remove_all(missingAssetDirectory, error);
    return expect(!result && !initialized &&
                      result.error().find("Failed to load Sponza") !=
                          std::string::npos,
                  "Level1 did not propagate a required model-load failure");
}

bool runAuthoringTransferTests() {
    bool passed = true;
    GameObject baseObject;
    const GameObject& constBaseObject = baseObject;
    passed &= expect(baseObject.attachedCamera() == nullptr &&
                         constBaseObject.attachedCamera() == nullptr,
                     "Base GameObject unexpectedly reports an attached camera");

    TestScene editing;
    TestScene runtime;
    editing.name = "Edited Level";
    passed &= expect(static_cast<bool>(
                         editing.setBackgroundColor({0.1f, 0.2f, 0.3f, 0.4f})),
                     "Could not configure authoring background color");
    passed &= expect(static_cast<bool>(
                         editing.setAmbientLight({0.4f, 0.5f, 0.6f}, 3.0f)),
                     "Could not configure authoring ambient light");

    auto editingObject = std::make_unique<GameObject>();
    GameObject* editingObjectPointer = editingObject.get();
    editingObject->name = "Edited object";
    editingObject->position = {1.0f, 2.0f, 3.0f};
    editingObject->rotation = {4.0f, 5.0f, 6.0f};
    editingObject->scale = {7.0f, 8.0f, 9.0f};
    editingObject->physics.enabled = true;
    editingObject->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    editingObject->physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    editingObject->physics.sphereRadius = 0.75f;
    auto runtimeObject = std::make_unique<GameObject>();
    GameObject* runtimeObjectPointer = runtimeObject.get();
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingObject))),
                     "Could not add editing object for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimeObject))),
                     "Could not add runtime object for transfer test");

    auto editingPoint = std::make_unique<PointLight>();
    PointLight* editingPointPointer = editingPoint.get();
    editingPoint->color = {0.2f, 0.3f, 0.4f};
    editingPoint->intensity = 11.0f;
    auto runtimePoint = std::make_unique<PointLight>();
    PointLight* runtimePointPointer = runtimePoint.get();
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingPoint))),
                     "Could not add editing point light for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimePoint))),
                     "Could not add runtime point light for transfer test");

    auto editingDirectional = std::make_unique<DirectionalLight>();
    DirectionalLight* editingDirectionalPointer = editingDirectional.get();
    editingDirectional->rotation = {23.0f, -41.0f, 17.0f};
    editingDirectional->color = {0.7f, 0.8f, 0.9f};
    editingDirectional->intensity = 12.0f;
    editingDirectional->shadow.focus = {4.0f, 5.0f, 6.0f};
    editingDirectional->shadow.halfExtent = 123.0f;
    editingDirectional->shadow.lightDistance = 234.0f;
    editingDirectional->shadow.nearPlane = 2.0f;
    editingDirectional->shadow.farPlane = 456.0f;
    auto runtimeDirectional = std::make_unique<DirectionalLight>();
    DirectionalLight* runtimeDirectionalPointer = runtimeDirectional.get();
    passed &= expect(static_cast<bool>(editing.addGameObject(
                         std::move(editingDirectional))),
                     "Could not add editing directional light");
    passed &= expect(static_cast<bool>(runtime.addGameObject(
                         std::move(runtimeDirectional))),
                     "Could not add runtime directional light");

    auto editingCamera = std::make_unique<Camera>();
    Camera* editingCameraPointer = editingCamera.get();
    editingCamera->position = {31.0f, 32.0f, 33.0f};
    editingCamera->rotation = {34.0f, 35.0f, 36.0f};
    editingCamera->scale = {37.0f, 38.0f, 39.0f};
    editingCamera->front = {0.5f, 0.25f, -0.75f};
    editingCamera->up = {0.0f, 0.5f, 0.5f};
    passed &= expect(editingCamera->setFov(90.0f),
                     "Could not configure standalone camera FOV");
    auto runtimeCamera = std::make_unique<Camera>();
    Camera* runtimeCameraPointer = runtimeCamera.get();
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingCamera))),
                     "Could not add editing camera for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimeCamera))),
                     "Could not add runtime camera for transfer test");

    auto editingPlayer = std::make_unique<Player>();
    Player* editingPlayerPointer = editingPlayer.get();
    editingPlayer->init();
    editingPlayer->position = {10.0f, 20.0f, 30.0f};
    editingPlayer->camera->position = {11.0f, 22.0f, 33.0f};
    editingPlayer->camera->front = {0.25f, 0.5f, -0.75f};
    editingPlayer->camera->up = {0.0f, 0.8f, 0.2f};
    passed &= expect(editingPlayer->camera->setFov(75.0f),
                     "Could not configure attached camera FOV");
    const glm::vec3 editingPlayerCameraPosition =
        editingPlayer->camera->position;
    const glm::vec3 editingPlayerCameraFront = editingPlayer->camera->front;
    const glm::vec3 editingPlayerCameraUp = editingPlayer->camera->up;
    passed &= expect(
        editingPlayer->attachedCamera() == editingPlayer->camera.get() &&
            static_cast<const Player&>(*editingPlayer).attachedCamera() ==
                editingPlayer->camera.get(),
        "Player does not report its attached camera through both accessors");

    auto runtimePlayer = std::make_unique<Player>();
    Player* runtimePlayerPointer = runtimePlayer.get();
    runtimePlayer->init();
    runtimePlayer->camera->position = {-1.0f, -2.0f, -3.0f};
    runtimePlayer->camera->front = {1.0f, 0.0f, 0.0f};
    runtimePlayer->camera->up = {0.0f, 0.0f, 1.0f};
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingPlayer))),
                     "Could not add editing Player for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimePlayer))),
                     "Could not add runtime Player for transfer test");

    const Result transfer = editing.copyAuthoringStateTo(runtime);
    passed &= expect(static_cast<bool>(transfer),
                     "Matching authoring topology was rejected");
    passed &= expect(runtime.name == editing.name &&
                         runtime.backgroundColor() == editing.backgroundColor() &&
                         runtime.ambientColor() == editing.ambientColor() &&
                         runtime.ambientIntensity() == editing.ambientIntensity(),
                     "Scene authoring properties were not transferred");
    passed &= expect(runtimeObjectPointer->name == "Edited object" &&
                         runtimeObjectPointer->position == glm::vec3(1.0f, 2.0f, 3.0f) &&
                         runtimeObjectPointer->rotation == glm::vec3(4.0f, 5.0f, 6.0f) &&
                         runtimeObjectPointer->scale == glm::vec3(7.0f, 8.0f, 9.0f) &&
                         runtimeObjectPointer->physics.enabled &&
                         runtimeObjectPointer->physics.motionType ==
                             GameObject::PhysicsMotionType::Dynamic &&
                         runtimeObjectPointer->physics.colliderType ==
                             GameObject::PhysicsColliderType::Sphere &&
                         nearlyEqual(runtimeObjectPointer->physics.sphereRadius, 0.75f),
                     "GameObject authoring transform was not transferred");
    passed &= expect(runtimePointPointer->color == editingPointPointer->color &&
                         runtimePointPointer->intensity == 11.0f,
                     "Point-light authoring state was not transferred");
    glm::vec3 editingDirectionalDirection;
    glm::vec3 runtimeDirectionalDirection;
    const bool calculatedEditingDirection =
        editingDirectionalPointer->calculateWorldDirection(
            editingDirectionalDirection);
    const bool calculatedRuntimeDirection =
        runtimeDirectionalPointer->calculateWorldDirection(
            runtimeDirectionalDirection);
    passed &= expect(calculatedEditingDirection && calculatedRuntimeDirection &&
                         sameVector(runtimeDirectionalDirection,
                                    editingDirectionalDirection) &&
                         runtimeDirectionalPointer->color ==
                             editingDirectionalPointer->color &&
                         runtimeDirectionalPointer->intensity == 12.0f &&
                         runtimeDirectionalPointer->shadow.focus ==
                             editingDirectionalPointer->shadow.focus &&
                         runtimeDirectionalPointer->shadow.halfExtent == 123.0f &&
                         runtimeDirectionalPointer->shadow.lightDistance == 234.0f &&
                         runtimeDirectionalPointer->shadow.nearPlane == 2.0f &&
                         runtimeDirectionalPointer->shadow.farPlane == 456.0f,
                     "Directional-light authoring state was not transferred");
    passed &= expect(runtimeCameraPointer->position == editingCameraPointer->position &&
                         runtimeCameraPointer->rotation == editingCameraPointer->rotation &&
                         runtimeCameraPointer->scale == editingCameraPointer->scale &&
                         runtimeCameraPointer->front == editingCameraPointer->front &&
                         runtimeCameraPointer->up == editingCameraPointer->up &&
                         runtimeCameraPointer->fov() == editingCameraPointer->fov(),
                     "Standalone Camera authoring state was not transferred");
    passed &= expect(
        runtimePlayerPointer->attachedCamera()->position ==
                editingPlayerCameraPosition &&
            runtimePlayerPointer->attachedCamera()->front ==
                editingPlayerCameraFront &&
            runtimePlayerPointer->attachedCamera()->up ==
                editingPlayerCameraUp &&
            runtimePlayerPointer->attachedCamera()->fov() ==
                editingPlayerPointer->attachedCamera()->fov(),
        "Attached camera authoring state was not transferred");
    passed &= expect(
        editingPlayerPointer->attachedCamera()->position ==
                editingPlayerCameraPosition &&
            editingPlayerPointer->attachedCamera()->front ==
                editingPlayerCameraFront &&
            editingPlayerPointer->attachedCamera()->up ==
                editingPlayerCameraUp,
        "Attached camera transfer mutated the editing camera");
    passed &= expect(editingObjectPointer->position == glm::vec3(1.0f, 2.0f, 3.0f),
                     "Authoring transfer mutated the editing scene");

    TestScene countSource;
    TestScene countDestination;
    auto countDestinationObject = std::make_unique<GameObject>();
    GameObject* countDestinationPointer = countDestinationObject.get();
    countDestinationPointer->position = {91.0f, 92.0f, 93.0f};
    passed &= expect(countSource.addGameObject(std::make_unique<GameObject>()) &&
                         countSource.addGameObject(std::make_unique<GameObject>()) &&
                         countDestination.addGameObject(
                             std::move(countDestinationObject)),
                     "Could not configure object-count mismatch test");
    passed &= expect(!countSource.copyAuthoringStateTo(countDestination) &&
                         countDestinationPointer->position ==
                             glm::vec3(91.0f, 92.0f, 93.0f),
                     "Object-count mismatch mutated the destination");

    TestScene typeSource;
    TestScene typeDestination;
    auto typeDestinationObject = std::make_unique<DerivedGameObject>();
    GameObject* typeDestinationPointer = typeDestinationObject.get();
    typeDestinationPointer->position = {81.0f, 82.0f, 83.0f};
    passed &= expect(typeSource.addGameObject(std::make_unique<GameObject>()) &&
                         typeDestination.addGameObject(
                             std::move(typeDestinationObject)),
                     "Could not configure object-type mismatch test");
    passed &= expect(!typeSource.copyAuthoringStateTo(typeDestination) &&
                         typeDestinationPointer->position ==
                             glm::vec3(81.0f, 82.0f, 83.0f),
                     "Object-type mismatch mutated the destination");

    TestScene attachedSource;
    TestScene attachedDestination;
    attachedSource.name = "Attached source";
    attachedDestination.name = "Attached destination";
    auto attachedSourcePlayer = std::make_unique<Player>();
    attachedSourcePlayer->init();
    Player* attachedSourcePointer = attachedSourcePlayer.get();
    auto attachedDestinationPlayer = std::make_unique<Player>();
    attachedDestinationPlayer->init();
    attachedDestinationPlayer->camera.reset();
    Player* attachedDestinationPointer = attachedDestinationPlayer.get();
    attachedDestinationPointer->position = {71.0f, 72.0f, 73.0f};
    attachedDestinationPointer->rotation = {74.0f, 75.0f, 76.0f};
    attachedDestinationPointer->scale = {77.0f, 78.0f, 79.0f};
    passed &= expect(static_cast<bool>(attachedSource.addGameObject(
                         std::move(attachedSourcePlayer))) &&
                         static_cast<bool>(attachedDestination.addGameObject(
                             std::move(attachedDestinationPlayer))),
                     "Could not configure attached-camera mismatch test");
    const Result attachedTopologyResult =
        attachedSource.copyAuthoringStateTo(attachedDestination);
    passed &= expect(
        !attachedTopologyResult && attachedSourcePointer->attachedCamera() != nullptr &&
            attachedDestinationPointer->attachedCamera() == nullptr &&
            attachedDestination.name == "Attached destination" &&
            attachedDestinationPointer->position ==
                glm::vec3(71.0f, 72.0f, 73.0f) &&
            attachedDestinationPointer->rotation ==
                glm::vec3(74.0f, 75.0f, 76.0f) &&
            attachedDestinationPointer->scale ==
                glm::vec3(77.0f, 78.0f, 79.0f),
        "Attached-camera topology mismatch mutated the destination");
    return passed;
}

bool runSceneManagerTests() {
    bool passed = true;
    ManagedScene::constructionCount = 0;
    ManagedObject::liveCount = 0;
    ManagedObject::nextInstanceId = 0;
    auto input = std::make_shared<InputManager>();
    SceneManager manager;
    passed &= expect(static_cast<bool>(
                         manager.initialize<ManagedScene>("Managed", input)),
                     "Scene Manager initialization failed");
    Scene* editing = manager.editingScene();
    GameObject* editingObject = editing->gameObjects().front().get();
    passed &= expect(manager.activeScene() == editing &&
                         !manager.isRuntimeSceneActive() &&
                         ManagedScene::constructionCount == 1 &&
                         ManagedObject::liveCount == 1,
                     "Scene Manager did not retain one editing scene");

    passed &= expect(static_cast<bool>(manager.prepareRuntimeScene()),
                     "Scene Manager could not prepare a runtime scene");
    Scene* firstRuntime = manager.runtimeScene();
    GameObject* firstRuntimeObject = firstRuntime->gameObjects().front().get();
    const int firstRuntimeObjectId =
        static_cast<ManagedObject*>(firstRuntimeObject)->instanceId;
    passed &= expect(firstRuntime != editing &&
                         firstRuntimeObject != editingObject &&
                         manager.activeScene() == editing &&
                         ManagedObject::liveCount == 2,
                     "Prepared runtime scene ownership is incorrect");
    passed &= expect(manager.commitRuntimeScene() &&
                         manager.activeScene() == firstRuntime &&
                         manager.isRuntimeSceneActive() &&
                         manager.returnToEditingScene() &&
                         !manager.isRuntimeSceneActive() &&
                         manager.destroyRuntimeScene(),
                     "Scene Manager could not complete a runtime lifecycle");
    passed &= expect(manager.editingScene() == editing &&
                         manager.runtimeScene() == nullptr &&
                         ManagedObject::liveCount == 1,
                     "Destroying runtime scene affected the editing scene");

    passed &= expect(static_cast<bool>(manager.prepareRuntimeScene()),
                     "Scene Manager could not prepare a second runtime scene");
    passed &= expect(
                         static_cast<ManagedObject*>(
                             manager.runtimeScene()->gameObjects().front().get())
                                 ->instanceId != firstRuntimeObjectId &&
                         ManagedScene::constructionCount == 3,
                     "Repeated Play did not construct fresh runtime objects");
    manager.cancelPreparedRuntimeScene();
    manager.shutdown();
    passed &= expect(ManagedObject::liveCount == 0,
                     "Scene Manager shutdown leaked owned scenes");
    return passed;
}

bool runPlayerCameraInitializationTests() {
    bool passed = true;
    const glm::vec3 playerWorldUp{0.0f, 1.0f, 0.0f};
    const glm::vec3 directions[] = {
        {0.0f, 0.0f, -1.0f},
        {0.98f, 0.15f, 0.1f},
        {-0.98f, -0.1f, 0.15f},
        {0.2f, 0.96f, -0.1f},
        {0.1f, -0.95f, -0.2f},
        {0.02f, 0.999f, 0.01f},
        {0.02f, -0.999f, 0.01f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
    };

    for (const glm::vec3& sourceDirection : directions) {
        Camera expectedCamera;
        expectedCamera.front = glm::normalize(sourceDirection);
        double expectedYaw = 0.0;
        double expectedPitch = 0.0;
        const bool expectedDerived = expectedCamera.deriveYawPitchDegrees(
            expectedYaw, expectedPitch);
        expectedPitch = std::clamp(expectedPitch, -89.0, 89.0);
        const bool expectedSet = expectedCamera.setYawPitchDegrees(
            expectedYaw, expectedPitch, playerWorldUp);
        passed &= expect(expectedDerived && expectedSet,
                         "Could not configure expected Player camera state");

        Player player;
        player.init();
        const glm::vec3 normalizedSource = glm::normalize(sourceDirection);
        player.camera->front = normalizedSource;
        player.camera->up = playerWorldUp;
        player.start(nullptr);

        const double playerYaw = PlayerTestAccess::yaw(player);
        const double playerPitch = PlayerTestAccess::pitch(player);
        passed &= expect(std::isfinite(playerYaw) && std::isfinite(playerPitch),
                         "Player startup produced invalid look angles");
        passed &= expect(
            std::fabs(playerYaw - expectedYaw) < 1.0e-3 &&
                std::fabs(playerPitch - expectedPitch) < 1.0e-3,
            "Player startup did not consume the authored Camera direction");
        passed &= expect(sameVector(player.camera->front,
                                    expectedCamera.front),
                         "Player startup produced an unexpected Camera direction");
        passed &= expect(
            sameVector(player.camera->up, playerWorldUp),
            "Player look-angle synchronization did not canonicalize world up");
        passed &= expect(playerPitch >= -89.0 && playerPitch <= 89.0,
                         "Player look synchronization exceeded pitch limits");
    }

    Player rotatedCameraPlayer;
    rotatedCameraPlayer.init();
    const float authoredPitch = glm::radians(35.0f);
    rotatedCameraPlayer.camera->front = {
        0.0f, std::sin(authoredPitch), -std::cos(authoredPitch)};
    rotatedCameraPlayer.camera->up = {
        0.0f, std::cos(authoredPitch), std::sin(authoredPitch)};

    Camera expectedRotatedCamera;
    expectedRotatedCamera.front = rotatedCameraPlayer.camera->front;
    double expectedRotatedYaw = 0.0;
    double expectedRotatedPitch = 0.0;
    const bool expectedRotatedDerived =
        expectedRotatedCamera.deriveYawPitchDegrees(
            expectedRotatedYaw, expectedRotatedPitch);
    expectedRotatedPitch = std::clamp(expectedRotatedPitch, -89.0, 89.0);
    const bool expectedRotatedSet = expectedRotatedCamera.setYawPitchDegrees(
        expectedRotatedYaw, expectedRotatedPitch, playerWorldUp);
    rotatedCameraPlayer.start(nullptr);

    passed &= expect(
        expectedRotatedDerived && expectedRotatedSet &&
            sameVector(rotatedCameraPlayer.camera->front,
                       expectedRotatedCamera.front) &&
            sameVector(rotatedCameraPlayer.camera->up, playerWorldUp) &&
            glm::length(glm::cross(rotatedCameraPlayer.camera->front,
                                   rotatedCameraPlayer.camera->up)) > 1.0e-4f,
        "Editor-rotated Camera up contaminated the runtime Player basis");

    auto editorInput = std::make_shared<InputManager>();
    Player simulatedPlayer;
    simulatedPlayer.init();
    simulatedPlayer.camera->front = {0.3f, 0.2f, -0.9f};
    simulatedPlayer.camera->up = playerWorldUp;
    simulatedPlayer.start(editorInput);
    passed &= expect(!editorInput->gameplayInputEnabled() &&
                         editorInput->inputMode() ==
                             InputMode::EditorInteractive &&
                         std::isfinite(PlayerTestAccess::yaw(simulatedPlayer)) &&
                         std::isfinite(PlayerTestAccess::pitch(simulatedPlayer)),
                     "Player startup captured gameplay input during editor ownership");

    simulatedPlayer.position = {4.0f, 5.0f, 6.0f};
    simulatedPlayer.camera->position = {4.0f, 5.0f, 6.0f};
    SDL_Event moveEvent{};
    moveEvent.type = SDL_EVENT_KEY_DOWN;
    moveEvent.key.key = SDLK_W;
    editorInput->handleEvent(moveEvent);
    simulatedPlayer.update(editorInput);
    passed &= expect(simulatedPlayer.position == glm::vec3(4.0f, 5.0f, 6.0f) &&
                         simulatedPlayer.camera->position ==
                             glm::vec3(4.0f, 5.0f, 6.0f),
                     "Player moved while gameplay input was editor-owned");

    return passed;
}

bool runPlayerMovementInvariantTests() {
    bool passed = true;
    Player player;
    player.init();
    player.camera->front = glm::normalize(glm::vec3(0.55f, 0.45f, -0.7f));
    player.camera->up = glm::normalize(glm::vec3(0.0f, 0.8f, 0.6f));
    player.start(nullptr);

    const glm::vec3 offset{1.5f, -2.0f, 3.25f};
    player.position = {10.0f, 20.0f, 30.0f};
    player.camera->position = player.position + offset;

    const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};
    const glm::vec3 forward = player.camera->front;
    const glm::vec3 right = glm::normalize(glm::cross(forward, worldUp));
    const glm::vec3 deltas[] = {forward, right, worldUp, -forward, -right,
                                -worldUp};
    const char* deltaNames[] = {"forward", "strafe", "vertical", "reverse",
                                "reverse strafe", "reverse vertical"};

    const std::size_t deltaCount = sizeof(deltas) / sizeof(deltas[0]);
    for (std::size_t index = 0; index < deltaCount; ++index) {
        const glm::vec3 previousPlayerPosition = player.position;
        const glm::vec3 previousCameraPosition = player.camera->position;
        PlayerTestAccess::applyMovementDelta(player, deltas[index]);

        passed &= expect(
            sameVector(player.camera->position - player.position, offset),
            "Player and Camera movement changed their relative offset");
        passed &= expect(
            sameVector(player.position - previousPlayerPosition, deltas[index]) &&
                sameVector(player.camera->position - previousCameraPosition,
                           deltas[index]),
            deltaNames[index]);
    }

    return passed;
}

struct CountingPixelDeleter {
    std::size_t* releaseCount = nullptr;

    void operator()(stbi_uc* pixels) const noexcept {
        ++*releaseCount;
        delete[] pixels;
    }
};

}  // namespace

static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().gameObjects()),
              const std::vector<std::unique_ptr<GameObject>>&>);
static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().pointLightAt(0)),
              const PointLight&>);
static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().directionalLight()),
              const DirectionalLight*>);
static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().activeCamera()),
              const Camera*>);
static_assert(std::is_same_v<
              decltype(std::declval<ModelRenderable&>().meshInstances()),
              const std::vector<MeshInstance>&>);
static_assert(std::is_same_v<
              decltype(std::declval<GameObject&>().loadModel()), Result>);
static_assert(offsetof(MaterialPushConstants, baseColorFactor) == 0);
static_assert(offsetof(MaterialPushConstants, metallicFactor) == 16);
static_assert(offsetof(MaterialPushConstants, roughnessFactor) == 20);
static_assert(offsetof(MaterialPushConstants, alphaMode) == 24);
static_assert(offsetof(MaterialPushConstants, alphaCutoff) == 28);
static_assert(offsetof(MaterialPushConstants, normalMapEnabled) == 32);
static_assert(offsetof(MaterialPushConstants,
                       metallicRoughnessMapEnabled) == 36);
static_assert(sizeof(MaterialPushConstants) == 40);
static_assert(alignof(MaterialPushConstants) % 4 == 0);

int main() {
    bool passed = true;
    passed &= runLevel1FailurePropagationTest();
    passed &= runAuthoringTransferTests();
    passed &= runSceneManagerTests();
    passed &= runPlayerCameraInitializationTests();
    passed &= runPlayerMovementInvariantTests();

    Vertex vertex{};
    vertex.normal = {0.0f, 0.0f, 1.0f};
    vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
    Vertex differentNormal = vertex;
    differentNormal.normal.x = 1.0f;
    Vertex differentTangent = vertex;
    differentTangent.tangent.y = 1.0f;
    Vertex differentHandedness = vertex;
    differentHandedness.tangent.w = -1.0f;
    std::unordered_map<Vertex, uint32_t> uniqueVertices;
    uniqueVertices.emplace(vertex, 0);
    uniqueVertices.emplace(differentNormal, 1);
    uniqueVertices.emplace(differentTangent, 2);
    uniqueVertices.emplace(differentHandedness, 3);
    passed &= expect(!(vertex == differentNormal) &&
                         !(vertex == differentTangent) &&
                         !(vertex == differentHandedness) &&
                         uniqueVertices.size() == 4,
                     "Vertex identity collapsed a normal or tangent seam");

    const auto attributes = Vertex::getAttributeDescriptions();
    passed &= expect(attributes.size() == 5 &&
                         Vertex::getBindingDescription().stride == sizeof(Vertex) &&
                         attributes[3].location == 3 &&
                         attributes[3].format == VK_FORMAT_R32G32B32_SFLOAT &&
                         attributes[3].offset == offsetof(Vertex, normal) &&
                         attributes[4].location == 4 &&
                         attributes[4].format == VK_FORMAT_R32G32B32A32_SFLOAT &&
                         attributes[4].offset == offsetof(Vertex, tangent),
                     "Vertex normal/tangent Vulkan layout is incorrect");

    const Material defaultMaterial;
    passed &= expect(defaultMaterial.baseColorFactor == glm::vec4(1.0f) &&
                         defaultMaterial.metallicFactor == 1.0f &&
                         defaultMaterial.roughnessFactor == 1.0f &&
                         !defaultMaterial.hasMetallicRoughnessMap &&
                         !defaultMaterial.normalMapEnabled &&
                         defaultMaterial.normalMapPath.empty() &&
                         defaultMaterial.normalMapPixels == nullptr &&
                         defaultMaterial.normalMapWidth == 0 &&
                         defaultMaterial.normalMapHeight == 0 &&
                         defaultMaterial.normalMapChannels == 0 &&
                         defaultMaterial.normalMapMipLevels == 0 &&
                         defaultMaterial.normalMapImage == VK_NULL_HANDLE &&
                         defaultMaterial.normalMapImageMemory == VK_NULL_HANDLE &&
                         defaultMaterial.normalMapImageView == VK_NULL_HANDLE &&
                         defaultMaterial.normalMapSampler == VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapPath.empty() &&
                         defaultMaterial.metallicRoughnessMapPixels == nullptr &&
                         defaultMaterial.metallicRoughnessMapWidth == 0 &&
                         defaultMaterial.metallicRoughnessMapHeight == 0 &&
                         defaultMaterial.metallicRoughnessMapChannels == 0 &&
                         defaultMaterial.metallicRoughnessMapMipLevels == 0 &&
                         defaultMaterial.metallicRoughnessMapImage ==
                             VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapImageMemory ==
                             VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapImageView ==
                             VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapSampler ==
                             VK_NULL_HANDLE,
                     "Default material state is not cleanup-safe");

    passed &= expect(scene_limits::maxPointLights == 16,
                     "The point-light limit is not 16");

    const PointLight defaultLight;
    passed &= expect(defaultLight.color.x == 1.0f &&
                         defaultLight.color.y == 1.0f &&
                         defaultLight.color.z == 1.0f,
                     "Point lights do not default to white");
    passed &= expect(defaultLight.intensity == 1.0f,
                     "Point lights do not default to intensity 1");
    passed &= expect(defaultLight.position.x == 0.0f &&
                         defaultLight.position.y == 0.0f &&
                         defaultLight.position.z == 0.0f,
                     "Point lights do not default to the origin");

    std::size_t pixelReleaseCount = 0;
    Material firstSharedMaterial;
    firstSharedMaterial.pixelsOwner = StbiPixelOwner(
        new stbi_uc[4], CountingPixelDeleter{&pixelReleaseCount});
    firstSharedMaterial.pixels = firstSharedMaterial.pixelsOwner.get();
    Material secondSharedMaterial = firstSharedMaterial;
    releaseStbiPixel(firstSharedMaterial.pixelsOwner,
                     firstSharedMaterial.pixels);
    passed &= expect(pixelReleaseCount == 0 && secondSharedMaterial.pixels != nullptr,
                     "Releasing one shared pixel reference destroyed live pixels");
    releaseStbiPixel(secondSharedMaterial.pixelsOwner,
                     secondSharedMaterial.pixels);
    passed &= expect(pixelReleaseCount == 1,
                     "Shared pixel allocation was not released exactly once");

    const DirectionalLight defaultDirectionalLight;
    glm::vec3 defaultDirectionalDirection;
    passed &= expect(defaultDirectionalLight.calculateWorldDirection(
                         defaultDirectionalDirection) &&
                         sameVector(defaultDirectionalDirection,
                                    glm::vec3(0.0f, -1.0f, 0.0f)) &&
                         nearlyEqual(glm::length(defaultDirectionalDirection),
                                     1.0f),
                     "Directional lights do not default to downward rays");
    passed &= expect(defaultDirectionalLight.color == glm::vec3(1.0f),
                     "Directional lights do not default to white");
    passed &= expect(defaultDirectionalLight.intensity == 1.0f,
                     "Directional lights do not default to intensity 1");

    TestScene scene;
    passed &= expect(scene.directionalLight() == nullptr,
                     "A new scene did not start without a directional light");
    passed &= expect(nearlyEqual(scene.backgroundColor().r, 0.639f) &&
                         nearlyEqual(scene.backgroundColor().g, 0.965f) &&
                         nearlyEqual(scene.backgroundColor().b, 1.0f) &&
                         nearlyEqual(scene.backgroundColor().a, 1.0f),
                     "A new scene has the wrong background color");
    passed &= expect(scene.ambientColor() == glm::vec3(1.0f) &&
                         nearlyEqual(scene.ambientIntensity(), 0.1f),
                     "A new scene has the wrong ambient light");

    const glm::vec4 background{0.2f, 0.3f, 0.4f, 0.5f};
    passed &= expect(static_cast<bool>(scene.setBackgroundColor(background)) &&
                         scene.backgroundColor() == background,
                     "A valid background color was rejected");
    passed &= expect(!scene.setBackgroundColor(glm::vec4(-0.1f, 0.3f, 0.4f, 0.5f)) &&
                         scene.backgroundColor() == background,
                     "A negative background component was accepted");
    passed &= expect(!scene.setBackgroundColor(glm::vec4(0.2f, 1.1f, 0.4f, 0.5f)) &&
                         scene.backgroundColor() == background,
                     "An oversized background component was accepted");
    passed &= expect(!scene.setBackgroundColor(glm::vec4(
                             std::numeric_limits<float>::quiet_NaN(),
                             0.3f, 0.4f, 0.5f)) &&
                         scene.backgroundColor() == background,
                     "A non-finite background component was accepted");

    const glm::vec3 ambient{0.3f, 0.4f, 0.5f};
    passed &= expect(static_cast<bool>(scene.setAmbientLight(ambient, 2.0f)) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A valid ambient light was rejected");
    passed &= expect(!scene.setAmbientLight(glm::vec3(-0.1f, 0.4f, 0.5f), 2.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A negative ambient component was accepted");
    passed &= expect(!scene.setAmbientLight(glm::vec3(0.3f, 1.1f, 0.5f), 2.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "An oversized ambient component was accepted");
    passed &= expect(!scene.setAmbientLight(ambient, -1.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A negative ambient intensity was accepted");
    passed &= expect(!scene.setAmbientLight(
                             glm::vec3(std::numeric_limits<float>::infinity(),
                                       0.4f, 0.5f),
                             2.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A non-finite ambient component was accepted");
    passed &= expect(!scene.setAmbientLight(
                             ambient,
                             std::numeric_limits<float>::infinity()) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A non-finite ambient intensity was accepted");

    passed &= expect(scene.pointLightCount() == 0,
                     "A new scene did not start with zero point lights");
    bool zeroLightLookupRejected = false;
    try {
        (void)scene.pointLightAt(0);
    } catch (const std::out_of_range&) {
        zeroLightLookupRejected = true;
    }
    passed &= expect(zeroLightLookupRejected,
                     "A zero-light scene accepted a light lookup");
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A scene without an active camera failed validation");
    passed &= expect(
        !scene.addGameObject(std::unique_ptr<GameObject>{}),
        "A scene accepted a null game object");

    const auto camera = std::make_shared<Camera>();
    const Result cameraResult = scene.setActiveCamera(camera);
    passed &= expect(static_cast<bool>(cameraResult),
                     "A scene rejected a valid active camera");
    passed &= expect(scene.activeCamera() == camera.get(),
                     "A scene did not retain its active camera");
    passed &= expect(
        !scene.setActiveCamera(std::shared_ptr<Camera>{}),
        "A scene accepted a null active camera");
    passed &= expect(scene.activeCamera() == camera.get(),
                     "A rejected null camera replaced the active camera");
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A valid zero-light scene failed validation");

    const std::size_t objectsBeforeDirectional = scene.gameObjects().size();
    const std::size_t pointsBeforeDirectional = scene.pointLightCount();
    auto directionalLight = std::make_unique<DirectionalLight>();
    const DirectionalLight* expectedDirectionalLight = directionalLight.get();
    const Result directionalResult =
        scene.addGameObject(std::move(directionalLight));
    passed &= expect(static_cast<bool>(directionalResult),
                     "A valid directional light was rejected");
    passed &= expect(scene.directionalLight() == expectedDirectionalLight,
                     "The scene did not retain the registered directional light");
    passed &= expect(scene.gameObjects().size() == objectsBeforeDirectional + 1,
                     "Adding a directional light did not add one owning object");
    passed &= expect(scene.pointLightCount() == pointsBeforeDirectional,
                     "Adding a directional light changed point-light registration");

    const std::size_t objectsBeforeSecondDirectional = scene.gameObjects().size();
    const std::size_t pointsBeforeSecondDirectional = scene.pointLightCount();
    const Result secondDirectionalResult = scene.addGameObject(
        std::make_unique<DirectionalLight>());
    passed &= expect(!static_cast<bool>(secondDirectionalResult),
                     "A scene accepted a second directional light");
    passed &= expect(scene.directionalLight() == expectedDirectionalLight,
                     "Rejecting a second directional light changed registration");
    passed &= expect(scene.gameObjects().size() == objectsBeforeSecondDirectional,
                     "Rejecting a second directional light changed ownership");
    passed &= expect(scene.pointLightCount() == pointsBeforeSecondDirectional,
                     "Rejecting a second directional light changed point lights");
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A valid scene with one directional light failed validation");

    auto expectInvalidDirectionalState =
        [&passed](const char* message, const auto& configure) {
            TestScene invalidScene;
            const Result cameraResult = invalidScene.setActiveCamera(
                std::make_shared<Camera>());
            if (!expect(static_cast<bool>(cameraResult),
                        "An invalid-light test could not set its camera")) {
                passed = false;
                return;
            }

            auto light = std::make_unique<DirectionalLight>();
            configure(*light);
            const Result addResult = invalidScene.addGameObject(
                std::move(light));
            if (!expect(static_cast<bool>(addResult),
                        "An invalid directional light could not be added")) {
                passed = false;
                return;
            }

            const Result validationResult =
                invalidScene.validateForActivation();
            passed &= expect(!static_cast<bool>(validationResult) &&
                                 hasDirectionalLightError(validationResult),
                             message);
        };

    expectInvalidDirectionalState(
        "A non-finite directional-light rotation passed validation",
        [](DirectionalLight& light) {
            light.rotation.x = std::numeric_limits<float>::quiet_NaN();
        });
    expectInvalidDirectionalState(
        "A negative directional-light intensity passed validation",
        [](DirectionalLight& light) { light.intensity = -1.0f; });
    expectInvalidDirectionalState(
        "A non-finite directional-light intensity passed validation",
        [](DirectionalLight& light) {
            light.intensity = std::numeric_limits<float>::infinity();
        });
    expectInvalidDirectionalState(
        "A negative directional-light color passed validation",
        [](DirectionalLight& light) { light.color.r = -1.0f; });
    expectInvalidDirectionalState(
        "A non-finite directional-light color passed validation",
        [](DirectionalLight& light) {
            light.color.g = std::numeric_limits<float>::infinity();
        });

    for (std::size_t index = 0;
         index < scene_limits::maxPointLights; ++index) {
        auto light = std::make_unique<PointLight>();
        const PointLight* expectedLight = light.get();
        const Result addResult = scene.addGameObject(std::move(light));
        passed &= expect(static_cast<bool>(addResult),
                         "A point light within the limit was rejected");
        if (addResult) {
            passed &= expect(&scene.pointLightAt(index) == expectedLight,
                             "A point light was not registered exactly once");
        }
    }

    passed &= expect(
        scene.pointLightCount() == scene_limits::maxPointLights,
        "The scene did not contain exactly 16 point lights");
    passed &= expect(
        scene.gameObjects().size() == scene_limits::maxPointLights + 1,
        "The 16 point lights did not preserve the directional-light object");
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A valid 16-light scene failed validation");

    const Result seventeenthResult =
        scene.addGameObject(std::make_unique<PointLight>());
    passed &= expect(!static_cast<bool>(seventeenthResult),
                     "A scene accepted a seventeenth point light");
    passed &= expect(
        scene.pointLightCount() == scene_limits::maxPointLights,
        "Rejecting the seventeenth light changed the light count");
    passed &= expect(
        scene.gameObjects().size() == scene_limits::maxPointLights + 1,
        "Rejecting the seventeenth light changed object ownership");
    bool seventeenthLookupRejected = false;
    try {
        (void)scene.pointLightAt(scene_limits::maxPointLights);
    } catch (const std::out_of_range&) {
        seventeenthLookupRejected = true;
    }
    passed &= expect(seventeenthLookupRejected,
                     "The light accessor accepted index 16");

    auto ordinaryObject = std::make_unique<GameObject>();
    passed &= expect(ordinaryObject->modelRenderable().meshInstances().empty(),
                     "A new game object did not start without meshes");
    MeshInstance meshInstance{};
    meshInstance.mesh.vertices = {
        Vertex{{0.0f, 0.0f, 0.0f}}, Vertex{{1.0f, 0.0f, 0.0f}},
        Vertex{{0.0f, 1.0f, 0.0f}}};
    meshInstance.mesh.indices = {0, 1, 2};
    const Result meshResult = ordinaryObject->modelRenderable().addMeshInstance(
        std::move(meshInstance));
    passed &= expect(static_cast<bool>(meshResult),
                     "An inactive game object rejected a mesh instance");
    passed &= expect(ordinaryObject->modelRenderable().meshInstances().size() == 1 &&
                         ordinaryObject->modelRenderable().meshInstances().front()
                                 .mesh.indices.front() == 0,
                     "The checked mesh API did not preserve one mesh");
    const Result ordinaryObjectResult =
        scene.addGameObject(std::move(ordinaryObject));
    passed &= expect(static_cast<bool>(ordinaryObjectResult),
                     "The point-light limit rejected an ordinary object");
    passed &= expect(
        scene.pointLightCount() == scene_limits::maxPointLights,
        "Adding an ordinary object changed the point-light count");

    return passed ? 0 : 1;
}
