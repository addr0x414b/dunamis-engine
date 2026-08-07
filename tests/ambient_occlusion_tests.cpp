#include "rendering/ambient_occlusion.h"

#include <cmath>
#include <iostream>
#include <limits>

#include <glm/ext/matrix_clip_space.hpp>

namespace {
bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}
bool nearlyEqual(const glm::vec3& a, const glm::vec3& b) {
    return glm::length(a - b) < 1.0e-3f;
}
}  // namespace

int main() {
    bool passed = true;
    ambient_occlusion::AmbientOcclusionSettings settings;
    passed &= expect(static_cast<bool>(ambient_occlusion::validate(settings)),
                     "Default AO settings must be valid");
    passed &= expect(settings.samples == ambient_occlusion::sampleCount,
                     "AO sample count must remain version-one 32");
    settings.radius = 0.0f;
    passed &= expect(!ambient_occlusion::validate(settings), "Zero radius accepted");
    settings.radius = 0.75f;
    settings.bias = -0.01f;
    passed &= expect(!ambient_occlusion::validate(settings), "Negative bias accepted");
    settings.bias = 0.025f;
    settings.radius = std::numeric_limits<float>::quiet_NaN();
    passed &= expect(!ambient_occlusion::validate(settings), "Nonfinite radius accepted");
    settings.radius = 0.75f;
    settings.power = std::numeric_limits<float>::infinity();
    passed &= expect(!ambient_occlusion::validate(settings), "Infinite power accepted");

    const auto kernel = ambient_occlusion::makeKernel();
    const auto repeatedKernel = ambient_occlusion::makeKernel();
    passed &= expect(kernel.size() == ambient_occlusion::sampleCount,
                     "AO kernel sample count is wrong");
    bool sawNear = false, sawFar = false;
    for (size_t i = 0; i < kernel.size(); ++i) {
        const glm::vec3 sample = kernel[i];
        passed &= expect(std::isfinite(sample.x) && std::isfinite(sample.y) &&
                             std::isfinite(sample.z) && sample.z >= 0.0f &&
                             glm::length(sample) <= 1.0f,
                         "Invalid AO kernel sample");
        passed &= expect(sample == repeatedKernel[i], "AO kernel is not deterministic");
        sawNear |= glm::length(sample) < 0.2f;
        sawFar |= glm::length(sample) > 0.8f;
    }
    passed &= expect(sawNear && sawFar, "AO kernel lacks progressive distribution");

    glm::mat4 projection = glm::perspective(glm::radians(45.0f), 16.0f / 9.0f,
                                             0.1f, 1000.0f);
    projection[1][1] *= -1.0f;
    const glm::mat4 inverse = glm::inverse(projection);
    passed &= expect(ambient_occlusion::isFiniteMatrix(projection) &&
                         ambient_occlusion::isFiniteMatrix(inverse),
                     "Vulkan projection or inverse is nonfinite");
    glm::vec3 projected;
    const glm::vec3 original(0.3f, -0.2f, -4.0f);
    passed &= expect(ambient_occlusion::projectViewPosition(projection, original,
                                                            projected),
                     "View-space point did not project");
    glm::vec3 reconstructed;
    passed &= expect(ambient_occlusion::reconstructViewPosition(
                         inverse, glm::vec2(projected), projected.z, reconstructed) &&
                         nearlyEqual(original, reconstructed),
                     "Vulkan depth reconstruction disagrees with projection");
    passed &= expect(projected.z > 0.0f && projected.z < 1.0f &&
                         projected.y > 0.5f,
                     "Vulkan zero-to-one depth or flipped Y convention is wrong");
    passed &= expect(ambient_occlusion::isBackgroundDepth(1.0f) &&
                         !ambient_occlusion::isBackgroundDepth(0.5f),
                     "Background depth classification is wrong");
    constexpr auto requirements = ambient_occlusion::descriptorPoolRequirements(2);
    passed &= expect(requirements.uniformBuffers == 2 &&
                         requirements.combinedImageSamplers == 8 &&
                         requirements.descriptorSets == 2 &&
                         ambient_occlusion::validExtent(1, 1) &&
                         !ambient_occlusion::validExtent(0, 1),
                     "AO resource configuration helpers are wrong");
    return passed ? 0 : 1;
}
