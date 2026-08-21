#ifndef CHARACTER_H
#define CHARACTER_H

#include "game_object.h"

// A generic game object whose movement is resolved by character physics.
// Values are authored in Dunamis units and Dunamis units per second.
class Character : public GameObject {
public:
    float capsuleHeight = 180.0f;
    float capsuleRadius = 35.0f;
    glm::vec3 desiredVelocity = glm::vec3(0.0f);
    bool grounded = false;
};

#endif
