#include "game_object.h"

void GameObject::loadModel(const char* modelPath) {
    spdlog::info("Loading game object model from path {}...", modelPath);

    const aiScene* scene =
        importer.ReadFile(modelPath, aiProcess_Triangulate | aiProcess_FlipUVs);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
        !scene->mRootNode) {
            spdlog::error("Failed to load model file: {}", modelPath);
    }

    std::unordered_map<Vertex, uint32_t> uniqueVertices{};

    for (unsigned int i = 0; i < scene->mNumMeshes; i++) {
        const aiMesh* m = scene->mMeshes[i];

        for (unsigned int j = 0; j < m->mNumVertices; j++) {
            Vertex vertex{};
            vertex.pos = {m->mVertices[j].x, m->mVertices[j].y,
                          m->mVertices[j].z};
            vertex.texCoord = {m->mTextureCoords[0][j].x,
                               m->mTextureCoords[0][j].y};
            vertex.color = {1.0f, 1.0f, 1.0f};

            if (uniqueVertices.count(vertex) == 0) {
                uniqueVertices[vertex] = static_cast<uint32_t>(mesh.vertices.size());
                mesh.vertices.push_back(vertex);
            }

            mesh.indices.push_back(uniqueVertices[vertex]);
        }
    }
    spdlog::info("Successfully loaded game object model");
    
}