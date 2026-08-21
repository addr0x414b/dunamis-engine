#ifndef DIRECTIONAL_SHADOW_H
#define DIRECTIONAL_SHADOW_H

#include <cstdint>

#include <glm/glm.hpp>

#include "../core/result.h"
#include "../scene/directional_light.h"

namespace directional_shadow {

constexpr uint32_t mapResolution = 2048;
constexpr float depthBiasConstantFactor = 1.25f;
constexpr float depthBiasSlopeFactor = 1.75f;
constexpr float depthBiasClamp = 0.0f;

struct DescriptorPoolRequirements {
    uint32_t uniformBuffers = 0;
    uint32_t combinedImageSamplers = 0;
    uint32_t descriptorSets = 0;
};

constexpr DescriptorPoolRequirements descriptorPoolRequirements(
    uint32_t frameCount) noexcept {
    return {frameCount, frameCount, frameCount};
}

constexpr bool shouldRender(const DirectionalLight* light) noexcept {
    return light != nullptr;
}

struct LightMatrices {
    glm::vec3 normalizedDirection{0.0f, -1.0f, 0.0f};
    glm::mat4 view{1.0f};
    glm::mat4 projection{1.0f};
    glm::mat4 viewProjection{1.0f};
};

[[nodiscard]] Result calculateLightMatrices(
    const DirectionalLight& light, LightMatrices& matrices);
[[nodiscard]] bool projectWorldPoint(const glm::mat4& lightViewProjection,
                                     const glm::vec3& worldPoint,
                                     glm::vec3& textureCoordinates) noexcept;
[[nodiscard]] bool isFiniteMatrix(const glm::mat4& matrix) noexcept;

}  // namespace directional_shadow

#endif
