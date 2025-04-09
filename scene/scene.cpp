#include "scene.h"

void Scene::start() {
    for (const auto& gameObject : gameObjects) {
        gameObject->storeDetails();
        gameObject->start();
    }
}

void Scene::run() {
    for (const auto& gameObject : gameObjects) {
        gameObject->run();
    }
}

void Scene::stop() {
    for (const auto& gameObject : gameObjects) {
        gameObject->stop();
    }
}

void Scene::init() {
    GameObject* test = new GameObject("test_models/models/dennis.obj",
                    "test_models/textures/dennis.jpg", "Dennis 1");
    test->scale = glm::vec3(0.01f, 0.01f, 0.01f);
    test->position = glm::vec3(0.0f, -80.0f, -100.0f);
    gameObjects.push_back(test);

    /*GameObject test2("test_models/models/dennis.obj",
                    "test_models/textures/dennis.jpg", "Dennis Big Boy");
    test2.scale = glm::vec3(0.01f, 0.01f, 0.01f);
    test2.position = glm::vec3(0.0f, -80.0f, -100.0f);
    test2.rotation = glm::vec3(0.0f, 90.0f, 0.0f);
    gameObjects.push_back(&test2);

    GameObject test3("test_models/models/dennis.obj",
                    "test_models/textures/dennis.jpg");
    test3.scale = glm::vec3(0.01f, 0.01f, 0.01f);
    test3.position = glm::vec3(20.0f, -40.0f, -100.0f);
    //test3.rotation = glm::vec3(0.0f, 90.0f, 0.0f);
    gameObjects.push_back(&test3);*/

    Camera* sceneCam = new Camera();
    sceneCam->position = glm::vec3(0.0f, 0.0f, 20.0f);
    sceneCam->name = "Main Scene Camera";
    sceneCamera = *sceneCam;
    cameras.push_back(sceneCam);

}

Scene::~Scene() {
    for (auto& gameObject : gameObjects) {
        delete gameObject;
    }
    gameObjects.clear();

    for (auto* cam : cameras) {
        delete cam;
    }
    cameras.clear();
}