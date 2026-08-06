#ifndef DIRECTIONAL_LIGHT_H
#define DIRECTIONAL_LIGHT_H

#include "game_object.h"

class DirectionalLight : public GameObject {
public:
    DirectionalLight();

    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
};

#endif
