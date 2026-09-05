#ifndef DIRECTIONAL_LIGHT_H
#define DIRECTIONAL_LIGHT_H

#include "game_object.h"

struct DirectionalShadowSettings {
    // All shadow-volume positions and distances are world meters.
    glm::vec3 focus{0.0f};
    float halfExtent = 5.0f;
    float lightDistance = 5.0f;
    float nearPlane = 0.01f;
    float farPlane = 10.0f;
};

class DirectionalLight : public GameObject {
public:
    DirectionalLight();

    [[nodiscard]] bool calculateWorldDirection(
        glm::vec3& worldDirection) const noexcept;

    glm::vec3 color{1.0f, 1.0f, 1.0f};
    float intensity = 1.0f;
    DirectionalShadowSettings shadow{};
};

#endif
