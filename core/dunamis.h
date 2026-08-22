#ifndef DUNAMIS_H
#define DUNAMIS_H

#include "platform.h"
#include "result.h"
#include "../editor/editor_camera_controller.h"
#include "../editor/editor_session.h"
#include "../editor/editor_state.h"
#include "../physics/physics_server.h"

#include <filesystem>

#include "../input/input_manager.h"
#include "../rendering/visual_server.h"
#include "../scene/scene_manager.h"

class Dunamis {
public:
    Dunamis() = default;
    ~Dunamis() noexcept;

    Dunamis(const Dunamis&) = delete;
    Dunamis& operator=(const Dunamis&) = delete;
    Dunamis(Dunamis&&) = delete;
    Dunamis& operator=(Dunamis&&) = delete;

    [[nodiscard]] Result initialize();
    [[nodiscard]] Result run();
    [[nodiscard]] bool shutdown() noexcept;

private:
    [[nodiscard]] Result beginRuntimeSession(SceneRunState targetState);
    [[nodiscard]] Result stopRuntimeSession();
    [[nodiscard]] const Camera& renderCamera() const noexcept;
    void synchronizeImGuiInput() noexcept;
    [[nodiscard]] Result loadEditingScene(
        const std::filesystem::path& path);
    void requestQuit(bool& running);
    void reportPersistenceResult(const Result& result,
                                 const std::string& successMessage);

    EditorSession editorSession_;
    EditorCameraController editorCameraController;
    Platform platform;
    SceneManager sceneManager_;
    VisualServer visualServer;
    PhysicsServer physicsServer_;
    std::shared_ptr<InputManager> inputManager;
    bool initializationAttempted = false;
    bool initialized = false;
    std::filesystem::path pendingLoadPath_;
    std::filesystem::path pendingSaveAsPath_;
    bool quitConfirmationPending_ = false;
};

#endif
