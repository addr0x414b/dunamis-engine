#include "game_object.h"
#include "loading_cache_key.h"
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

#include <assimp/GltfMaterial.h>

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

constexpr const char* fallbackTexturePath =
    "rendering/default_textures/error.jpg";

using TextureUsage = model_loading::TextureUsage;
using TextureCacheKey = model_loading::TextureCacheKey;
using TextureCacheKeyHash = model_loading::TextureCacheKeyHash;

struct TextureDecodeProfile {
    std::size_t references = 0;
    std::unordered_set<std::string> uniqueSources;
    std::size_t stbiLoadCalls = 0;
    std::size_t stbiLoadFromMemoryCalls = 0;
    std::size_t decodedRgbaBytes = 0;
};

struct LoadModelProfile {
    using Clock = std::chrono::steady_clock;

    Clock::time_point totalStart = Clock::now();
    Clock::duration assimpReadTime{};
    Clock::duration geometryConversionTime{};
    Clock::duration materialPropertyExtractionTime{};
    Clock::duration baseColorDecodeTime{};
    Clock::duration normalMapDecodeTime{};
    Clock::duration metallicRoughnessDecodeTime{};
    Clock::duration finalCommitTime{};
    std::size_t totalFaceIndexCount = 0;
    std::size_t totalUniqueVertices = 0;
    std::size_t materialPreparations = 0;
    std::size_t fallbackTextureDecodeCount = 0;
    std::size_t totalDecodedRgbaBytes = 0;
    TextureDecodeProfile baseColor;
    TextureDecodeProfile normalMap;
    TextureDecodeProfile metallicRoughness;
};

struct PhaseTimer {
    using Clock = std::chrono::steady_clock;

    Clock::duration& total;
    Clock::time_point start = Clock::now();

    explicit PhaseTimer(Clock::duration& duration) : total(duration) {}

    ~PhaseTimer() { total += Clock::now() - start; }
};

TextureDecodeProfile& textureProfile(LoadModelProfile& profile,
                                      TextureUsage usage) {
    switch (usage) {
        case TextureUsage::BaseColor:
            return profile.baseColor;
        case TextureUsage::Normal:
            return profile.normalMap;
        case TextureUsage::MetallicRoughness:
            return profile.metallicRoughness;
    }
    return profile.baseColor;
}

void recordTextureSource(LoadModelProfile& profile, TextureUsage usage,
                         std::string sourceKey) {
    TextureDecodeProfile& texture = textureProfile(profile, usage);
    texture.uniqueSources.insert(std::move(sourceKey));
}

void recordTextureReference(LoadModelProfile& profile, TextureUsage usage,
                            std::string sourceKey) {
    TextureDecodeProfile& texture = textureProfile(profile, usage);
    ++texture.references;
    recordTextureSource(profile, usage, std::move(sourceKey));
}

void recordDecodeCall(LoadModelProfile& profile, TextureUsage usage,
                      bool fromMemory, bool fallback) {
    TextureDecodeProfile& texture = textureProfile(profile, usage);
    if (fromMemory) {
        ++texture.stbiLoadFromMemoryCalls;
    } else {
        ++texture.stbiLoadCalls;
    }
    if (fallback) {
        ++profile.fallbackTextureDecodeCount;
    }
}

void recordDecodedImage(LoadModelProfile& profile, TextureUsage usage, int width,
                        int height) {
    if (width <= 0 || height <= 0) {
        return;
    }
    const std::size_t rgbaBytes = static_cast<std::size_t>(width) *
                                  static_cast<std::size_t>(height) * 4;
    textureProfile(profile, usage).decodedRgbaBytes += rgbaBytes;
    profile.totalDecodedRgbaBytes += rgbaBytes;
}

std::string normalizedTextureSourceKey(const std::filesystem::path& source) {
    return source.lexically_normal().generic_string();
}

std::optional<std::size_t> parseEmbeddedTextureIndex(
    std::string_view textureReference) {
    if (textureReference.size() < 2 || textureReference.front() != '*') {
        return std::nullopt;
    }

    std::uint64_t index = 0;
    const char* begin = textureReference.data() + 1;
    const char* end = textureReference.data() + textureReference.size();
    const auto parsed = std::from_chars(begin, end, index);
    if (parsed.ec != std::errc{} || parsed.ptr != end ||
        index > std::numeric_limits<std::size_t>::max()) {
        return std::nullopt;
    }
    return static_cast<std::size_t>(index);
}

std::string stbFailureReason() {
    const char* reason = stbi_failure_reason();
    return reason ? reason : "unknown stb_image failure";
}

Result calculateMipLevels(int width, int height, uint32_t& mipLevels) {
    if (width <= 0 || height <= 0) {
        return Result::failure("Texture dimensions must be positive");
    }
    mipLevels = static_cast<uint32_t>(std::floor(
        std::log2(std::max(width, height)))) + 1;
    return Result::success();
}

struct TextureSource {
    std::string sourceIdentity;
    std::string displayPath;
    std::filesystem::path externalPath;
    const aiTexture* embeddedTexture = nullptr;
    bool embedded = false;
    bool valid = true;
    std::string invalidReason;

    [[nodiscard]] bool isEmbedded() const noexcept {
        return embedded;
    }
};

TextureSource makeExternalTextureSource(std::filesystem::path path,
                                        std::string displayPath) {
    TextureSource source;
    source.sourceIdentity =
        model_loading::normalizedExternalSourceIdentity(path);
    source.displayPath = std::move(displayPath);
    source.externalPath = std::move(path);
    return source;
}

TextureSource resolveTextureSource(const aiScene* scene, const char* modelPath,
                                   std::string_view textureReference) {
    if (textureReference.empty() || textureReference.front() != '*') {
        const std::filesystem::path path =
            std::filesystem::path(modelPath).parent_path() /
            std::filesystem::path(textureReference);
        return makeExternalTextureSource(path, path.string());
    }

    TextureSource source;
    source.displayPath = std::string(textureReference);
    source.embedded = true;
    const std::string modelIdentity = normalizedTextureSourceKey(modelPath);
    const std::optional<std::size_t> textureIndex =
        parseEmbeddedTextureIndex(textureReference);
    if (!textureIndex) {
        source.sourceIdentity = modelIdentity + "|embedded:" +
                                std::string(textureReference);
        source.valid = false;
        source.invalidReason = "invalid embedded texture reference";
        return source;
    }

    source.sourceIdentity =
        model_loading::embeddedSourceIdentity(modelIdentity, *textureIndex);
    if (*textureIndex >= scene->mNumTextures) {
        source.valid = false;
        source.invalidReason = "embedded texture index is out of range";
        return source;
    }

    const aiTexture* texture = scene->mTextures[*textureIndex];
    if (!texture || texture->mHeight != 0 || !texture->pcData ||
        texture->mWidth == 0 ||
        texture->mWidth >
            static_cast<unsigned int>(std::numeric_limits<int>::max())) {
        source.valid = false;
        source.invalidReason = "invalid embedded texture reference";
        return source;
    }

    source.embeddedTexture = texture;
    return source;
}

TextureSource makeExplicitTextureSource(const char* texturePath) {
    const std::filesystem::path path(texturePath);
    return makeExternalTextureSource(path, texturePath);
}

TextureSource makeFallbackTextureSource() {
    return makeExternalTextureSource(fallbackTexturePath,
                                     fallbackTexturePath);
}

struct DecodedImage {
    StbiPixelOwner pixels;
    int width = 0;
    int height = 0;
    int channels = 0;
    uint32_t mipLevels = 0;
};

struct TextureCacheEntry {
    std::shared_ptr<DecodedImage> image;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept {
        return static_cast<bool>(image);
    }
};

using TextureCache =
    std::unordered_map<TextureCacheKey, TextureCacheEntry,
                       TextureCacheKeyHash>;

LoadModelProfile::Clock::duration& textureDecodeTime(
    LoadModelProfile& profile, TextureUsage usage) {
    switch (usage) {
        case TextureUsage::BaseColor:
            return profile.baseColorDecodeTime;
        case TextureUsage::Normal:
            return profile.normalMapDecodeTime;
        case TextureUsage::MetallicRoughness:
            return profile.metallicRoughnessDecodeTime;
    }
    return profile.baseColorDecodeTime;
}

TextureCacheEntry decodeTexture(const TextureSource& source,
                                TextureUsage usage, bool fallback,
                                TextureCache& cache,
                                LoadModelProfile& profile) {
    TextureCacheKey key{source.sourceIdentity, usage};
    const auto cached = cache.find(key);
    if (cached != cache.end()) {
        return cached->second;
    }

    PhaseTimer phaseTimer(textureDecodeTime(profile, usage));
    const bool fromMemory = source.isEmbedded();
    recordDecodeCall(profile, usage, fromMemory, fallback);

    TextureCacheEntry entry;
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc* decodedPixels = nullptr;
    if (fromMemory) {
        const aiTexture* texture = source.embeddedTexture;
        decodedPixels = stbi_load_from_memory(
            reinterpret_cast<const unsigned char*>(texture->pcData),
            static_cast<int>(texture->mWidth), &width, &height, &channels,
            STBI_rgb_alpha);
    } else {
        decodedPixels = stbi_load(source.externalPath.string().c_str(), &width,
                                  &height, &channels, STBI_rgb_alpha);
    }

    if (!decodedPixels) {
        entry.error = (fromMemory ? "stbi_load_from_memory failed for "
                                  : "stbi_load failed for ") +
                      source.displayPath + ": " + stbFailureReason();
        const auto inserted = cache.emplace(std::move(key), entry);
        return inserted.first->second;
    }

    StbiPixelOwner pixelOwner = makeStbiPixelOwner(decodedPixels);
    uint32_t mipLevels = 0;
    const Result mipResult = calculateMipLevels(width, height, mipLevels);
    if (!mipResult) {
        entry.error = "Invalid texture dimensions for " + source.displayPath +
                      ": " + mipResult.error();
        const auto inserted = cache.emplace(std::move(key), entry);
        return inserted.first->second;
    }

    entry.image = std::make_shared<DecodedImage>();
    entry.image->pixels = std::move(pixelOwner);
    entry.image->width = width;
    entry.image->height = height;
    entry.image->channels = channels;
    entry.image->mipLevels = mipLevels;
    recordDecodedImage(profile, usage, width, height);
    const auto inserted = cache.emplace(std::move(key), entry);
    return inserted.first->second;
}

TextureCacheEntry loadFileTexture(const TextureSource& source,
                                  TextureCache& cache,
                                  LoadModelProfile& profile, bool fallback) {
    return decodeTexture(source, TextureUsage::BaseColor, fallback, cache,
                         profile);
}

TextureCacheEntry loadNormalMapFileTexture(const TextureSource& source,
                                           TextureCache& cache,
                                           LoadModelProfile& profile) {
    return decodeTexture(source, TextureUsage::Normal, false, cache, profile);
}

TextureCacheEntry loadMetallicRoughnessFileTexture(
    const TextureSource& source, TextureCache& cache,
    LoadModelProfile& profile) {
    return decodeTexture(source, TextureUsage::MetallicRoughness, false, cache,
                         profile);
}

void assignBaseColorImage(Material& material,
                          const TextureCacheEntry& texture) {
    if (!texture.image) {
        return;
    }
    material.pixelsOwner = texture.image->pixels;
    material.pixels = material.pixelsOwner.get();
    material.texWidth = texture.image->width;
    material.texHeight = texture.image->height;
    material.texChannels = texture.image->channels;
    material.mipLevels = texture.image->mipLevels;
}

void assignNormalMapImage(Material& material,
                          const TextureCacheEntry& texture) {
    if (!texture.image) {
        return;
    }
    material.normalMapPixelsOwner = texture.image->pixels;
    material.normalMapPixels = material.normalMapPixelsOwner.get();
    material.normalMapWidth = texture.image->width;
    material.normalMapHeight = texture.image->height;
    material.normalMapChannels = texture.image->channels;
    material.normalMapMipLevels = texture.image->mipLevels;
}

void assignMetallicRoughnessImage(Material& material,
                                  const TextureCacheEntry& texture) {
    if (!texture.image) {
        return;
    }
    material.metallicRoughnessMapPixelsOwner = texture.image->pixels;
    material.metallicRoughnessMapPixels =
        material.metallicRoughnessMapPixelsOwner.get();
    material.metallicRoughnessMapWidth = texture.image->width;
    material.metallicRoughnessMapHeight = texture.image->height;
    material.metallicRoughnessMapChannels = texture.image->channels;
    material.metallicRoughnessMapMipLevels = texture.image->mipLevels;
}

float sanitizeMaterialFactor(float value, float fallback);
glm::vec4 sanitizeBaseColorFactor(const aiColor4D& color);

struct ImportedMaterialTemplate {
    Material material{};
    std::string texturePath;
    std::string baseColorIntendedSourceIdentity;
    std::string baseColorSourceIdentity;
    std::string normalMapSourceIdentity;
    std::string metallicRoughnessSourceIdentity;
    std::string normalMapError;
    std::string metallicRoughnessError;
    bool prepared = false;
};

Result prepareImportedMaterial(
    const aiScene* scene, aiMaterial* importedMaterial,
    unsigned int materialIndex, const char* modelPath,
    const char* fallbackTexturePathOverride, const char* firstMeshName,
    std::vector<bool>& warnedBlendMaterials, TextureCache& textureCache,
    LoadModelProfile& profile, ImportedMaterialTemplate& output) {
    ++profile.materialPreparations;

    {
        PhaseTimer phaseTimer(profile.materialPropertyExtractionTime);
        aiString alphaMode;
        if (importedMaterial->Get(AI_MATKEY_GLTF_ALPHAMODE, alphaMode) ==
            AI_SUCCESS) {
            const std::string importedAlphaMode = alphaMode.C_Str();
            if (importedAlphaMode == "MASK") {
                output.material.alphaMode = MaterialAlphaMode::Mask;
                float alphaCutoff = output.material.alphaCutoff;
                if (importedMaterial->Get(AI_MATKEY_GLTF_ALPHACUTOFF,
                                           alphaCutoff) == AI_SUCCESS &&
                    std::isfinite(alphaCutoff) && alphaCutoff >= 0.0f &&
                    alphaCutoff <= 1.0f) {
                    output.material.alphaCutoff = alphaCutoff;
                }
            } else if (importedAlphaMode == "BLEND") {
                output.material.alphaMode = MaterialAlphaMode::Blend;
                if (!warnedBlendMaterials[materialIndex]) {
                    spdlog::warn(
                        "Material {} uses BLEND alpha mode; blended "
                        "transparency is not implemented and will render as "
                        "opaque",
                        firstMeshName);
                    warnedBlendMaterials[materialIndex] = true;
                }
            }
        }

        bool doubleSided = false;
        if (importedMaterial->Get(AI_MATKEY_TWOSIDED, doubleSided) ==
            AI_SUCCESS) {
            output.material.doubleSided = doubleSided;
        }

        aiColor4D importedBaseColorFactor{};
        if (importedMaterial->Get(AI_MATKEY_BASE_COLOR,
                                  importedBaseColorFactor) == AI_SUCCESS) {
            output.material.baseColorFactor =
                sanitizeBaseColorFactor(importedBaseColorFactor);
        }
        float importedMetallicFactor = output.material.metallicFactor;
        if (importedMaterial->Get(AI_MATKEY_METALLIC_FACTOR,
                                  importedMetallicFactor) == AI_SUCCESS) {
            output.material.metallicFactor =
                sanitizeMaterialFactor(importedMetallicFactor, 1.0f);
        }
        float importedRoughnessFactor = output.material.roughnessFactor;
        if (importedMaterial->Get(AI_MATKEY_ROUGHNESS_FACTOR,
                                  importedRoughnessFactor) == AI_SUCCESS) {
            output.material.roughnessFactor =
                sanitizeMaterialFactor(importedRoughnessFactor, 1.0f);
        }
    }

    const TextureSource fallbackSource = makeFallbackTextureSource();
    const auto decodeBaseColor = [&](const TextureSource& source) {
        if (!source.valid) {
            return TextureCacheEntry{nullptr, source.invalidReason};
        }
        return loadFileTexture(source, textureCache, profile, false);
    };

    const auto loadBaseWithFallback = [&](const TextureSource& intendedSource,
                                          bool allowFallback,
                                          bool noTextureSpecified) -> Result {
        recordTextureSource(profile, TextureUsage::BaseColor,
                            intendedSource.sourceIdentity);
        output.baseColorIntendedSourceIdentity =
            intendedSource.sourceIdentity;
        const TextureCacheEntry intended =
            intendedSource.sourceIdentity == fallbackSource.sourceIdentity
                ? loadFileTexture(intendedSource, textureCache, profile, true)
                : decodeBaseColor(intendedSource);
        if (intended.succeeded()) {
            assignBaseColorImage(output.material, intended);
            output.texturePath = intendedSource.displayPath;
            output.baseColorSourceIdentity = intendedSource.sourceIdentity;
            return Result::success();
        }

        if (!allowFallback) {
            if (noTextureSpecified) {
                return Result::failure(
                    "No texture was specified for " +
                    std::string(firstMeshName) + "; fallback texture " +
                    std::string(fallbackTexturePath) + ": " + intended.error);
            }
            return Result::failure(intended.error);
        }

        spdlog::error("Failed to load intended texture {}: {}",
                      intendedSource.displayPath, intended.error);
        recordTextureSource(profile, TextureUsage::BaseColor,
                            fallbackSource.sourceIdentity);
        const TextureCacheEntry fallback =
            loadFileTexture(fallbackSource, textureCache, profile, true);
        if (!fallback.succeeded()) {
            return Result::failure(
                "Failed to load intended texture " +
                intendedSource.displayPath + " (" + firstMeshName +
                ") and fallback texture " + std::string(fallbackTexturePath) +
                ": " + fallback.error);
        }

        assignBaseColorImage(output.material, fallback);
        output.texturePath = fallbackSource.displayPath;
        output.baseColorSourceIdentity = fallbackSource.sourceIdentity;
        spdlog::warn("Using fallback texture {} for {}",
                     fallbackTexturePath, firstMeshName);
        return Result::success();
    };

    aiString textureReference;
    if (importedMaterial->GetTexture(aiTextureType_BASE_COLOR, 0,
                                     &textureReference) == AI_SUCCESS) {
        const TextureSource source = resolveTextureSource(
            scene, modelPath, textureReference.C_Str());
        const Result baseResult = loadBaseWithFallback(source, true, false);
        if (!baseResult) {
            return baseResult;
        }
    } else if (fallbackTexturePathOverride != nullptr) {
        const TextureSource source =
            makeExplicitTextureSource(fallbackTexturePathOverride);
        const Result baseResult = loadBaseWithFallback(source, true, false);
        if (!baseResult) {
            return baseResult;
        }
    } else {
        const Result baseResult =
            loadBaseWithFallback(fallbackSource, false, true);
        if (!baseResult) {
            return baseResult;
        }
    }

    aiString normalMapReference;
    if (importedMaterial->GetTexture(aiTextureType_NORMALS, 0,
                                     &normalMapReference) == AI_SUCCESS) {
        const TextureSource source = resolveTextureSource(
            scene, modelPath, normalMapReference.C_Str());
        recordTextureSource(profile, TextureUsage::Normal,
                            source.sourceIdentity);
        output.material.normalMapPath = source.displayPath;
        output.normalMapSourceIdentity = source.sourceIdentity;
        const TextureCacheEntry normalMap =
            source.valid
                ? loadNormalMapFileTexture(source, textureCache, profile)
                : TextureCacheEntry{nullptr, source.invalidReason};
        if (normalMap.succeeded()) {
            assignNormalMapImage(output.material, normalMap);
        } else {
            output.normalMapError = normalMap.error;
        }
    }

    aiString metallicRoughnessMapReference;
    if (importedMaterial->GetTexture(
            AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE,
            &metallicRoughnessMapReference) == AI_SUCCESS) {
        const TextureSource source = resolveTextureSource(
            scene, modelPath, metallicRoughnessMapReference.C_Str());
        recordTextureSource(profile, TextureUsage::MetallicRoughness,
                            source.sourceIdentity);
        output.material.metallicRoughnessMapPath = source.displayPath;
        output.metallicRoughnessSourceIdentity = source.sourceIdentity;
        const TextureCacheEntry metallicRoughnessMap =
            source.valid
                ? loadMetallicRoughnessFileTexture(source, textureCache, profile)
                : TextureCacheEntry{nullptr, source.invalidReason};
        if (metallicRoughnessMap.succeeded()) {
            output.material.hasMetallicRoughnessMap = true;
            assignMetallicRoughnessImage(output.material, metallicRoughnessMap);
        } else {
            output.metallicRoughnessError = metallicRoughnessMap.error;
        }
    }

    return Result::success();
}

float sanitizeMaterialFactor(float value, float fallback) {
    if (!std::isfinite(value)) {
        return fallback;
    }
    return std::clamp(value, 0.0f, 1.0f);
}

glm::vec4 sanitizeBaseColorFactor(const aiColor4D& color) {
    return glm::vec4(
        sanitizeMaterialFactor(static_cast<float>(color.r), 1.0f),
        sanitizeMaterialFactor(static_cast<float>(color.g), 1.0f),
        sanitizeMaterialFactor(static_cast<float>(color.b), 1.0f),
        sanitizeMaterialFactor(static_cast<float>(color.a), 1.0f));
}

bool isFiniteVector(const glm::vec3& vector) {
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
           std::isfinite(vector.z);
}

bool normalizeVector(const glm::vec3& input, glm::vec3& output) {
    const float lengthSquared = glm::dot(input, input);
    if (!isFiniteVector(input) || !std::isfinite(lengthSquared) ||
        lengthSquared <= std::numeric_limits<float>::epsilon()) {
        return false;
    }
    output = input / std::sqrt(lengthSquared);
    return isFiniteVector(output);
}

}  // namespace

Result GameObject::loadModel() {
    LoadModelProfile profile;
    spdlog::info("Loading game object model from path {}...", modelPath);

    if (modelPath == nullptr) {
        spdlog::error("Model path is null. Cannot load model.");
        return Result::failure("Model path is null");
    }

    const auto assimpReadStart = LoadModelProfile::Clock::now();
    const aiScene* scene =
        importer.ReadFile(modelPath, aiProcess_Triangulate | aiProcess_FlipUVs |
                                    aiProcess_GenSmoothNormals |
                                    aiProcess_CalcTangentSpace);
    profile.assimpReadTime = LoadModelProfile::Clock::now() - assimpReadStart;
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
        !scene->mRootNode) {
        return Result::failure(
            "Failed to load model file " + std::string(modelPath) + ": " +
            (importer.GetErrorString() ? importer.GetErrorString()
                                       : "unknown Assimp error"));
    }

    try {
        std::vector<MeshInstance> pendingMeshes;
        std::vector<std::string> pendingTexturePaths;
        std::vector<bool> warnedBlendMaterials(scene->mNumMaterials, false);
        std::vector<ImportedMaterialTemplate> materialTemplates(
            scene->mNumMaterials);
        TextureCache textureCache;
        textureCache.reserve(static_cast<std::size_t>(scene->mNumMaterials) *
                             3);
        pendingMeshes.reserve(scene->mNumMeshes);
        pendingTexturePaths.reserve(scene->mNumMeshes);

        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* mesh = scene->mMeshes[i];
            if (!mesh || mesh->mMaterialIndex >= scene->mNumMaterials) {
                return Result::failure(
                    "Model contains an invalid mesh material index");
            }
            aiMaterial* material = scene->mMaterials[mesh->mMaterialIndex];
            if (!material) {
                return Result::failure(
                    "Model contains a null material for mesh " +
                    std::string(mesh->mName.C_Str()));
            }
            if (!mesh->HasNormals()) {
                return Result::failure(
                    "Mesh " + std::string(mesh->mName.C_Str()) +
                    " has no usable normals after import");
            }

            ImportedMaterialTemplate& importedMaterial =
                materialTemplates[mesh->mMaterialIndex];
            if (!importedMaterial.prepared) {
                const Result preparationResult = prepareImportedMaterial(
                    scene, material, mesh->mMaterialIndex, modelPath,
                    texturePath, mesh->mName.C_Str(), warnedBlendMaterials,
                    textureCache, profile, importedMaterial);
                if (!preparationResult) {
                    return preparationResult;
                }
                importedMaterial.prepared = true;
            }

            MeshInstance instance{};
            instance.material = importedMaterial.material;
            recordTextureReference(
                profile, TextureUsage::BaseColor,
                importedMaterial.baseColorIntendedSourceIdentity);
            if (importedMaterial.baseColorSourceIdentity !=
                importedMaterial.baseColorIntendedSourceIdentity) {
                recordTextureReference(profile, TextureUsage::BaseColor,
                                       importedMaterial.baseColorSourceIdentity);
            }
            if (!importedMaterial.normalMapSourceIdentity.empty()) {
                recordTextureReference(profile, TextureUsage::Normal,
                                       importedMaterial.normalMapSourceIdentity);
            }
            if (!importedMaterial.metallicRoughnessSourceIdentity.empty()) {
                recordTextureReference(
                    profile, TextureUsage::MetallicRoughness,
                    importedMaterial.metallicRoughnessSourceIdentity);
            }
            const bool hasTextureCoordinates = mesh->HasTextureCoords(0);
            const bool hasTangents = mesh->HasTangentsAndBitangents();
            bool meshHasUsableTangents = hasTangents;

            std::size_t faceIndexCount = 0;
            for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
                faceIndexCount += mesh->mFaces[f].mNumIndices;
            }
            instance.mesh.indices.reserve(faceIndexCount);
            instance.mesh.vertices.reserve(mesh->mNumVertices);
            std::unordered_map<Vertex, uint32_t> uniqueVertices;
            uniqueVertices.reserve(mesh->mNumVertices);

            {
                PhaseTimer phaseTimer(profile.geometryConversionTime);
                for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
                    const aiFace& face = mesh->mFaces[f];
                    for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                        ++profile.totalFaceIndexCount;
                        const unsigned int index = face.mIndices[j];
                        Vertex vertex{};
                        vertex.pos = {mesh->mVertices[index].x,
                                      mesh->mVertices[index].y,
                                      mesh->mVertices[index].z};
                        vertex.texCoord = hasTextureCoordinates
                            ? glm::vec2{mesh->mTextureCoords[0][index].x,
                                        mesh->mTextureCoords[0][index].y}
                            : glm::vec2{0.0f, 0.0f};
                        vertex.color = {1.0f, 1.0f, 1.0f};
                        if (!normalizeVector(
                                {mesh->mNormals[index].x,
                                 mesh->mNormals[index].y,
                                 mesh->mNormals[index].z},
                                vertex.normal)) {
                            return Result::failure(
                                "Mesh " + std::string(mesh->mName.C_Str()) +
                                " contains an unusable vertex normal");
                        }

                        if (hasTangents) {
                            glm::vec3 tangent;
                            const glm::vec3 importedTangent{
                                mesh->mTangents[index].x,
                                mesh->mTangents[index].y,
                                mesh->mTangents[index].z};
                            const glm::vec3 importedBitangent{
                                mesh->mBitangents[index].x,
                                mesh->mBitangents[index].y,
                                mesh->mBitangents[index].z};
                            const glm::vec3 orthogonalTangent =
                                importedTangent -
                                glm::dot(importedTangent, vertex.normal) *
                                    vertex.normal;
                            if (normalizeVector(orthogonalTangent, tangent) &&
                                isFiniteVector(importedBitangent)) {
                                const float handedness =
                                    glm::dot(glm::cross(vertex.normal, tangent),
                                             importedBitangent) < 0.0f
                                        ? -1.0f
                                        : 1.0f;
                                vertex.tangent = glm::vec4(tangent, handedness);
                            } else {
                                meshHasUsableTangents = false;
                            }
                        } else {
                            meshHasUsableTangents = false;
                        }

                        const auto [vertexIt, inserted] = uniqueVertices.emplace(
                            vertex,
                            static_cast<uint32_t>(instance.mesh.vertices.size()));
                        if (inserted) {
                            instance.mesh.vertices.push_back(vertex);
                        }
                        instance.mesh.indices.push_back(vertexIt->second);
                    }
                }
            }
            profile.totalUniqueVertices += instance.mesh.vertices.size();

            if (instance.material.normalMapPixels) {
                if (meshHasUsableTangents) {
                    instance.material.normalMapEnabled = true;
                } else {
                    spdlog::warn("Mesh {} has a normal map but no usable tangent "
                                 "basis; normal mapping is disabled",
                                 mesh->mName.C_Str());
                    releaseStbiPixel(instance.material.normalMapPixelsOwner,
                                     instance.material.normalMapPixels);
                    instance.material.normalMapWidth = 0;
                    instance.material.normalMapHeight = 0;
                    instance.material.normalMapChannels = 0;
                    instance.material.normalMapMipLevels = 0;
                }
            } else if (!importedMaterial.normalMapError.empty()) {
                spdlog::warn("Failed to load normal map {} for mesh {}: {}; "
                             "using geometric normals",
                             instance.material.normalMapPath,
                             mesh->mName.C_Str(),
                             importedMaterial.normalMapError);
            }

            if (!importedMaterial.metallicRoughnessError.empty()) {
                spdlog::warn(
                    "Failed to load optional metallic-roughness texture {} "
                    "for mesh {}: {}; using scalar factors",
                    instance.material.metallicRoughnessMapPath,
                    mesh->mName.C_Str(),
                    importedMaterial.metallicRoughnessError);
            }

            pendingTexturePaths.push_back(importedMaterial.texturePath);
            pendingMeshes.push_back(std::move(instance));
        }

        const auto finalCommitStart = LoadModelProfile::Clock::now();
        const std::size_t originalTexturePathCount =
            texturePathStorage_.size();
        try {
            meshInstances_.reserve(meshInstances_.size() + pendingMeshes.size());
            for (std::size_t i = 0; i < pendingMeshes.size(); ++i) {
                texturePathStorage_.push_back(std::move(pendingTexturePaths[i]));
                pendingMeshes[i].material.texturePath =
                    texturePathStorage_.back().c_str();
            }
            meshInstances_.insert(
                meshInstances_.end(),
                std::make_move_iterator(pendingMeshes.begin()),
                std::make_move_iterator(pendingMeshes.end()));
        } catch (const std::exception& exception) {
            texturePathStorage_.resize(originalTexturePathCount);
            return Result::failure("Failed to commit meshes from model " +
                                   std::string(modelPath) + ": " +
                                   exception.what());
        } catch (...) {
            texturePathStorage_.resize(originalTexturePathCount);
            return Result::failure("Failed to commit meshes from model " +
                                   std::string(modelPath) +
                                   ": unknown error");
        }
        profile.finalCommitTime =
            LoadModelProfile::Clock::now() - finalCommitStart;

        const auto milliseconds =
            [](LoadModelProfile::Clock::duration duration) {
                return std::chrono::duration<double, std::milli>(duration)
                    .count();
            };
        const auto totalTime =
            LoadModelProfile::Clock::now() - profile.totalStart;
        spdlog::info(
            "loadModel summary: total={:.3f}ms assimpRead={:.3f}ms "
            "geometry={:.3f}ms materialProperties={:.3f}ms "
            "baseDecode={:.3f}ms normalDecode={:.3f}ms "
            "metallicRoughnessDecode={:.3f}ms finalCommit={:.3f}ms; "
            "meshes={} materials={} materialPreparations={} faceIndices={} "
            "uniqueVertices={}; refs(base={} normal={} metallicRoughness={}) "
            "uniqueSources(base={} normal={} metallicRoughness={}); "
            "decodeCalls(base={} [file={} memory={}] normal={} [file={} "
            "memory={}] metallicRoughness={} [file={} memory={}]); "
            "fallbackDecodes={} decodedRgbaBytes={}",
            milliseconds(totalTime), milliseconds(profile.assimpReadTime),
            milliseconds(profile.geometryConversionTime),
            milliseconds(profile.materialPropertyExtractionTime),
            milliseconds(profile.baseColorDecodeTime),
            milliseconds(profile.normalMapDecodeTime),
            milliseconds(profile.metallicRoughnessDecodeTime),
            milliseconds(profile.finalCommitTime), scene->mNumMeshes,
            scene->mNumMaterials, profile.materialPreparations,
            profile.totalFaceIndexCount, profile.totalUniqueVertices,
            profile.baseColor.references, profile.normalMap.references,
            profile.metallicRoughness.references,
            profile.baseColor.uniqueSources.size(),
            profile.normalMap.uniqueSources.size(),
            profile.metallicRoughness.uniqueSources.size(),
            profile.baseColor.stbiLoadCalls +
                profile.baseColor.stbiLoadFromMemoryCalls,
            profile.baseColor.stbiLoadCalls,
            profile.baseColor.stbiLoadFromMemoryCalls,
            profile.normalMap.stbiLoadCalls +
                profile.normalMap.stbiLoadFromMemoryCalls,
            profile.normalMap.stbiLoadCalls,
            profile.normalMap.stbiLoadFromMemoryCalls,
            profile.metallicRoughness.stbiLoadCalls +
                profile.metallicRoughness.stbiLoadFromMemoryCalls,
            profile.metallicRoughness.stbiLoadCalls,
            profile.metallicRoughness.stbiLoadFromMemoryCalls,
            profile.fallbackTextureDecodeCount, profile.totalDecodedRgbaBytes);
        spdlog::info("Successfully loaded game object model");
        return Result::success();
    } catch (const std::exception& exception) {
        return Result::failure("Failed to load model " + std::string(modelPath) +
                               ": " + exception.what());
    } catch (...) {
        return Result::failure("Failed to load model " + std::string(modelPath) +
                               ": unknown error");
    }
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
