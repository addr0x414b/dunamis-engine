#ifndef CHARACTER_H
#define CHARACTER_H

#include "game_object.h"

// A generic game object whose movement is resolved by character physics.
// Physical dimensions and desired linear velocity use meters and meters per
// second, respectively.
class Character : public GameObject {
public:
    float capsuleHeight = 1.8f;
    float capsuleRadius = 0.35f;
    glm::vec3 desiredVelocity = glm::vec3(0.0f);
    bool grounded = false;
};

#endif
