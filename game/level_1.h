#ifndef LEVEL_1_H
#define LEVEL_1_H

#include "spdlog/spdlog.h"

#include "player.h"
#include "../scene/scene.h"

class Level1 : public Scene {
public:
    void init() override;
    void start() override;
    void update() override;

private:
    Player player;
};

#endif