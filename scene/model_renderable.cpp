#include "model_renderable.h"
#include "loading_cache_key.h"

#include "../assets/model_asset.h"

#include <spdlog/spdlog.h>
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"

#include <assimp/GltfMaterial.h>
#include <assimp/Importer.hpp>
#include <assimp/postprocess.h>
#include <assimp/scene.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace model_loading {

struct CachedCpuMaterial {
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    uint32_t mipLevels = 0;
    StbiPixelOwner pixelsOwner{};
    int texWidth = 0;
    int texHeight = 0;
    int texChannels = 0;
    std::string texturePath;
    std::string normalMapPath;
    uint32_t normalMapMipLevels = 0;
    StbiPixelOwner normalMapPixelsOwner{};
    int normalMapWidth = 0;
    int normalMapHeight = 0;
    int normalMapChannels = 0;
    bool normalMapEnabled = false;
    std::string metallicRoughnessMapPath;
    uint32_t metallicRoughnessMapMipLevels = 0;
    StbiPixelOwner metallicRoughnessMapPixelsOwner{};
    int metallicRoughnessMapWidth = 0;
    int metallicRoughnessMapHeight = 0;
    int metallicRoughnessMapChannels = 0;
    bool hasMetallicRoughnessMap = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

struct CachedCpuMesh {
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    Mesh::Bounds bounds;
    CachedCpuMaterial material;
};

struct CachedCpuModel {
    std::vector<CachedCpuMesh> meshes;
    std::size_t sourceMaterialCount = 0;
    std::size_t sourceFaceIndexCount = 0;
    std::size_t sourceUniqueVertexCount = 0;
    std::size_t sourceMaterialPreparations = 0;
};

}  // namespace model_loading

namespace {

constexpr const char* fallbackTexturePath =
    "rendering/default_textures/error.jpg";

using TextureUsage = model_loading::TextureUsage;
using TextureCacheKey = model_loading::TextureCacheKey;
using TextureCacheKeyHash = model_loading::TextureCacheKeyHash;
using CachedCpuMaterial = model_loading::CachedCpuMaterial;
using CachedCpuMesh = model_loading::CachedCpuMesh;
using CachedCpuModel = model_loading::CachedCpuModel;
using ModelAssetCacheKey = model_loading::ModelAssetCacheKey;
using ModelAssetCacheKeyHash = model_loading::ModelAssetCacheKeyHash;

struct ModelAssetCache {
    std::mutex mutex;
    std::unordered_map<ModelAssetCacheKey, std::weak_ptr<const CachedCpuModel>,
                       ModelAssetCacheKeyHash>
        entries;
    std::size_t hits = 0;
    std::size_t misses = 0;
};

ModelAssetCache& modelAssetCache() {
    static ModelAssetCache cache;
    return cache;
}

struct CacheLookupResult {
    std::shared_ptr<const CachedCpuModel> asset;
    std::size_t hits = 0;
    std::size_t misses = 0;
};

CacheLookupResult findCachedCpuModel(const ModelAssetCacheKey& key) {
    ModelAssetCache& cache = modelAssetCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    const auto found = cache.entries.find(key);
    if (found != cache.entries.end()) {
        if (std::shared_ptr<const CachedCpuModel> asset = found->second.lock()) {
            ++cache.hits;
            return {std::move(asset), cache.hits, cache.misses};
        }
        cache.entries.erase(found);
    }
    ++cache.misses;
    return {nullptr, cache.hits, cache.misses};
}

void publishCachedCpuModel(const ModelAssetCacheKey& key,
                           const std::shared_ptr<const CachedCpuModel>& asset) {
    ModelAssetCache& cache = modelAssetCache();
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.entries[key] = asset;
}

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
    Clock::duration cacheLookupTime{};
    Clock::duration cachedInstantiationTime{};
    Clock::duration sourceLoadingTime{};
    Clock::duration geometryConversionTime{};
    Clock::duration materialPropertyExtractionTime{};
    Clock::duration textureDecodeWallTime{};
    Clock::duration baseColorDecodeTime{};
    Clock::duration normalMapDecodeTime{};
    Clock::duration metallicRoughnessDecodeTime{};
    Clock::duration finalCommitTime{};
    std::size_t totalFaceIndexCount = 0;
    std::size_t totalUniqueVertices = 0;
    std::size_t materialPreparations = 0;
    std::size_t fallbackTextureDecodeCount = 0;
    std::size_t decodeWorkers = 0;
    std::size_t decodeJobs = 0;
    std::size_t decodeSuccesses = 0;
    std::size_t decodeFailures = 0;
    std::size_t maxConcurrentDecodes = 0;
    std::size_t totalDecodedRgbaBytes = 0;
    std::size_t copiedMeshes = 0;
    std::size_t copiedVertices = 0;
    std::size_t copiedIndices = 0;
    std::size_t cacheHits = 0;
    std::size_t cacheMisses = 0;
    bool cacheHit = false;
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

struct TextureDecodeJob {
    TextureCacheKey key;
    TextureSource source;
    bool fallback = false;
};

struct TextureDecodeJobResult {
    TextureCacheEntry entry;
    LoadModelProfile::Clock::duration cpuTime{};
    bool fromMemory = false;
    bool fallback = false;
};

TextureDecodeJobResult decodeTextureJob(const TextureDecodeJob& job) noexcept {
    TextureDecodeJobResult result;
    result.fromMemory = job.source.isEmbedded();
    result.fallback = job.fallback;
    const auto start = LoadModelProfile::Clock::now();
    try {
        if (!job.source.valid) {
            result.entry.error = job.source.invalidReason;
        } else {
            int width = 0;
            int height = 0;
            int channels = 0;
            stbi_uc* decodedPixels = nullptr;
            if (result.fromMemory) {
                const aiTexture* texture = job.source.embeddedTexture;
                decodedPixels = stbi_load_from_memory(
                    reinterpret_cast<const unsigned char*>(texture->pcData),
                    static_cast<int>(texture->mWidth), &width, &height,
                    &channels, STBI_rgb_alpha);
            } else {
                decodedPixels = stbi_load(job.source.externalPath.string().c_str(),
                                          &width, &height, &channels,
                                          STBI_rgb_alpha);
            }
            if (!decodedPixels) {
                result.entry.error =
                    (result.fromMemory ? "stbi_load_from_memory failed for "
                                       : "stbi_load failed for ") +
                    job.source.displayPath + ": " + stbFailureReason();
            } else {
                StbiPixelOwner pixelOwner = makeStbiPixelOwner(decodedPixels);
                uint32_t mipLevels = 0;
                const Result mipResult =
                    calculateMipLevels(width, height, mipLevels);
                if (!mipResult) {
                    result.entry.error = "Invalid texture dimensions for " +
                                         job.source.displayPath + ": " +
                                         mipResult.error();
                } else {
                    result.entry.image = std::make_shared<DecodedImage>();
                    result.entry.image->pixels = std::move(pixelOwner);
                    result.entry.image->width = width;
                    result.entry.image->height = height;
                    result.entry.image->channels = channels;
                    result.entry.image->mipLevels = mipLevels;
                }
            }
        }
    } catch (const std::exception& exception) {
        result.entry.error = "Texture decode failed for " +
                             job.source.displayPath + ": " + exception.what();
    } catch (...) {
        result.entry.error = "Texture decode failed for " +
                             job.source.displayPath + ": unknown error";
    }
    result.cpuTime = LoadModelProfile::Clock::now() - start;
    return result;
}

class ThreadJoiner {
public:
    explicit ThreadJoiner(std::vector<std::thread>& threads) : threads_(threads) {}
    ~ThreadJoiner() {
        for (std::thread& thread : threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
    }

private:
    std::vector<std::thread>& threads_;
};

Result decodeTextureJobs(const std::vector<TextureDecodeJob>& jobs,
                         std::vector<TextureDecodeJobResult>& results,
                         LoadModelProfile& profile) {
    if (jobs.empty()) {
        return Result::success();
    }

    const unsigned int hardware = std::thread::hardware_concurrency();
    const std::size_t availableWorkers = hardware == 0 ? 1 : hardware;
    const std::size_t workerCount = std::min(
        jobs.size(), std::min<std::size_t>(4, availableWorkers));
    results.resize(jobs.size());
    std::atomic<std::size_t> nextJob{0};
    std::atomic<std::size_t> activeWorkers{0};
    std::atomic<std::size_t> maxActiveWorkers{0};
    std::vector<std::thread> workers;
    workers.reserve(workerCount);
    const auto wallStart = LoadModelProfile::Clock::now();
    {
        ThreadJoiner joiner(workers);
        try {
            for (std::size_t worker = 0; worker < workerCount; ++worker) {
                workers.emplace_back([&] {
                    for (;;) {
                        const std::size_t jobIndex = nextJob.fetch_add(1);
                        if (jobIndex >= jobs.size()) {
                            return;
                        }
                        const std::size_t active = activeWorkers.fetch_add(1) + 1;
                        std::size_t observed = maxActiveWorkers.load();
                        while (observed < active &&
                               !maxActiveWorkers.compare_exchange_weak(
                                   observed, active)) {
                        }
                        results[jobIndex] = decodeTextureJob(jobs[jobIndex]);
                        activeWorkers.fetch_sub(1);
                    }
                });
            }
        } catch (const std::exception& exception) {
            return Result::failure("Failed to create texture decoder worker: " +
                                   std::string(exception.what()));
        } catch (...) {
            return Result::failure(
                "Failed to create texture decoder worker: unknown error");
        }
    }
    profile.textureDecodeWallTime += LoadModelProfile::Clock::now() - wallStart;
    profile.decodeWorkers = workerCount;
    profile.maxConcurrentDecodes = maxActiveWorkers.load();
    return Result::success();
}

void gatherDecodeResult(const TextureDecodeJob& job,
                        TextureDecodeJobResult result, TextureCache& cache,
                        LoadModelProfile& profile) {
    recordDecodeCall(profile, job.key.usage, result.fromMemory, result.fallback);
    textureDecodeTime(profile, job.key.usage) += result.cpuTime;
    ++profile.decodeJobs;
    if (result.entry.succeeded()) {
        ++profile.decodeSuccesses;
        recordDecodedImage(profile, job.key.usage, result.entry.image->width,
                           result.entry.image->height);
    } else {
        ++profile.decodeFailures;
    }
    cache.emplace(job.key, std::move(result.entry));
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

struct PendingImportedMaterial {
    Material material{};
    std::string texturePath;
    std::string baseColorIntendedSourceIdentity;
    std::string baseColorSourceIdentity;
    std::string normalMapSourceIdentity;
    std::string metallicRoughnessSourceIdentity;
    std::string normalMapError;
    std::string metallicRoughnessError;
    std::string firstMeshName;
    unsigned int materialIndex = 0;
    TextureSource baseColorSource;
    TextureSource normalMapSource;
    TextureSource metallicRoughnessSource;
    bool hasNormalMapSource = false;
    bool hasMetallicRoughnessSource = false;
    bool baseFallbackAllowed = false;
    bool noBaseTextureSpecified = false;
    bool blendWarning = false;
    bool discovered = false;
};

void addDecodeJob(const TextureSource& source, TextureUsage usage,
                  bool fallback, std::vector<TextureDecodeJob>& jobs,
                  std::unordered_set<TextureCacheKey, TextureCacheKeyHash>& keys) {
    if (!source.valid) {
        return;
    }
    TextureCacheKey key{source.sourceIdentity, usage};
    if (keys.insert(key).second) {
        jobs.push_back({std::move(key), source, fallback});
    }
}

Result discoverImportedMaterial(
    const aiScene* scene, aiMaterial* importedMaterial,
    unsigned int materialIndex, const char* modelPath,
    const char* fallbackTexturePathOverride, const char* firstMeshName,
    LoadModelProfile& profile, PendingImportedMaterial& output,
    std::vector<TextureDecodeJob>& jobs,
    std::unordered_set<TextureCacheKey, TextureCacheKeyHash>& keys) {
    ++profile.materialPreparations;
    output.materialIndex = materialIndex;
    output.firstMeshName = firstMeshName;

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
                output.blendWarning = true;
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
    const auto setBaseSource = [&](TextureSource source, bool allowFallback,
                                   bool noTextureSpecified) {
        recordTextureSource(profile, TextureUsage::BaseColor,
                            source.sourceIdentity);
        output.baseColorIntendedSourceIdentity =
            source.sourceIdentity;
        output.baseColorSource = std::move(source);
        output.baseFallbackAllowed = allowFallback;
        output.noBaseTextureSpecified = noTextureSpecified;
        addDecodeJob(output.baseColorSource, TextureUsage::BaseColor,
                     output.baseColorSource.sourceIdentity ==
                         fallbackSource.sourceIdentity,
                     jobs, keys);
    };

    aiString textureReference;

    if (importedMaterial->GetTexture(aiTextureType_BASE_COLOR, 0,
                                     &textureReference) == AI_SUCCESS) {
        // Preferred PBR/glTF base-color texture.
        setBaseSource(resolveTextureSource(
                          scene, modelPath, textureReference.C_Str()),
                      true, false);
    } else if (importedMaterial->GetTexture(aiTextureType_DIFFUSE, 0,
                                            &textureReference) == AI_SUCCESS) {
        // Legacy/FBX diffuse texture fallback.
        setBaseSource(resolveTextureSource(
                          scene, modelPath, textureReference.C_Str()),
                      true, false);
    } else if (fallbackTexturePathOverride != nullptr) {
        setBaseSource(makeExplicitTextureSource(fallbackTexturePathOverride),
                      true, false);
    } else {
        setBaseSource(fallbackSource, false, true);
    }




    aiString normalMapReference;
    if (importedMaterial->GetTexture(aiTextureType_NORMALS, 0,
                                     &normalMapReference) == AI_SUCCESS) {
        output.normalMapSource = resolveTextureSource(
            scene, modelPath, normalMapReference.C_Str());
        recordTextureSource(profile, TextureUsage::Normal,
                            output.normalMapSource.sourceIdentity);
        output.material.normalMapPath = output.normalMapSource.displayPath;
        output.normalMapSourceIdentity = output.normalMapSource.sourceIdentity;
        output.hasNormalMapSource = true;
        addDecodeJob(output.normalMapSource, TextureUsage::Normal, false, jobs,
                     keys);
    }

    aiString metallicRoughnessMapReference;
    if (importedMaterial->GetTexture(
            AI_MATKEY_GLTF_PBRMETALLICROUGHNESS_METALLICROUGHNESS_TEXTURE,
            &metallicRoughnessMapReference) == AI_SUCCESS) {
        output.metallicRoughnessSource = resolveTextureSource(
            scene, modelPath, metallicRoughnessMapReference.C_Str());
        recordTextureSource(profile, TextureUsage::MetallicRoughness,
                            output.metallicRoughnessSource.sourceIdentity);
        output.material.metallicRoughnessMapPath =
            output.metallicRoughnessSource.displayPath;
        output.metallicRoughnessSourceIdentity =
            output.metallicRoughnessSource.sourceIdentity;
        output.hasMetallicRoughnessSource = true;
        addDecodeJob(output.metallicRoughnessSource,
                     TextureUsage::MetallicRoughness, false, jobs, keys);
    }

    return Result::success();
}

TextureCacheEntry textureEntry(const TextureSource& source, TextureUsage usage,
                               const TextureCache& cache) {
    if (!source.valid) {
        return {nullptr, source.invalidReason};
    }
    const auto found = cache.find({source.sourceIdentity, usage});
    if (found == cache.end()) {
        return {nullptr, "Texture decode result was not found for " +
                             source.displayPath};
    }
    return found->second;
}

Result finalizeImportedMaterial(PendingImportedMaterial& pending,
                               TextureCache& textureCache,
                               LoadModelProfile& profile) {
    if (pending.blendWarning) {
        spdlog::warn("Material {} uses BLEND alpha mode; blended transparency is "
                     "not implemented and will render as opaque",
                     pending.firstMeshName);
    }
    const TextureCacheEntry intended = textureEntry(
        pending.baseColorSource, TextureUsage::BaseColor, textureCache);
    if (intended.succeeded()) {
        assignBaseColorImage(pending.material, intended);
        pending.texturePath = pending.baseColorSource.displayPath;
        pending.baseColorSourceIdentity = pending.baseColorSource.sourceIdentity;
    } else if (!pending.baseFallbackAllowed) {
        if (pending.noBaseTextureSpecified) {
            return Result::failure("No texture was specified for " +
                                   pending.firstMeshName + "; fallback texture " +
                                   fallbackTexturePath + ": " + intended.error);
        }
        return Result::failure(intended.error);
    } else {
        spdlog::error("Failed to load intended texture {}: {}",
                      pending.baseColorSource.displayPath, intended.error);
        const TextureSource fallbackSource = makeFallbackTextureSource();
        recordTextureSource(profile, TextureUsage::BaseColor,
                            fallbackSource.sourceIdentity);
        TextureCacheEntry fallback = textureEntry(
            fallbackSource, TextureUsage::BaseColor, textureCache);
        if (!fallback.succeeded() &&
            textureCache.find({fallbackSource.sourceIdentity,
                               TextureUsage::BaseColor}) == textureCache.end()) {
            const auto fallbackStart = LoadModelProfile::Clock::now();
            TextureDecodeJob fallbackJob{{fallbackSource.sourceIdentity,
                                          TextureUsage::BaseColor},
                                         fallbackSource, true};
            TextureDecodeJobResult fallbackResult = decodeTextureJob(fallbackJob);
            profile.textureDecodeWallTime +=
                LoadModelProfile::Clock::now() - fallbackStart;
            gatherDecodeResult(fallbackJob, std::move(fallbackResult), textureCache,
                               profile);
            fallback = textureEntry(fallbackSource, TextureUsage::BaseColor,
                                    textureCache);
        }
        if (!fallback.succeeded()) {
            return Result::failure("Failed to load intended texture " +
                                   pending.baseColorSource.displayPath + " (" +
                                   pending.firstMeshName + ") and fallback texture " +
                                   fallbackTexturePath + ": " + fallback.error);
        }
        assignBaseColorImage(pending.material, fallback);
        pending.texturePath = fallbackSource.displayPath;
        pending.baseColorSourceIdentity = fallbackSource.sourceIdentity;
        spdlog::warn("Using fallback texture {} for {}", fallbackTexturePath,
                     pending.firstMeshName);
    }

    if (pending.hasNormalMapSource) {
        const TextureCacheEntry normal = textureEntry(
            pending.normalMapSource, TextureUsage::Normal, textureCache);
        if (normal.succeeded()) {
            assignNormalMapImage(pending.material, normal);
        } else {
            pending.normalMapError = normal.error;
        }
    }
    if (pending.hasMetallicRoughnessSource) {
        const TextureCacheEntry metallicRoughness = textureEntry(
            pending.metallicRoughnessSource, TextureUsage::MetallicRoughness,
            textureCache);
        if (metallicRoughness.succeeded()) {
            pending.material.hasMetallicRoughnessMap = true;
            assignMetallicRoughnessImage(pending.material, metallicRoughness);
        } else {
            pending.metallicRoughnessError = metallicRoughness.error;
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

Result validateIncomingMeshInstance(const MeshInstance& instance) {
    const auto& mesh = instance.mesh;
    const auto& material = instance.material;

    if (!mesh.indices.empty() && mesh.vertices.empty()) {
        return Result::failure(
            "MeshInstance indexed geometry has no vertices");
    }
    if (!mesh.indices.empty() && mesh.indices.size() % 3 != 0) {
        return Result::failure(
            "MeshInstance index count is not a complete triangle list");
    }
    for (const Vertex& vertex : mesh.vertices) {
        if (!isFiniteVector(vertex.pos)) {
            return Result::failure(
                "MeshInstance contains a non-finite vertex position");
        }
    }
    for (uint32_t index : mesh.indices) {
        if (index >= mesh.vertices.size()) {
            return Result::failure("MeshInstance contains an out-of-range index");
        }
    }

    const auto ownerMatches = [](const StbiPixelOwner& owner,
                                 const stbi_uc* pixels) {
        return !owner || owner.get() == pixels;
    };
    if (!ownerMatches(material.pixelsOwner, material.pixels) ||
        !ownerMatches(material.normalMapPixelsOwner,
                      material.normalMapPixels) ||
        !ownerMatches(material.metallicRoughnessMapPixelsOwner,
                      material.metallicRoughnessMapPixels)) {
        return Result::failure(
            "MeshInstance material pixel ownership is inconsistent");
    }
    return Result::success();
}

CachedCpuMaterial cacheMaterial(const Material& material,
                                std::string texturePath) {
    CachedCpuMaterial cached;
    cached.baseColorFactor = material.baseColorFactor;
    cached.metallicFactor = material.metallicFactor;
    cached.roughnessFactor = material.roughnessFactor;
    cached.mipLevels = material.mipLevels;
    cached.pixelsOwner = material.pixelsOwner;
    cached.texWidth = material.texWidth;
    cached.texHeight = material.texHeight;
    cached.texChannels = material.texChannels;
    cached.texturePath = std::move(texturePath);
    cached.normalMapPath = material.normalMapPath;
    cached.normalMapMipLevels = material.normalMapMipLevels;
    cached.normalMapPixelsOwner = material.normalMapPixelsOwner;
    cached.normalMapWidth = material.normalMapWidth;
    cached.normalMapHeight = material.normalMapHeight;
    cached.normalMapChannels = material.normalMapChannels;
    cached.normalMapEnabled = material.normalMapEnabled;
    cached.metallicRoughnessMapPath = material.metallicRoughnessMapPath;
    cached.metallicRoughnessMapMipLevels = material.metallicRoughnessMapMipLevels;
    cached.metallicRoughnessMapPixelsOwner =
        material.metallicRoughnessMapPixelsOwner;
    cached.metallicRoughnessMapWidth = material.metallicRoughnessMapWidth;
    cached.metallicRoughnessMapHeight = material.metallicRoughnessMapHeight;
    cached.metallicRoughnessMapChannels = material.metallicRoughnessMapChannels;
    cached.hasMetallicRoughnessMap = material.hasMetallicRoughnessMap;
    cached.alphaMode = material.alphaMode;
    cached.alphaCutoff = material.alphaCutoff;
    cached.doubleSided = material.doubleSided;
    return cached;
}

Material instantiateMaterial(const CachedCpuMaterial& cached) {
    Material material{};
    material.baseColorFactor = cached.baseColorFactor;
    material.metallicFactor = cached.metallicFactor;
    material.roughnessFactor = cached.roughnessFactor;
    material.mipLevels = cached.mipLevels;
    material.pixelsOwner = cached.pixelsOwner;
    material.pixels = material.pixelsOwner.get();
    material.texWidth = cached.texWidth;
    material.texHeight = cached.texHeight;
    material.texChannels = cached.texChannels;
    material.normalMapPath = cached.normalMapPath;
    material.normalMapMipLevels = cached.normalMapMipLevels;
    material.normalMapPixelsOwner = cached.normalMapPixelsOwner;
    material.normalMapPixels = material.normalMapPixelsOwner.get();
    material.normalMapWidth = cached.normalMapWidth;
    material.normalMapHeight = cached.normalMapHeight;
    material.normalMapChannels = cached.normalMapChannels;
    material.normalMapEnabled = cached.normalMapEnabled;
    material.metallicRoughnessMapPath = cached.metallicRoughnessMapPath;
    material.metallicRoughnessMapMipLevels =
        cached.metallicRoughnessMapMipLevels;
    material.metallicRoughnessMapPixelsOwner =
        cached.metallicRoughnessMapPixelsOwner;
    material.metallicRoughnessMapPixels =
        material.metallicRoughnessMapPixelsOwner.get();
    material.metallicRoughnessMapWidth = cached.metallicRoughnessMapWidth;
    material.metallicRoughnessMapHeight = cached.metallicRoughnessMapHeight;
    material.metallicRoughnessMapChannels = cached.metallicRoughnessMapChannels;
    material.hasMetallicRoughnessMap = cached.hasMetallicRoughnessMap;
    material.alphaMode = cached.alphaMode;
    material.alphaCutoff = cached.alphaCutoff;
    material.doubleSided = cached.doubleSided;
    return material;
}

Result instantiateCachedCpuModel(const CachedCpuModel& asset,
                                 std::vector<MeshInstance>& pendingMeshes,
                                 std::vector<std::string>& pendingTexturePaths,
                                 LoadModelProfile& profile) {
    PhaseTimer phaseTimer(profile.cachedInstantiationTime);
    pendingMeshes.reserve(asset.meshes.size());
    pendingTexturePaths.reserve(asset.meshes.size());
    for (const CachedCpuMesh& cached : asset.meshes) {
        MeshInstance instance{};
        instance.mesh.vertices = cached.vertices;
        instance.mesh.indices = cached.indices;
        instance.mesh.bounds = cached.bounds;
        instance.material = instantiateMaterial(cached.material);
        const Result validation = validateIncomingMeshInstance(instance);
        if (!validation) {
            return Result::failure("Cached CPU model is invalid: " +
                                   validation.error());
        }
        pendingTexturePaths.push_back(cached.material.texturePath);
        profile.copiedVertices += instance.mesh.vertices.size();
        profile.copiedIndices += instance.mesh.indices.size();
        ++profile.copiedMeshes;
        pendingMeshes.push_back(std::move(instance));
    }
    return Result::success();
}

bool calculateCachedMeshBounds(CachedCpuMesh& mesh) noexcept {
    mesh.bounds = {};
    bool foundFinitePosition = false;
    for (const Vertex& vertex : mesh.vertices) {
        if (!isFiniteVector(vertex.pos)) {
            continue;
        }
        if (!foundFinitePosition) {
            mesh.bounds.minimum = vertex.pos;
            mesh.bounds.maximum = vertex.pos;
            foundFinitePosition = true;
        } else {
            mesh.bounds.minimum = glm::min(mesh.bounds.minimum, vertex.pos);
            mesh.bounds.maximum = glm::max(mesh.bounds.maximum, vertex.pos);
        }
    }
    mesh.bounds.valid = foundFinitePosition;
    return foundFinitePosition;
}

double milliseconds(LoadModelProfile::Clock::duration duration) {
    return std::chrono::duration<double, std::milli>(duration).count();
}

void logLoadSummary(const LoadModelProfile& profile,
                    const CachedCpuModel& asset) {
    const auto totalTime = LoadModelProfile::Clock::now() - profile.totalStart;
    spdlog::info(
        "loadModel summary: cache={} total={:.3f}ms lookup={:.3f}ms "
        "instantiate={:.3f}ms sourceLoad={:.3f}ms assimpRead={:.3f}ms "
        "geometry={:.3f}ms materialProperties={:.3f}ms "
        "textureDecodeWall={:.3f}ms "
        "decodeCpu(base={:.3f}ms normal={:.3f}ms metallicRoughness={:.3f}ms) "
        "finalCommit={:.3f}ms; "
        "meshes={} materials={} materialPreparations={} faceIndices={} "
        "uniqueVertices={} copied(meshes={} vertices={} indices={}); "
        "refs(base={} normal={} metallicRoughness={}) "
        "uniqueSources(base={} normal={} metallicRoughness={}); "
        "decodeCalls(base={} [file={} memory={}] normal={} [file={} memory={}] "
        "metallicRoughness={} [file={} memory={}]); fallbackDecodes={} "
        "decodedRgbaBytes={}; decodeWorkers={} decodeJobs={} "
        "decodeSuccesses={} decodeFailures={} maxConcurrentDecodes={}; "
        "cacheCounts(hits={} misses={})",
        profile.cacheHit ? "hit" : "miss", milliseconds(totalTime),
        milliseconds(profile.cacheLookupTime),
        milliseconds(profile.cachedInstantiationTime),
        milliseconds(profile.sourceLoadingTime), milliseconds(profile.assimpReadTime),
        milliseconds(profile.geometryConversionTime),
        milliseconds(profile.materialPropertyExtractionTime),
        milliseconds(profile.textureDecodeWallTime),
        milliseconds(profile.baseColorDecodeTime),
        milliseconds(profile.normalMapDecodeTime),
        milliseconds(profile.metallicRoughnessDecodeTime),
        milliseconds(profile.finalCommitTime), asset.meshes.size(),
        asset.sourceMaterialCount, asset.sourceMaterialPreparations,
        asset.sourceFaceIndexCount, asset.sourceUniqueVertexCount,
        profile.copiedMeshes, profile.copiedVertices, profile.copiedIndices,
        profile.baseColor.references, profile.normalMap.references,
        profile.metallicRoughness.references,
        profile.baseColor.uniqueSources.size(), profile.normalMap.uniqueSources.size(),
        profile.metallicRoughness.uniqueSources.size(),
        profile.baseColor.stbiLoadCalls + profile.baseColor.stbiLoadFromMemoryCalls,
        profile.baseColor.stbiLoadCalls, profile.baseColor.stbiLoadFromMemoryCalls,
        profile.normalMap.stbiLoadCalls + profile.normalMap.stbiLoadFromMemoryCalls,
        profile.normalMap.stbiLoadCalls, profile.normalMap.stbiLoadFromMemoryCalls,
        profile.metallicRoughness.stbiLoadCalls +
            profile.metallicRoughness.stbiLoadFromMemoryCalls,
        profile.metallicRoughness.stbiLoadCalls,
        profile.metallicRoughness.stbiLoadFromMemoryCalls,
        profile.fallbackTextureDecodeCount, profile.totalDecodedRgbaBytes,
        profile.decodeWorkers, profile.decodeJobs, profile.decodeSuccesses,
        profile.decodeFailures, profile.maxConcurrentDecodes,
        profile.cacheHits, profile.cacheMisses);
}

}  // namespace

std::string ModelRenderable::authoredModelPath() const {
    return modelPath ? std::string(modelPath) : std::string{};
}

Result ModelRenderable::setAuthoredModelPath(std::string path) {
    if (path == authoredModelPath()) {
        return Result::success();
    }
    if (!renderTopologyMutable()) {
        return Result::failure(
            "Cannot change a model path while render resources are attached");
    }

    meshInstances_.clear();
    texturePathStorage_.clear();
    loadedModelAsset_.reset();
    modelPathStorage_ = std::move(path);
    modelPath = modelPathStorage_.empty() ? nullptr : modelPathStorage_.c_str();
    if (!modelPath) {
        return Result::success();
    }
    return loadModel();
}

std::string ModelRenderable::authoredTexturePath() const {
    return texturePath ? std::string(texturePath) : std::string{};
}

Result ModelRenderable::setAuthoredTexturePath(std::string path) {
    if (path == authoredTexturePath()) return Result::success();
    if (!renderTopologyMutable()) {
        return Result::failure(
            "Cannot change a texture path while render resources are attached");
    }
    meshInstances_.clear();
    texturePathStorage_.clear();
    loadedModelAsset_.reset();
    authoredTexturePathStorage_ = std::move(path);
    texturePath = authoredTexturePathStorage_.empty()
                      ? nullptr
                      : authoredTexturePathStorage_.c_str();
    return modelPath ? loadModel() : Result::success();
}

Result ModelRenderable::loadModel() {
    LoadModelProfile profile;
    if (!renderTopologyMutable()) {
        return Result::failure(
            "Cannot modify GameObject mesh topology while render resources are attached");
    }
    if (!meshInstances_.empty()) {
        return Result::failure(
            "GameObject already contains mesh instances; repeated model loading is not supported");
    }

    if (modelPath == nullptr) {
        spdlog::error("Model path is null. Cannot load model.");
        return Result::failure("Model path is null");
    }
    const std::string requestedModelPath(modelPath);
    const std::string requestedTexturePath =
        texturePath ? std::string(texturePath) : std::string{};
    spdlog::info("Loading game object model from path {}...", requestedModelPath);

    const ModelAssetCacheKey cacheKey =
        model_loading::makeModelAssetCacheKey(
            requestedModelPath.c_str(),
            requestedTexturePath.empty() ? nullptr : requestedTexturePath.c_str());
    const auto lookupStart = LoadModelProfile::Clock::now();
    const CacheLookupResult lookup = findCachedCpuModel(cacheKey);
    profile.cacheLookupTime = LoadModelProfile::Clock::now() - lookupStart;
    profile.cacheHits = lookup.hits;
    profile.cacheMisses = lookup.misses;

    const auto commitAsset = [&](const std::shared_ptr<const CachedCpuModel>& asset)
        -> Result {
        std::vector<MeshInstance> pendingMeshes;
        std::vector<std::string> pendingTexturePaths;
        const Result instantiation = instantiateCachedCpuModel(
            *asset, pendingMeshes, pendingTexturePaths, profile);
        if (!instantiation) {
            return instantiation;
        }

        const auto finalCommitStart = LoadModelProfile::Clock::now();
        const std::size_t originalTexturePathCount = texturePathStorage_.size();
        std::string originalModelPath = modelPathStorage_;
        const char* originalModelPathPointer = modelPath;
        std::string originalAuthoredTexturePath = authoredTexturePathStorage_;
        const char* originalTexturePathPointer = texturePath;
        try {
            for (std::string& path : pendingTexturePaths) {
                texturePathStorage_.push_back(std::move(path));
            }
            modelPathStorage_ = requestedModelPath;
            authoredTexturePathStorage_ = requestedTexturePath;
            const char* stableModelPath = modelPathStorage_.c_str();
            modelPath = stableModelPath;
            texturePath = authoredTexturePathStorage_.empty()
                              ? nullptr
                              : authoredTexturePathStorage_.c_str();
            for (std::size_t i = 0; i < pendingMeshes.size(); ++i) {
                pendingMeshes[i].material.texturePath =
                    texturePathStorage_[originalTexturePathCount + i].c_str();
                pendingMeshes[i].mesh.modelPath = stableModelPath;
            }
            meshInstances_.swap(pendingMeshes);
            loadedModelAsset_ = asset;
        } catch (const std::exception& exception) {
            texturePathStorage_.resize(originalTexturePathCount);
            modelPathStorage_.swap(originalModelPath);
            authoredTexturePathStorage_.swap(originalAuthoredTexturePath);
            modelPath = modelPathStorage_.empty()
                            ? originalModelPathPointer
                            : modelPathStorage_.c_str();
            texturePath = authoredTexturePathStorage_.empty()
                              ? originalTexturePathPointer
                              : authoredTexturePathStorage_.c_str();
            return Result::failure("Failed to commit meshes from model " +
                                   requestedModelPath + ": " +
                                   exception.what());
        } catch (...) {
            texturePathStorage_.resize(originalTexturePathCount);
            modelPathStorage_.swap(originalModelPath);
            authoredTexturePathStorage_.swap(originalAuthoredTexturePath);
            modelPath = modelPathStorage_.empty()
                            ? originalModelPathPointer
                            : modelPathStorage_.c_str();
            texturePath = authoredTexturePathStorage_.empty()
                              ? originalTexturePathPointer
                              : authoredTexturePathStorage_.c_str();
            return Result::failure("Failed to commit meshes from model " +
                                   requestedModelPath +
                                   ": unknown error");
        }
        profile.finalCommitTime =
            LoadModelProfile::Clock::now() - finalCommitStart;
        return Result::success();
    };

    if (lookup.asset) {
        profile.cacheHit = true;
        const Result result = commitAsset(lookup.asset);
        if (result) {
            logLoadSummary(profile, *lookup.asset);
            spdlog::info("Successfully loaded game object model");
        }
        return result;
    }

    Assimp::Importer importer;
    const aiScene* scene = nullptr;
    const auto sourceLoadingStart = LoadModelProfile::Clock::now();
    try {
        const auto assimpReadStart = LoadModelProfile::Clock::now();
        scene = importer.ReadFile(modelPath, aiProcess_Triangulate |
                                                 aiProcess_FlipUVs |
                                                 aiProcess_GenSmoothNormals |
                                                 aiProcess_CalcTangentSpace | aiProcess_PreTransformVertices | aiProcess_FindInvalidData);
        profile.assimpReadTime =
            LoadModelProfile::Clock::now() - assimpReadStart;
    } catch (const std::exception& exception) {
        return Result::failure("Failed to load model " +
                               std::string(modelPath) + ": " +
                               exception.what());
    } catch (...) {
        return Result::failure("Failed to load model " +
                               std::string(modelPath) +
                               ": unknown Assimp error");
    }
    if (!scene || scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE ||
        !scene->mRootNode || scene->mNumMeshes == 0 || !scene->mMeshes ||
        scene->mNumMaterials == 0 || !scene->mMaterials) {
        return Result::failure(
            "Failed to load model file " + std::string(modelPath) + ": " +
            (importer.GetErrorString() ? importer.GetErrorString()
                                       : "unknown Assimp error"));
    }

    try {
        auto loadedAsset = std::make_shared<CachedCpuModel>();
        loadedAsset->sourceMaterialCount = scene->mNumMaterials;
        loadedAsset->meshes.reserve(scene->mNumMeshes);
        std::vector<PendingImportedMaterial> materialTemplates(
            scene->mNumMaterials);
        TextureCache textureCache;
        textureCache.reserve(static_cast<std::size_t>(scene->mNumMaterials) *
                             3);
        std::vector<TextureDecodeJob> decodeJobs;
        decodeJobs.reserve(static_cast<std::size_t>(scene->mNumMaterials) * 3);
        std::unordered_set<TextureCacheKey, TextureCacheKeyHash> decodeKeys;
        decodeKeys.reserve(static_cast<std::size_t>(scene->mNumMaterials) * 3);
        std::vector<unsigned int> materialOrder;
        materialOrder.reserve(scene->mNumMaterials);

        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* mesh = scene->mMeshes[i];
            if (!mesh || mesh->mMaterialIndex >= scene->mNumMaterials) {
                return Result::failure(
                    "Model contains an invalid mesh material index");
            }
            if (mesh->mNumVertices == 0 || !mesh->mVertices) {
                return Result::failure("Mesh " +
                                       std::string(mesh->mName.C_Str()) +
                                       " has no vertices");
            }
            if (mesh->mNumFaces == 0 || !mesh->mFaces) {
                return Result::failure("Mesh " +
                                       std::string(mesh->mName.C_Str()) +
                                       " has no faces");
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

            PendingImportedMaterial& importedMaterial =
                materialTemplates[mesh->mMaterialIndex];
            if (!importedMaterial.discovered) {
                const Result discoveryResult = discoverImportedMaterial(
                    scene, material, mesh->mMaterialIndex, modelPath,
                    texturePath, mesh->mName.C_Str(), profile, importedMaterial,
                    decodeJobs, decodeKeys);
                if (!discoveryResult) {
                    return discoveryResult;
                }
                importedMaterial.discovered = true;
                materialOrder.push_back(mesh->mMaterialIndex);
            }
        }

        std::vector<TextureDecodeJobResult> decodeResults;
        const Result decodeResult =
            decodeTextureJobs(decodeJobs, decodeResults, profile);
        if (!decodeResult) {
            return decodeResult;
        }
        for (std::size_t jobIndex = 0; jobIndex < decodeJobs.size(); ++jobIndex) {
            gatherDecodeResult(decodeJobs[jobIndex], std::move(decodeResults[jobIndex]),
                               textureCache, profile);
        }
        for (unsigned int materialIndex : materialOrder) {
            const Result finalization = finalizeImportedMaterial(
                materialTemplates[materialIndex], textureCache, profile);
            if (!finalization) {
                return finalization;
            }
        }

        for (unsigned int i = 0; i < scene->mNumMeshes; ++i) {
            const aiMesh* mesh = scene->mMeshes[i];
            PendingImportedMaterial& importedMaterial =
                materialTemplates[mesh->mMaterialIndex];
            CachedCpuMesh cachedMesh;
            Material meshMaterial = importedMaterial.material;
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
            cachedMesh.indices.reserve(faceIndexCount);
            cachedMesh.vertices.reserve(mesh->mNumVertices);
            std::unordered_map<Vertex, uint32_t> uniqueVertices;
            uniqueVertices.reserve(mesh->mNumVertices);

            {
                PhaseTimer phaseTimer(profile.geometryConversionTime);
                for (unsigned int f = 0; f < mesh->mNumFaces; ++f) {
                    const aiFace& face = mesh->mFaces[f];
                    if (face.mNumIndices != 3 || !face.mIndices) {
                        return Result::failure(
                            "Mesh " + std::string(mesh->mName.C_Str()) +
                            " does not contain triangle-list faces");
                    }
                    for (unsigned int j = 0; j < face.mNumIndices; ++j) {
                        ++profile.totalFaceIndexCount;
                        const unsigned int index = face.mIndices[j];
                        if (index >= mesh->mNumVertices) {
                            return Result::failure(
                                "Mesh " + std::string(mesh->mName.C_Str()) +
                                " contains an out-of-range index");
                        }
                        Vertex vertex{};
                        vertex.pos = {mesh->mVertices[index].x,
                                      mesh->mVertices[index].y,
                                      mesh->mVertices[index].z};
                        if (!isFiniteVector(vertex.pos)) {
                            return Result::failure(
                                "Mesh " + std::string(mesh->mName.C_Str()) +
                                " contains a non-finite vertex position");
                        }
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
                            static_cast<uint32_t>(cachedMesh.vertices.size()));
                        if (inserted) {
                            cachedMesh.vertices.push_back(vertex);
                        }
                        cachedMesh.indices.push_back(vertexIt->second);
                    }
                }
            }
            profile.totalUniqueVertices += cachedMesh.vertices.size();
            if (!calculateCachedMeshBounds(cachedMesh)) {
                return Result::failure("Mesh " +
                                       std::string(mesh->mName.C_Str()) +
                                       " has invalid bounds");
            }

            if (meshMaterial.normalMapPixels) {
                if (meshHasUsableTangents) {
                    meshMaterial.normalMapEnabled = true;
                } else {
                    spdlog::warn("Mesh {} has a normal map but no usable tangent "
                                 "basis; normal mapping is disabled",
                                 mesh->mName.C_Str());
                    releaseStbiPixel(meshMaterial.normalMapPixelsOwner,
                                     meshMaterial.normalMapPixels);
                    meshMaterial.normalMapWidth = 0;
                    meshMaterial.normalMapHeight = 0;
                    meshMaterial.normalMapChannels = 0;
                    meshMaterial.normalMapMipLevels = 0;
                }
            } else if (!importedMaterial.normalMapError.empty()) {
                spdlog::warn("Failed to load normal map {} for mesh {}: {}; "
                             "using geometric normals",
                             meshMaterial.normalMapPath,
                             mesh->mName.C_Str(),
                             importedMaterial.normalMapError);
            }

            if (!importedMaterial.metallicRoughnessError.empty()) {
                spdlog::warn(
                    "Failed to load optional metallic-roughness texture {} "
                    "for mesh {}: {}; using scalar factors",
                    meshMaterial.metallicRoughnessMapPath,
                    mesh->mName.C_Str(),
                    importedMaterial.metallicRoughnessError);
            }

            cachedMesh.material = cacheMaterial(meshMaterial,
                                                importedMaterial.texturePath);
            loadedAsset->meshes.push_back(std::move(cachedMesh));
        }
        loadedAsset->sourceFaceIndexCount = profile.totalFaceIndexCount;
        loadedAsset->sourceUniqueVertexCount = profile.totalUniqueVertices;
        loadedAsset->sourceMaterialPreparations = profile.materialPreparations;
        profile.sourceLoadingTime =
            LoadModelProfile::Clock::now() - sourceLoadingStart;

        const Result result = commitAsset(loadedAsset);
        if (!result) {
            return result;
        }
        try {
            publishCachedCpuModel(cacheKey, loadedAsset);
        } catch (const std::exception& exception) {
            spdlog::warn("Model loaded without publishing a cache entry: {}",
                         exception.what());
        }
        logLoadSummary(profile, *loadedAsset);
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

Result ModelRenderable::addMeshInstance(MeshInstance&& meshInstance) {
    if (!renderTopologyMutable()) {
        return Result::failure(
            "Cannot modify GameObject mesh topology while render resources are attached");
    }
    const Result validationResult = validateIncomingMeshInstance(meshInstance);
    if (!validationResult) {
        return Result::failure("Cannot add MeshInstance: " +
                               validationResult.error());
    }
    try {
        calculateMeshBounds(meshInstance.mesh);
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

const std::vector<MeshInstance>& ModelRenderable::meshInstances() const noexcept {
    return meshInstances_;
}

Result ModelRenderable::markRenderResourcesAttached() {
    if (!renderTopologyMutable()) {
        return Result::failure("GameObject render resources are already attached");
    }
    renderTopologyState_ = RenderTopologyState::ResourcesAttached;
    return Result::success();
}

void ModelRenderable::markRenderResourcesDetached() noexcept {
    renderTopologyState_ = RenderTopologyState::Mutable;
}

bool ModelRenderable::renderTopologyMutable() const noexcept {
    return renderTopologyState_ == RenderTopologyState::Mutable;
}
