#ifndef VULKAN_UTILS_H
#define VULKAN_UTILS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>
#include <vulkan/vulkan.h>
#include "../../scene/scene_limits.h"
#include "../../third_party/stb/stb_image.h"

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

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 5>
    getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 5>
            attributeDescriptions{};

        attributeDescriptions[0].binding = 0;
        attributeDescriptions[0].location = 0;
        attributeDescriptions[0].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[0].offset = offsetof(Vertex, pos);

        attributeDescriptions[1].binding = 0;
        attributeDescriptions[1].location = 1;
        attributeDescriptions[1].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[1].offset = offsetof(Vertex, color);

        attributeDescriptions[2].binding = 0;
        attributeDescriptions[2].location = 2;
        attributeDescriptions[2].format = VK_FORMAT_R32G32_SFLOAT;
        attributeDescriptions[2].offset = offsetof(Vertex, texCoord);

        attributeDescriptions[3].binding = 0;
        attributeDescriptions[3].location = 3;
        attributeDescriptions[3].format = VK_FORMAT_R32G32B32_SFLOAT;
        attributeDescriptions[3].offset = offsetof(Vertex, normal);

        attributeDescriptions[4].binding = 0;
        attributeDescriptions[4].location = 4;
        attributeDescriptions[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        attributeDescriptions[4].offset = offsetof(Vertex, tangent);

        return attributeDescriptions;
    }
};

struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 cameraPosition;
};

struct LightData {
    glm::vec3 position;
    float padding;
    glm::vec3 color;
    float intensity;
};

struct alignas(16) DirectionalLightData {
    glm::vec4 directionEnabled{};
    glm::vec4 colorIntensity{};
};

struct alignas(16) LightsUBO {
    std::array<LightData, scene_limits::maxPointLights> lights{};
    glm::vec4 ambientColorIntensity{};
    DirectionalLightData directionalLight{};
    std::int32_t numLights = 0;
    std::array<std::uint32_t, 3> padding{};
};

static_assert(sizeof(LightData) == 32,
              "LightData must match the fragment shader's std140 layout");
static_assert(alignof(DirectionalLightData) == 16,
              "DirectionalLightData must be 16-byte aligned");
static_assert(offsetof(DirectionalLightData, directionEnabled) == 0,
              "DirectionalLightData direction must match std140 offset");
static_assert(offsetof(DirectionalLightData, colorIntensity) == 16,
              "DirectionalLightData color must match std140 offset");
static_assert(sizeof(DirectionalLightData) == 32,
              "DirectionalLightData must match the fragment shader layout");
static_assert(alignof(LightsUBO) == 16,
              "LightsUBO must be 16-byte aligned");
static_assert(offsetof(LightsUBO, lights) == 0,
              "LightsUBO point lights must start at offset zero");
static_assert(offsetof(LightsUBO, ambientColorIntensity) == 512,
              "LightsUBO ambient field must match std140 offset");
static_assert(offsetof(LightsUBO, directionalLight) == 528,
              "LightsUBO directional field must match std140 offset");
static_assert(
    offsetof(LightsUBO, numLights) ==
        560,
    "LightsUBO must match the fragment shader's std140 layout");
static_assert(offsetof(LightsUBO, padding) == 564,
              "LightsUBO trailing padding must match std140 offset");
static_assert(sizeof(LightsUBO) == 576,
              "LightsUBO must include explicit std140 trailing padding");

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
    const char* modelPath;
    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
};

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
    uint32_t mipLevels = 0;
    VkImage textureImage = VK_NULL_HANDLE;
    VkDeviceMemory textureImageMemory = VK_NULL_HANDLE;
    VkImageView textureImageView = VK_NULL_HANDLE;
    VkSampler textureSampler = VK_NULL_HANDLE;
    stbi_uc* pixels = nullptr;
    int texWidth = 0;
    int texHeight = 0;
    int texChannels = 0;
    // Raw pixel fields are views; the owner keeps decoded CPU pixels alive
    // until Vulkan upload (or the final Material reference) releases them.
    StbiPixelOwner pixelsOwner{};
    std::string normalMapPath{};
    uint32_t normalMapMipLevels = 0;
    VkImage normalMapImage = VK_NULL_HANDLE;
    VkDeviceMemory normalMapImageMemory = VK_NULL_HANDLE;
    VkImageView normalMapImageView = VK_NULL_HANDLE;
    VkSampler normalMapSampler = VK_NULL_HANDLE;
    stbi_uc* normalMapPixels = nullptr;
    int normalMapWidth = 0;
    int normalMapHeight = 0;
    int normalMapChannels = 0;
    // See pixelsOwner above. This owner may be shared by mesh instances.
    StbiPixelOwner normalMapPixelsOwner{};
    bool normalMapEnabled = false;
    std::string metallicRoughnessMapPath{};
    uint32_t metallicRoughnessMapMipLevels = 0;
    VkImage metallicRoughnessMapImage = VK_NULL_HANDLE;
    VkDeviceMemory metallicRoughnessMapImageMemory = VK_NULL_HANDLE;
    VkImageView metallicRoughnessMapImageView = VK_NULL_HANDLE;
    VkSampler metallicRoughnessMapSampler = VK_NULL_HANDLE;
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

struct MaterialPushConstants {
    glm::vec4 baseColorFactor{1.0f};
    float metallicFactor = 1.0f;
    float roughnessFactor = 1.0f;
    std::int32_t alphaMode =
        static_cast<std::int32_t>(MaterialAlphaMode::Opaque);
    float alphaCutoff = 0.5f;
    std::int32_t normalMapEnabled = 0;
    std::int32_t metallicRoughnessMapEnabled = 0;
};

struct OutlinePushConstants {
    glm::vec4 color;
    glm::vec4 parameters;
};

static_assert(sizeof(OutlinePushConstants) == 32,
              "OutlinePushConstants must match the outline shader layout");

static_assert(sizeof(float) == 4 && sizeof(std::int32_t) == 4,
              "MaterialPushConstants requires four-byte scalar types");
static_assert(sizeof(MaterialPushConstants) == 40,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, baseColorFactor) == 0,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, metallicFactor) == 16,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, roughnessFactor) == 20,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, alphaMode) == 24,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, alphaCutoff) == 28,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, normalMapEnabled) == 32,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, metallicRoughnessMapEnabled) == 36,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(alignof(MaterialPushConstants) % 4 == 0,
              "MaterialPushConstants must be four-byte aligned");
static_assert(offsetof(MaterialPushConstants, baseColorFactor) % 4 == 0 &&
                  offsetof(MaterialPushConstants, metallicFactor) % 4 == 0 &&
                  offsetof(MaterialPushConstants, roughnessFactor) % 4 == 0 &&
                  offsetof(MaterialPushConstants, alphaMode) % 4 == 0 &&
                  offsetof(MaterialPushConstants, alphaCutoff) % 4 == 0 &&
                  offsetof(MaterialPushConstants, normalMapEnabled) % 4 == 0 &&
                  offsetof(MaterialPushConstants,
                           metallicRoughnessMapEnabled) % 4 == 0,
              "MaterialPushConstants fields must be four-byte aligned");

struct RenderData {
    std::vector<VkBuffer> uniformBuffers;
    std::vector<VkDeviceMemory> uniformBuffersMemory;
    std::vector<void*> uniformBuffersMapped;
    std::vector<VkDescriptorSet> descriptorSets;
};

struct MeshInstance {
    Mesh mesh;
    Material material;
    RenderData renderData;
};


#endif
