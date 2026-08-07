#ifndef LOADING_CACHE_KEY_H
#define LOADING_CACHE_KEY_H

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace model_loading {

enum class TextureUsage : std::uint8_t {
    BaseColor,
    Normal,
    MetallicRoughness,
};

inline std::string normalizedExternalSourceIdentity(
    const std::filesystem::path& source) {
    return "external:" + source.lexically_normal().generic_string();
}

inline std::string normalizedFilesystemIdentity(
    const std::filesystem::path& source) {
    std::error_code error;
    std::filesystem::path normalized = std::filesystem::absolute(source, error);
    if (error) {
        normalized = source;
        error.clear();
    }
    normalized = std::filesystem::weakly_canonical(normalized, error);
    if (error) {
        normalized = normalized.lexically_normal();
    }
    return normalized.generic_string();
}

inline std::string embeddedSourceIdentity(std::string_view modelIdentity,
                                          std::size_t textureIndex) {
    return std::string(modelIdentity) + "|embedded:" +
           std::to_string(textureIndex);
}

inline std::string fallbackSourceIdentity() {
    return normalizedExternalSourceIdentity(
        "rendering/default_textures/error.jpg");
}

struct TextureCacheKey {
    std::string sourceIdentity;
    TextureUsage usage = TextureUsage::BaseColor;

    bool operator==(const TextureCacheKey& other) const noexcept {
        return usage == other.usage && sourceIdentity == other.sourceIdentity;
    }
};

struct ModelAssetCacheKey {
    std::string modelIdentity;
    bool hasTextureFallbackOverride = false;
    std::string textureFallbackIdentity;

    bool operator==(const ModelAssetCacheKey& other) const noexcept {
        return modelIdentity == other.modelIdentity &&
               hasTextureFallbackOverride == other.hasTextureFallbackOverride &&
               textureFallbackIdentity == other.textureFallbackIdentity;
    }
};

struct ModelAssetCacheKeyHash {
    std::size_t operator()(const ModelAssetCacheKey& key) const noexcept {
        std::size_t result = std::hash<std::string>{}(key.modelIdentity);
        const auto combine = [&result](std::size_t value) {
            result ^= value + static_cast<std::size_t>(0x9e3779b9) +
                      (result << 6) + (result >> 2);
        };
        combine(std::hash<bool>{}(key.hasTextureFallbackOverride));
        combine(std::hash<std::string>{}(key.textureFallbackIdentity));
        return result;
    }
};

inline ModelAssetCacheKey makeModelAssetCacheKey(
    const char* modelPath, const char* textureFallbackOverride) {
    ModelAssetCacheKey key;
    key.modelIdentity = normalizedFilesystemIdentity(modelPath ? modelPath : "");
    key.hasTextureFallbackOverride = textureFallbackOverride != nullptr;
    if (textureFallbackOverride != nullptr) {
        key.textureFallbackIdentity =
            normalizedFilesystemIdentity(textureFallbackOverride);
    }
    return key;
}

struct TextureCacheKeyHash {
    std::size_t operator()(const TextureCacheKey& key) const noexcept {
        const std::size_t sourceHash =
            std::hash<std::string>{}(key.sourceIdentity);
        const std::size_t usageHash =
            std::hash<unsigned int>{}(static_cast<unsigned int>(key.usage));
        return sourceHash ^ (usageHash + static_cast<std::size_t>(0x9e3779b9) +
                             (sourceHash << 6) + (sourceHash >> 2));
    }
};

}  // namespace model_loading

#endif
