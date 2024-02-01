#ifndef SCENE_H
#define SCENE_H

#include <vector>

#include "3d/camera.h"
#include "3d/game_object.h"

class Scene {
   public:
    std::vector<GameObject*> gameObjects;
    Camera sceneCamera;

   private:
};

#endif  // SCENE_H
