#ifndef PLAYER_H
#define PLAYER_H

#include <glm/glm.hpp>
#include "../input/input_manager.h"
#include "../scene/camera.h"
#include "../scene/game_object.h"
#include "spdlog/spdlog.h"

class Player : public GameObject {
public:
    void init();
    void start(std::shared_ptr<InputManager> input);
    void update(std::shared_ptr<InputManager> input);
    std::shared_ptr<Camera> camera;

    glm::vec3 front = glm::vec3(0.0f, 0.0f, -1.0f);
    glm::vec3 up = glm::vec3(0.0f, 1.0f, 0.0f);

private:
    float speed = 1.0f;
    double lastX;
    double lastY;
    double yaw = -90.0f;
    double pitch = 0.0f;
    bool firstMouse = true;
};

#endif