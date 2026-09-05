#include "level_1.h"

#include <stdexcept>

#include "../core/time.h"
#include "../scene/type_registry.h"

namespace {

void requireSuccess(Result result, const std::string& context) {
    if (!result) {
        throw std::runtime_error(context + ": " + result.error());
    }
}

}  // namespace

Result Level1::registerTypes(TypeRegistry& registry) {
    Result result = registry.registerType<Player>(
        "Player", "Character", [] {
            auto player = std::make_unique<Player>();
            player->init();
            return player;
        });
    return result;
}

void Level1::buildDefaults() {

    spdlog::info("Initializing scene {}...", name);

    /*auto test = std::make_unique<GameObject>();
    test->name = "Viking Room";
    test->scale = glm::vec3(50.0f, 50.0f, 50.0f);
    test->rotation = glm::vec3(-80.0f, 0.0f, -90.0f);
    test->position.y = -25.0f;
    //test->mesh.modelPath = "game/assets/models/viking_room.obj";
    test->modelPath = "game/assets/models/viking_room.obj";
    test->texturePath = "game/assets/textures/viking_room.png";
    (void)test->loadModel();
    (void)addGameObject(std::move(test));*/

    //auto c = std::make_unique<Camera>();
    //c->position.z = 150.0f;
    //camera = std::move(c);

    /*auto test2 = std::make_unique<GameObject>();
    test2->name = "Bistro";
    test2->scale = glm::vec3(0.1f, 0.1f, 0.1f);
    test2->rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
    //test2->position.y = -25.0f;
    test2->mesh.modelPath = "game/assets/models/BistroExterior.fbx";
    test2->material.texturePath = "game/assets/textures/viking_room.png";
    //(void)test2->loadModel();
    (void)addGameObject(std::move(test2));*/

    /*auto test3 = std::make_unique<GameObject>();
    //test3->persistentId = "sponza";
    test3->name = "Sponza";
    test3->scale = glm::vec3(1.0f, 1.0f, 1.0f);
    test3->physics.enabled = true;
    test3->physics.motionType = GameObject::PhysicsMotionType::Static;
    test3->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    test3->modelPath = "game/assets/models/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf";
    requireSuccess(test3->loadModel(), "Failed to load Sponza");
    requireSuccess(addGameObject(std::move(test3)), "Failed to add Sponza");*/

    auto test3 = std::make_unique<GameObject>();
    //test3->persistentId = "sponza";
    test3->name = "City";
    test3->scale = glm::vec3(1.0f, 1.0f, 1.0f);
    test3->physics.enabled = true;
    test3->physics.motionType = GameObject::PhysicsMotionType::Static;
    test3->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    test3->modelPath = "game/assets/city-scene/source/city.fbx";
    //test3->modelPath = "game/assets/test/source/Mobile Ready City Road.fbx";
    requireSuccess(test3->loadModel(), "Failed to load Sponza");
    requireSuccess(addGameObject(std::move(test3)), "Failed to add Sponza");

    auto test8 = std::make_unique<GameObject>();
    //test8->persistentId = "sponzaaaa";
    test8->name = "Fox";
    // glTF numeric coordinates are preserved at ingestion; this legacy
    // default scene needs the versioned-scene equivalent of a 0.01 scale.
    test8->scale = glm::vec3(0.01f, 0.01f, 0.01f);
    test8->physics.enabled = true;
    test8->physics.motionType = GameObject::PhysicsMotionType::Static;
    test8->physics.colliderType = GameObject::PhysicsColliderType::Mesh;
    test8->modelPath = "game/assets/models/glTF-Sample-Assets/Models/Fox/glTF/Fox.gltf";
    requireSuccess(test8->loadModel(), "Failed to load Sponza");
    requireSuccess(addGameObject(std::move(test8)), "Failed to add Sponza");

    auto test9 = std::make_unique<GameObject>();
    //test9->persistentId = "sponzaaaa23123";
    test9->name = "Pot";
    test9->scale = glm::vec3(7.5f, 7.5f, 7.5f);
    test9->physics.enabled = true;
    test9->physics.motionType = GameObject::PhysicsMotionType::Static;
    test9->physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    test9->modelPath = "game/assets/models/glTF-Sample-Assets/Models/PotOfCoals/glTF/PotOfCoals.gltf";
    requireSuccess(test9->loadModel(), "Failed to load Sponza");
    requireSuccess(addGameObject(std::move(test9)), "Failed to add Sponza");

    auto test10 = std::make_unique<GameObject>();
    test10->name = "Brah";
    //test10->persistentId = "90du-918dey798qwdgd";
    test10->scale = glm::vec3(1.0f, 1.0f, 1.0f);
    test10->rotation = glm::vec3(-90.0f, 0.0f, 0.0f);
    test10->position = glm::vec3(3.22f, 0.0f, -1.22f);
    test10->modelPath = "game/assets/models/glTF-Sample-Assets/Models/BrainStem/glTF/BrainStem.gltf";
    requireSuccess(test10->loadModel(), "Failed to load Sponza");
    requireSuccess(addGameObject(std::move(test10)), "Failed to add Sponza");

    // The loaded CPU Sponza mesh has a horizontal surface at (x=0, z=0,
    // y≈-2.5). In meter-valued world space, this gives the test body a
    // visible three-meter drop before it reaches the floor.
    auto avocado = std::make_unique<GameObject>();
    //avocado->persistentId = "physics_test_avocado";
    avocado->name = "Physics Test Avocado";
    avocado->position = glm::vec3(0.0f, 3.0f, -1.75f);
    avocado->scale = glm::vec3(5.0f);
    avocado->physics.enabled = true;
    avocado->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    avocado->physics.colliderType = GameObject::PhysicsColliderType::ConvexHull;
    avocado->modelPath = "game/assets/models/Avocado.glb";
    requireSuccess(avocado->loadModel(), "Failed to load physics test Avocado");
    requireSuccess(addGameObject(std::move(avocado)),
                   "Failed to add physics test Avocado");

    /*auto test4 = std::make_unique<PointLight>();
    test4->name = "GLTF test";
    test4->scale = glm::vec3(200.0f, 200.0f, 200.0f);
    test4->position = glm::vec3(50.0f, 20.0f, 100.0f);
    test4->modelPath = "game/assets/models/Avocado.glb";
    test4->shadow.focus = glm::vec3(0.0f);
    test4->shadow.halfExtent = 500.0f;
    test4->shadow.lightDistance = 500.0f;
    test4->shadow.nearPlane = 1.0f;
    test4->shadow.farPlane = 1000.0f;
    test4->intensity = 100.0f;
    (void)test4->loadModel();
    (void)addGameObject(std::move(test4));*/

    auto test4 = std::make_unique<DirectionalLight>();
    //test4->persistentId = "directional_light";
    test4->name = "Direc Light";
    test4->scale = glm::vec3(200.0f, 200.0f, 200.0f);
    test4->position = glm::vec3(0.5f, 0.2f, 1.0f);
    //test4->modelPath = "game/assets/models/Avocado.glb";
    //test4->intensity = 100.0f;
    //requireSuccess(test4->loadModel(), "Failed to load Directional Light Avocado");
    requireSuccess(addGameObject(std::move(test4)), "Failed to add Directional Light");

    auto test5 = std::make_unique<PointLight>();
    //test5->persistentId = "point_light";
    test5->name = "Point Light";
    test5->scale = glm::vec3(500.0f, 500.0f, 500.0f);
    test5->position = glm::vec3(2.5f, 0.2f, 1.0f);
    //test5->modelPath = "game/assets/models/Avocado.glb";
    test5->intensity = 100.0f;
    //requireSuccess(test5->loadModel(), "Failed to load Point Light Avocado");
    requireSuccess(addGameObject(std::move(test5)), "Failed to add Point Light");

    /*
    auto test4 = std::make_unique<GameObject>();
    test4->name = "GLTF test";
    test4->scale = glm::vec3(10.0f, 10.0f, 10.0f);
    //test4->mesh.modelPath = "game/assets/models/Bistro_Godot.glb";
    test4->modelPath = "game/assets/models/Bistro_Godot.glb";
    //test4->material.texturePath = "game/assets/textures/san_giuseppe_bridge_4k.png";
    (void)test4->loadModel();
    (void)addGameObject(std::move(test4));*/

    /*auto test5 = std::make_unique<GameObject>();
    test5->name = "GLTF test";
    test5->modelPath = "game/assets/models/tester.glb";
    (void)test5->loadModel();
    (void)addGameObject(std::move(test5));*/

    /*auto light = std::make_unique<PointLight>();
    light->name = "Point Light";
    light->modelPath = "game/assets/models/flatsphere.glb";
    light->intensity = 3.0f;
    light->position = glm::vec3(0.0f, 20.0f, 20.0f);
    light->color = glm::vec3(1.0f, 1.0f, 1.0f);
    (void)light->loadModel();
    (void)addGameObject(std::move(light));

    auto light2 = std::make_unique<PointLight>();
    light2->name = "Point Light";
    light2->modelPath = "game/assets/models/flatsphere.glb";
    light2->intensity = 10.0f;
    light2->position = glm::vec3(0.0f, 20.0f, 50.0f);
    light2->color = glm::vec3(1.0f, 0.0f, 0.0f);
    (void)light2->loadModel();
    (void)addGameObject(std::move(light2));*/

    auto player = std::make_unique<Player>();
    //player->persistentId = "player";
    Player* playerObserver = player.get();
    playerObserver->init();
    requireSuccess(addGameObject(std::move(player)),
                   "Failed to add Player");

    const Result cameraResult = setActiveCamera(playerObserver->camera);
    if (!cameraResult) {
        throw std::runtime_error(
            "Failed to set the Level 1 camera: " + cameraResult.error());
    }
    spdlog::info("Scene successfully initialized");

} 

void Level1::start() {
    player_ = nullptr;
    for (const auto& obj : gameObjects()) {
        if (auto* player = dynamic_cast<Player*>(obj.get())) {
            if (player_ != nullptr) {
                throw std::runtime_error(
                    "Level 1 requires exactly one Player");
            }
            player_ = player;
        }
    }
    if (player_ == nullptr) {
        throw std::runtime_error("Level 1 requires a Player");
    }
    for (const auto& obj : gameObjects()) {
        obj->start();
    }
    player_->start(inputManager);
}

void Level1::update() {
    constexpr float directionalLightRotationSpeedDegreesPerSecond = 30.0f;
    for (const auto& obj : gameObjects()) {
        obj->update();
    }
    if (player_ != nullptr) {
        player_->update(inputManager);
    }
}
