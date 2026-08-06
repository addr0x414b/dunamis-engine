#include "rendering/renderer_configuration.h"
#include "rendering/utils/vulkan_utils.h"

#include <cmath>
#include <iostream>

#include <glm/gtc/matrix_transform.hpp>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool sameFloat(float first, float second) {
    return std::abs(first - second) < 1.0e-5f;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second) {
    return glm::length(first - second) < 1.0e-5f;
}

bool runProjectionAndCameraUboTests() {
    glm::mat4 projection = glm::perspective(
        glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 10000.0f);
    projection[1][1] *= -1.0f;
    const glm::vec4 nearClip = projection * glm::vec4(0.0f, 0.0f, -0.1f, 1.0f);
    const glm::vec4 farClip = projection * glm::vec4(0.0f, 0.0f, -10000.0f, 1.0f);
    const glm::vec3 cameraPosition{17.0f, -3.0f, 42.0f};
    const glm::vec3 cameraDirection{0.0f, 0.0f, -1.0f};
    const UniformBufferObject ubo = makeUniformBufferObject(
        glm::mat4(1.0f), glm::mat4(1.0f), projection, cameraPosition);

    return expect(sameFloat(nearClip.z / nearClip.w, 0.0f),
                  "Perspective near plane is not zero in NDC") &&
           expect(sameFloat(farClip.z / farClip.w, 1.0f),
                  "Perspective far plane is not one in NDC") &&
           expect(projection[1][1] < 0.0f,
                  "Vulkan projection Y flip was not retained") &&
           expect(sameVector(ubo.cameraPosition, cameraPosition),
                  "Camera UBO did not receive the camera world position") &&
           expect(!sameVector(ubo.cameraPosition, cameraDirection),
                  "Camera UBO incorrectly contains the camera direction");
}

bool runSamplerMipTests() {
    float maxLod = -1.0f;
    bool passed = expect(renderer_configuration::samplerMaxLod(1, maxLod) &&
                             sameFloat(maxLod, 0.0f),
                         "One mip level did not select maxLod zero");
    passed &= expect(renderer_configuration::samplerMaxLod(2, maxLod) &&
                         sameFloat(maxLod, 1.0f),
                     "Two mip levels did not select maxLod one");
    passed &= expect(renderer_configuration::samplerMaxLod(5, maxLod) &&
                         sameFloat(maxLod, 4.0f),
                     "Five mip levels did not select maxLod four");
    passed &= expect(!renderer_configuration::samplerMaxLod(0, maxLod),
                     "Zero mip levels were accepted");
    return passed;
}

bool runMsaaAndSampleShadingTests() {
    bool passed = expect(
        renderer_configuration::selectMsaaSampleCount(
            VK_SAMPLE_COUNT_64_BIT | VK_SAMPLE_COUNT_32_BIT |
            VK_SAMPLE_COUNT_16_BIT | VK_SAMPLE_COUNT_8_BIT |
            VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_2_BIT |
            VK_SAMPLE_COUNT_1_BIT) == VK_SAMPLE_COUNT_4_BIT,
        "MSAA selection was not capped at 4x");
    passed &= expect(renderer_configuration::selectMsaaSampleCount(
                         VK_SAMPLE_COUNT_4_BIT | VK_SAMPLE_COUNT_2_BIT) ==
                         VK_SAMPLE_COUNT_4_BIT,
                     "4x support did not select 4x MSAA");
    passed &= expect(renderer_configuration::selectMsaaSampleCount(
                         VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_1_BIT) ==
                         VK_SAMPLE_COUNT_2_BIT,
                     "2x support did not select 2x MSAA");
    passed &= expect(renderer_configuration::selectMsaaSampleCount(
                         VK_SAMPLE_COUNT_1_BIT) == VK_SAMPLE_COUNT_1_BIT,
                     "1x-only support did not select 1x MSAA");
    passed &= expect(renderer_configuration::shouldEnableSampleRateShading(
                         true, VK_SAMPLE_COUNT_4_BIT),
                     "Supported sample-rate shading was disabled for 4x MSAA");
    passed &= expect(!renderer_configuration::shouldEnableSampleRateShading(
                         true, VK_SAMPLE_COUNT_1_BIT),
                     "Sample-rate shading was enabled for 1x MSAA");
    passed &= expect(!renderer_configuration::shouldEnableSampleRateShading(
                         false, VK_SAMPLE_COUNT_4_BIT),
                     "Unsupported sample-rate shading was enabled for 4x MSAA");
    passed &= expect(!renderer_configuration::shouldEnableSampleRateShading(
                         false, VK_SAMPLE_COUNT_1_BIT),
                     "Unsupported sample-rate shading was enabled for 1x MSAA");
    return passed;
}

}  // namespace

int main() {
    return runProjectionAndCameraUboTests() && runSamplerMipTests() &&
                   runMsaaAndSampleShadingTests()
               ? 0
               : 1;
}
