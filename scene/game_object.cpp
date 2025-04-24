#include "game_object.h"
#include "../third_party/stb/stb_image.h"

void GameObject::loadModel() {

    spdlog::info("Loading game object model from path {}...", modelPath);

    if (modelPath == nullptr) {
        spdlog::error("Model path is null. Cannot load model.");
        return;
    }

    const aiScene* scene =
        importer.ReadFile(modelPath, aiProcess_Triangulate | aiProcess_FlipUVs);

    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
        !scene->mRootNode) {
            spdlog::error("Failed to load model file: {}", modelPath);
    }

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

            // If the material is baked into the gbl file
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
                    spdlog::info("Successfully loaded texture for {}", m->mName.C_Str());
                }
            } else {
                // Not sure if this else statement is needed or if setting to
                // texRef is even the right thing to do. Haven't seen this be
                // called yet
                instance.material.pixels = stbi_load(
                    texRef,
                    &instance.material.texWidth,
                    &instance.material.texHeight,
                    &instance.material.texChannels,
                    STBI_rgb_alpha
                );
                instance.material.mipLevels = static_cast<uint32_t>(std::floor(
                    std::log2(std::max(instance.material.texWidth, instance.material.texHeight)))) + 1;
                spdlog::info("Successfully loaded texture for {}", m->mName.C_Str());
            }
       } else {
            bool useDefaultTexture = false;
            if (texturePath != nullptr) {
                instance.material.texturePath = texturePath;
            } else {
                instance.material.texturePath = "rendering/default_textures/error.jpg";
                useDefaultTexture = true;
            }
            instance.material.pixels = stbi_load(
                instance.material.texturePath,
                &instance.material.texWidth,
                &instance.material.texHeight,
                &instance.material.texChannels,
                STBI_rgb_alpha
            );

            instance.material.mipLevels = static_cast<uint32_t>(std::floor(
                                    std::log2(std::max(instance.material.texWidth, instance.material.texHeight)))) +
                                1;
            if (!useDefaultTexture) {
                spdlog::info("Successfully loaded texture for {}", m->mName.C_Str());
            } else {
                spdlog::error("No texture found for {}. Using default texture.", m->mName.C_Str());
            }
       }

    meshInstances.push_back(instance);

    }

    spdlog::info("Successfully loaded game object model");
    
}