#ifndef DIRECTIONAL_LIGHT_H
#define DIRECTIONAL_LIGHT_H

#include "game_object.h"

struct DirectionalShadowSettings {
    glm::vec3 focus{0.0f};
    float halfExtent = 500.0f;
    float lightDistance = 500.0f;
    float nearPlane = 1.0f;
    float farPlane = 1000.0f;
};

class DirectionalLight : public GameObject {
public:
    DirectionalLight();

    glm::vec3 direction{0.0f, -1.0f, 0.0f};
    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    DirectionalShadowSettings shadow{};
};

#endif
