#ifndef SCENE_H
#define SCENE_H

#include "game_object.h"
#include "../input/input_manager.h"
#include "camera.h"
#include <vector>
#include "point_light.h"

class Scene {
public:
    virtual void init() = 0;
    virtual void start() = 0;
    virtual void update() = 0;
    virtual ~Scene() = default;

    std::vector<std::unique_ptr<GameObject>> gameObjects;
    std::vector<PointLight*> pointLights;
    //Camera* camera = nullptr;
    std::shared_ptr<Camera> camera;

    std::string name;
    std::shared_ptr<InputManager> inputManager;

private:

};

#endif