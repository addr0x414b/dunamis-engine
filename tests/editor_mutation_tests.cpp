#include "editor/editor_mutation.h"
#include "math/transform_math.h"
#include "scene/camera.h"
#include "scene/directional_light.h"
#include "scene/game_object.h"
#include "scene/point_light.h"
#include "scene/scene.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace {

class AttachedCameraObject final : public GameObject {
public:
    Camera camera;

    Camera* attachedCamera() noexcept override { return &camera; }
    const Camera* attachedCamera() const noexcept override {
        return &camera;
    }
};

class MutationTestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second,
                float tolerance = 1.0e-5f) {
    return glm::length(first - second) <= tolerance;
}

bool runNameTests() {
    GameObject object;
    object.name = "Pot";
    object.persistentId = "authored-id";

    bool passed = expect(static_cast<bool>(editor_mutation::applyName(
                             object, "Flower Pot")),
                         "Valid name mutation failed");
    passed &= expect(object.name == "Flower Pot",
                     "Valid name mutation did not update the object");
    passed &= expect(object.persistentId == "authored-id",
                     "Name mutation changed persistent identity");

    passed &= expect(static_cast<bool>(editor_mutation::applyName(object, "")) &&
                         object.name.empty() &&
                         object.persistentId == "authored-id",
                     "Name mutation did not preserve empty-name semantics");
    return passed;
}

bool runPositionTests() {
    GameObject object;
    object.position = {1.0f, 2.0f, 3.0f};
    bool passed = expect(static_cast<bool>(editor_mutation::applyPosition(
                             object, {4.0f, 5.0f, 6.0f})),
                         "Valid position mutation failed");
    passed &= expect(sameVector(object.position, {4.0f, 5.0f, 6.0f}),
                     "Valid position did not update the object");

    AttachedCameraObject attachedObject;
    attachedObject.position = {10.0f, 20.0f, 30.0f};
    attachedObject.camera.position = {12.0f, 19.0f, 34.0f};
    const glm::vec3 originalCameraPosition = attachedObject.camera.position;
    const glm::vec3 newOwnerPosition{15.0f, 16.0f, 35.0f};
    const glm::vec3 ownerDelta = newOwnerPosition - attachedObject.position;
    passed &= expect(static_cast<bool>(editor_mutation::applyPosition(
                             attachedObject, newOwnerPosition)),
                     "Attached-camera position mutation failed");
    passed &= expect(
        sameVector(attachedObject.camera.position,
                   originalCameraPosition + ownerDelta),
        "Attached Camera did not move by the exact owner position delta");

    {
        MutationTestScene scene;
        auto parentObject = std::make_unique<GameObject>();
        auto attachedObjectInScene = std::make_unique<AttachedCameraObject>();
        GameObject* parent = parentObject.get();
        AttachedCameraObject* child = attachedObjectInScene.get();
        parent->persistentId = "mutation-position-parent";
        child->persistentId = "mutation-position-child";
        if (!scene.addGameObject(std::move(parentObject)) ||
            !scene.addGameObject(std::move(attachedObjectInScene))) {
            return expect(false, "Could not create hierarchy position fixture");
        }

        parent->position = {100.0f, 50.0f, -10.0f};
        parent->rotation = {10.0f, 20.0f, 90.0f};
        parent->scale = {2.0f, 3.0f, 1.5f};
        child->position = {10.0f, 5.0f, 2.0f};
        child->rotation = {15.0f, -20.0f, 25.0f};
        child->scale = {1.5f, 0.75f, 2.0f};
        child->camera.position = {130.0f, 75.0f, 4.0f};
        if (!scene.reparentGameObject(
                *child, parent, MutationTestScene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not parent hierarchy position fixture");
        }

        const glm::vec3 oldCameraPosition = child->camera.position;
        const glm::vec3 newLocalPosition{12.0f, 8.0f, 1.0f};
        const glm::mat4 oldOwnerWorld = child->worldTransformMatrix();
        const glm::mat4 candidateOwnerWorld = parent->worldTransformMatrix() *
            transform_math::makeModelMatrix(
                newLocalPosition, child->rotation, child->scale);
        const glm::vec3 expectedWorldDelta =
            glm::vec3(candidateOwnerWorld[3]) - glm::vec3(oldOwnerWorld[3]);

        passed &= expect(static_cast<bool>(editor_mutation::applyPosition(
                             *child, newLocalPosition)),
                         "Parented attached-camera position mutation failed");
        passed &= expect(child->position == newLocalPosition &&
                             sameVector(child->camera.position,
                                        oldCameraPosition + expectedWorldDelta),
                         "Parented attached Camera did not follow the owner's world position delta");
    }

    Camera standaloneCamera;
    standaloneCamera.position = {7.0f, 8.0f, 9.0f};
    passed &= expect(static_cast<bool>(editor_mutation::applyPosition(
                             standaloneCamera, {11.0f, 12.0f, 13.0f})),
                     "Standalone Camera position mutation failed");
    passed &= expect(sameVector(standaloneCamera.position, {11.0f, 12.0f, 13.0f}),
                     "Standalone Camera position did not update normally");

    AttachedCameraObject invalidObject;
    invalidObject.position = {2.0f, 3.0f, 4.0f};
    invalidObject.camera.position = {5.0f, 6.0f, 7.0f};
    const glm::vec3 originalObjectPosition = invalidObject.position;
    const glm::vec3 originalAttachedPosition = invalidObject.camera.position;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Result invalidResult = editor_mutation::applyPosition(
        invalidObject, {nan, 0.0f, 0.0f});
    passed &= expect(!invalidResult &&
                         invalidResult.error() ==
                             "Transform values must be finite.",
                     "Non-finite position was not rejected with a useful error");
    passed &= expect(sameVector(invalidObject.position, originalObjectPosition),
                     "Rejected position mutated the object");
    passed &= expect(sameVector(invalidObject.camera.position,
                                originalAttachedPosition),
                     "Rejected position mutated the attached Camera");
    return passed;
}

bool runRotationTests() {
    GameObject object;
    object.rotation = {4.0f, 5.0f, 6.0f};
    bool passed = expect(static_cast<bool>(editor_mutation::applyRotation(
                             object, {14.0f, 15.0f, 16.0f})),
                         "Valid object rotation mutation failed");
    passed &= expect(sameVector(object.rotation, {14.0f, 15.0f, 16.0f}),
                     "Valid rotation did not update the object");

    AttachedCameraObject attachedObject;
    attachedObject.position = {10.0f, 2.0f, 3.0f};
    attachedObject.rotation = glm::vec3(0.0f);
    attachedObject.camera.position = {12.0f, 2.0f, 3.0f};
    attachedObject.camera.front = {0.0f, 0.0f, -1.0f};
    attachedObject.camera.up = {0.0f, 1.0f, 0.0f};
    const glm::vec3 originalOffset =
        attachedObject.camera.position - attachedObject.position;
    const glm::vec3 originalFront = attachedObject.camera.front;
    const glm::vec3 originalUp = attachedObject.camera.up;
    const glm::vec3 attachedNewRotation{0.0f, 90.0f, 0.0f};
    const glm::mat4 attachedDelta =
        transform_math::makeRotationMatrix(attachedNewRotation) *
        glm::inverse(transform_math::makeRotationMatrix(
            attachedObject.rotation));
    const glm::vec3 expectedAttachedPosition =
        attachedObject.position +
        glm::vec3(attachedDelta * glm::vec4(originalOffset, 0.0f));
    const glm::vec3 expectedAttachedFront = glm::normalize(
        glm::mat3(attachedDelta) * originalFront);
    const glm::vec3 expectedAttachedUp =
        glm::normalize(glm::mat3(attachedDelta) * originalUp);
    passed &= expect(static_cast<bool>(editor_mutation::applyRotation(
                             attachedObject, attachedNewRotation)),
                     "Attached-camera rotation mutation failed");
    passed &= expect(sameVector(attachedObject.camera.position,
                                expectedAttachedPosition),
                     "Attached Camera positional orbit was incorrect");
    passed &= expect(sameVector(attachedObject.camera.front,
                                expectedAttachedFront),
                     "Attached Camera front did not receive the orientation delta");
    passed &= expect(sameVector(attachedObject.camera.up, expectedAttachedUp),
                     "Attached Camera up did not receive the orientation delta");

    {
        MutationTestScene scene;
        auto parentObject = std::make_unique<GameObject>();
        auto attachedObjectInScene = std::make_unique<AttachedCameraObject>();
        GameObject* parent = parentObject.get();
        AttachedCameraObject* child = attachedObjectInScene.get();
        parent->persistentId = "mutation-rotation-parent";
        child->persistentId = "mutation-rotation-child";
        if (!scene.addGameObject(std::move(parentObject)) ||
            !scene.addGameObject(std::move(attachedObjectInScene))) {
            return expect(false, "Could not create hierarchy rotation fixture");
        }

        parent->position = {100.0f, 50.0f, -10.0f};
        parent->rotation = {15.0f, 25.0f, 40.0f};
        parent->scale = {2.0f, 3.0f, 1.5f};
        child->position = {10.0f, 5.0f, 2.0f};
        child->rotation = {15.0f, -20.0f, 25.0f};
        child->scale = {1.5f, 0.75f, 2.0f};
        child->camera.position = {130.0f, 75.0f, 4.0f};
        child->camera.front = glm::normalize(glm::vec3(0.3f, 0.2f, -0.9f));
        child->camera.up = glm::vec3(0.0f, 1.0f, 0.0f);
        if (!scene.reparentGameObject(
                *child, parent, MutationTestScene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not parent hierarchy rotation fixture");
        }

        const glm::mat4 oldOwnerWorld = child->worldTransformMatrix();
        const glm::mat4 newOwnerWorld = parent->worldTransformMatrix() *
            transform_math::makeModelMatrix(
                child->position, {35.0f, 10.0f, -15.0f}, child->scale);
        const glm::mat4 inverseOldOwnerWorld = glm::inverse(oldOwnerWorld);
        const glm::vec3 oldCameraPosition = child->camera.position;
        const glm::vec3 oldCameraFront = child->camera.front;
        const glm::vec3 oldCameraUp = child->camera.up;
        const glm::vec3 expectedCameraPosition = glm::vec3(
            newOwnerWorld *
            (inverseOldOwnerWorld * glm::vec4(oldCameraPosition, 1.0f)));
        const glm::vec3 expectedCameraFront = glm::normalize(glm::vec3(
            newOwnerWorld *
            (inverseOldOwnerWorld * glm::vec4(oldCameraFront, 0.0f))));
        const glm::vec3 expectedCameraUp = glm::normalize(glm::vec3(
            newOwnerWorld *
            (inverseOldOwnerWorld * glm::vec4(oldCameraUp, 0.0f))));

        passed &= expect(static_cast<bool>(editor_mutation::applyRotation(
                             *child, {35.0f, 10.0f, -15.0f})),
                         "Parented attached-camera rotation mutation failed");
        passed &= expect(child->rotation == glm::vec3(35.0f, 10.0f, -15.0f) &&
                             sameVector(child->camera.position,
                                        expectedCameraPosition) &&
                             sameVector(child->camera.front,
                                        expectedCameraFront) &&
                             sameVector(child->camera.up, expectedCameraUp),
                         "Parented attached Camera did not follow the owner world-frame rotation");
        passed &= expect(std::fabs(glm::length(child->camera.front) - 1.0f) <
                             1.0e-5f &&
                             std::fabs(glm::length(child->camera.up) - 1.0f) <
                                 1.0e-5f &&
                             glm::length(glm::cross(child->camera.front,
                                                    child->camera.up)) >
                                 1.0e-4f,
                         "Parented attached Camera rotation produced an invalid basis");
    }

    Camera standaloneCamera;
    standaloneCamera.position = {20.0f, 21.0f, 22.0f};
    standaloneCamera.rotation = glm::vec3(0.0f);
    standaloneCamera.front = {0.0f, 0.0f, -1.0f};
    standaloneCamera.up = {0.0f, 1.0f, 0.0f};
    const glm::vec3 standalonePosition = standaloneCamera.position;
    const glm::vec3 standaloneFront = standaloneCamera.front;
    const glm::vec3 standaloneUp = standaloneCamera.up;
    const glm::vec3 standaloneNewRotation{15.0f, -25.0f, 35.0f};
    const glm::mat4 standaloneDelta =
        transform_math::makeRotationMatrix(standaloneNewRotation);
    passed &= expect(static_cast<bool>(editor_mutation::applyRotation(
                             standaloneCamera, standaloneNewRotation)),
                     "Standalone Camera rotation mutation failed");
    passed &= expect(sameVector(standaloneCamera.position, standalonePosition),
                     "Standalone Camera rotation changed its position");
    passed &= expect(sameVector(
                         standaloneCamera.front,
                         glm::normalize(glm::mat3(standaloneDelta) * standaloneFront)),
                     "Standalone Camera front did not follow editor rotation");
    passed &= expect(sameVector(
                         standaloneCamera.up,
                         glm::normalize(glm::mat3(standaloneDelta) * standaloneUp)),
                     "Standalone Camera up did not follow editor rotation");

    {
        MutationTestScene scene;
        auto parentObject = std::make_unique<GameObject>();
        auto cameraObject = std::make_unique<Camera>();
        GameObject* parent = parentObject.get();
        Camera* camera = cameraObject.get();
        parent->persistentId = "standalone-camera-parent";
        camera->persistentId = "standalone-camera-child";
        if (!scene.addGameObject(std::move(parentObject)) ||
            !scene.addGameObject(std::move(cameraObject))) {
            return expect(false, "Could not create parented standalone Camera fixture");
        }
        parent->position = {100.0f, 50.0f, -10.0f};
        parent->rotation = {10.0f, 20.0f, 90.0f};
        parent->scale = {2.0f, 3.0f, 1.5f};
        camera->position = {1.0f, 2.0f, 3.0f};
        camera->rotation = glm::vec3(0.0f);
        camera->front = {0.0f, 0.0f, -1.0f};
        camera->up = {0.0f, 1.0f, 0.0f};
        if (!scene.reparentGameObject(
                *camera, parent, MutationTestScene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not parent standalone Camera fixture");
        }

        const glm::vec3 newLocalPosition{4.0f, 5.0f, 6.0f};
        const glm::vec3 newLocalRotation{15.0f, -25.0f, 35.0f};
        const glm::vec3 originalFront = camera->front;
        const glm::vec3 originalUp = camera->up;
        passed &= expect(static_cast<bool>(editor_mutation::applyPosition(
                             *camera, newLocalPosition)) &&
                             camera->position == newLocalPosition,
                         "Parented standalone Camera position was not local");
        const glm::mat3 localRotation = glm::mat3(
            transform_math::makeRotationMatrix(newLocalRotation));
        passed &= expect(static_cast<bool>(editor_mutation::applyRotation(
                             *camera, newLocalRotation)) &&
                             sameVector(camera->front,
                                        glm::normalize(localRotation * originalFront)) &&
                             sameVector(camera->up,
                                        glm::normalize(localRotation * originalUp)),
                         "Parented standalone Camera mutation used parent orientation");
    }

    const glm::vec3 authoredRotation{23.0f, -41.0f, 17.0f};
    const glm::vec3 authoredScale{-2.0f, 3.0f, -4.0f};
    const glm::mat4 authoredMatrix = transform_math::makeModelMatrix(
        {3.0f, 4.0f, 5.0f}, authoredRotation, authoredScale);
    glm::vec3 extractedRotation;
    const Result extractionResult = editor_mutation::extractDunamisRotation(
        authoredMatrix, authoredScale, extractedRotation);
    passed &= expect(static_cast<bool>(extractionResult),
                     "Dunamis rotation extraction failed");
    passed &= expect(sameVector(extractedRotation, authoredRotation, 1.0e-4f),
                     "Dunamis authored Euler convention changed");

    AttachedCameraObject invalidObject;
    invalidObject.position = {1.0f, 2.0f, 3.0f};
    invalidObject.rotation = {4.0f, 5.0f, 6.0f};
    invalidObject.camera.position = {7.0f, 8.0f, 9.0f};
    invalidObject.camera.front = {0.0f, 0.0f, -1.0f};
    invalidObject.camera.up = {0.0f, 1.0f, 0.0f};
    const glm::vec3 invalidOriginalRotation = invalidObject.rotation;
    const glm::vec3 invalidOriginalPosition = invalidObject.camera.position;
    const glm::vec3 invalidOriginalFront = invalidObject.camera.front;
    const glm::vec3 invalidOriginalUp = invalidObject.camera.up;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Result invalidResult = editor_mutation::applyRotation(
        invalidObject, {nan, 0.0f, 0.0f});
    passed &= expect(!invalidResult &&
                         invalidResult.error() ==
                             "Transform values must be finite.",
                     "Non-finite rotation was not rejected");
    passed &= expect(sameVector(invalidObject.rotation, invalidOriginalRotation) &&
                         sameVector(invalidObject.camera.position,
                                    invalidOriginalPosition) &&
                         sameVector(invalidObject.camera.front,
                                    invalidOriginalFront) &&
                         sameVector(invalidObject.camera.up, invalidOriginalUp),
                     "Rejected rotation partially mutated object or Camera");

    AttachedCameraObject invalidCameraObject;
    invalidCameraObject.position = {1.0f, 2.0f, 3.0f};
    invalidCameraObject.rotation = {4.0f, 5.0f, 6.0f};
    invalidCameraObject.camera.position = {7.0f, 8.0f, 9.0f};
    invalidCameraObject.camera.front = {0.0f, 0.0f, -1.0f};
    invalidCameraObject.camera.up = glm::vec3(0.0f);
    const glm::vec3 invalidCameraRotation = invalidCameraObject.rotation;
    const glm::vec3 invalidCameraPosition = invalidCameraObject.camera.position;
    const glm::vec3 invalidCameraFront = invalidCameraObject.camera.front;
    const glm::vec3 invalidCameraUp = invalidCameraObject.camera.up;
    const Result invalidCameraResult = editor_mutation::applyRotation(
        invalidCameraObject, {10.0f, 20.0f, 30.0f});
    passed &= expect(!invalidCameraResult,
                     "Invalid Camera orientation was accepted for rotation");
    passed &= expect(
        sameVector(invalidCameraObject.rotation, invalidCameraRotation) &&
            sameVector(invalidCameraObject.camera.position,
                       invalidCameraPosition) &&
            sameVector(invalidCameraObject.camera.front, invalidCameraFront) &&
            sameVector(invalidCameraObject.camera.up, invalidCameraUp),
        "Failed Camera rotation left partial state behind");

    {
        MutationTestScene scene;
        auto parentObject = std::make_unique<GameObject>();
        auto attachedObjectInScene = std::make_unique<AttachedCameraObject>();
        GameObject* parent = parentObject.get();
        AttachedCameraObject* child = attachedObjectInScene.get();
        parent->persistentId = "singular-mutation-parent";
        child->persistentId = "singular-mutation-child";
        if (!scene.addGameObject(std::move(parentObject)) ||
            !scene.addGameObject(std::move(attachedObjectInScene))) {
            return expect(false, "Could not create singular mutation fixture");
        }
        parent->scale = {0.0f, 1.0f, 1.0f};
        child->position = {2.0f, 3.0f, 4.0f};
        child->camera.position = {7.0f, 8.0f, 9.0f};
        child->camera.front = {0.0f, 0.0f, -1.0f};
        child->camera.up = {0.0f, 1.0f, 0.0f};
        if (!scene.reparentGameObject(
                *child, parent, MutationTestScene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not parent singular mutation fixture");
        }

        const glm::vec3 originalPosition = child->position;
        const glm::vec3 originalRotation = child->rotation;
        const glm::vec3 originalCameraPosition = child->camera.position;
        const glm::vec3 originalCameraFront = child->camera.front;
        const glm::vec3 originalCameraUp = child->camera.up;
        const Result result = editor_mutation::applyRotation(
            *child, {10.0f, 20.0f, 30.0f});
        passed &= expect(!result && child->position == originalPosition &&
                             child->rotation == originalRotation &&
                             child->camera.position == originalCameraPosition &&
                             child->camera.front == originalCameraFront &&
                             child->camera.up == originalCameraUp,
                         "Singular attached-camera rotation was not transactional");
    }
    return passed;
}

bool runScaleTests() {
    GameObject object;
    object.scale = {1.0f, 2.0f, 3.0f};
    bool passed = expect(static_cast<bool>(editor_mutation::applyScale(
                             object, {4.0f, 5.0f, 6.0f})),
                         "Valid scale mutation failed");
    passed &= expect(sameVector(object.scale, {4.0f, 5.0f, 6.0f}),
                     "Nonuniform scale did not apply");
    passed &= expect(static_cast<bool>(editor_mutation::applyScale(
                             object, {-4.0f, 5.0f, -6.0f})),
                     "Negative scale mutation was rejected");
    passed &= expect(sameVector(object.scale, {-4.0f, 5.0f, -6.0f}),
                     "Negative authored scale did not persist");

    const glm::vec3 originalScale = object.scale;
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const Result invalidResult =
        editor_mutation::applyScale(object, {nan, 1.0f, 1.0f});
    passed &= expect(!invalidResult,
                     "Non-finite scale was not rejected");
    passed &= expect(sameVector(object.scale, originalScale),
                     "Rejected scale mutated the object");

    const glm::vec3 authoredSigns{-1.0f, 1.0f, -1.0f};
    const glm::mat4 manipulatedMatrix = transform_math::makeModelMatrix(
        glm::vec3(0.0f), {19.0f, -27.0f, 13.0f}, {4.0f, 2.5f, 7.0f});
    glm::vec3 extractedScale;
    const Result extractionResult = editor_mutation::extractDunamisScale(
        manipulatedMatrix, authoredSigns, extractedScale);
    passed &= expect(static_cast<bool>(extractionResult),
                     "Dunamis scale extraction failed");
    passed &= expect(sameVector(extractedScale, {-4.0f, 2.5f, -7.0f}),
                     "Authored scale sign preservation changed");
    return passed;
}

bool runLightTests() {
    bool passed = true;
    const float nan = std::numeric_limits<float>::quiet_NaN();

    PointLight pointLight;
    pointLight.color = {1.0f, 2.0f, 3.0f};
    pointLight.intensity = 4.0f;
    passed &= expect(static_cast<bool>(editor_mutation::applyPointLightColor(
                             pointLight, {0.0f, 1.5f, 2.0f})),
                     "Valid PointLight color was rejected");
    passed &= expect(sameVector(pointLight.color, {0.0f, 1.5f, 2.0f}),
                     "Valid PointLight color did not apply");
    const glm::vec3 pointColor = pointLight.color;
    passed &= expect(!editor_mutation::applyPointLightColor(
                         pointLight, {-1.0f, 0.0f, 0.0f}) &&
                         sameVector(pointLight.color, pointColor),
                     "Negative PointLight color was not rejected transactionally");
    passed &= expect(!editor_mutation::applyPointLightColor(
                         pointLight, {nan, 0.0f, 0.0f}) &&
                         sameVector(pointLight.color, pointColor),
                     "Non-finite PointLight color was not rejected transactionally");
    passed &= expect(static_cast<bool>(editor_mutation::applyPointLightIntensity(
                             pointLight, 8.0f)),
                     "Valid PointLight intensity was rejected");
    const float pointIntensity = pointLight.intensity;
    passed &= expect(!editor_mutation::applyPointLightIntensity(pointLight, -1.0f) &&
                         pointLight.intensity == pointIntensity,
                     "Negative PointLight intensity was not rejected transactionally");
    passed &= expect(!editor_mutation::applyPointLightIntensity(pointLight, nan) &&
                         pointLight.intensity == pointIntensity,
                     "Non-finite PointLight intensity was not rejected transactionally");

    DirectionalLight directionalLight;
    directionalLight.color = {1.0f, 2.0f, 3.0f};
    directionalLight.intensity = 4.0f;
    passed &= expect(static_cast<bool>(
                         editor_mutation::applyDirectionalLightColor(
                             directionalLight, {0.0f, 1.5f, 2.0f})),
                     "Valid DirectionalLight color was rejected");
    passed &= expect(sameVector(directionalLight.color, {0.0f, 1.5f, 2.0f}),
                     "Valid DirectionalLight color did not apply");
    const glm::vec3 directionalColor = directionalLight.color;
    passed &= expect(!editor_mutation::applyDirectionalLightColor(
                         directionalLight, {-1.0f, 0.0f, 0.0f}) &&
                         sameVector(directionalLight.color, directionalColor),
                     "Negative DirectionalLight color was not rejected transactionally");
    passed &= expect(!editor_mutation::applyDirectionalLightColor(
                         directionalLight, {nan, 0.0f, 0.0f}) &&
                         sameVector(directionalLight.color, directionalColor),
                     "Non-finite DirectionalLight color was not rejected transactionally");
    passed &= expect(static_cast<bool>(
                         editor_mutation::applyDirectionalLightIntensity(
                             directionalLight, 8.0f)),
                     "Valid DirectionalLight intensity was rejected");
    const float directionalIntensity = directionalLight.intensity;
    passed &= expect(
        !editor_mutation::applyDirectionalLightIntensity(directionalLight, -1.0f) &&
            directionalLight.intensity == directionalIntensity,
        "Negative DirectionalLight intensity was not rejected transactionally");
    passed &= expect(
        !editor_mutation::applyDirectionalLightIntensity(directionalLight, nan) &&
            directionalLight.intensity == directionalIntensity,
        "Non-finite DirectionalLight intensity was not rejected transactionally");
    return passed;
}

}  // namespace

int main() {
    bool passed = true;
    passed &= runNameTests();
    passed &= runPositionTests();
    passed &= runRotationTests();
    passed &= runScaleTests();
    passed &= runLightTests();
    return passed ? 0 : 1;
}
