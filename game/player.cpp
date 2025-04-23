#include "player.h"

void Player::init() {
    name = "Player";

    auto c = std::make_unique<Camera>();
    c->position.z = 300.0f;
    camera = std::move(c);

}

void Player::start(std::shared_ptr<InputManager> input) {
    input->setRelativeMouseMode(true);
}

void Player::update(std::shared_ptr<InputManager> input) {
    if (input->isKeyDown(SDLK_W)) {
        position += speed * camera->front;
        camera->position += speed * camera->front;
    }

    if (input->isKeyDown(SDLK_D)) {
        position += glm::normalize(glm::cross(front, up)) * speed;
        camera->position += glm::normalize(glm::cross(camera->front, camera->up)) * speed;
    }

    if (input->isKeyDown(SDLK_A)) {
        position -= glm::normalize(glm::cross(front, up)) * speed;
        camera->position -= glm::normalize(glm::cross(camera->front, camera->up)) * speed;
    }

    if (input->isKeyDown(SDLK_S)) {
        position -= speed * camera->front;
        camera->position -= speed * camera->front;
    }

    if (input->isKeyDown(SDLK_E)) {
        position += speed * up;
        camera->position += speed * up;
    }

    if (input->isKeyDown(SDLK_Q)) {
        position -= speed * up;
        camera->position -= speed * up;
    }

    double xPos = input->getMouseRelX();
    double yPos = input->getMouseRelY();
    float xOffset = xPos;
    float yOffset = -yPos;

    float sensitivity = 0.1f;
    xOffset *= sensitivity;
    yOffset *= sensitivity;

    yaw += xOffset;
    pitch += yOffset;

    if (pitch > 89.0f) {
        pitch = 89.0f;
    }
    if (pitch < -89.0f) {
        pitch = -89.0f;
    }

    glm::vec3 direction;
    direction.x = cos(glm::radians(yaw)) * cos(glm::radians(pitch));
    direction.y = sin(glm::radians(pitch));
    direction.z = sin(glm::radians(yaw)) * cos(glm::radians(pitch));
    camera->front = glm::normalize(direction);
    front = glm::normalize(direction);
}