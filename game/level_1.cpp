#include "level_1.h"

void Level1::init() {

    spdlog::info("Initializing scene {}...", name);

    auto test = std::make_unique<GameObject>();
    test->name = "Viking Room";
    test->scale = glm::vec3(50.0f, 50.0f, 50.0f);
    test->rotation = glm::vec3(-80.0f, 0.0f, -90.0f);
    test->position.y = -25.0f;
    test->mesh.modelPath = "game/assets/models/viking_room.obj";
    test->material.texturePath = "game/assets/textures/viking_room.png";
    test->loadModel(test->mesh.modelPath);
    gameObjects.push_back(std::move(test));

    auto c = std::make_unique<Camera>();
    c->position.z = 150.0f;
    camera = std::move(c);

    spdlog::info("Scene successfully initialized");

} 

void Level1::start() {

}

void Level1::update() {

}