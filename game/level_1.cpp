#include "level_1.h"

#include <stdexcept>

void Level1::init() {

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

    auto test3 = std::make_unique<GameObject>();
    test3->name = "Sponza";
    test3->scale = glm::vec3(1.0f, 1.0f, 1.0f);
    test3->modelPath = "game/assets/models/glTF-Sample-Assets/Models/Sponza/glTF/Sponza.gltf";
    (void)test3->loadModel();
    (void)addGameObject(std::move(test3));

    /*auto test4 = std::make_unique<PointLight>();
    test4->name = "GLTF test";
    test4->scale = glm::vec3(200.0f, 200.0f, 200.0f);
    test4->position = glm::vec3(50.0f, 20.0f, 100.0f);
    test4->modelPath = "game/assets/models/Avocado.glb";
    test4->intensity = 100.0f;
    (void)test4->loadModel();
    (void)addGameObject(std::move(test4));*/

    auto test4 = std::make_unique<DirectionalLight>();
    test4->name = "Direc Light";
    test4->scale = glm::vec3(200.0f, 200.0f, 200.0f);
    test4->position = glm::vec3(50.0f, 20.0f, 100.0f);
    test4->modelPath = "game/assets/models/Avocado.glb";
    //test4->intensity = 100.0f;
    (void)test4->loadModel();
    (void)addGameObject(std::move(test4));

    auto test5 = std::make_unique<PointLight>();
    test5->name = "Point Light";
    test5->scale = glm::vec3(500.0f, 500.0f, 500.0f);
    test5->position = glm::vec3(250.0f, 20.0f, 100.0f);
    test5->modelPath = "game/assets/models/Avocado.glb";
    test5->intensity = 100.0f;
    (void)test5->loadModel();
    (void)addGameObject(std::move(test5));

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

    player.init();
    const Result cameraResult = setActiveCamera(player.camera);
    if (!cameraResult) {
        throw std::runtime_error(
            "Failed to set the Level 1 camera: " + cameraResult.error());
    }

    spdlog::info("Scene successfully initialized");

} 

void Level1::start() {
    for (const auto& obj : gameObjects()) {
        obj->start();
    }
    player.start(inputManager);
}

void Level1::update() {
    for (const auto& obj : gameObjects()) {
        obj->update();
        if (obj->name == "Point Light") {
            obj->position.z += 0.1f;
        }
    }
    player.update(inputManager);
}
