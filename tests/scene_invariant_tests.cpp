#include "scene/scene.h"

#include <cstddef>
#include <cmath>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>


namespace {

class TestScene final : public Scene {
public:
    void init() override {}
    void start() override {}
    void update() override {}
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool nearlyEqual(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

bool hasDirectionalLightError(const Result& result) {
    return result.error().find("Directional light") != std::string::npos;
}

struct CountingPixelDeleter {
    std::size_t* releaseCount = nullptr;

    void operator()(stbi_uc* pixels) const noexcept {
        ++*releaseCount;
        delete[] pixels;
    }
};

}  // namespace

static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().gameObjects()),
              const std::vector<std::unique_ptr<GameObject>>&>);
static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().pointLightAt(0)),
              const PointLight&>);
static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().directionalLight()),
              const DirectionalLight*>);
static_assert(std::is_same_v<
              decltype(std::declval<TestScene&>().activeCamera()),
              const Camera*>);
static_assert(std::is_same_v<
              decltype(std::declval<GameObject&>().meshInstances()),
              const std::vector<MeshInstance>&>);
static_assert(std::is_same_v<
              decltype(std::declval<GameObject&>().loadModel()), Result>);
static_assert(offsetof(MaterialPushConstants, baseColorFactor) == 0);
static_assert(offsetof(MaterialPushConstants, metallicFactor) == 16);
static_assert(offsetof(MaterialPushConstants, roughnessFactor) == 20);
static_assert(offsetof(MaterialPushConstants, alphaMode) == 24);
static_assert(offsetof(MaterialPushConstants, alphaCutoff) == 28);
static_assert(offsetof(MaterialPushConstants, normalMapEnabled) == 32);
static_assert(offsetof(MaterialPushConstants,
                       metallicRoughnessMapEnabled) == 36);
static_assert(sizeof(MaterialPushConstants) == 40);
static_assert(alignof(MaterialPushConstants) % 4 == 0);

int main() {
    bool passed = true;

    Vertex vertex{};
    vertex.normal = {0.0f, 0.0f, 1.0f};
    vertex.tangent = {1.0f, 0.0f, 0.0f, 1.0f};
    Vertex differentNormal = vertex;
    differentNormal.normal.x = 1.0f;
    Vertex differentTangent = vertex;
    differentTangent.tangent.y = 1.0f;
    Vertex differentHandedness = vertex;
    differentHandedness.tangent.w = -1.0f;
    std::unordered_map<Vertex, uint32_t> uniqueVertices;
    uniqueVertices.emplace(vertex, 0);
    uniqueVertices.emplace(differentNormal, 1);
    uniqueVertices.emplace(differentTangent, 2);
    uniqueVertices.emplace(differentHandedness, 3);
    passed &= expect(!(vertex == differentNormal) &&
                         !(vertex == differentTangent) &&
                         !(vertex == differentHandedness) &&
                         uniqueVertices.size() == 4,
                     "Vertex identity collapsed a normal or tangent seam");

    const auto attributes = Vertex::getAttributeDescriptions();
    passed &= expect(attributes.size() == 5 &&
                         Vertex::getBindingDescription().stride == sizeof(Vertex) &&
                         attributes[3].location == 3 &&
                         attributes[3].format == VK_FORMAT_R32G32B32_SFLOAT &&
                         attributes[3].offset == offsetof(Vertex, normal) &&
                         attributes[4].location == 4 &&
                         attributes[4].format == VK_FORMAT_R32G32B32A32_SFLOAT &&
                         attributes[4].offset == offsetof(Vertex, tangent),
                     "Vertex normal/tangent Vulkan layout is incorrect");

    const Material defaultMaterial;
    passed &= expect(defaultMaterial.baseColorFactor == glm::vec4(1.0f) &&
                         defaultMaterial.metallicFactor == 1.0f &&
                         defaultMaterial.roughnessFactor == 1.0f &&
                         !defaultMaterial.hasMetallicRoughnessMap &&
                         !defaultMaterial.normalMapEnabled &&
                         defaultMaterial.normalMapPath.empty() &&
                         defaultMaterial.normalMapPixels == nullptr &&
                         defaultMaterial.normalMapWidth == 0 &&
                         defaultMaterial.normalMapHeight == 0 &&
                         defaultMaterial.normalMapChannels == 0 &&
                         defaultMaterial.normalMapMipLevels == 0 &&
                         defaultMaterial.normalMapImage == VK_NULL_HANDLE &&
                         defaultMaterial.normalMapImageMemory == VK_NULL_HANDLE &&
                         defaultMaterial.normalMapImageView == VK_NULL_HANDLE &&
                         defaultMaterial.normalMapSampler == VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapPath.empty() &&
                         defaultMaterial.metallicRoughnessMapPixels == nullptr &&
                         defaultMaterial.metallicRoughnessMapWidth == 0 &&
                         defaultMaterial.metallicRoughnessMapHeight == 0 &&
                         defaultMaterial.metallicRoughnessMapChannels == 0 &&
                         defaultMaterial.metallicRoughnessMapMipLevels == 0 &&
                         defaultMaterial.metallicRoughnessMapImage ==
                             VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapImageMemory ==
                             VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapImageView ==
                             VK_NULL_HANDLE &&
                         defaultMaterial.metallicRoughnessMapSampler ==
                             VK_NULL_HANDLE,
                     "Default material state is not cleanup-safe");

    passed &= expect(scene_limits::maxPointLights == 16,
                     "The point-light limit is not 16");

    const PointLight defaultLight;
    passed &= expect(defaultLight.color.x == 1.0f &&
                         defaultLight.color.y == 1.0f &&
                         defaultLight.color.z == 1.0f,
                     "Point lights do not default to white");
    passed &= expect(defaultLight.intensity == 1.0f,
                     "Point lights do not default to intensity 1");
    passed &= expect(defaultLight.position.x == 0.0f &&
                         defaultLight.position.y == 0.0f &&
                         defaultLight.position.z == 0.0f,
                     "Point lights do not default to the origin");

    std::size_t pixelReleaseCount = 0;
    Material firstSharedMaterial;
    firstSharedMaterial.pixelsOwner = StbiPixelOwner(
        new stbi_uc[4], CountingPixelDeleter{&pixelReleaseCount});
    firstSharedMaterial.pixels = firstSharedMaterial.pixelsOwner.get();
    Material secondSharedMaterial = firstSharedMaterial;
    releaseStbiPixel(firstSharedMaterial.pixelsOwner,
                     firstSharedMaterial.pixels);
    passed &= expect(pixelReleaseCount == 0 && secondSharedMaterial.pixels != nullptr,
                     "Releasing one shared pixel reference destroyed live pixels");
    releaseStbiPixel(secondSharedMaterial.pixelsOwner,
                     secondSharedMaterial.pixels);
    passed &= expect(pixelReleaseCount == 1,
                     "Shared pixel allocation was not released exactly once");

    const DirectionalLight defaultDirectionalLight;
    passed &= expect(defaultDirectionalLight.direction ==
                         glm::vec3(0.0f, -1.0f, 0.0f),
                     "Directional lights do not default to downward rays");
    passed &= expect(defaultDirectionalLight.color == glm::vec3(1.0f),
                     "Directional lights do not default to white");
    passed &= expect(defaultDirectionalLight.intensity == 1.0f,
                     "Directional lights do not default to intensity 1");

    TestScene scene;
    passed &= expect(scene.directionalLight() == nullptr,
                     "A new scene did not start without a directional light");
    passed &= expect(nearlyEqual(scene.backgroundColor().r, 0.639f) &&
                         nearlyEqual(scene.backgroundColor().g, 0.965f) &&
                         nearlyEqual(scene.backgroundColor().b, 1.0f) &&
                         nearlyEqual(scene.backgroundColor().a, 1.0f),
                     "A new scene has the wrong background color");
    passed &= expect(scene.ambientColor() == glm::vec3(1.0f) &&
                         nearlyEqual(scene.ambientIntensity(), 0.1f),
                     "A new scene has the wrong ambient light");

    const glm::vec4 background{0.2f, 0.3f, 0.4f, 0.5f};
    passed &= expect(static_cast<bool>(scene.setBackgroundColor(background)) &&
                         scene.backgroundColor() == background,
                     "A valid background color was rejected");
    passed &= expect(!scene.setBackgroundColor(glm::vec4(-0.1f, 0.3f, 0.4f, 0.5f)) &&
                         scene.backgroundColor() == background,
                     "A negative background component was accepted");
    passed &= expect(!scene.setBackgroundColor(glm::vec4(0.2f, 1.1f, 0.4f, 0.5f)) &&
                         scene.backgroundColor() == background,
                     "An oversized background component was accepted");
    passed &= expect(!scene.setBackgroundColor(glm::vec4(
                             std::numeric_limits<float>::quiet_NaN(),
                             0.3f, 0.4f, 0.5f)) &&
                         scene.backgroundColor() == background,
                     "A non-finite background component was accepted");

    const glm::vec3 ambient{0.3f, 0.4f, 0.5f};
    passed &= expect(static_cast<bool>(scene.setAmbientLight(ambient, 2.0f)) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A valid ambient light was rejected");
    passed &= expect(!scene.setAmbientLight(glm::vec3(-0.1f, 0.4f, 0.5f), 2.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A negative ambient component was accepted");
    passed &= expect(!scene.setAmbientLight(glm::vec3(0.3f, 1.1f, 0.5f), 2.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "An oversized ambient component was accepted");
    passed &= expect(!scene.setAmbientLight(ambient, -1.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A negative ambient intensity was accepted");
    passed &= expect(!scene.setAmbientLight(
                             glm::vec3(std::numeric_limits<float>::infinity(),
                                       0.4f, 0.5f),
                             2.0f) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A non-finite ambient component was accepted");
    passed &= expect(!scene.setAmbientLight(
                             ambient,
                             std::numeric_limits<float>::infinity()) &&
                         scene.ambientColor() == ambient &&
                         nearlyEqual(scene.ambientIntensity(), 2.0f),
                     "A non-finite ambient intensity was accepted");

    passed &= expect(scene.pointLightCount() == 0,
                     "A new scene did not start with zero point lights");
    bool zeroLightLookupRejected = false;
    try {
        (void)scene.pointLightAt(0);
    } catch (const std::out_of_range&) {
        zeroLightLookupRejected = true;
    }
    passed &= expect(zeroLightLookupRejected,
                     "A zero-light scene accepted a light lookup");
    passed &= expect(!scene.validateForActivation(),
                     "A scene without an active camera passed validation");
    passed &= expect(
        !scene.addGameObject(std::unique_ptr<GameObject>{}),
        "A scene accepted a null game object");

    const auto camera = std::make_shared<Camera>();
    const Result cameraResult = scene.setActiveCamera(camera);
    passed &= expect(static_cast<bool>(cameraResult),
                     "A scene rejected a valid active camera");
    passed &= expect(scene.activeCamera() == camera.get(),
                     "A scene did not retain its active camera");
    passed &= expect(
        !scene.setActiveCamera(std::shared_ptr<Camera>{}),
        "A scene accepted a null active camera");
    passed &= expect(scene.activeCamera() == camera.get(),
                     "A rejected null camera replaced the active camera");
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A valid zero-light scene failed validation");

    const std::size_t objectsBeforeDirectional = scene.gameObjects().size();
    const std::size_t pointsBeforeDirectional = scene.pointLightCount();
    auto directionalLight = std::make_unique<DirectionalLight>();
    const DirectionalLight* expectedDirectionalLight = directionalLight.get();
    const Result directionalResult =
        scene.addGameObject(std::move(directionalLight));
    passed &= expect(static_cast<bool>(directionalResult),
                     "A valid directional light was rejected");
    passed &= expect(scene.directionalLight() == expectedDirectionalLight,
                     "The scene did not retain the registered directional light");
    passed &= expect(scene.gameObjects().size() == objectsBeforeDirectional + 1,
                     "Adding a directional light did not add one owning object");
    passed &= expect(scene.pointLightCount() == pointsBeforeDirectional,
                     "Adding a directional light changed point-light registration");

    const std::size_t objectsBeforeSecondDirectional = scene.gameObjects().size();
    const std::size_t pointsBeforeSecondDirectional = scene.pointLightCount();
    const Result secondDirectionalResult = scene.addGameObject(
        std::make_unique<DirectionalLight>());
    passed &= expect(!static_cast<bool>(secondDirectionalResult),
                     "A scene accepted a second directional light");
    passed &= expect(scene.directionalLight() == expectedDirectionalLight,
                     "Rejecting a second directional light changed registration");
    passed &= expect(scene.gameObjects().size() == objectsBeforeSecondDirectional,
                     "Rejecting a second directional light changed ownership");
    passed &= expect(scene.pointLightCount() == pointsBeforeSecondDirectional,
                     "Rejecting a second directional light changed point lights");
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A valid scene with one directional light failed validation");

    auto expectInvalidDirectionalState =
        [&passed](const char* message, const auto& configure) {
            TestScene invalidScene;
            const Result cameraResult = invalidScene.setActiveCamera(
                std::make_shared<Camera>());
            if (!expect(static_cast<bool>(cameraResult),
                        "An invalid-light test could not set its camera")) {
                passed = false;
                return;
            }

            auto light = std::make_unique<DirectionalLight>();
            configure(*light);
            const Result addResult = invalidScene.addGameObject(
                std::move(light));
            if (!expect(static_cast<bool>(addResult),
                        "An invalid directional light could not be added")) {
                passed = false;
                return;
            }

            const Result validationResult =
                invalidScene.validateForActivation();
            passed &= expect(!static_cast<bool>(validationResult) &&
                                 hasDirectionalLightError(validationResult),
                             message);
        };

    expectInvalidDirectionalState(
        "A zero directional-light direction passed validation",
        [](DirectionalLight& light) { light.direction = glm::vec3(0.0f); });
    expectInvalidDirectionalState(
        "A non-finite directional-light direction passed validation",
        [](DirectionalLight& light) {
            light.direction.x = std::numeric_limits<float>::quiet_NaN();
        });
    expectInvalidDirectionalState(
        "A negative directional-light intensity passed validation",
        [](DirectionalLight& light) { light.intensity = -1.0f; });
    expectInvalidDirectionalState(
        "A non-finite directional-light intensity passed validation",
        [](DirectionalLight& light) {
            light.intensity = std::numeric_limits<float>::infinity();
        });
    expectInvalidDirectionalState(
        "A negative directional-light color passed validation",
        [](DirectionalLight& light) { light.color.r = -1.0f; });
    expectInvalidDirectionalState(
        "A non-finite directional-light color passed validation",
        [](DirectionalLight& light) {
            light.color.g = std::numeric_limits<float>::infinity();
        });

    for (std::size_t index = 0;
         index < scene_limits::maxPointLights; ++index) {
        auto light = std::make_unique<PointLight>();
        const PointLight* expectedLight = light.get();
        const Result addResult = scene.addGameObject(std::move(light));
        passed &= expect(static_cast<bool>(addResult),
                         "A point light within the limit was rejected");
        if (addResult) {
            passed &= expect(&scene.pointLightAt(index) == expectedLight,
                             "A point light was not registered exactly once");
        }
    }

    passed &= expect(
        scene.pointLightCount() == scene_limits::maxPointLights,
        "The scene did not contain exactly 16 point lights");
    passed &= expect(
        scene.gameObjects().size() == scene_limits::maxPointLights + 1,
        "The 16 point lights did not preserve the directional-light object");
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A valid 16-light scene failed validation");

    const Result seventeenthResult =
        scene.addGameObject(std::make_unique<PointLight>());
    passed &= expect(!static_cast<bool>(seventeenthResult),
                     "A scene accepted a seventeenth point light");
    passed &= expect(
        scene.pointLightCount() == scene_limits::maxPointLights,
        "Rejecting the seventeenth light changed the light count");
    passed &= expect(
        scene.gameObjects().size() == scene_limits::maxPointLights + 1,
        "Rejecting the seventeenth light changed object ownership");
    bool seventeenthLookupRejected = false;
    try {
        (void)scene.pointLightAt(scene_limits::maxPointLights);
    } catch (const std::out_of_range&) {
        seventeenthLookupRejected = true;
    }
    passed &= expect(seventeenthLookupRejected,
                     "The light accessor accepted index 16");

    auto ordinaryObject = std::make_unique<GameObject>();
    passed &= expect(ordinaryObject->meshInstances().empty(),
                     "A new game object did not start without meshes");
    MeshInstance meshInstance{};
    meshInstance.mesh.indices.push_back(42);
    const Result meshResult = ordinaryObject->addMeshInstance(
        std::move(meshInstance));
    passed &= expect(static_cast<bool>(meshResult),
                     "An inactive game object rejected a mesh instance");
    passed &= expect(ordinaryObject->meshInstances().size() == 1 &&
                         ordinaryObject->meshInstances().front()
                                 .mesh.indices.front() == 42,
                     "The checked mesh API did not preserve one mesh");
    const Result ordinaryObjectResult =
        scene.addGameObject(std::move(ordinaryObject));
    passed &= expect(static_cast<bool>(ordinaryObjectResult),
                     "The point-light limit rejected an ordinary object");
    passed &= expect(
        scene.pointLightCount() == scene_limits::maxPointLights,
        "Adding an ordinary object changed the point-light count");

    return passed ? 0 : 1;
}
