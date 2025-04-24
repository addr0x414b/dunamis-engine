#include "level_1.h"

void Level1::init() {

    spdlog::info("Initializing scene {}...", name);

    auto test = std::make_unique<GameObject>();
    test->name = "Viking Room";
    test->scale = glm::vec3(50.0f, 50.0f, 50.0f);
    test->rotation = glm::vec3(-80.0f, 0.0f, -90.0f);
    test->position.y = -25.0f;
    //test->mesh.modelPath = "game/assets/models/viking_room.obj";
    test->modelPath = "game/assets/models/viking_room.obj";
    test->texturePath = "game/assets/textures/viking_room.png";
    test->loadModel();
    gameObjects.push_back(std::move(test));

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
    test2->loadModel(test2->mesh.modelPath);
    gameObjects.push_back(std::move(test2));*/

    auto test3 = std::make_unique<GameObject>();
    test3->name = "GLTF test";
    test3->scale = glm::vec3(200.0f, 200.0f, 200.0f);
    test3->modelPath = "game/assets/models/Avocado.glb";
    test3->loadModel();
    gameObjects.push_back(std::move(test3));

    auto test4 = std::make_unique<GameObject>();
    test4->name = "GLTF test";
    test4->scale = glm::vec3(10.0f, 10.0f, 10.0f);
    //test4->mesh.modelPath = "game/assets/models/Bistro_Godot.glb";
    test4->modelPath = "game/assets/models/Bistro_Godot.glb";
    //test4->material.texturePath = "game/assets/textures/san_giuseppe_bridge_4k.png";
    test4->loadModel();
    gameObjects.push_back(std::move(test4));

    auto test5 = std::make_unique<GameObject>();
    test5->name = "GLTF test";
    test5->modelPath = "game/assets/models/tester.glb";
    test5->loadModel();
    gameObjects.push_back(std::move(test5));

    player.init();
    camera = player.camera;

    spdlog::info("Scene successfully initialized");

} 

void Level1::start() {
    for (auto& obj : gameObjects) {
        obj->start();
    }
    player.start(inputManager);
}

void Level1::update() {
    for (auto& obj : gameObjects) {
        obj->update();
    }
    player.update(inputManager);
}