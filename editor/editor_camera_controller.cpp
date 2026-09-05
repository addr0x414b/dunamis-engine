#include "editor_camera_controller.h"

#include <algorithm>
#include <cmath>

#include <glm/glm.hpp>

#include "../input/input_manager.h"
#include "../core/time.h"

EditorCameraController::EditorCameraController() {
    camera_.position = glm::vec3(0.0f, 0.0f, startingDistanceMeters);
    camera_.front = glm::vec3(0.0f, 0.0f, -1.0f);
    camera_.up = glm::vec3(0.0f, 1.0f, 0.0f);
}

void EditorCameraController::update(const InputManager& input) {
    if (!input.editorCameraInputEnabled()) {
        return;
    }

    float movementSpeedMetersPerSecond = normalSpeedMetersPerSecond;
    if (input.isKeyDown(SDLK_LSHIFT)) {
        movementSpeedMetersPerSecond = fastSpeedMetersPerSecond;
    } else if (input.isKeyDown(SDLK_LCTRL)) {
        movementSpeedMetersPerSecond = slowSpeedMetersPerSecond;
    }

    const float movementDistance =
        movementSpeedMetersPerSecond * Time::deltaTime();
    const glm::vec3 right =
        glm::normalize(glm::cross(camera_.front, camera_.up));
    if (input.isKeyDown(SDLK_W)) {
        camera_.position += movementDistance * camera_.front;
    }
    if (input.isKeyDown(SDLK_S)) {
        camera_.position -= movementDistance * camera_.front;
    }
    if (input.isKeyDown(SDLK_D)) {
        camera_.position += movementDistance * right;
    }
    if (input.isKeyDown(SDLK_A)) {
        camera_.position -= movementDistance * right;
    }
    if (input.isKeyDown(SDLK_E)) {
        camera_.position += movementDistance * camera_.up;
    }
    if (input.isKeyDown(SDLK_Q)) {
        camera_.position -= movementDistance * camera_.up;
    }

    constexpr float sensitivity = 0.1f;
    yaw_ += static_cast<double>(input.getMouseRelX()) * sensitivity;
    pitch_ -= static_cast<double>(input.getMouseRelY()) * sensitivity;
    pitch_ = std::clamp(pitch_, -89.0, 89.0);

    glm::vec3 direction;
    direction.x = static_cast<float>(
        std::cos(glm::radians(yaw_)) * std::cos(glm::radians(pitch_)));
    direction.y = static_cast<float>(std::sin(glm::radians(pitch_)));
    direction.z = static_cast<float>(
        std::sin(glm::radians(yaw_)) * std::cos(glm::radians(pitch_)));
    camera_.front = glm::normalize(direction);
}

const Camera& EditorCameraController::camera() const noexcept {
    return camera_;
}

Camera& EditorCameraController::camera() noexcept {
    return camera_;
}

Result EditorCameraController::restore(const EditorCameraState& state) noexcept {
    Camera candidate;
    candidate.position = state.position;
    candidate.front = state.front;
    candidate.up = state.up;
    double yaw = 0.0;
    double pitch = 0.0;
    if (!candidate.deriveYawPitchDegrees(yaw, pitch) ||
        !candidate.setYawPitchDegrees(yaw, pitch, state.up)) {
        return Result::failure("Editor camera orientation is invalid");
    }
    camera_.position = candidate.position;
    camera_.front = candidate.front;
    camera_.up = candidate.up;
    yaw_ = yaw;
    pitch_ = std::clamp(pitch, -89.0, 89.0);
    return Result::success();
}
