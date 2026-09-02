#ifndef LEVEL_1_H
#define LEVEL_1_H

#include "spdlog/spdlog.h"

#include "player.h"
#include "../scene/scene.h"

class TypeRegistry;

class Level1 : public Scene {
public:
    [[nodiscard]] static Result registerTypes(TypeRegistry& registry);
    void buildDefaults() override;
    void start() override;
    void update() override;

private:
    Player* player_ = nullptr;
};

#endif
