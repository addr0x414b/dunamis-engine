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