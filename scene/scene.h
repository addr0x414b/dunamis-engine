#ifndef SCENE_H
#define SCENE_H

#include "game_object.h"
#include "camera.h"
#include <vector>

class Scene {
public:
    virtual void init() = 0;
    virtual void start() = 0;
    virtual void update() = 0;
    virtual ~Scene() = default;

    std::vector<std::unique_ptr<GameObject>> gameObjects;
    //Camera* camera = nullptr;
    std::unique_ptr<Camera> camera;

    std::string name;

private:
};

#endif