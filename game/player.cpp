#include "player.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

#include "../core/time.h"

namespace {

const glm::vec3 playerWorldUp{0.0f, 1.0f, 0.0f};

bool isFiniteMovementDelta(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

}  // namespace

void Player::init() {
    name = "Player";

    auto c = std::make_unique<Camera>();
    camera = std::move(c);
}

void Player::applyMovementDelta(const glm::vec3& delta) noexcept {
    if (!camera || !isFiniteMovementDelta(delta)) {
        return;
    }

    position += delta;
    camera->position += delta;
}

void Player::start(std::shared_ptr<InputManager> input) {
    if (!camera) {
        spdlog::error("Player could not synchronize look angles from its camera");
        return;
    }

    double startingYaw;
    double startingPitch;
    if (!camera->deriveYawPitchDegrees(startingYaw, startingPitch)) {
        spdlog::error("Player could not synchronize look angles from its camera");
        return;
    }

    startingPitch = std::clamp(startingPitch, -89.0, 89.0);
    if (!camera->setYawPitchDegrees(startingYaw, startingPitch,
                                   playerWorldUp)) {
        spdlog::error("Player could not synchronize look angles from its camera");
        return;
    }

    yaw = startingYaw;
    pitch = startingPitch;

    if (!input) {
        return;
    }

    if (!input->gameplayInputEnabled()) {
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

    float movementSpeedUnitsPerSecond = 60.0f;
    if (input->isKeyDown(SDLK_LSHIFT)) {
        movementSpeedUnitsPerSecond = 300.0f;
    } else if (input->isKeyDown(SDLK_LCTRL)) {
        movementSpeedUnitsPerSecond = 18.0f;
    }
    const float movementDistance =
        movementSpeedUnitsPerSecond * Time::deltaTime();

    glm::vec3 right = glm::cross(camera->front, playerWorldUp);
    const float rightLength = glm::length(right);
    if (!std::isfinite(rightLength) || rightLength <= 0.0f) {
        spdlog::error("Player could not calculate a valid movement right vector");
        return;
    }
    right /= rightLength;
    if (!std::isfinite(right.x) || !std::isfinite(right.y) ||
        !std::isfinite(right.z)) {
        spdlog::error("Player could not calculate a valid movement right vector");
        return;
    }

    if (input->isKeyDown(SDLK_W)) {
        applyMovementDelta(movementDistance * camera->front);
    }

    if (input->isKeyDown(SDLK_D)) {
        applyMovementDelta(movementDistance * right);
    }

    if (input->isKeyDown(SDLK_A)) {
        applyMovementDelta(-movementDistance * right);
    }

    if (input->isKeyDown(SDLK_S)) {
        applyMovementDelta(-movementDistance * camera->front);
    }

    if (input->isKeyDown(SDLK_E)) {
        applyMovementDelta(movementDistance * playerWorldUp);
    }

    if (input->isKeyDown(SDLK_Q)) {
        applyMovementDelta(-movementDistance * playerWorldUp);
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

    if (!camera->setYawPitchDegrees(yaw, pitch, playerWorldUp)) {
        spdlog::error("Player could not reconstruct a valid look direction");
        return;
    }
}
