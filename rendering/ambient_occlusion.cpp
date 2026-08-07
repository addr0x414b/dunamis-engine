#include "ambient_occlusion.h"

#include <cmath>
#include <limits>
#include <random>

namespace ambient_occlusion {
namespace {

bool finiteVector(const glm::vec3& value) noexcept {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

}  // namespace

Result validate(const AmbientOcclusionSettings& settings) {
    if (settings.samples != sampleCount) {
        return Result::failure("Ambient occlusion requires exactly 32 samples");
    }
    if (!std::isfinite(settings.radius) || settings.radius <= 0.0f) {
        return Result::failure("Ambient occlusion radius must be finite and positive");
    }
    if (!std::isfinite(settings.bias) || settings.bias < 0.0f) {
        return Result::failure("Ambient occlusion bias must be finite and nonnegative");
    }
    if (!std::isfinite(settings.power) || settings.power <= 0.0f) {
        return Result::failure("Ambient occlusion power must be finite and positive");
    }
    return Result::success();
}

Kernel makeKernel() {
    // This fixed seed intentionally makes captures and tests reproducible.
    std::mt19937 generator(0xD00D5A0u);
    std::uniform_real_distribution<float> distribution(0.0f, 1.0f);
    Kernel kernel{};
    for (uint32_t index = 0; index < sampleCount; ++index) {
        glm::vec3 sample(distribution(generator) * 2.0f - 1.0f,
                         distribution(generator) * 2.0f - 1.0f,
                         distribution(generator));
        const float lengthSquared = glm::dot(sample, sample);
        if (lengthSquared > std::numeric_limits<float>::epsilon()) {
            sample *= 1.0f / std::sqrt(lengthSquared);
        } else {
            sample = glm::vec3(0.0f, 0.0f, 1.0f);
        }
        const float t = static_cast<float>(index) /
                        static_cast<float>(sampleCount);
        const float scale = 0.1f + 0.9f * t * t;
        sample *= scale;
        kernel[index] = sample;
    }
    return kernel;
}

bool isFiniteMatrix(const glm::mat4& matrix) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (!std::isfinite(matrix[column][row])) return false;
        }
    }
    return true;
}

bool isBackgroundDepth(float depth) noexcept {
    return !std::isfinite(depth) || depth >= 1.0f - 1.0e-6f;
}

bool projectViewPosition(const glm::mat4& projection,
                         const glm::vec3& viewPosition,
                         glm::vec3& screenDepth) noexcept {
    if (!isFiniteMatrix(projection) || !finiteVector(viewPosition)) return false;
    const glm::vec4 clip = projection * glm::vec4(viewPosition, 1.0f);
    if (!std::isfinite(clip.w) || std::abs(clip.w) < 1.0e-6f) return false;
    const glm::vec3 ndc = glm::vec3(clip) / clip.w;
    if (!finiteVector(ndc)) return false;
    screenDepth = glm::vec3(ndc.x * 0.5f + 0.5f, ndc.y * 0.5f + 0.5f,
                            ndc.z);
    return true;
}

bool reconstructViewPosition(const glm::mat4& inverseProjection,
                             const glm::vec2& screenUv, float depth,
                             glm::vec3& viewPosition) noexcept {
    if (!isFiniteMatrix(inverseProjection) || !std::isfinite(screenUv.x) ||
        !std::isfinite(screenUv.y) || !std::isfinite(depth)) return false;
    const glm::vec4 view = inverseProjection * glm::vec4(
        screenUv * 2.0f - 1.0f, depth, 1.0f);
    if (!std::isfinite(view.w) || std::abs(view.w) < 1.0e-6f) return false;
    viewPosition = glm::vec3(view) / view.w;
    return finiteVector(viewPosition);
}

}  // namespace ambient_occlusion
