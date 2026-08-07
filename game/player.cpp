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

const glm::vec3 Player::worldUp = glm::vec3(0.0f, 1.0f, 0.0f);

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
    if (!normalizeFinite(camera->front, normalizedFront)) {
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

    glm::vec3 canonicalForward;
    if (!reconstructForward(canonicalForward)) {
        return false;
    }

    camera->front = canonicalForward;
    front = canonicalForward;
    camera->up = worldUp;
    up = worldUp;
    return true;
}

bool Player::reconstructForward(glm::vec3& forward) const noexcept {
    forward = {};
    if (!std::isfinite(yaw) || !std::isfinite(pitch)) {
        return false;
    }

    glm::vec3 direction;
    direction.x = std::cos(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    direction.y = std::sin(glm::radians(pitch));
    direction.z = std::sin(glm::radians(yaw)) * std::cos(glm::radians(pitch));
    return normalizeFinite(direction, forward);
}

void Player::applyMovementDelta(const glm::vec3& delta) noexcept {
    if (!camera || !isFiniteVector(delta)) {
        return;
    }

    position += delta;
    camera->position += delta;
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

    up = worldUp;
    camera->up = worldUp;

    if (input->isKeyDown(SDLK_LSHIFT)) {
        speed = 5.0f;
    }else if (input->isKeyDown(SDLK_LCTRL)) {
        speed = 0.3f;
    }
    else {
        speed = 1.0f;
    }

    glm::vec3 right;
    if (!normalizeFinite(glm::cross(front, worldUp), right)) {
        spdlog::error("Player could not calculate a valid movement right vector");
        return;
    }

    if (input->isKeyDown(SDLK_W)) {
        applyMovementDelta(speed * front);
    }

    if (input->isKeyDown(SDLK_D)) {
        applyMovementDelta(speed * right);
    }

    if (input->isKeyDown(SDLK_A)) {
        applyMovementDelta(-speed * right);
    }

    if (input->isKeyDown(SDLK_S)) {
        applyMovementDelta(-speed * front);
    }

    if (input->isKeyDown(SDLK_E)) {
        applyMovementDelta(speed * worldUp);
    }

    if (input->isKeyDown(SDLK_Q)) {
        applyMovementDelta(-speed * worldUp);
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

    glm::vec3 canonicalForward;
    if (!reconstructForward(canonicalForward)) {
        spdlog::error("Player could not reconstruct a valid look direction");
        return;
    }

    camera->front = canonicalForward;
    front = canonicalForward;
    camera->up = worldUp;
    up = worldUp;
}
