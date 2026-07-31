#ifndef DUNAMIS_H
#define DUNAMIS_H

#include "platform.h"
#include "result.h"

#include "../game/level_1.h"
#include "../input/input_manager.h"
#include "../rendering/visual_server.h"

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
    // Members are destroyed in reverse declaration order. The scene and
    // platform must remain alive while the renderer shuts down.
    Level1 level1;
    Platform platform;
    VisualServer visualServer;
    std::shared_ptr<InputManager> inputManager;
    bool initializationAttempted = false;
    bool initialized = false;
};

#endif
