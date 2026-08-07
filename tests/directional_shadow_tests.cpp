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

bool sameVector(const glm::vec3& first, const glm::vec3& second) {
    return glm::length(first - second) < 1.0e-5f;
}

bool runDirectionDeltaTests() {
    const glm::vec3 originalDirection{0.0f, -2.0f, 0.0f};
    DirectionalLight xRotated;
    xRotated.direction = originalDirection;
    DirectionalLight yRotated;
    yRotated.direction = originalDirection;
    DirectionalLight zRotated;
    zRotated.direction = originalDirection;

    bool passed = expect(
        xRotated.applyDirectionDelta(glm::mat3(
            editor_picking::makeRotationMatrix({90.0f, 0.0f, 0.0f}))) &&
            sameVector(xRotated.direction, {0.0f, 0.0f, -2.0f}),
        "Directional light X rotation produced the wrong direction");
    passed &= expect(
        yRotated.applyDirectionDelta(glm::mat3(
            editor_picking::makeRotationMatrix({0.0f, 90.0f, 0.0f}))) &&
            sameVector(yRotated.direction, originalDirection),
        "Directional light Y rotation produced the wrong direction");
    passed &= expect(
        zRotated.applyDirectionDelta(glm::mat3(
            editor_picking::makeRotationMatrix({0.0f, 0.0f, 90.0f}))) &&
            sameVector(zRotated.direction, {2.0f, 0.0f, 0.0f}),
        "Directional light Z rotation produced the wrong direction");
    passed &= expect(std::abs(glm::length(xRotated.direction) - 2.0f) <
                         1.0e-5f &&
                         std::isfinite(xRotated.direction.x) &&
                         std::isfinite(xRotated.direction.y) &&
                         std::isfinite(xRotated.direction.z),
                     "Directional light rotation did not preserve magnitude");

    const glm::vec3 originalAfterInvalidAttempt = zRotated.direction;
    glm::mat3 nonfiniteDelta(1.0f);
    nonfiniteDelta[0][0] = std::numeric_limits<float>::quiet_NaN();
    passed &= expect(!zRotated.applyDirectionDelta(nonfiniteDelta) &&
                         sameVector(zRotated.direction,
                                    originalAfterInvalidAttempt),
                     "Nonfinite directional rotation partially mutated light");
    passed &= expect(!zRotated.applyDirectionDelta(glm::mat3(0.0f)) &&
                         sameVector(zRotated.direction,
                                    originalAfterInvalidAttempt),
                     "Degenerate directional rotation was accepted");
    return passed;
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
    passed &= runDirectionDeltaTests();
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
