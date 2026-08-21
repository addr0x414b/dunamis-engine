#ifndef MODEL_ASSET_H
#define MODEL_ASSET_H

#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtx/hash.hpp>

#include "../third_party/stb/stb_image.h"

struct StbiPixelDeleter {
    void operator()(stbi_uc* pixels) const noexcept {
        if (pixels) {
            stbi_image_free(pixels);
        }
    }
};

using StbiPixelOwner = std::shared_ptr<stbi_uc>;

inline StbiPixelOwner makeStbiPixelOwner(stbi_uc* pixels) {
    return StbiPixelOwner(pixels, StbiPixelDeleter{});
}

inline void releaseStbiPixel(StbiPixelOwner& owner,
                             stbi_uc*& pixels) noexcept {
    if (owner) {
        owner.reset();
    } else if (pixels) {
        stbi_image_free(pixels);
    }
    pixels = nullptr;
}

struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;
    glm::vec3 normal;
    glm::vec4 tangent;

    bool operator==(const Vertex& other) const {
        return pos == other.pos && color == other.color &&
               texCoord == other.texCoord && normal == other.normal &&
               tangent == other.tangent;
    }
};

namespace std {
template <>
struct hash<Vertex> {
    size_t operator()(Vertex const& vertex) const {
        return ((hash<glm::vec3>()(vertex.pos) ^
                 (hash<glm::vec3>()(vertex.color) << 1)) >>
                1) ^
               (hash<glm::vec2>()(vertex.texCoord) << 1) ^
               (hash<glm::vec3>()(vertex.normal) << 1) ^
               (hash<glm::vec4>()(vertex.tangent) << 1);
    }
};
}  // namespace std

struct Mesh {
    const char* modelPath = nullptr;
    std::vector<Vertex> vertices;
    std::vector<std::uint32_t> indices;
    struct Bounds {
        glm::vec3 minimum{0.0f};
        glm::vec3 maximum{0.0f};
        bool valid = false;
    } bounds;
};

inline void calculateMeshBounds(Mesh& mesh) noexcept {
    mesh.bounds = {};
    bool foundFinitePosition = false;
    glm::vec3 minimum(0.0f);
    glm::vec3 maximum(0.0f);
    for (const Vertex& vertex : mesh.vertices) {
        const glm::vec3& position = vertex.pos;
        if (!std::isfinite(position.x) || !std::isfinite(position.y) ||
            !std::isfinite(position.z)) {
            continue;
        }
        if (!foundFinitePosition) {
            minimum = position;
            maximum = position;
            foundFinitePosition = true;
            continue;
        }
        minimum = glm::min(minimum, position);
        maximum = glm::max(maximum, position);
    }

    if (!foundFinitePosition) {
        return;
    }

    mesh.bounds.minimum = minimum;
    mesh.bounds.maximum = maximum;
    mesh.bounds.valid = true;
}

enum class MaterialAlphaMode : std::uint32_t {
    Opaque = 0,
    Mask = 1,
    Blend = 2,
};

struct Material {
    const char* texturePath = nullptr;
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    std::uint32_t mipLevels = 0;
    stbi_uc* pixels = nullptr;
    int texWidth = 0;
    int texHeight = 0;
    int texChannels = 0;
    // Raw pixel fields are views; the owner keeps decoded CPU pixels alive
    // until the final Material reference releases them.
    StbiPixelOwner pixelsOwner{};
    std::string normalMapPath{};
    std::uint32_t normalMapMipLevels = 0;
    stbi_uc* normalMapPixels = nullptr;
    int normalMapWidth = 0;
    int normalMapHeight = 0;
    int normalMapChannels = 0;
    // See pixelsOwner above. This owner may be shared by mesh instances.
    StbiPixelOwner normalMapPixelsOwner{};
    bool normalMapEnabled = false;
    std::string metallicRoughnessMapPath{};
    std::uint32_t metallicRoughnessMapMipLevels = 0;
    stbi_uc* metallicRoughnessMapPixels = nullptr;
    int metallicRoughnessMapWidth = 0;
    int metallicRoughnessMapHeight = 0;
    int metallicRoughnessMapChannels = 0;
    // See pixelsOwner above. This owner may be shared by mesh instances.
    StbiPixelOwner metallicRoughnessMapPixelsOwner{};
    bool hasMetallicRoughnessMap = false;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

struct MeshInstance {
    Mesh mesh;
    Material material;
};

#endif
