#include "game_object.h"
#include "../third_party/stb/stb_image.h"

void GameObject::loadModel(const char* modelPath) {
    spdlog::info("Loading game object model from path {}...", modelPath);

    const aiScene* scene =
        importer.ReadFile(modelPath, aiProcess_Triangulate | aiProcess_FlipUVs);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
        !scene->mRootNode) {
            spdlog::error("Failed to load model file: {}", modelPath);
    }

    aiString texturePath;
    scene->mMaterials[0]->GetTexture(aiTextureType_DIFFUSE, 0, &texturePath);
    for (int i = 0; i < scene->mNumMaterials; i++) {
        aiMaterial* m = scene->mMaterials[i];
        if (m->GetTextureCount(aiTextureType_DIFFUSE) > 0) {
            m->GetTexture(aiTextureType_BASE_COLOR, 0, &texturePath);
            //spdlog::error("PATH FOR TEXTURE {}", texturePath.C_Str());
        }
    }

    /*std::unordered_map<Vertex, uint32_t> uniqueVertices{};

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

            //mesh.indices.push_back(uniqueVertices[vertex]);
        }
    }*/

    

    //std::unordered_map<Vertex, uint32_t> uniqueVertices{};

    for (unsigned int i = 0; i < scene->mNumMeshes; i++) {
        const aiMesh* m = scene->mMeshes[i];
        aiMaterial* mat = scene->mMaterials[m->mMaterialIndex];

        MeshInstance instance{};
        std::unordered_map<Vertex, uint32_t> uniqueVertices{};

        for (unsigned int f = 0; f < m->mNumFaces; f++) {
            const aiFace& face = m->mFaces[f];

            for (unsigned int j = 0; j < face.mNumIndices; j++) {
                unsigned int index = face.mIndices[j];

                Vertex vertex{};
                vertex.pos = {
                    m->mVertices[index].x,
                    m->mVertices[index].y,
                    m->mVertices[index].z
                };

                if (m->HasTextureCoords(0)) {
                    vertex.texCoord = {
                        m->mTextureCoords[0][index].x,
                        m->mTextureCoords[0][index].y
                    };
                } else {
                    vertex.texCoord = {0.0f, 0.0f};
                }

                vertex.color = {1.0f, 1.0f, 1.0f};

                if (uniqueVertices.count(vertex) == 0) {
                    //uniqueVertices[vertex] = static_cast<uint32_t>(mesh.vertices.size());
                    uniqueVertices[vertex] = static_cast<uint32_t>(instance.mesh.vertices.size());
                    instance.mesh.vertices.push_back(vertex);
                }

                instance.mesh.indices.push_back(uniqueVertices[vertex]);
            }
        }


       aiString texPath; 
       if (mat->GetTexture(aiTextureType_BASE_COLOR, 0, &texPath) == AI_SUCCESS) {
            const char* texRef = texPath.C_Str();
            instance.material.texturePath = texRef;

            if (texRef[0] == '*') {
                int texIndex = std::stoi(&texRef[1]);
                const aiTexture* tex = scene->mTextures[texIndex];

                if (tex->mHeight == 0) {
                    instance.material.pixels = stbi_load_from_memory(
                        reinterpret_cast<const unsigned char*>(tex->pcData),
                        tex->mWidth,
                        &instance.material.texWidth,
                        &instance.material.texHeight,
                        &instance.material.texChannels,
                        STBI_rgb_alpha
                    );

                    instance.material.mipLevels = static_cast<uint32_t>(std::floor(
                        std::log2(std::max(instance.material.texWidth, instance.material.texHeight)))) + 1;
                }
            } else {
                instance.material.pixels = stbi_load(
                    texRef,
                    &instance.material.texWidth,
                    &instance.material.texHeight,
                    &instance.material.texChannels,
                    STBI_rgb_alpha
                );
            }
       } else {
            instance.material.pixels = stbi_load(
                "game/assets/textures/viking_room.png",
                &instance.material.texWidth,
                &instance.material.texHeight,
                &instance.material.texChannels,
                STBI_rgb_alpha
            );

            instance.material.mipLevels = static_cast<uint32_t>(std::floor(
                                    std::log2(std::max(instance.material.texWidth, instance.material.texHeight)))) +
                                1;
       }

    meshInstances.push_back(instance);

    }





    spdlog::info("Successfully loaded game object model");
    
}