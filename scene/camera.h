#ifndef CAMERA_H
#define CAMERA_H

#include "game_object.h"

class Camera : public GameObject {
public:
    Camera() {
        name = "Camera";
    }
    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

private:
};

#endif