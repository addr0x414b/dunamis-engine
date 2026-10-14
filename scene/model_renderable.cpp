#include "model_renderable.h"
#include "loading_cache_key.h"

#include "../assets/model_asset.h"
#include "../assets/imported_model_asset.h"
#include "../assets/model_import_policy.h"
#include "../math/transform_math.h"

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
#include <cstring>
#include <exception>
#include <filesystem>
#include <iomanip>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/gtc/matrix_inverse.hpp>

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

struct ImportedModelAssetCache {
    std::mutex mutex;
    std::unordered_map<ModelAssetCacheKey,
                       std::weak_ptr<const model_loading::ImportedModelAsset>,
                       ModelAssetCacheKeyHash>
        entries;
    std::size_t hits = 0;
    std::size_t misses = 0;
};

ModelAssetCache& modelAssetCache() {
    static ModelAssetCache cache;
    return cache;
}

ImportedModelAssetCache& importedModelAssetCache() {
    static ImportedModelAssetCache cache;
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

std::filesystem::path normalizedTextureReferencePath(
    std::string_view textureReference) {
    std::string normalizedReference(textureReference);
    std::replace(normalizedReference.begin(), normalizedReference.end(), '\\',
                 '/');
    return std::filesystem::path(normalizedReference);
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
        const std::filesystem::path modelDirectory =
            std::filesystem::path(modelPath).parent_path();
        const std::filesystem::path normalizedReference =
            normalizedTextureReferencePath(textureReference);
        const std::filesystem::path originalPath =
            modelDirectory / normalizedReference;
        std::error_code existsError;
        if (std::filesystem::exists(originalPath, existsError)) {
            return makeExternalTextureSource(originalPath, originalPath.string());
        }

        const std::filesystem::path fallbackPath =
            modelDirectory / "textures" / normalizedReference.filename();
        spdlog::warn("Texture reference not found at {}; trying {}",
                     originalPath.string(), fallbackPath.string());
        existsError.clear();
        if (std::filesystem::exists(fallbackPath, existsError)) {
            spdlog::info("Resolved texture from fallback directory: {}",
                         fallbackPath.string());
            return makeExternalTextureSource(fallbackPath, fallbackPath.string());
        }

        TextureSource source =
            makeExternalTextureSource(originalPath, originalPath.string());
        source.valid = false;
        source.invalidReason =
            "texture was not found at original reference " +
            originalPath.string() + " or textures fallback " +
            fallbackPath.string();
        return source;
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
    const auto& vertices = mesh.vertexData();
    const auto& indices = mesh.indexData();

    if (!indices.empty() && vertices.empty()) {
        return Result::failure(
            "MeshInstance indexed geometry has no vertices");
    }
    if (!indices.empty() && indices.size() % 3 != 0) {
        return Result::failure(
            "MeshInstance index count is not a complete triangle list");
    }
    for (const Vertex& vertex : vertices) {
        if (!isFiniteVector(vertex.pos)) {
            return Result::failure(
                "MeshInstance contains a non-finite vertex position");
        }
    }
    for (uint32_t index : indices) {
        if (index >= vertices.size()) {
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

bool isFiniteMatrix(const glm::mat4& matrix) {
    return transform_math::isFiniteMatrix(matrix);
}

glm::mat4 convertAssimpMatrix(const aiMatrix4x4& input) {
    // aiMatrix4x4 transforms column vectors using row-major named fields;
    // GLM indexes matrix[column][row]. Assign explicitly to preserve both
    // translation placement and multiplication order.
    glm::mat4 output(1.0f);
    output[0][0] = input.a1; output[1][0] = input.a2;
    output[2][0] = input.a3; output[3][0] = input.a4;
    output[0][1] = input.b1; output[1][1] = input.b2;
    output[2][1] = input.b3; output[3][1] = input.b4;
    output[0][2] = input.c1; output[1][2] = input.c2;
    output[2][2] = input.c3; output[3][2] = input.c4;
    output[0][3] = input.d1; output[1][3] = input.d2;
    output[2][3] = input.d3; output[3][3] = input.d4;
    return output;
}

Result extractRigidWorldTransform(const glm::mat4& sourceWorld,
                                  glm::mat4& rigidWorld) {
    rigidWorld = glm::mat4(1.0f);
    if (!isFiniteMatrix(sourceWorld) ||
        std::fabs(sourceWorld[0][3]) > 1.0e-5f ||
        std::fabs(sourceWorld[1][3]) > 1.0e-5f ||
        std::fabs(sourceWorld[2][3]) > 1.0e-5f ||
        std::fabs(sourceWorld[3][3] - 1.0f) > 1.0e-5f) {
        return Result::failure(
            "Source node transform is non-finite or non-affine");
    }

    const glm::mat3 linear(sourceWorld);
    const float determinant = glm::determinant(linear);
    float maximum = 0.0f;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            maximum = std::max(maximum, std::fabs(linear[column][row]));
        }
    }
    const float tolerance =
        128.0f * std::numeric_limits<float>::epsilon() * maximum * maximum * maximum;
    if (!std::isfinite(determinant) || !std::isfinite(maximum) ||
        maximum <= 0.0f || std::fabs(determinant) <= tolerance) {
        return Result::failure(
            "Source node transform has a singular or unusable linear part");
    }

    const glm::vec3 first = linear[0];
    const glm::vec3 second = linear[1];
    glm::vec3 axisX;
    if (!normalizeVector(first, axisX)) {
        return Result::failure("Source node transform has an unusable X axis");
    }
    glm::vec3 axisY;
    if (!normalizeVector(second - glm::dot(second, axisX) * axisX, axisY)) {
        return Result::failure("Source node transform has an unusable Y axis");
    }
    glm::vec3 axisZ;
    if (!normalizeVector(glm::cross(axisX, axisY), axisZ)) {
        return Result::failure("Source node transform has an unusable Z axis");
    }

    const glm::mat3 rotation(axisX, axisY, axisZ);
    rigidWorld = glm::mat4(1.0f);
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            rigidWorld[column][row] = rotation[column][row];
        }
    }
    rigidWorld[3] = glm::vec4(glm::vec3(sourceWorld[3]), 1.0f);
    return isFiniteMatrix(rigidWorld)
        ? Result::success()
        : Result::failure("Source node rigid transform is non-finite");
}

glm::mat4 canonicalBakeMatrix(glm::mat4 matrix) {
    constexpr float tolerance = 1.0e-6f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float& value = matrix[column][row];
            if (std::fabs(value) <= tolerance) value = 0.0f;
            else if (std::fabs(value - 1.0f) <= tolerance) value = 1.0f;
            else if (std::fabs(value + 1.0f) <= tolerance) value = -1.0f;
        }
    }
    return matrix;
}

std::string bakeMatrixIdentity(const glm::mat4& matrix) {
    std::ostringstream identity;
    identity << std::hex << std::setfill('0');
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            std::uint32_t bits = 0;
            std::memcpy(&bits, &matrix[column][row], sizeof(bits));
            identity << std::setw(8) << bits;
        }
    }
    return identity.str();
}

bool calculateGeometryBounds(MeshGeometry& geometry) {
    geometry.bounds = {};
    bool found = false;
    for (const Vertex& vertex : geometry.vertices) {
        if (!isFiniteVector(vertex.pos)) continue;
        if (!found) {
            geometry.bounds.minimum = vertex.pos;
            geometry.bounds.maximum = vertex.pos;
            found = true;
        } else {
            geometry.bounds.minimum =
                glm::min(geometry.bounds.minimum, vertex.pos);
            geometry.bounds.maximum =
                glm::max(geometry.bounds.maximum, vertex.pos);
        }
    }
    geometry.bounds.valid = found;
    if (found) {
        constexpr std::uint64_t fnvOffset = 14695981039346656037ULL;
        constexpr std::uint64_t fnvPrime = 1099511628211ULL;
        std::uint64_t fingerprint = fnvOffset;
        const auto addBytes = [&fingerprint](const void* data,
                                             std::size_t size) {
            const auto* bytes = static_cast<const std::uint8_t*>(data);
            for (std::size_t index = 0; index < size; ++index) {
                fingerprint ^= bytes[index];
                fingerprint *= fnvPrime;
            }
        };
        const auto addValue = [&addBytes](const auto& value) {
            addBytes(&value, sizeof(value));
        };
        const std::uint64_t vertexCount = geometry.vertices.size();
        const std::uint64_t indexCount = geometry.indices.size();
        addValue(vertexCount);
        addValue(indexCount);
        for (const Vertex& vertex : geometry.vertices) {
            for (int axis = 0; axis < 3; ++axis) {
                const float coordinate =
                    vertex.pos[axis] == 0.0f ? 0.0f : vertex.pos[axis];
                std::uint32_t bits = 0;
                std::memcpy(&bits, &coordinate, sizeof(bits));
                addValue(bits);
            }
        }
        for (const std::uint32_t index : geometry.indices) addValue(index);
        geometry.collisionFingerprint = fingerprint == 0 ? 1 : fingerprint;
    }
    return found;
}

Result bakeImportedGeometry(const CachedCpuMesh& source,
                            const glm::mat4& bakeTransform,
                            const std::string& identity,
                            std::shared_ptr<const MeshGeometry>& output) {
    output.reset();
    if (!isFiniteMatrix(bakeTransform)) {
        return Result::failure("Derived mesh transform is not finite");
    }
    const glm::mat3 linear(bakeTransform);
    const float determinant = glm::determinant(linear);
    if (!std::isfinite(determinant) || std::fabs(determinant) <= 1.0e-12f) {
        return Result::failure("Derived mesh transform is singular");
    }
    const glm::mat3 normalMatrix = glm::transpose(glm::inverse(linear));
    if (!std::isfinite(glm::determinant(normalMatrix))) {
        return Result::failure("Derived mesh normal transform is invalid");
    }

    auto geometry = std::make_shared<MeshGeometry>();
    geometry->assetIdentity = identity;
    geometry->vertices.reserve(source.vertices.size());
    for (const Vertex& imported : source.vertices) {
        Vertex vertex = imported;
        vertex.pos = glm::vec3(bakeTransform * glm::vec4(imported.pos, 1.0f));
        if (!isFiniteVector(vertex.pos)) {
            return Result::failure(
                "Derived mesh contains a non-finite vertex position");
        }
        if (!normalizeVector(normalMatrix * imported.normal, vertex.normal)) {
            return Result::failure(
                "Derived mesh contains an unusable transformed normal");
        }
        const glm::vec3 sourceTangent(imported.tangent);
        glm::vec3 transformedTangent = linear * sourceTangent;
        transformedTangent -=
            glm::dot(transformedTangent, vertex.normal) * vertex.normal;
        if (glm::dot(sourceTangent, sourceTangent) > 1.0e-12f) {
            if (!normalizeVector(transformedTangent, transformedTangent)) {
                return Result::failure(
                    "Derived mesh contains an unusable transformed tangent");
            }
            const glm::vec3 sourceBitangent =
                imported.tangent.w * glm::cross(imported.normal, sourceTangent);
            const glm::vec3 transformedBitangent = linear * sourceBitangent;
            const float handedness =
                glm::dot(glm::cross(vertex.normal, transformedTangent),
                         transformedBitangent) < 0.0f
                    ? -1.0f
                    : 1.0f;
            vertex.tangent = glm::vec4(transformedTangent, handedness);
        }
        geometry->vertices.push_back(vertex);
    }

    geometry->indices = source.indices;
    if (determinant < 0.0f) {
        for (std::size_t index = 0; index + 2 < geometry->indices.size();
             index += 3) {
            std::swap(geometry->indices[index + 1],
                      geometry->indices[index + 2]);
        }
    }
    if (!calculateGeometryBounds(*geometry)) {
        return Result::failure("Derived mesh has no finite bounds");
    }
    output = std::move(geometry);
    return Result::success();
}

struct HierarchyNodeVisit {
    const aiNode* source = nullptr;
    std::size_t parentIndex = model_loading::ImportedModelNode::noParent;
    std::size_t siblingIndex = 0;
    std::size_t depth = 0;
    glm::mat4 parentWorld{1.0f};
    std::string identity;
};

struct HierarchyNodeMeshes {
    std::vector<unsigned int> sourceMeshIndices;
    glm::mat4 bakeTransform{1.0f};
};

struct ImportedBaseMesh {
    CachedCpuMesh geometry;
    bool usableTangents = false;
    Material material;
    std::string texturePath;
    std::size_t materialIndex = 0;
};

std::string stableNodeSegment(std::size_t siblingIndex,
                              const std::string& sourceName) {
    return "/" + std::to_string(siblingIndex) + ":" +
           std::to_string(sourceName.size()) + ":" + sourceName;
}

std::string sourceNodeName(const aiNode& node) {
    return node.mName.length == 0 ? std::string{} : node.mName.C_Str();
}

bool isAssimpGeneratedNodeName(const std::string& name) {
    constexpr char prefix[] = "nodes[";
    if (name.size() <= sizeof(prefix) ||
        name.compare(0, sizeof(prefix) - 1, prefix) != 0 ||
        name.back() != ']') {
        return false;
    }
    for (std::size_t index = sizeof(prefix) - 1;
         index + 1 < name.size(); ++index) {
        if (name[index] < '0' || name[index] > '9') return false;
    }
    return name.size() >= sizeof(prefix) + 1;
}

Result convertSourceMesh(const aiMesh& mesh, const std::string& meshName,
                         ImportedBaseMesh& output,
                         LoadModelProfile& profile) {
    if (mesh.mNumVertices == 0 || mesh.mVertices == nullptr) {
        return Result::failure("Mesh " + meshName + " has no vertices");
    }
    if (mesh.mNumFaces == 0 || mesh.mFaces == nullptr) {
        return Result::failure("Mesh " + meshName + " has no faces");
    }
    if (!mesh.HasNormals()) {
        return Result::failure(
            "Mesh " + meshName + " has no usable normals after import");
    }
    if (mesh.mNumBones != 0 || mesh.mNumAnimMeshes != 0) {
        return Result::failure(
            "Mesh " + meshName +
            " uses bones or morph targets, which the static importer does not support");
    }

    const bool hasTextureCoordinates = mesh.HasTextureCoords(0);
    const bool hasTangents = mesh.HasTangentsAndBitangents();
    bool meshHasUsableTangents = hasTangents;
    std::size_t faceIndexCount = 0;
    for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; ++faceIndex) {
        faceIndexCount += mesh.mFaces[faceIndex].mNumIndices;
    }
    output.geometry.indices.reserve(faceIndexCount);
    output.geometry.vertices.reserve(mesh.mNumVertices);
    std::unordered_map<Vertex, std::uint32_t> uniqueVertices;
    uniqueVertices.reserve(mesh.mNumVertices);

    for (unsigned int faceIndex = 0; faceIndex < mesh.mNumFaces; ++faceIndex) {
        const aiFace& face = mesh.mFaces[faceIndex];
        if (face.mNumIndices != 3 || face.mIndices == nullptr) {
            return Result::failure(
                "Mesh " + meshName +
                " does not contain valid triangle-list faces after triangulation");
        }
        for (unsigned int corner = 0; corner < face.mNumIndices; ++corner) {
            ++profile.totalFaceIndexCount;
            const unsigned int sourceIndex = face.mIndices[corner];
            if (sourceIndex >= mesh.mNumVertices) {
                return Result::failure(
                    "Mesh " + meshName + " contains an out-of-range index");
            }
            Vertex vertex{};
            vertex.pos = {mesh.mVertices[sourceIndex].x,
                          mesh.mVertices[sourceIndex].y,
                          mesh.mVertices[sourceIndex].z};
            if (!isFiniteVector(vertex.pos)) {
                return Result::failure(
                    "Mesh " + meshName + " contains a non-finite position");
            }
            vertex.texCoord = hasTextureCoordinates
                ? glm::vec2(mesh.mTextureCoords[0][sourceIndex].x,
                            mesh.mTextureCoords[0][sourceIndex].y)
                : glm::vec2(0.0f);
            if (!std::isfinite(vertex.texCoord.x) ||
                !std::isfinite(vertex.texCoord.y)) {
                return Result::failure(
                    "Mesh " + meshName + " contains non-finite UV coordinates");
            }
            vertex.color = glm::vec3(1.0f);
            if (!normalizeVector(
                    {mesh.mNormals[sourceIndex].x,
                     mesh.mNormals[sourceIndex].y,
                     mesh.mNormals[sourceIndex].z},
                    vertex.normal)) {
                return Result::failure(
                    "Mesh " + meshName + " contains an unusable normal");
            }
            if (hasTangents) {
                const glm::vec3 tangentInput(mesh.mTangents[sourceIndex].x,
                                             mesh.mTangents[sourceIndex].y,
                                             mesh.mTangents[sourceIndex].z);
                const glm::vec3 bitangentInput(
                    mesh.mBitangents[sourceIndex].x,
                    mesh.mBitangents[sourceIndex].y,
                    mesh.mBitangents[sourceIndex].z);
                const glm::vec3 orthogonal = tangentInput -
                    glm::dot(tangentInput, vertex.normal) * vertex.normal;
                glm::vec3 tangent;
                if (normalizeVector(orthogonal, tangent) &&
                    isFiniteVector(bitangentInput)) {
                    const float handedness =
                        glm::dot(glm::cross(vertex.normal, tangent),
                                 bitangentInput) < 0.0f
                            ? -1.0f
                            : 1.0f;
                    vertex.tangent = glm::vec4(tangent, handedness);
                } else {
                    meshHasUsableTangents = false;
                }
            } else {
                meshHasUsableTangents = false;
            }
            const auto [found, inserted] = uniqueVertices.emplace(
                vertex, static_cast<std::uint32_t>(
                            output.geometry.vertices.size()));
            if (inserted) output.geometry.vertices.push_back(vertex);
            output.geometry.indices.push_back(found->second);
        }
    }
    if (!calculateCachedMeshBounds(output.geometry)) {
        return Result::failure("Mesh " + meshName + " has invalid bounds");
    }
    profile.totalUniqueVertices += output.geometry.vertices.size();
    output.usableTangents = meshHasUsableTangents;
    return Result::success();
}

Result buildImportedModelAsset(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& textureFallbackPath,
    const ModelAssetCacheKey& cacheKey,
    std::shared_ptr<const model_loading::ImportedModelAsset>& output) {
    LoadModelProfile profile;
    output.reset();
    const std::string modelPath = sourcePath.lexically_normal().generic_string();
    const std::string fallbackPath =
        textureFallbackPath.lexically_normal().generic_string();
    Assimp::Importer importer;
    const aiScene* scene = nullptr;
    const auto assimpStart = LoadModelProfile::Clock::now();
    try {
        scene = importer.ReadFile(
            modelPath, model_loading::hierarchyPreservingImportFlags());
    } catch (const std::exception& exception) {
        return Result::failure("Assimp failed to read model '" + modelPath +
                               "': " + exception.what());
    } catch (...) {
        return Result::failure(
            "Assimp failed to read model '" + modelPath +
            "' with an unknown error");
    }
    profile.assimpReadTime = LoadModelProfile::Clock::now() - assimpStart;
    if (!scene || (scene->mFlags & AI_SCENE_FLAGS_INCOMPLETE) != 0 ||
        scene->mRootNode == nullptr || scene->mNumMeshes == 0 ||
        scene->mMeshes == nullptr || scene->mNumMaterials == 0 ||
        scene->mMaterials == nullptr) {
        const char* error = importer.GetErrorString();
        return Result::failure(
            "Failed to load hierarchical model '" + modelPath + "': " +
            (error != nullptr && error[0] != '\0' ? error
                                                   : "invalid Assimp scene"));
    }
    if (scene->mNumAnimations != 0) {
        return Result::failure(
            "Hierarchical static importing does not support animation channels; " +
            modelPath + " contains animation data");
    }

    try {
        auto asset = std::make_shared<model_loading::ImportedModelAsset>();
        asset->sourcePath = modelPath;
        asset->sourceIdentity = cacheKey.modelIdentity + "|hierarchy:" +
            std::to_string(model_loading::hierarchicalGeometryPolicyVersion);
        if (cacheKey.hasTextureFallbackOverride) {
            asset->sourceIdentity += "|fallback:" +
                std::to_string(cacheKey.textureFallbackIdentity.size()) + ":" +
                cacheKey.textureFallbackIdentity;
        }

        constexpr std::size_t maximumNodeCount = 100000;
        constexpr std::size_t maximumHierarchyDepth = 4096;
        constexpr std::size_t maximumMeshReferences = 1000000;
        std::vector<HierarchyNodeVisit> pendingNodes;
        pendingNodes.reserve(128);
        const std::string rootSourceName = sourceNodeName(*scene->mRootNode);
        HierarchyNodeVisit rootVisit;
        rootVisit.source = scene->mRootNode;
        rootVisit.identity = "root:" +
            std::to_string(rootSourceName.size()) + ":" + rootSourceName;
        pendingNodes.push_back(std::move(rootVisit));

        std::vector<HierarchyNodeMeshes> nodeMeshes;
        std::vector<glm::mat4> rigidWorlds;
        std::vector<bool> usedSourceMeshes(scene->mNumMeshes, false);
        std::unordered_set<const aiNode*> visitedNodes;
        visitedNodes.reserve(128);
        std::size_t totalMeshReferences = 0;

        while (!pendingNodes.empty()) {
            HierarchyNodeVisit visit = std::move(pendingNodes.back());
            pendingNodes.pop_back();
            if (visit.source == nullptr) {
                return Result::failure(
                    "Assimp hierarchy contains a null child node");
            }
            if (visit.depth > maximumHierarchyDepth) {
                return Result::failure(
                    "Assimp node hierarchy exceeds the supported depth limit");
            }
            if (asset->nodes.size() >= maximumNodeCount) {
                return Result::failure(
                    "Assimp node hierarchy exceeds the supported node limit");
            }
            if (!visitedNodes.insert(visit.source).second) {
                return Result::failure(
                    "Assimp node hierarchy contains a cycle or repeated node pointer");
            }
            if ((visit.source->mNumChildren != 0 &&
                 visit.source->mChildren == nullptr) ||
                (visit.source->mNumMeshes != 0 &&
                 visit.source->mMeshes == nullptr)) {
                return Result::failure(
                    "Assimp node has a nonzero child or mesh count with a null array");
            }

            const glm::mat4 local =
                convertAssimpMatrix(visit.source->mTransformation);
            if (!isFiniteMatrix(local)) {
                return Result::failure(
                    "Assimp node '" + visit.identity +
                    "' has a non-finite local transform");
            }
            const glm::mat4 sourceWorld = visit.parentWorld * local;
            glm::mat4 rigidWorld;
            Result rigidResult =
                extractRigidWorldTransform(sourceWorld, rigidWorld);
            if (!rigidResult) {
                return Result::failure("Assimp node '" + visit.identity +
                                       "': " + rigidResult.error());
            }

            glm::mat4 bake = glm::inverse(rigidWorld) * sourceWorld;
            if (!isFiniteMatrix(bake)) {
                return Result::failure(
                    "Assimp node '" + visit.identity +
                    "' produced a non-finite derived mesh transform");
            }
            bake = canonicalBakeMatrix(bake);

            transform_math::DecomposedTransform localRigid;
            glm::mat4 relativeRigid = rigidWorld;
            if (visit.parentIndex !=
                model_loading::ImportedModelNode::noParent) {
                relativeRigid =
                    glm::inverse(rigidWorlds[visit.parentIndex]) * rigidWorld;
            }
            const Result decomposition = transform_math::decomposeModelMatrix(
                relativeRigid, localRigid);
            if (!decomposition ||
                std::fabs(localRigid.scale.x - 1.0f) > 1.0e-3f ||
                std::fabs(localRigid.scale.y - 1.0f) > 1.0e-3f ||
                std::fabs(localRigid.scale.z - 1.0f) > 1.0e-3f) {
                return Result::failure(
                    "Assimp node '" + visit.identity +
                    "' has a rigid transform Dunamis cannot represent");
            }

            const std::size_t nodeIndex = asset->nodes.size();
            const std::string importedName = sourceNodeName(*visit.source);
            model_loading::ImportedModelNode node;
            node.identity = visit.identity;
            node.nameWasEmpty = importedName.empty() ||
                                isAssimpGeneratedNodeName(importedName);
            node.name = node.nameWasEmpty
                ? "Node_" + std::to_string(nodeIndex)
                : importedName;
            node.parentIndex = visit.parentIndex;
            node.position = localRigid.position;
            node.rotation = localRigid.rotation;
            if (!transform_math::isFiniteVector(node.position) ||
                !transform_math::isFiniteVector(node.rotation)) {
                return Result::failure(
                    "Assimp node '" + visit.identity +
                    "' produced a non-finite authored transform");
            }
            asset->nodeByIdentity.emplace(node.identity, nodeIndex);
            asset->nodes.push_back(std::move(node));
            if (visit.parentIndex !=
                model_loading::ImportedModelNode::noParent) {
                asset->nodes[visit.parentIndex].children.push_back(nodeIndex);
            }
            HierarchyNodeMeshes nodeData;
            nodeData.bakeTransform = bake;
            nodeData.sourceMeshIndices.reserve(visit.source->mNumMeshes);
            for (unsigned int meshIndex = 0;
                 meshIndex < visit.source->mNumMeshes; ++meshIndex) {
                const unsigned int sourceMeshIndex =
                    visit.source->mMeshes[meshIndex];
                if (sourceMeshIndex >= scene->mNumMeshes) {
                    return Result::failure(
                        "Assimp node '" + visit.identity +
                        "' references an out-of-range source mesh index");
                }
                if (++totalMeshReferences > maximumMeshReferences) {
                    return Result::failure(
                        "Assimp hierarchy exceeds the supported mesh-reference limit");
                }
                usedSourceMeshes[sourceMeshIndex] = true;
                nodeData.sourceMeshIndices.push_back(sourceMeshIndex);
            }
            nodeMeshes.push_back(std::move(nodeData));
            rigidWorlds.push_back(rigidWorld);

            for (std::size_t childOffset = visit.source->mNumChildren;
                 childOffset > 0; --childOffset) {
                const std::size_t childIndex = childOffset - 1;
                const aiNode* child = visit.source->mChildren[childIndex];
                if (child == nullptr) {
                    return Result::failure(
                        "Assimp node '" + visit.identity +
                        "' contains a null child pointer");
                }
                HierarchyNodeVisit childVisit;
                childVisit.source = child;
                childVisit.parentIndex = nodeIndex;
                childVisit.siblingIndex = childIndex;
                childVisit.depth = visit.depth + 1;
                childVisit.parentWorld = sourceWorld;
                childVisit.identity = visit.identity + stableNodeSegment(
                    childIndex, sourceNodeName(*child));
                pendingNodes.push_back(std::move(childVisit));
            }
        }

        if (totalMeshReferences == 0) {
            return Result::failure(
                "Assimp model contains no mesh references in its node hierarchy");
        }
        asset->meshReferences = totalMeshReferences;
        asset->uniqueSourceMeshes = static_cast<std::size_t>(std::count(
            usedSourceMeshes.begin(), usedSourceMeshes.end(), true));

        std::vector<PendingImportedMaterial> materialTemplates(
            scene->mNumMaterials);
        std::vector<unsigned int> materialOrder;
        materialOrder.reserve(scene->mNumMaterials);
        std::vector<TextureDecodeJob> decodeJobs;
        decodeJobs.reserve(static_cast<std::size_t>(scene->mNumMaterials) * 3);
        std::unordered_set<TextureCacheKey, TextureCacheKeyHash> decodeKeys;
        decodeKeys.reserve(static_cast<std::size_t>(scene->mNumMaterials) * 3);
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes;
             ++meshIndex) {
            if (!usedSourceMeshes[meshIndex]) continue;
            const aiMesh* mesh = scene->mMeshes[meshIndex];
            if (mesh == nullptr) {
                return Result::failure(
                    "Assimp scene contains a null referenced mesh");
            }
            if (mesh->mMaterialIndex >= scene->mNumMaterials ||
                scene->mMaterials[mesh->mMaterialIndex] == nullptr) {
                return Result::failure(
                    "Mesh " + std::string(mesh->mName.C_Str()) +
                    " references an invalid material");
            }
            PendingImportedMaterial& importedMaterial =
                materialTemplates[mesh->mMaterialIndex];
            if (!importedMaterial.discovered) {
                const std::string firstMeshName = mesh->mName.length == 0
                    ? "Mesh_" + std::to_string(meshIndex)
                    : std::string(mesh->mName.C_Str());
                const Result discovery = discoverImportedMaterial(
                    scene, scene->mMaterials[mesh->mMaterialIndex],
                    mesh->mMaterialIndex, modelPath.c_str(),
                    fallbackPath.empty() ? nullptr : fallbackPath.c_str(),
                    firstMeshName.c_str(),
                    profile, importedMaterial, decodeJobs, decodeKeys);
                if (!discovery) return discovery;
                importedMaterial.discovered = true;
                materialOrder.push_back(mesh->mMaterialIndex);
            }
        }

        TextureCache textureCache;
        textureCache.reserve(decodeJobs.size());
        std::vector<TextureDecodeJobResult> decodeResults;
        const Result decoded =
            decodeTextureJobs(decodeJobs, decodeResults, profile);
        if (!decoded) return decoded;
        for (std::size_t index = 0; index < decodeJobs.size(); ++index) {
            gatherDecodeResult(decodeJobs[index], std::move(decodeResults[index]),
                               textureCache, profile);
        }
        for (const unsigned int materialIndex : materialOrder) {
            const Result finalized = finalizeImportedMaterial(
                materialTemplates[materialIndex], textureCache, profile);
            if (!finalized) return finalized;
        }

        std::vector<std::unique_ptr<ImportedBaseMesh>> baseMeshes(
            scene->mNumMeshes);
        for (unsigned int meshIndex = 0; meshIndex < scene->mNumMeshes;
             ++meshIndex) {
            if (!usedSourceMeshes[meshIndex]) continue;
            const aiMesh& sourceMesh = *scene->mMeshes[meshIndex];
            const std::string meshName = sourceMesh.mName.length == 0
                ? "Mesh_" + std::to_string(meshIndex)
                : sourceMesh.mName.C_Str();
            auto baseMesh = std::make_unique<ImportedBaseMesh>();
            Result converted = convertSourceMesh(
                sourceMesh, meshName, *baseMesh, profile);
            if (!converted) return converted;
            PendingImportedMaterial& importedMaterial =
                materialTemplates[sourceMesh.mMaterialIndex];
            recordTextureReference(
                profile, TextureUsage::BaseColor,
                importedMaterial.baseColorIntendedSourceIdentity);
            if (importedMaterial.baseColorSourceIdentity !=
                importedMaterial.baseColorIntendedSourceIdentity) {
                recordTextureReference(
                    profile, TextureUsage::BaseColor,
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
            baseMesh->material = importedMaterial.material;
            if (baseMesh->material.normalMapPixels) {
                if (baseMesh->usableTangents) {
                    baseMesh->material.normalMapEnabled = true;
                } else {
                    releaseStbiPixel(baseMesh->material.normalMapPixelsOwner,
                                     baseMesh->material.normalMapPixels);
                    baseMesh->material.normalMapWidth = 0;
                    baseMesh->material.normalMapHeight = 0;
                    baseMesh->material.normalMapChannels = 0;
                    baseMesh->material.normalMapMipLevels = 0;
                }
            } else if (!importedMaterial.normalMapError.empty()) {
                spdlog::warn(
                    "Failed to load optional normal map for imported mesh {}: {}",
                    meshName, importedMaterial.normalMapError);
            }
            if (!importedMaterial.metallicRoughnessError.empty()) {
                spdlog::warn(
                    "Failed to load optional metallic-roughness map for imported mesh {}: {}",
                    meshName, importedMaterial.metallicRoughnessError);
            }
            baseMesh->texturePath = importedMaterial.texturePath;
            baseMesh->materialIndex = sourceMesh.mMaterialIndex;
            baseMeshes[meshIndex] = std::move(baseMesh);
        }

        std::unordered_map<std::string,
                           std::shared_ptr<const model_loading::ImportedMeshAsset>>
            derivedMeshCache;
        derivedMeshCache.reserve(totalMeshReferences);
        for (std::size_t nodeIndex = 0; nodeIndex < nodeMeshes.size();
             ++nodeIndex) {
            const HierarchyNodeMeshes& sourceNodeMeshes = nodeMeshes[nodeIndex];
            auto& destinationMeshes = asset->nodes[nodeIndex].meshes;
            destinationMeshes.reserve(sourceNodeMeshes.sourceMeshIndices.size());
            for (const unsigned int sourceMeshIndex :
                 sourceNodeMeshes.sourceMeshIndices) {
                if (sourceMeshIndex >= baseMeshes.size() ||
                    !baseMeshes[sourceMeshIndex]) {
                    return Result::failure(
                        "Imported node lost a referenced source mesh");
                }
                const std::string geometryIdentity =
                    asset->sourceIdentity + "|mesh:" +
                    std::to_string(sourceMeshIndex) + "|bake:" +
                    bakeMatrixIdentity(sourceNodeMeshes.bakeTransform);
                const auto cached = derivedMeshCache.find(geometryIdentity);
                if (cached != derivedMeshCache.end()) {
                    destinationMeshes.push_back(cached->second);
                    continue;
                }

                auto importedMesh =
                    std::make_shared<model_loading::ImportedMeshAsset>();
                Result baked = bakeImportedGeometry(
                    baseMeshes[sourceMeshIndex]->geometry,
                    sourceNodeMeshes.bakeTransform, geometryIdentity,
                    importedMesh->geometry);
                if (!baked) {
                    return Result::failure(
                        "Failed to normalize source mesh " +
                        std::to_string(sourceMeshIndex) + ": " + baked.error());
                }
                importedMesh->material =
                    baseMeshes[sourceMeshIndex]->material;
                importedMesh->texturePath =
                    baseMeshes[sourceMeshIndex]->texturePath;
                importedMesh->sourceMeshIndex = sourceMeshIndex;
                importedMesh->sourceMaterialIndex =
                    baseMeshes[sourceMeshIndex]->materialIndex;
                importedMesh->assetIdentity = geometryIdentity + "|material:" +
                    std::to_string(importedMesh->sourceMaterialIndex);
                const std::shared_ptr<const model_loading::ImportedMeshAsset>
                    immutableMesh = std::move(importedMesh);
                derivedMeshCache.emplace(geometryIdentity, immutableMesh);
                destinationMeshes.push_back(immutableMesh);
                ++asset->uniqueGeometryVariants;
            }
        }

        spdlog::info(
            "Hierarchical Assimp import '{}': read={:.3f}ms nodes={} "
            "uniqueMeshes={} meshReferences={} geometryVariants={} textures={}",
            modelPath, milliseconds(profile.assimpReadTime), asset->nodes.size(),
            asset->uniqueSourceMeshes, asset->meshReferences,
            asset->uniqueGeometryVariants,
            profile.baseColor.uniqueSources.size() +
                profile.normalMap.uniqueSources.size() +
                profile.metallicRoughness.uniqueSources.size());
        output = std::move(asset);
        return Result::success();
    } catch (const std::exception& exception) {
        return Result::failure("Failed to import model hierarchy '" + modelPath +
                               "': " + exception.what());
    } catch (...) {
        return Result::failure(
            "Failed to import model hierarchy '" + modelPath +
            "': unknown error");
    }
}

}  // namespace

namespace model_loading {

static Result loadImportedModelAssetImpl(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& textureFallbackPath,
    std::shared_ptr<const ImportedModelAsset>& output) {
    output.reset();
    if (sourcePath.empty()) {
        return Result::failure("Hierarchical model source path is empty");
    }
    const std::string sourceString = sourcePath.lexically_normal().generic_string();
    const std::string fallbackString =
        textureFallbackPath.lexically_normal().generic_string();
    const ModelAssetCacheKey key = makeModelAssetCacheKey(
        sourceString.c_str(),
        fallbackString.empty() ? nullptr : fallbackString.c_str());

    ImportedModelAssetCache& cache = importedModelAssetCache();
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        const auto found = cache.entries.find(key);
        if (found != cache.entries.end()) {
            output = found->second.lock();
            if (output) {
                ++cache.hits;
                spdlog::debug("Hierarchical CPU asset cache HIT: {}",
                              key.modelIdentity);
                return Result::success();
            }
            cache.entries.erase(found);
        }
        ++cache.misses;
    }

    std::shared_ptr<const ImportedModelAsset> loaded;
    const Result result = buildImportedModelAsset(
        sourcePath, textureFallbackPath, key, loaded);
    if (!result) return result;
    if (!loaded) {
        return Result::failure(
            "Hierarchical model import completed without an asset");
    }

    try {
        std::lock_guard<std::mutex> lock(cache.mutex);
        auto& entry = cache.entries[key];
        if (std::shared_ptr<const ImportedModelAsset> concurrent = entry.lock()) {
            output = std::move(concurrent);
        } else {
            entry = loaded;
            output = std::move(loaded);
        }
    } catch (const std::exception& exception) {
        spdlog::warn(
            "Hierarchical model loaded without publishing a CPU cache entry: {}",
            exception.what());
        output = std::move(loaded);
    } catch (...) {
        spdlog::warn(
            "Hierarchical model loaded without publishing a CPU cache entry");
        output = std::move(loaded);
    }
    return Result::success();
}

Result loadImportedModelAsset(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& textureFallbackPath,
    std::shared_ptr<const ImportedModelAsset>& output) {
    try {
        return loadImportedModelAssetImpl(sourcePath, textureFallbackPath,
                                          output);
    } catch (const std::exception& exception) {
        output.reset();
        return Result::failure(
            "Failed to resolve hierarchical model asset '" +
            sourcePath.string() + "': " + exception.what());
    } catch (...) {
        output.reset();
        return Result::failure(
            "Failed to resolve hierarchical model asset: unknown error");
    }
}

}  // namespace model_loading

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
    importedAssetReference_ = {};
    importedModelAsset_.reset();
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
    importedAssetReference_ = {};
    importedModelAsset_.reset();
    authoredTexturePathStorage_ = std::move(path);
    texturePath = authoredTexturePathStorage_.empty()
                      ? nullptr
                      : authoredTexturePathStorage_.c_str();
    return modelPath ? loadModel() : Result::success();
}

ImportedAssetReference ModelRenderable::importedAssetReference() const {
    return importedAssetReference_;
}

Result ModelRenderable::setImportedAssetReference(
    ImportedAssetReference reference) {
    if (!renderTopologyMutable()) {
        return Result::failure(
            "Cannot change an imported asset reference while render resources are attached");
    }
    if (reference == importedAssetReference_ &&
        (reference.empty() || importedModelAsset_ != nullptr)) {
        return Result::success();
    }

    if (reference.empty()) {
        if (importedAssetReference_.empty()) return Result::success();
        meshInstances_.clear();
        importedAssetReference_ = {};
        importedModelAsset_.reset();
        importedNodeIndex_ = 0;
        return Result::success();
    }
    if (reference.sourcePath.empty() || reference.nodeIdentity.empty()) {
        return Result::failure(
            "Imported asset reference requires both a source path and node identity");
    }

    std::shared_ptr<const model_loading::ImportedModelAsset> asset;
    const Result loaded = model_loading::loadImportedModelAsset(
        reference.sourcePath, reference.textureFallbackPath, asset);
    if (!loaded) {
        return Result::failure("Failed to resolve imported asset '" +
                               reference.sourcePath + "': " + loaded.error());
    }
    const auto node = asset->nodeByIdentity.find(reference.nodeIdentity);
    if (node == asset->nodeByIdentity.end() ||
        node->second >= asset->nodes.size()) {
        return Result::failure(
            "Imported asset '" + reference.sourcePath +
            "' no longer contains source node '" + reference.nodeIdentity + "'");
    }

    std::vector<MeshInstance> pendingMeshes;
    try {
        const model_loading::ImportedModelNode& importedNode =
            asset->nodes[node->second];
        pendingMeshes.reserve(importedNode.meshes.size());
        for (const auto& importedMesh : importedNode.meshes) {
            if (!importedMesh || !importedMesh->geometry ||
                importedMesh->geometry->vertices.empty() ||
                importedMesh->geometry->indices.empty()) {
                return Result::failure(
                    "Imported source node references invalid mesh geometry");
            }
            MeshInstance instance{};
            instance.mesh.sharedGeometry = importedMesh->geometry;
            instance.mesh.bounds = importedMesh->geometry->bounds;
            instance.mesh.assetIdentity = importedMesh->assetIdentity;
            instance.mesh.modelPath = asset->sourcePath.c_str();
            instance.material = importedMesh->material;
            instance.material.texturePath = importedMesh->texturePath.empty()
                ? nullptr
                : importedMesh->texturePath.c_str();
            const Result validation = validateIncomingMeshInstance(instance);
            if (!validation) {
                return Result::failure(
                    "Imported source node contains an invalid mesh: " +
                    validation.error());
            }
            pendingMeshes.push_back(std::move(instance));
        }
    } catch (const std::exception& exception) {
        return Result::failure("Failed to prepare imported mesh references: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to prepare imported mesh references: unknown error");
    }

    meshInstances_.swap(pendingMeshes);
    texturePathStorage_.clear();
    loadedModelAsset_.reset();
    modelPath = nullptr;
    texturePath = nullptr;
    modelPathStorage_.clear();
    authoredTexturePathStorage_.clear();
    importedAssetReference_ = std::move(reference);
    importedModelAsset_ = std::move(asset);
    importedNodeIndex_ = node->second;
    return Result::success();
}

std::optional<model_loading::ModelAssetCacheKey>
ModelRenderable::gpuModelCacheKey() const {
    if (loadedModelAsset_) {
        return model_loading::makeModelAssetCacheKey(modelPath, texturePath);
    }
    if (!importedModelAsset_ || meshInstances_.empty()) {
        return std::nullopt;
    }

    model_loading::ModelAssetCacheKey key;
    key.modelIdentity = importedModelAsset_->sourceIdentity + "|subset:";
    for (const MeshInstance& instance : meshInstances_) {
        const std::string& identity = instance.mesh.assetIdentity;
        key.modelIdentity += std::to_string(identity.size());
        key.modelIdentity.push_back(':');
        key.modelIdentity += identity;
    }
    key.hasTextureFallbackOverride =
        !importedAssetReference_.textureFallbackPath.empty();
    if (key.hasTextureFallbackOverride) {
        key.textureFallbackIdentity =
            model_loading::normalizedFilesystemIdentity(
                importedAssetReference_.textureFallbackPath);
    }
    return key;
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

    // Imported geometry policy is fixed globally for this process, and this
    // cache is in-process only. There cannot be a pre-normalization asset in
    // the cache alongside the current policy, so the existing model identity
    // and texture-fallback key remains sufficient.
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
        // This is the asset-to-meter boundary. GlobalScale consumes Assimp's
        // format-specific source scale (FBX UnitScaleFactor in the pinned
        // implementation), and it runs before PreTransformVertices. The
        // resulting Vertex::pos values are meter-valued exactly once; render,
        // picking, and physics all consume this same CPU geometry.
        scene = importer.ReadFile(
            modelPath, model_loading::meterNormalizedImportFlags());
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

namespace {

std::size_t saturatedBytesAdd(std::size_t total,
                              std::size_t value) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() - total) {
        return std::numeric_limits<std::size_t>::max();
    }
    return total + value;
}

std::size_t saturatedBytesMultiply(std::size_t count,
                                   std::size_t elementSize) noexcept {
    if (count != 0 &&
        elementSize > std::numeric_limits<std::size_t>::max() / count) {
        return std::numeric_limits<std::size_t>::max();
    }
    return count * elementSize;
}

std::size_t pixelStorageBytes(int width, int height, int channels) noexcept {
    if (width <= 0 || height <= 0 || channels <= 0) return 0;
    const std::size_t w = static_cast<std::size_t>(width);
    const std::size_t h = static_cast<std::size_t>(height);
    const std::size_t c = static_cast<std::size_t>(channels);
    if (h > std::numeric_limits<std::size_t>::max() / w ||
        c > std::numeric_limits<std::size_t>::max() / (w * h)) {
        return std::numeric_limits<std::size_t>::max();
    }
    return w * h * c;
}

std::size_t estimateCachedModelBytes(
    const model_loading::CachedCpuModel& model) {
    std::size_t bytes = sizeof(model_loading::CachedCpuModel);
    bytes = saturatedBytesAdd(
        bytes, saturatedBytesMultiply(
                   model.meshes.capacity(),
                   sizeof(model_loading::CachedCpuMesh)));
    std::unordered_set<const void*> pixelOwners;
    for (const model_loading::CachedCpuMesh& mesh : model.meshes) {
        bytes = saturatedBytesAdd(
            bytes, saturatedBytesMultiply(mesh.vertices.capacity(),
                                          sizeof(Vertex)));
        bytes = saturatedBytesAdd(
            bytes, saturatedBytesMultiply(mesh.indices.capacity(),
                                          sizeof(std::uint32_t)));
        const auto& material = mesh.material;
        bytes = saturatedBytesAdd(bytes, material.texturePath.capacity());
        bytes = saturatedBytesAdd(bytes, material.normalMapPath.capacity());
        bytes = saturatedBytesAdd(
            bytes, material.metallicRoughnessMapPath.capacity());
        const auto addPixels = [&bytes, &pixelOwners](
                                  const StbiPixelOwner& owner, int width,
                                  int height, int channels) {
            if (owner && pixelOwners.insert(owner.get()).second) {
                bytes = saturatedBytesAdd(
                    bytes, pixelStorageBytes(width, height, channels));
            }
        };
        addPixels(material.pixelsOwner, material.texWidth,
                  material.texHeight, material.texChannels);
        addPixels(material.normalMapPixelsOwner, material.normalMapWidth,
                  material.normalMapHeight, material.normalMapChannels);
        addPixels(material.metallicRoughnessMapPixelsOwner,
                  material.metallicRoughnessMapWidth,
                  material.metallicRoughnessMapHeight,
                  material.metallicRoughnessMapChannels);
    }
    return bytes;
}

std::size_t estimateImportedModelBytes(
    const model_loading::ImportedModelAsset& model) {
    std::size_t bytes = sizeof(model_loading::ImportedModelAsset);
    bytes = saturatedBytesAdd(bytes, model.sourcePath.capacity());
    bytes = saturatedBytesAdd(bytes, model.sourceIdentity.capacity());
        bytes = saturatedBytesAdd(
            bytes, saturatedBytesMultiply(
                       model.nodes.capacity(),
                       sizeof(model_loading::ImportedModelNode)));
    bytes = saturatedBytesAdd(
        bytes, model.nodeByIdentity.bucket_count() * sizeof(void*));
    std::unordered_set<const void*> pixelOwners;
    std::unordered_set<const MeshGeometry*> geometries;
    for (const model_loading::ImportedModelNode& node : model.nodes) {
        bytes = saturatedBytesAdd(bytes, node.identity.capacity());
        bytes = saturatedBytesAdd(bytes, node.name.capacity());
        bytes = saturatedBytesAdd(
            bytes, node.children.capacity() * sizeof(std::size_t));
        bytes = saturatedBytesAdd(
            bytes, node.meshes.capacity() *
                       sizeof(std::shared_ptr<const model_loading::ImportedMeshAsset>));
        for (const auto& mesh : node.meshes) {
            if (!mesh) continue;
            bytes = saturatedBytesAdd(bytes, sizeof(*mesh));
            bytes = saturatedBytesAdd(bytes, mesh->texturePath.capacity());
            bytes = saturatedBytesAdd(bytes, mesh->assetIdentity.capacity());
            if (mesh->geometry && geometries.insert(mesh->geometry.get()).second) {
                bytes = saturatedBytesAdd(bytes, sizeof(MeshGeometry));
            bytes = saturatedBytesAdd(
                bytes, saturatedBytesMultiply(
                           mesh->geometry->vertices.capacity(), sizeof(Vertex)));
                bytes = saturatedBytesAdd(
                bytes, saturatedBytesMultiply(
                           mesh->geometry->indices.capacity(),
                           sizeof(std::uint32_t)));
                bytes = saturatedBytesAdd(
                    bytes, mesh->geometry->assetIdentity.capacity());
            }
            const Material& material = mesh->material;
            bytes = saturatedBytesAdd(bytes, material.normalMapPath.capacity());
            bytes = saturatedBytesAdd(
                bytes, material.metallicRoughnessMapPath.capacity());
            const auto addPixels = [&bytes, &pixelOwners](
                                      const StbiPixelOwner& owner, int width,
                                      int height, int channels) {
                if (owner && pixelOwners.insert(owner.get()).second) {
                    bytes = saturatedBytesAdd(
                        bytes, pixelStorageBytes(width, height, channels));
                }
            };
            addPixels(material.pixelsOwner, material.texWidth,
                      material.texHeight, material.texChannels);
            addPixels(material.normalMapPixelsOwner,
                      material.normalMapWidth, material.normalMapHeight,
                      material.normalMapChannels);
            addPixels(material.metallicRoughnessMapPixelsOwner,
                      material.metallicRoughnessMapWidth,
                      material.metallicRoughnessMapHeight,
                      material.metallicRoughnessMapChannels);
        }
    }
    return bytes;
}

}  // namespace

std::size_t ModelRenderable::estimatedPrivateCpuMemoryBytes() const noexcept {
    std::size_t bytes = sizeof(ModelRenderable);
    bytes = saturatedBytesAdd(bytes, modelPathStorage_.capacity());
    bytes = saturatedBytesAdd(bytes, authoredTexturePathStorage_.capacity());
    bytes = saturatedBytesAdd(
        bytes, importedAssetReference_.sourcePath.capacity());
    bytes = saturatedBytesAdd(
        bytes, importedAssetReference_.nodeIdentity.capacity());
    bytes = saturatedBytesAdd(
        bytes, importedAssetReference_.textureFallbackPath.capacity());
    bytes = saturatedBytesAdd(bytes, modelPathStorage_.size());
    bytes = saturatedBytesAdd(
        bytes, saturatedBytesMultiply(texturePathStorage_.size(),
                                      sizeof(std::string)));
    for (const std::string& path : texturePathStorage_) {
        bytes = saturatedBytesAdd(bytes, path.capacity());
    }
    bytes = saturatedBytesAdd(
        bytes, saturatedBytesMultiply(meshInstances_.capacity(),
                                      sizeof(MeshInstance)));
    for (const MeshInstance& instance : meshInstances_) {
        bytes = saturatedBytesAdd(bytes, instance.mesh.assetIdentity.capacity());
        if (!instance.mesh.sharedGeometry) {
            bytes = saturatedBytesAdd(
                bytes, saturatedBytesMultiply(
                           instance.mesh.vertices.capacity(), sizeof(Vertex)));
            bytes = saturatedBytesAdd(
                bytes, saturatedBytesMultiply(
                           instance.mesh.indices.capacity(),
                           sizeof(std::uint32_t)));
        }
        const Material& material = instance.material;
        bytes = saturatedBytesAdd(bytes, material.normalMapPath.capacity());
        bytes = saturatedBytesAdd(
            bytes, material.metallicRoughnessMapPath.capacity());
        if (material.pixelsOwner.use_count() == 1) {
            bytes = saturatedBytesAdd(
                bytes, pixelStorageBytes(material.texWidth, material.texHeight,
                                         material.texChannels));
        }
        if (material.normalMapPixelsOwner.use_count() == 1) {
            bytes = saturatedBytesAdd(
                bytes, pixelStorageBytes(material.normalMapWidth,
                                         material.normalMapHeight,
                                         material.normalMapChannels));
        }
        if (material.metallicRoughnessMapPixelsOwner.use_count() == 1) {
            bytes = saturatedBytesAdd(
                bytes, pixelStorageBytes(
                           material.metallicRoughnessMapWidth,
                           material.metallicRoughnessMapHeight,
                           material.metallicRoughnessMapChannels));
        }
    }
    return bytes;
}

void ModelRenderable::appendSharedCpuMemoryEstimates(
    std::vector<SharedCpuMemoryEstimate>& estimates) const {
    const auto append = [&estimates](const void* identity,
                                     std::size_t referenceCount,
                                     auto&& estimateBytes) {
        const bool alreadyEstimated = std::any_of(
            estimates.begin(), estimates.end(),
            [identity](const SharedCpuMemoryEstimate& estimate) {
                return estimate.identity == identity && estimate.bytes != 0;
            });
        estimates.push_back({identity,
                             alreadyEstimated ? 0 : estimateBytes(),
                             referenceCount});
    };
    if (loadedModelAsset_) {
        append(loadedModelAsset_.get(),
               static_cast<std::size_t>(loadedModelAsset_.use_count()),
               [this] {
                   return estimateCachedModelBytes(*loadedModelAsset_);
               });
    }
    if (importedModelAsset_) {
        append(importedModelAsset_.get(),
               static_cast<std::size_t>(importedModelAsset_.use_count()),
               [this] {
                   return estimateImportedModelBytes(*importedModelAsset_);
               });
    }
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
