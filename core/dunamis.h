#ifndef DUNAMIS_H
#define DUNAMIS_H

#include "../input/input_manager.h"
#include <SDL3/SDL.h>
#include "spdlog/spdlog.h"

#include "../game/level_1.h"
#include "../rendering/visual_server.h"

class Dunamis {
public:
    Dunamis();
    ~Dunamis();

private:
    Level1 level1; // Put before visualServer to avoid dangling pointer
    VisualServer visualServer;
    void run();
    std::shared_ptr<InputManager> inputManager;

};

#endif