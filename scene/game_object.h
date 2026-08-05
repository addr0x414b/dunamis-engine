#ifndef GAME_OBJECT_H
#define GAME_OBJECT_H

#include <assimp/postprocess.h>
#include <assimp/scene.h>
#include <assimp/Importer.hpp>
#include <deque>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include "spdlog/spdlog.h"
#include "../core/result.h"
#include "../rendering/utils/vulkan_utils.h"

class Scene;
class VulkanContext;

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

    [[nodiscard]] Result addMeshInstance(MeshInstance meshInstance);
    [[nodiscard]] const std::vector<MeshInstance>&
    meshInstances() const noexcept;

    // Call after setting modelPath (and texturePath if needed)
    // Loads the model and sets up the mesh and materials
    [[nodiscard]] Result loadModel();

private:
    friend class Scene;
    friend class VulkanContext;


    Assimp::Importer importer;
    std::vector<MeshInstance> meshInstances_;
    std::deque<std::string> texturePathStorage_;
};

#endif
