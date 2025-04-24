#ifndef GAME_OBJECT_H
#define GAME_OBJECT_H

#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <glm/glm.hpp>
#include "spdlog/spdlog.h"
#include "../rendering/utils/vulkan_utils.h"


class GameObject {
public:
    std::string name;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f);
    glm::vec3 scale = glm::vec3(1.0f);

    virtual void start() {}
    virtual void update() {}
    virtual ~GameObject() = default;

    const char* modelPath = nullptr;
    // texturePath is only used if the textures are not baked into the model file
    const char* texturePath = nullptr;


    std::vector<MeshInstance> meshInstances;

    // Call after setting modelPath (and texturePath if needed)
    // Loads the model and sets up the mesh and materials
    void loadModel();

private:
    Assimp::Importer importer;

};

#endif