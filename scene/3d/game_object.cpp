#include "game_object.h"

GameObject::GameObject(const char* modelPath, const char* texturePath) {
    this->modelPath = modelPath;
    this->texturePath = texturePath;
    loadModel(modelPath);
}

void GameObject::loadModel(const char* modelPath) {
    Debugger::subSubSection("Load Model");

    Debugger::print("Reading in model file...");
    const aiScene* scene =
        importer.ReadFile(modelPath,
                          aiProcess_Triangulate | aiProcess_FlipUVs);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
        !scene->mRootNode) {
        Debugger::print("Failed to load model file!", true);
    } else {
        Debugger::print("Successfully loaded model file");
    }

    std::unordered_map<Vertex, uint32_t> uniqueVertices{};

    Debugger::print("Begin reading in vertices...");
    for (unsigned int i = 0; i < scene->mNumMeshes; i++) {
        const aiMesh* mesh = scene->mMeshes[i];

        for (unsigned int j = 0; j < mesh->mNumVertices; j++) {
            Vertex vertex{};
            vertex.pos = {mesh->mVertices[j].x, mesh->mVertices[j].y,
                          mesh->mVertices[j].z};
            vertex.texCoord = {mesh->mTextureCoords[0][j].x,
                               mesh->mTextureCoords[0][j].y};
            vertex.color = {1.0f, 1.0f, 1.0f};

            if (uniqueVertices.count(vertex) == 0) {
                uniqueVertices[vertex] = static_cast<uint32_t>(vertices.size());
                vertices.push_back(vertex);
            }

            indices.push_back(uniqueVertices[vertex]);
        }
    }
    Debugger::print("Successfully loaded model");
    Debugger::subSubSection("Load Model\n");
}

void GameObject::updateUniformBuffer(uint32_t currentImage, Camera& camera,
                                     VkExtent2D swapchainExtent) {
    UniformBufferObject ubo{};

    ubo.model = glm::scale(glm::mat4(1.0f), scale);

    ubo.model *= glm::rotate(glm::mat4(1.0f), glm::radians(rotation.x),
                             glm::vec3(1.0f, 0.0f, 0.0f));

    ubo.model *= glm::rotate(glm::mat4(1.0f), glm::radians(rotation.y),
                             glm::vec3(0.0f, 1.0f, 0.0f));

    ubo.model *= glm::rotate(glm::mat4(1.0f), glm::radians(rotation.z),
                             glm::vec3(0.0f, 0.0f, 1.0f));

    ubo.model *= glm::translate(glm::mat4(1.0f), position);

    ubo.view =
        glm::lookAt(camera.position, camera.position + camera.front, camera.up);

    ubo.proj = glm::perspective(
        glm::radians(45.0f),
        swapchainExtent.width / (float)swapchainExtent.height, 0.1f, 10000.0f);

    ubo.proj[1][1] *= -1;
    memcpy(uniformBuffersMapped[currentImage], &ubo, sizeof(ubo));
}