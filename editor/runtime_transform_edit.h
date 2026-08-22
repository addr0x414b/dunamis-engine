#ifndef RUNTIME_TRANSFORM_EDIT_H
#define RUNTIME_TRANSFORM_EDIT_H

#include <glm/vec3.hpp>

class GameObject;

// Runtime-only editor handoff. It is intentionally independent of Jolt and
// persistence; the engine decides whether the selected object owns a body.
struct RuntimeTransformEdit {
    GameObject* object = nullptr;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    bool manipulating = false;
};

#endif
