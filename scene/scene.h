#ifndef SCENE_H
#define SCENE_H

#include <vector>

#include "3d/camera.h"
#include "3d/game_object.h"

class Scene {
   public:
    std::vector<GameObject*> gameObjects;
    std::vector<Camera*> cameras;
    Camera sceneCamera;
    bool simulating = false;

   private:
};

#endif  // SCENE_H
