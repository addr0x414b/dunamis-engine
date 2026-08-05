#ifndef VULKAN_UTILS_H
#define VULKAN_UTILS_H

#include <array>
#include <cstddef>
#include <cstdint>

#define GLM_FORCE_RADIANS
#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#define GLM_ENABLE_EXPERIMENTAL
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/hash.hpp>
#include <vulkan/vulkan.h>
#include "../../scene/scene_limits.h"
#include "../../third_party/stb/stb_image.h"

struct Vertex {
    glm::vec3 pos;
    glm::vec3 color;
    glm::vec2 texCoord;

    bool operator==(const Vertex& other) const {
        return pos == other.pos && color == other.color &&
               texCoord == other.texCoord;
    }

    static VkVertexInputBindingDescription getBindingDescription() {
        VkVertexInputBindingDescription bindingDescription{};
        bindingDescription.binding = 0;
        bindingDescription.stride = sizeof(Vertex);
        bindingDescription.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
        return bindingDescription;
    }

    static std::array<VkVertexInputAttributeDescription, 3>
    getAttributeDescriptions() {
        std::array<VkVertexInputAttributeDescription, 3>
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

struct LightsUBO {
    std::array<LightData, scene_limits::maxPointLights> lights{};
    glm::vec4 ambientColorIntensity{};
    std::int32_t numLights = 0;
    std::array<std::uint32_t, 3> padding{};
};

static_assert(sizeof(LightData) == 32,
              "LightData must match the fragment shader's std140 layout");
static_assert(
    offsetof(LightsUBO, numLights) ==
        sizeof(LightData) * scene_limits::maxPointLights + sizeof(glm::vec4),
    "LightsUBO must match the fragment shader's std140 layout");
static_assert(offsetof(LightsUBO, ambientColorIntensity) == 512,
              "LightsUBO ambient field must match std140 offset");
static_assert(offsetof(LightsUBO, numLights) == 528,
              "LightsUBO light count must match std140 offset");
static_assert(sizeof(LightsUBO) == 544,
              "LightsUBO must include explicit std140 trailing padding");

namespace std {
template <>
struct hash<Vertex> {
    size_t operator()(Vertex const& vertex) const {
        return ((hash<glm::vec3>()(vertex.pos) ^
                 (hash<glm::vec3>()(vertex.color) << 1)) >>
                1) ^
               (hash<glm::vec2>()(vertex.texCoord) << 1);
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
    uint32_t mipLevels = 0;
    VkImage textureImage = VK_NULL_HANDLE;
    VkDeviceMemory textureImageMemory = VK_NULL_HANDLE;
    VkImageView textureImageView = VK_NULL_HANDLE;
    VkSampler textureSampler = VK_NULL_HANDLE;
    stbi_uc* pixels = nullptr;
    int texWidth = 0;
    int texHeight = 0;
    int texChannels = 0;
    MaterialAlphaMode alphaMode = MaterialAlphaMode::Opaque;
    float alphaCutoff = 0.5f;
    bool doubleSided = false;
};

struct MaterialPushConstants {
    std::int32_t alphaMode =
        static_cast<std::int32_t>(MaterialAlphaMode::Opaque);
    float alphaCutoff = 0.5f;
};

static_assert(sizeof(MaterialPushConstants) == 8,
              "MaterialPushConstants must match the fragment shader layout");
static_assert(offsetof(MaterialPushConstants, alphaCutoff) == 4,
              "MaterialPushConstants must match the fragment shader layout");

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
