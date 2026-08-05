#include "game_object.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

#include <assimp/GltfMaterial.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <iterator>
#include <limits>
#include <utility>

namespace {

constexpr const char* fallbackTexturePath =
    "rendering/default_textures/error.jpg";

std::string stbFailureReason() {
    const char* reason = stbi_failure_reason();
    return reason ? reason : "unknown stb_image failure";
}

struct PixelCleanup {
    std::vector<stbi_uc*>& pixels;
    bool committed = false;

    ~PixelCleanup() {
        if (!committed) {
            for (stbi_uc* pixel : pixels) {
                stbi_image_free(pixel);
            }
        }
    }
};

Result calculateMipLevels(int width, int height, uint32_t& mipLevels) {
    if (width <= 0 || height <= 0) {
        return Result::failure("Texture dimensions must be positive");
    }
    mipLevels = static_cast<uint32_t>(std::floor(
        std::log2(std::max(width, height)))) + 1;
    return Result::success();
}

Result loadFileTexture(const std::filesystem::path& path,
                       Material& material) {
    material.pixels = stbi_load(
        path.string().c_str(), &material.texWidth, &material.texHeight,
        &material.texChannels, STBI_rgb_alpha);
    if (!material.pixels) {
        return Result::failure("stbi_load failed for " + path.string() +
                               ": " + stbFailureReason());
    }

    Result mipResult = calculateMipLevels(material.texWidth,
                                           material.texHeight,
                                           material.mipLevels);
    if (!mipResult) {
        stbi_image_free(material.pixels);
        material.pixels = nullptr;
        return Result::failure("Invalid texture dimensions for " +
                               path.string() + ": " + mipResult.error());
    }
    return Result::success();
}

}  // namespace

Result GameObject::loadModel() {
    spdlog::info("Loading game object model from path {}...", modelPath);

    if (modelPath == nullptr) {
        spdlog::error("Model path is null. Cannot load model.");
        return Result::failure("Model path is null");
    }

    const aiScene* scene =
        importer.ReadFile(modelPath, aiProcess_Triangulate | aiProcess_FlipUVs);
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
        !scene->mRootNode) {
        return Result::failure(
            "Failed to load model file " + std::string(modelPath) + ": " +
            (importer.GetErrorString() ? importer.GetErrorString()
                                       : "unknown Assimp error"));
    }

    std::vector<MeshInstance> pendingMeshes;
    std::vector<std::string> pendingTexturePaths;
    std::vector<stbi_uc*> allocatedPixels;
    std::vector<bool> warnedBlendMaterials(scene->mNumMaterials, false);
    PixelCleanup pixelCleanup{allocatedPixels};
    auto releaseAllocatedPixels = [&allocatedPixels]() {
        for (stbi_uc* pixels : allocatedPixels) {
            stbi_image_free(pixels);
        }
        allocatedPixels.clear();
    };

    auto loadWithFallback = [&](MeshInstance& instance,
                                const std::string& intendedPath,
                                const std::string& context,
                                bool& usedFallback) -> Result {
        Result intendedResult = loadFileTexture(intendedPath, instance.material);
        if (intendedResult) {
            allocatedPixels.push_back(instance.material.pixels);
            usedFallback = false;
            return Result::success();
        }
        spdlog::error("Failed to load intended texture {}: {}", intendedPath,
                      intendedResult.error());

        Result fallbackResult = loadFileTexture(fallbackTexturePath,
                                                instance.material);
        if (fallbackResult) {
            allocatedPixels.push_back(instance.material.pixels);
            usedFallback = true;
            spdlog::warn("Using fallback texture {} for {}", fallbackTexturePath,
                         context);
            return Result::success();
        }
        return Result::failure("Failed to load intended texture " + intendedPath +
                               " (" + context + ") and fallback texture " +
                               fallbackTexturePath + ": " +
                               fallbackResult.error());
    };

    for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
        const aiMesh* mesh = scene->mMeshes[i];
        if (!mesh || mesh->mMaterialIndex >= scene->mNumMaterials) {
            releaseAllocatedPixels();
            return Result::failure("Model contains an invalid mesh material index");
        }
        aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
        MeshInstance instance{};

        aiString alphaMode;
        if (material->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) == AI_SUCCESS) {
            const std::string importedAlphaMode = alphaMode.C_Str();
            if (importedAlphaMode == "MASK") {
                instance.material.alphaMode = MaterialAlphaMode::Mask;
                float alphaCutoff = instance.material.alphaCutoff;
                if (material->Get(AI_MATKEY_GLTF_ALPHACUTOFF, alphaCutoff) ==
                        AI_SUCCESS &&
                    std::isfinite(alphaCutoff) && alphaCutoff >= 0.0f &&
                    alphaCutoff <= 1.0f) {
                    instance.material.alphaCutoff = alphaCutoff;
                }
            } else if (importedAlphaMode == "BLEND") {
                instance.material.alphaMode = MaterialAlphaMode::Blend;
                if (!warnedBlendMaterials[mesh->mMaterialIndex]) {
                    spdlog::warn(
                        "Material {} uses BLEND alpha mode; blended "
                        "transparency is not implemented and will render as "
                        "opaque",
                        mesh->mName.C_Str());
                    warnedBlendMaterials[mesh->mMaterialIndex] = true;
                }
            }
        }

        bool doubleSided = false;
        if (material->Get(AI_MATKEY_TWOSIDED, doubleSided) == AI_SUCCESS) {
            instance.material.doubleSided = doubleSided;
        }
        std::unordered_map<Vertex, uint32_t> uniqueVertices{};

        for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
            const aiFace& face = mesh->mFaces[f];
            for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                const unsigned int index = face.mIndices[j];
                Vertex vertex{};
                vertex.pos = {mesh->mVertices[index].x,
                              mesh->mVertices[index].y,
                              mesh->mVertices[index].z};
                vertex.texCoord = mesh->HasTextureCoords(0)
                    ? glm::vec2{mesh->mTextureCoords[0][index].x,
                                mesh->mTextureCoords[0][index].y}
                    : glm::vec2{0.0f, 0.0f};
                vertex.color = {1.0f, 1.0f, 1.0f};

                if (uniqueVertices.count(vertex) == 0) {
                    uniqueVertices[vertex] =
                        static_cast<uint32_t>(instance.mesh.vertices.size());
                    instance.mesh.vertices.push_back(vertex);
                }
                instance.mesh.indices.push_back(uniqueVertices[vertex]);
            }
        }

        std::string effectiveTexturePath = fallbackTexturePath;
        bool usedFallback = false;
        aiString textureReference;
        if (material->GetTexture(aiTextureType_BASE_COLOR, 0,
                                 &textureReference) == AI_SUCCESS) {
            const char* texRef = textureReference.C_Str();
            if (texRef[0] == '*') {
                bool embeddedValid = false;
                unsigned long textureIndex = 0;
                try {
                    std::size_t parsed = 0;
                    textureIndex = std::stoul(texRef + 1, &parsed);
                    embeddedValid = texRef[1 + parsed] == '\0';
                } catch (...) {
                    embeddedValid = false;
                }

                const aiTexture* texture =
                    embeddedValid && textureIndex < scene->mNumTextures
                    ? scene->mTextures[textureIndex]
                    : nullptr;
                if (texture && texture->mHeight == 0 && texture->pcData &&
                    texture->mWidth > 0 &&
                    texture->mWidth <=
                        static_cast<unsigned int>(std::numeric_limits<int>::max())) {
                    instance.material.pixels = stbi_load_from_memory(
                        reinterpret_cast<const unsigned char*>(texture->pcData),
                        texture->mWidth, &instance.material.texWidth,
                        &instance.material.texHeight,
                        &instance.material.texChannels, STBI_rgb_alpha);
                    if (instance.material.pixels) {
                        Result mipResult = calculateMipLevels(
                            instance.material.texWidth,
                            instance.material.texHeight,
                            instance.material.mipLevels);
                        embeddedValid = static_cast<bool>(mipResult);
                        if (!embeddedValid) {
                            stbi_image_free(instance.material.pixels);
                            instance.material.pixels = nullptr;
                        } else {
                            allocatedPixels.push_back(instance.material.pixels);
                        }
                    } else {
                        spdlog::error("Failed to load embedded texture {}: {}",
                                      texRef, stbFailureReason());
                        embeddedValid = false;
                    }
                }

                if (!embeddedValid) {
                    Result result = loadWithFallback(instance, texRef,
                                                     mesh->mName.C_Str(),
                                                     usedFallback);
                    if (!result) {
                        releaseAllocatedPixels();
                        return result;
                    }
                }
                effectiveTexturePath = usedFallback ? fallbackTexturePath : texRef;
            } else {
                const std::filesystem::path resolvedPath =
                    std::filesystem::path(modelPath).parent_path() / texRef;
                Result result = loadWithFallback(instance, resolvedPath.string(),
                                                 mesh->mName.C_Str(), usedFallback);
                if (!result) {
                    releaseAllocatedPixels();
                    return result;
                }
                effectiveTexturePath = usedFallback ? fallbackTexturePath
                                                     : resolvedPath.string();
            }
        } else if (texturePath != nullptr) {
            Result result = loadWithFallback(instance, texturePath,
                                             mesh->mName.C_Str(), usedFallback);
            if (!result) {
                releaseAllocatedPixels();
                return result;
            }
            effectiveTexturePath = usedFallback ? fallbackTexturePath : texturePath;
        } else {
            Result result = loadFileTexture(fallbackTexturePath, instance.material);
            if (!result) {
                releaseAllocatedPixels();
                return Result::failure(
                    "No texture was specified for " +
                    std::string(mesh->mName.C_Str()) + "; fallback texture " +
                    fallbackTexturePath + " failed: " + result.error());
            }
            allocatedPixels.push_back(instance.material.pixels);
        }
        pendingTexturePaths.push_back(std::move(effectiveTexturePath));
        pendingMeshes.push_back(std::move(instance));
    }

    try {
        meshInstances_.reserve(meshInstances_.size() + pendingMeshes.size());
        for (std::size_t i = 0; i < pendingMeshes.size(); ++i) {
            texturePathStorage_.push_back(std::move(pendingTexturePaths[i]));
            pendingMeshes[i].material.texturePath =
                texturePathStorage_.back().c_str();
        }
        meshInstances_.insert(meshInstances_.end(),
                              std::make_move_iterator(pendingMeshes.begin()),
                              std::make_move_iterator(pendingMeshes.end()));
    } catch (const std::exception& exception) {
        releaseAllocatedPixels();
        return Result::failure("Failed to commit meshes from model " +
                               std::string(modelPath) + ": " + exception.what());
    } catch (...) {
        releaseAllocatedPixels();
        return Result::failure("Failed to commit meshes from model " +
                               std::string(modelPath) + ": unknown error");
    }

    allocatedPixels.clear();
    pixelCleanup.committed = true;
    spdlog::info("Successfully loaded game object model");
    return Result::success();
}

Result GameObject::addMeshInstance(MeshInstance meshInstance) {
    try {
        meshInstances_.push_back(std::move(meshInstance));
    } catch (const std::exception& exception) {
        return Result::failure("Failed to add mesh instance: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to add mesh instance with an unknown error");
    }
    return Result::success();
}

const std::vector<MeshInstance>& GameObject::meshInstances() const noexcept {
    return meshInstances_;
}
