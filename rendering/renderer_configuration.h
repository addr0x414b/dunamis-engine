#ifndef RENDERER_CONFIGURATION_H
#define RENDERER_CONFIGURATION_H

#include <cstdint>

#include <vulkan/vulkan.h>

#include "../core/result.h"

namespace renderer_configuration {

constexpr VkSampleCountFlagBits maximumRequestedMsaa = VK_SAMPLE_COUNT_4_BIT;

inline Result samplerMaxLod(uint32_t mipLevels, float& maxLod) {
    if (mipLevels == 0) {
        return Result::failure("Mip level count must be greater than zero");
    }
    maxLod = static_cast<float>(mipLevels - 1);
    return Result::success();
}

constexpr VkSampleCountFlagBits selectMsaaSampleCount(
    VkSampleCountFlags supportedSampleCounts) noexcept {
    if (supportedSampleCounts & maximumRequestedMsaa) {
        return maximumRequestedMsaa;
    }
    if (supportedSampleCounts & VK_SAMPLE_COUNT_2_BIT) {
        return VK_SAMPLE_COUNT_2_BIT;
    }
    return VK_SAMPLE_COUNT_1_BIT;
}

constexpr bool shouldEnableSampleRateShading(
    bool sampleRateShadingSupported,
    VkSampleCountFlagBits selectedMsaaSamples) noexcept {
    return sampleRateShadingSupported &&
           selectedMsaaSamples != VK_SAMPLE_COUNT_1_BIT;
}

constexpr const char* sampleCountName(
    VkSampleCountFlagBits sampleCount) noexcept {
    switch (sampleCount) {
        case VK_SAMPLE_COUNT_1_BIT:
            return "1x";
        case VK_SAMPLE_COUNT_2_BIT:
            return "2x";
        case VK_SAMPLE_COUNT_4_BIT:
            return "4x";
        default:
            return "unknown";
    }
}

}  // namespace renderer_configuration

#endif
