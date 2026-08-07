#ifndef AMBIENT_OCCLUSION_H
#define AMBIENT_OCCLUSION_H

#include <array>
#include <cstdint>

#include <glm/glm.hpp>

#include "../core/result.h"

namespace ambient_occlusion {

constexpr uint32_t sampleCount = 32;

struct AmbientOcclusionSettings {
    uint32_t samples = sampleCount;
    float radius = 0.75f;
    float bias = 0.025f;
    float power = 1.5f;
    bool enabled = true;
};

using Kernel = std::array<glm::vec3, sampleCount>;

[[nodiscard]] Result validate(const AmbientOcclusionSettings& settings);
[[nodiscard]] Kernel makeKernel();
[[nodiscard]] bool isFiniteMatrix(const glm::mat4& matrix) noexcept;
[[nodiscard]] bool isBackgroundDepth(float depth) noexcept;
[[nodiscard]] bool projectViewPosition(const glm::mat4& projection,
                                       const glm::vec3& viewPosition,
                                       glm::vec3& screenDepth) noexcept;
[[nodiscard]] bool reconstructViewPosition(const glm::mat4& inverseProjection,
                                           const glm::vec2& screenUv,
                                           float depth,
                                           glm::vec3& viewPosition) noexcept;

struct DescriptorPoolRequirements {
    uint32_t uniformBuffers = 0;
    uint32_t combinedImageSamplers = 0;
    uint32_t descriptorSets = 0;
};

constexpr DescriptorPoolRequirements descriptorPoolRequirements(
    uint32_t framesInFlight) noexcept {
    return {framesInFlight, framesInFlight * 4, framesInFlight};
}

[[nodiscard]] constexpr bool validExtent(uint32_t width,
                                         uint32_t height) noexcept {
    return width > 0 && height > 0;
}

}  // namespace ambient_occlusion

#endif
