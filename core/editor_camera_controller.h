#ifndef EDITOR_CAMERA_CONTROLLER_H
#define EDITOR_CAMERA_CONTROLLER_H

#include "../scene/camera.h"
#include "../scene/scene_serializer.h"

class InputManager;

class EditorCameraController {
public:
    EditorCameraController();

    void update(const InputManager& input);

    [[nodiscard]] const Camera& camera() const noexcept;
    [[nodiscard]] Camera& camera() noexcept;
    [[nodiscard]] Result restore(const EditorCameraState& state) noexcept;

private:
    Camera camera_;
    double yaw_ = -90.0;
    double pitch_ = 0.0;
};

#endif
