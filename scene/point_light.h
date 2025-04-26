#ifndef POINT_LIGHT_H
#define POINT_LIGHT_H

#include "game_object.h"

class PointLight : public GameObject {
public:
    glm::vec3 color;
    float intensity = 1.0f;

private:
};

#endif