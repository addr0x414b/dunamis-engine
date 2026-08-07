#include "player.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace {

constexpr float minimumLookDirectionLengthSquared = 1.0e-8f;
constexpr double radiansToDegrees = 57.2957795130823208768;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool normalizeFinite(const glm::vec3& value,
                     glm::vec3& normalized) noexcept {
    normalized = {};
    if (!isFiniteVector(value)) {
        return false;
    }

    const float lengthSquared = glm::dot(value, value);
    if (!std::isfinite(lengthSquared) ||
        lengthSquared <= minimumLookDirectionLengthSquared) {
        return false;
    }
    const float length = std::sqrt(lengthSquared);
    if (!std::isfinite(length) || length <= 0.0f) {
        return false;
    }

    normalized = value / length;
    return isFiniteVector(normalized);
}

}  // namespace

void Player::init() {
    name = "Player";

    auto c = std::make_unique<Camera>();
    camera = std::move(c);
}

bool Player::syncLookAnglesFromCamera() noexcept {
    if (!camera) {
        return false;
    }

    glm::vec3 normalizedFront;
    glm::vec3 normalizedUp;
    if (!normalizeFinite(camera->front, normalizedFront) ||
        !normalizeFinite(camera->up, normalizedUp)) {
        return false;
    }

    const glm::vec3 basisCross = glm::cross(normalizedFront, normalizedUp);
    const float crossLengthSquared = glm::dot(basisCross, basisCross);
    if (!isFiniteVector(basisCross) || !std::isfinite(crossLengthSquared) ||
        crossLengthSquared <= minimumLookDirectionLengthSquared) {
        return false;
    }

    const float clampedY = std::clamp(normalizedFront.y, -1.0f, 1.0f);
    const double synchronizedPitch =
        std::asin(static_cast<double>(clampedY)) * radiansToDegrees;
    const double synchronizedYaw =
        std::atan2(static_cast<double>(normalizedFront.z),
                   static_cast<double>(normalizedFront.x)) *
        radiansToDegrees;
    if (!std::isfinite(synchronizedPitch) ||
        !std::isfinite(synchronizedYaw)) {
        return false;
    }

    pitch = std::clamp(synchronizedPitch, -89.0, 89.0);
    yaw = synchronizedYaw;
    camera->front = normalizedFront;
    front = camera->front;
    return true;
}

void Player::start(std::shared_ptr<InputManager> input) {
    if (!syncLookAnglesFromCamera()) {
        spdlog::error("Player could not synchronize look angles from its camera");
        return;
    }

    if (!input) {
        return;
    }

    const Result result = input->requestGameplayMouseCapture();
    if (!result) {
        spdlog::error("Player failed to capture gameplay mouse: {}",
                      result.error());
    }
}

void Player::update(std::shared_ptr<InputManager> input) {
    if (!input->gameplayInputEnabled()) {
        return;
    }

    if (input->isKeyDown(SDLK_LSHIFT)) {
        speed = 5.0f;
    }else if (input->isKeyDown(SDLK_LCTRL)) {
        speed = 0.3f;
    }
    else {
        speed = 1.0f;
    }

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
