#include "rendering/directional_shadow.h"
#include "rendering/editor_picking.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) std::cerr << message << '\n';
    return condition;
}

bool nearlyEqual(float first, float second) {
    return std::abs(first - second) < 1.0e-4f;
}

bool finiteMatrix(const glm::mat4& matrix) {
    return directional_shadow::isFiniteMatrix(matrix);
}

bool expectRejected(const DirectionalLight& light, const char* message) {
    directional_shadow::LightMatrices matrices;
    return expect(!directional_shadow::calculateLightMatrices(light, matrices),
                  message);
}

}  // namespace

int main() {
    bool passed = true;
    constexpr directional_shadow::DescriptorPoolRequirements pool =
        directional_shadow::descriptorPoolRequirements(2);
    passed &= expect(pool.uniformBuffers == 2 &&
                         pool.combinedImageSamplers == 2 &&
                         pool.descriptorSets == 2,
                     "Directional-shadow descriptor-pool requirements are wrong");
    passed &= expect(!directional_shadow::shouldRender(nullptr),
                     "Missing directional light did not disable the shadow pass");
    DirectionalLight light;
    light.direction = glm::vec3(0.0f, -2.0f, 0.0f);
    directional_shadow::LightMatrices matrices;
    const Result result = directional_shadow::calculateLightMatrices(light, matrices);
    passed &= expect(static_cast<bool>(result), "Valid directional shadow settings failed");
    if (result) {
        passed &= expect(nearlyEqual(glm::length(matrices.normalizedDirection), 1.0f),
                         "Directional light direction was not normalized");
        passed &= expect(finiteMatrix(matrices.view) && finiteMatrix(matrices.projection) &&
                             finiteMatrix(matrices.viewProjection),
                         "Directional shadow matrices were not finite");
        glm::vec3 focusCoordinates;
        passed &= expect(directional_shadow::projectWorldPoint(
                             matrices.viewProjection, light.shadow.focus,
                             focusCoordinates),
                         "Configured focus was not inside the shadow volume");
        passed &= expect(focusCoordinates.z > 0.0f && focusCoordinates.z < 1.0f,
                         "Vulkan zero-to-one shadow depth mapping was not retained");
        glm::vec3 insideCoordinates;
        passed &= expect(directional_shadow::projectWorldPoint(
                             matrices.viewProjection, glm::vec3(10.0f, 0.0f, 10.0f),
                             insideCoordinates),
                         "Known inside shadow point was rejected");
        glm::vec3 outsideCoordinates;
        passed &= expect(!directional_shadow::projectWorldPoint(
                             matrices.viewProjection, glm::vec3(1000.0f),
                             outsideCoordinates),
                         "Known outside shadow point was accepted");
    }

    DirectionalLight parallelLight;
    parallelLight.direction = glm::vec3(0.0f, 1.0f, 0.0f);
    directional_shadow::LightMatrices parallelMatrices;
    passed &= expect(static_cast<bool>(directional_shadow::calculateLightMatrices(
                         parallelLight, parallelMatrices)) &&
                         finiteMatrix(parallelMatrices.view),
                     "Parallel world-Y light direction did not choose a robust up vector");

    DirectionalLight invalid = light;
    invalid.direction = glm::vec3(0.0f);
    passed &= expectRejected(invalid, "Zero direction was accepted");
    invalid = light;
    invalid.direction.x = std::numeric_limits<float>::quiet_NaN();
    passed &= expectRejected(invalid, "Nonfinite direction was accepted");
    invalid = light;
    invalid.shadow.focus.x = std::numeric_limits<float>::infinity();
    passed &= expectRejected(invalid, "Nonfinite focus was accepted");
    invalid = light;
    invalid.shadow.halfExtent = 0.0f;
    passed &= expectRejected(invalid, "Zero half extent was accepted");
    invalid = light;
    invalid.shadow.nearPlane = invalid.shadow.farPlane;
    passed &= expectRejected(invalid, "Invalid near/far range was accepted");

    const glm::mat4 firstModel = editor_picking::makeModelMatrix(
        glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::mat4 movedModel = editor_picking::makeModelMatrix(
        glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec4 firstLightPosition = matrices.viewProjection * firstModel *
                                         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 movedLightPosition = matrices.viewProjection * movedModel *
                                         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    passed &= expect(firstLightPosition != movedLightPosition,
                     "Moved model did not produce a new light-space position");
    return passed ? 0 : 1;
}
