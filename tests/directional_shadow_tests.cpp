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

glm::vec3 expectedDirection(const glm::vec3& rotation) {
    return glm::vec3(editor_picking::makeRotationMatrix(rotation) *
                     glm::vec4(0.0f, -1.0f, 0.0f, 0.0f));
}

bool runDirectionalLightDirectionTests() {
    struct RotationCase {
        glm::vec3 rotation;
        glm::vec3 expected;
        const char* message;
    };
    const RotationCase axisCases[] = {
        {{0.0f, 0.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
         "Zero rotation did not produce the default direction"},
        {{90.0f, 0.0f, 0.0f}, {0.0f, 0.0f, -1.0f},
         "Directional light X rotation produced the wrong direction"},
        {{0.0f, 90.0f, 0.0f}, {0.0f, -1.0f, 0.0f},
         "Directional light Y rotation produced the wrong direction"},
        {{0.0f, 0.0f, 90.0f}, {1.0f, 0.0f, 0.0f},
         "Directional light Z rotation produced the wrong direction"}};

    bool passed = true;
    for (const RotationCase& rotationCase : axisCases) {
        DirectionalLight light;
        light.rotation = rotationCase.rotation;
        glm::vec3 direction;
        const bool calculated = light.calculateWorldDirection(direction);
        passed &= expect(
            calculated && sameVector(direction, rotationCase.expected) &&
                nearlyEqual(glm::length(direction), 1.0f) &&
                std::isfinite(direction.x) && std::isfinite(direction.y) &&
                std::isfinite(direction.z),
            rotationCase.message);
    }

    const glm::vec3 combinedRotations[] = {
        {30.0f, 45.0f, 0.0f}, {-20.0f, 70.0f, 15.0f}};
    for (const glm::vec3& rotation : combinedRotations) {
        DirectionalLight light;
        light.rotation = rotation;
        glm::vec3 direction;
        const glm::vec3 expected = glm::normalize(expectedDirection(rotation));
        passed &= expect(light.calculateWorldDirection(direction) &&
                             sameVector(direction, expected),
                         "Combined directional-light rotation drifted from "
                         "the Dunamis convention");
    }

    DirectionalLight directlyMutated;
    glm::vec3 initialDirection;
    const bool initialCalculated =
        directlyMutated.calculateWorldDirection(initialDirection);
    directlyMutated.rotation.x += 0.5f;
    directlyMutated.rotation.y += 0.5f;
    glm::vec3 updatedDirection;
    const bool updatedCalculated =
        directlyMutated.calculateWorldDirection(updatedDirection);
    const glm::vec3 expectedUpdated =
        glm::normalize(expectedDirection(directlyMutated.rotation));
    passed &= expect(
        initialCalculated && updatedCalculated &&
            glm::length(updatedDirection - initialDirection) > 1.0e-5f &&
            sameVector(updatedDirection, expectedUpdated),
        "Direct directional-light rotation mutation did not update direction");

    DirectionalLight editorPath;
    editorPath.rotation = {31.0f, -22.0f, 17.0f};
    glm::vec3 editorDirection;
    passed &= expect(
        editorPath.calculateWorldDirection(editorDirection) &&
            sameVector(editorDirection,
                       glm::normalize(expectedDirection(editorPath.rotation))),
        "Editor rotation path did not produce the derived direction");

    DirectionalLight invalid;
    invalid.rotation.x = std::numeric_limits<float>::quiet_NaN();
    glm::vec3 invalidDirection{1.0f, 2.0f, 3.0f};
    passed &= expect(!invalid.calculateWorldDirection(invalidDirection) &&
                         invalidDirection == glm::vec3(0.0f),
                     "Nonfinite directional-light rotation was not rejected");
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
    passed &= runDirectionalLightDirectionTests();
    DirectionalLight light;
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
    parallelLight.rotation = {180.0f, 0.0f, 0.0f};
    directional_shadow::LightMatrices parallelMatrices;
    passed &= expect(static_cast<bool>(directional_shadow::calculateLightMatrices(
                         parallelLight, parallelMatrices)) &&
                         finiteMatrix(parallelMatrices.view),
                     "Parallel world-Y light direction did not choose a robust up vector");

    DirectionalLight invalid = light;
    invalid.rotation.x = std::numeric_limits<float>::quiet_NaN();
    passed &= expectRejected(invalid,
                             "Nonfinite directional-light rotation was accepted");
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
