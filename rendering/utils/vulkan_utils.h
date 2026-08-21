#ifndef VULKAN_UTILS_H
#define VULKAN_UTILS_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <vulkan/vulkan.h>

#include "../../assets/model_asset.h"
#include "../../scene/scene_limits.h"

inline VkVertexInputBindingDescription
getVulkanVertexBindingDescription() noexcept {
    return {0, static_cast<std::uint32_t>(sizeof(Vertex)),
            VK_VERTEX_INPUT_RATE_VERTEX};
}

inline std::array<VkVertexInputAttributeDescription, 5>
getVulkanVertexAttributeDescriptions() noexcept {
    return {{
        {0, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(Vertex, pos))},
        {1, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(Vertex, color))},
        {2, 0, VK_FORMAT_R32G32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(Vertex, texCoord))},
        {3, 0, VK_FORMAT_R32G32B32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(Vertex, normal))},
        {4, 0, VK_FORMAT_R32G32B32A32_SFLOAT,
         static_cast<std::uint32_t>(offsetof(Vertex, tangent))},
    }};
}

struct UniformBufferObject {
    glm::mat4 model;
    glm::mat4 view;
    glm::mat4 proj;
    glm::vec3 cameraPosition;
};

inline UniformBufferObject makeUniformBufferObject(
    const glm::mat4& model, const glm::mat4& view,
    const glm::mat4& projection,
    const glm::vec3& cameraPosition) noexcept {
    return {model, view, projection, cameraPosition};
}

struct alignas(16) DirectionalShadowUBO {
    glm::mat4 lightViewProjection{1.0f};
};

static_assert(alignof(DirectionalShadowUBO) == 16,
              "DirectionalShadowUBO must be 16-byte aligned");
static_assert(sizeof(DirectionalShadowUBO) == sizeof(glm::mat4),
              "DirectionalShadowUBO must contain exactly one matrix");

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

// Immutable device resources are owned by VulkanContext's asset cache.
struct GpuMeshAsset {
    VkBuffer vertexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory vertexBufferMemory = VK_NULL_HANDLE;
    VkBuffer indexBuffer = VK_NULL_HANDLE;
    VkDeviceMemory indexBufferMemory = VK_NULL_HANDLE;
    VkImage textureImage = VK_NULL_HANDLE;
    VkDeviceMemory textureImageMemory = VK_NULL_HANDLE;
    VkImageView textureImageView = VK_NULL_HANDLE;
    VkSampler textureSampler = VK_NULL_HANDLE;
    VkImage normalMapImage = VK_NULL_HANDLE;
    VkDeviceMemory normalMapImageMemory = VK_NULL_HANDLE;
    VkImageView normalMapImageView = VK_NULL_HANDLE;
    VkSampler normalMapSampler = VK_NULL_HANDLE;
    VkImage metallicRoughnessMapImage = VK_NULL_HANDLE;
    VkDeviceMemory metallicRoughnessMapImageMemory = VK_NULL_HANDLE;
    VkImageView metallicRoughnessMapImageView = VK_NULL_HANDLE;
    VkSampler metallicRoughnessMapSampler = VK_NULL_HANDLE;
};

#endif
