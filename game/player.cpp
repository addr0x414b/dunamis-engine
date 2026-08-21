#include "player.h"

#include <algorithm>
#include <cmath>
#include <utility>

#include <spdlog/spdlog.h>

namespace {

const glm::vec3 playerWorldUp{0.0f, 1.0f, 0.0f};
constexpr float playerEyeHeightDunamisUnits = 150.0f;
constexpr float walkSpeedDunamisUnitsPerSecond = 190.0f;
constexpr float sprintSpeedDunamisUnitsPerSecond = 500.0f;
constexpr float minimumMovementVectorLength = 1.0e-4f;

bool isFiniteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

}  // namespace

void Player::init() {
    name = "Player";

    auto c = std::make_unique<Camera>();
    camera = std::move(c);
    synchronizeCameraPosition();
}

void Player::synchronizeCameraPosition() noexcept {
    if (!camera || !isFiniteVector(position)) {
        return;
    }
    camera->position = position + playerEyeHeightDunamisUnits * playerWorldUp;
}

void Player::onPhysicsTransformResolved() noexcept {
    synchronizeCameraPosition();
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
    synchronizeCameraPosition();

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
    if (!input || !input->gameplayInputEnabled() || !camera) {
        desiredVelocity = glm::vec3(0.0f);
        return;
    }

    float movementSpeedUnitsPerSecond = walkSpeedDunamisUnitsPerSecond;
    if (input->isKeyDown(SDLK_LSHIFT)) {
        movementSpeedUnitsPerSecond = sprintSpeedDunamisUnitsPerSecond;
    }

    glm::vec3 forward{camera->front.x, 0.0f, camera->front.z};
    const float forwardLength = glm::length(forward);
    if (!std::isfinite(forwardLength) ||
        forwardLength <= minimumMovementVectorLength) {
        desiredVelocity = glm::vec3(0.0f);
        spdlog::error("Player could not calculate a valid horizontal movement forward vector");
    } else {
        forward /= forwardLength;
        glm::vec3 right = glm::cross(forward, playerWorldUp);
        const float rightLength = glm::length(right);
        if (!std::isfinite(rightLength) || rightLength <= 0.0f) {
            desiredVelocity = glm::vec3(0.0f);
            spdlog::error("Player could not calculate a valid movement right vector");
        } else {
            right /= rightLength;
            glm::vec3 movement(0.0f);
            if (input->isKeyDown(SDLK_W)) movement += forward;
            if (input->isKeyDown(SDLK_S)) movement -= forward;
            if (input->isKeyDown(SDLK_D)) movement += right;
            if (input->isKeyDown(SDLK_A)) movement -= right;

            const float movementLength = glm::length(movement);
            if (std::isfinite(movementLength) &&
                movementLength > minimumMovementVectorLength) {
                desiredVelocity = movement *
                    (movementSpeedUnitsPerSecond / movementLength);
            } else {
                desiredVelocity = glm::vec3(0.0f);
            }
        }
    }

    if (!isFiniteVector(desiredVelocity)) {
        desiredVelocity = glm::vec3(0.0f);
        spdlog::error("Player calculated a non-finite movement velocity");
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
