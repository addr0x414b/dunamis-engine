#include "rendering/directional_shadow.h"
#include "math/transform_math.h"
#include "scene/scene.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

template <typename ObjectType>
ObjectType* addHierarchyObject(TestScene& scene, const char* persistentId) {
    auto object = std::make_unique<ObjectType>();
    object->persistentId = persistentId;
    ObjectType* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) return nullptr;
    return pointer;
}

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
    return glm::vec3(transform_math::makeRotationMatrix(rotation) *
                     glm::vec4(0.0f, -1.0f, 0.0f, 0.0f));
}

glm::vec3 expectedWorldDirection(const DirectionalLight& light) {
    return glm::normalize(glm::vec3(
        light.worldTransformMatrix() * glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)));
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

    DirectionalLight knownConvention;
    knownConvention.rotation = {30.0f, 45.0f, 0.0f};
    glm::vec3 knownDirection;
    passed &= expect(
        knownConvention.calculateWorldDirection(knownDirection) &&
            sameVector(knownDirection,
                       {0.0f, -0.866025f, -0.5f}),
        "Known combined directional-light direction changed");

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

    {
        TestScene scene;
        GameObject* root = addHierarchyObject<GameObject>(
            scene, "direction-root");
        GameObject* parent = addHierarchyObject<GameObject>(
            scene, "direction-parent");
        DirectionalLight* light = addHierarchyObject<DirectionalLight>(
            scene, "direction-light");
        if (!root || !parent || !light) {
            return expect(false, "Could not create directional hierarchy fixture");
        }
        root->rotation = {0.0f, 0.0f, 25.0f};
        root->scale = {1.5f, 1.5f, 1.5f};
        parent->rotation = {15.0f, 20.0f, 5.0f};
        parent->scale = {1.0f, 2.0f, 0.75f};
        light->rotation = {30.0f, -10.0f, 40.0f};
        const Result parentResult = scene.reparentGameObject(
            *parent, root, Scene::ReparentMode::PreserveLocal);
        const Result lightResult = scene.reparentGameObject(
            *light, parent, Scene::ReparentMode::PreserveLocal);
        const glm::vec3 expected = expectedWorldDirection(*light);
        glm::vec3 actual{1.0f};
        const bool calculated = light->calculateWorldDirection(actual);
        directional_shadow::LightMatrices shadowMatrices;
        const Result shadowResult = directional_shadow::calculateLightMatrices(
            *light, shadowMatrices);
        passed &= expect(
            parentResult && lightResult && calculated &&
                sameVector(actual, expected) &&
                nearlyEqual(glm::length(actual), 1.0f) && shadowResult &&
                sameVector(shadowMatrices.normalizedDirection, expected),
            "Parented directional light did not use its complete world transform");
    }

    DirectionalLight invalid;
    invalid.rotation.x = std::numeric_limits<float>::quiet_NaN();
    glm::vec3 invalidDirection{1.0f, 2.0f, 3.0f};
    passed &= expect(!invalid.calculateWorldDirection(invalidDirection) &&
                         invalidDirection == glm::vec3(0.0f),
                     "Nonfinite directional-light rotation was not rejected");

    DirectionalLight unusable;
    unusable.scale = {1.0f, 0.0f, 1.0f};
    glm::vec3 unusableDirection{1.0f, 2.0f, 3.0f};
    passed &= expect(!unusable.calculateWorldDirection(unusableDirection) &&
                         unusableDirection == glm::vec3(0.0f),
                     "An unusable transformed directional vector was accepted");

    DirectionalLight nonfiniteWorld;
    nonfiniteWorld.position.x = std::numeric_limits<float>::infinity();
    glm::vec3 nonfiniteWorldDirection{1.0f, 2.0f, 3.0f};
    passed &= expect(
        !nonfiniteWorld.calculateWorldDirection(nonfiniteWorldDirection) &&
            nonfiniteWorldDirection == glm::vec3(0.0f),
        "A nonfinite directional-light world transform was accepted");
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

    const auto expectInvalid = [&](const auto& mutate, const char* message) {
        DirectionalLight invalid;
        invalid.rotation = light.rotation;
        invalid.shadow = light.shadow;
        mutate(invalid);
        return expectRejected(invalid, message);
    };
    passed &= expectInvalid(
        [](DirectionalLight& invalid) {
            invalid.rotation.x = std::numeric_limits<float>::quiet_NaN();
        },
        "Nonfinite directional-light rotation was accepted");
    passed &= expectInvalid(
        [](DirectionalLight& invalid) {
            invalid.shadow.focus.x = std::numeric_limits<float>::infinity();
        },
        "Nonfinite focus was accepted");
    passed &= expectInvalid(
        [](DirectionalLight& invalid) { invalid.shadow.halfExtent = 0.0f; },
        "Zero half extent was accepted");
    passed &= expectInvalid(
        [](DirectionalLight& invalid) {
            invalid.shadow.nearPlane = invalid.shadow.farPlane;
        },
        "Invalid near/far range was accepted");

    const glm::mat4 firstModel = transform_math::makeModelMatrix(
        glm::vec3(0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::mat4 movedModel = transform_math::makeModelMatrix(
        glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f), glm::vec3(1.0f));
    const glm::vec4 firstLightPosition = matrices.viewProjection * firstModel *
                                         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const glm::vec4 movedLightPosition = matrices.viewProjection * movedModel *
                                         glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    passed &= expect(firstLightPosition != movedLightPosition,
                     "Moved model did not produce a new light-space position");
    return passed ? 0 : 1;
}
