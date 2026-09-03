#include "editor/editor_transform.h"
#include "rendering/editor_picking.h"
#include "scene/camera.h"
#include "scene/directional_light.h"
#include "scene/game_object.h"
#include "scene/point_light.h"
#include "scene/scene.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <memory>

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

class AttachedCameraObject final : public GameObject {
public:
    Camera camera;

    Camera* attachedCamera() noexcept override { return &camera; }
    const Camera* attachedCamera() const noexcept override {
        return &camera;
    }
};

template <typename ObjectType>
ObjectType* addObject(TestScene& scene, const char* persistentId) {
    auto object = std::make_unique<ObjectType>();
    object->persistentId = persistentId;
    ObjectType* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) {
        return nullptr;
    }
    return pointer;
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second,
                float tolerance = 1.0e-4f) {
    return glm::length(first - second) <= tolerance;
}

bool sameMatrix(const glm::mat4& first, const glm::mat4& second,
                float tolerance = 2.0e-4f) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::fabs(first[column][row] - second[column][row]) >
                tolerance) {
                return false;
            }
        }
    }
    return true;
}

bool runRootConversionTests() {
    GameObject object;
    object.position = {2.0f, -3.0f, 4.0f};
    object.rotation = {11.0f, -23.0f, 37.0f};
    object.scale = {-2.0f, 3.0f, -4.0f};

    const glm::vec3 expectedPosition{-7.0f, 8.0f, 9.0f};
    const glm::vec3 expectedRotation{31.0f, -17.0f, 13.0f};
    const glm::vec3 expectedScale{-1.5f, 2.5f, -3.5f};
    const glm::mat4 candidateWorld = transform_math::makeModelMatrix(
        expectedPosition, expectedRotation, expectedScale);

    transform_math::DecomposedTransform local;
    const Result result = editor_transform::deriveLocalTransformFromWorld(
        object, candidateWorld, local);
    bool passed = expect(static_cast<bool>(result),
                         "Root world-to-local gizmo conversion failed");
    passed &= expect(sameVector(local.position, expectedPosition) &&
                         sameVector(local.rotation, expectedRotation) &&
                         sameVector(local.scale, expectedScale),
                     "Root world-to-local conversion changed authored TRS");
    passed &= expect(sameMatrix(
                         transform_math::makeModelMatrix(
                             local.position, local.rotation, local.scale),
                         candidateWorld),
                     "Root world-to-local conversion did not preserve its matrix");

    GameObject zeroScaleObject;
    zeroScaleObject.rotation = {17.0f, -29.0f, 41.0f};
    zeroScaleObject.scale = {-2.0f, 3.0f, 4.0f};
    const glm::mat4 zeroScaleWorld = transform_math::makeModelMatrix(
        {4.0f, 5.0f, 6.0f}, zeroScaleObject.rotation, {0.0f, 3.0f, 4.0f});
    transform_math::DecomposedTransform zeroScaleLocal;
    const Result zeroScaleResult =
        editor_transform::deriveLocalTransformFromWorld(
            zeroScaleObject, zeroScaleWorld, zeroScaleLocal);
    passed &= expect(static_cast<bool>(zeroScaleResult) &&
                         sameVector(zeroScaleLocal.position, {4.0f, 5.0f, 6.0f}) &&
                         sameVector(zeroScaleLocal.scale, {0.0f, 3.0f, 4.0f}) &&
                         sameMatrix(transform_math::makeModelMatrix(
                                         zeroScaleLocal.position,
                                         zeroScaleLocal.rotation,
                                         zeroScaleLocal.scale),
                                     zeroScaleWorld),
                     "Root zero-scale gizmo conversion changed compatibility");
    return passed;
}

bool runChildTranslationTests() {
    TestScene scene;
    GameObject* parent = addObject<GameObject>(scene, "translation-parent");
    GameObject* child = addObject<GameObject>(scene, "translation-child");
    if (!parent || !child ||
        !scene.reparentGameObject(*child, parent,
                                  Scene::ReparentMode::PreserveLocal)) {
        return expect(false, "Could not create child-translation fixture");
    }

    parent->position = {100.0f, 0.0f, 0.0f};
    child->position = {20.0f, 0.0f, 0.0f};
    const glm::vec3 oldParentPosition = parent->position;
    const glm::mat4 candidateWorld = transform_math::makeModelMatrix(
        {150.0f, 0.0f, 0.0f}, child->rotation, child->scale);

    transform_math::DecomposedTransform local;
    const Result result = editor_transform::deriveLocalTransformFromWorld(
        *child, candidateWorld, local);
    bool passed = expect(static_cast<bool>(result),
                         "Child translation world-to-local conversion failed");
    passed &= expect(sameVector(local.position, {50.0f, 0.0f, 0.0f}) &&
                         parent->position == oldParentPosition,
                     "Child translation was not converted into parent space");
    return passed;
}

bool runRotatedAndScaledParentTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "rotated-parent");
        GameObject* child = addObject<GameObject>(scene, "rotated-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create rotated-parent fixture");
        }

        parent->position = {100.0f, 20.0f, -3.0f};
        parent->rotation = {0.0f, 0.0f, 90.0f};
        child->position = {10.0f, 2.0f, 1.0f};
        child->rotation = {15.0f, -20.0f, 25.0f};
        child->scale = {1.5f, 0.75f, 2.0f};

        const glm::vec3 expectedLocalPosition{20.0f, -4.0f, 5.0f};
        const glm::mat4 candidateLocal = transform_math::makeModelMatrix(
            expectedLocalPosition, child->rotation, child->scale);
        const glm::mat4 candidateWorld =
            parent->worldTransformMatrix() * candidateLocal;
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, candidateWorld, local);
        passed &= expect(static_cast<bool>(result) &&
                             sameVector(local.position, expectedLocalPosition),
                         "Rotated parent translation used a naive world delta");
        passed &= expect(sameMatrix(
                             parent->worldTransformMatrix() *
                                 transform_math::makeModelMatrix(
                                     local.position, local.rotation, local.scale),
                             candidateWorld),
                         "Rotated parent conversion did not preserve world matrix");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "scaled-parent");
        GameObject* child = addObject<GameObject>(scene, "scaled-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create scaled-parent fixture");
        }

        parent->position = {5.0f, 6.0f, 7.0f};
        parent->scale = {2.0f, 3.0f, 4.0f};
        child->rotation = {10.0f, 20.0f, 30.0f};
        child->scale = {0.5f, 1.5f, 2.0f};
        const glm::vec3 expectedLocalPosition{-3.0f, 8.0f, 2.5f};
        const glm::vec3 expectedLocalScale{1.25f, 0.75f, 1.5f};
        const glm::mat4 candidateWorld = parent->worldTransformMatrix() *
            transform_math::makeModelMatrix(expectedLocalPosition,
                                             child->rotation,
                                             expectedLocalScale);
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, candidateWorld, local);
        passed &= expect(static_cast<bool>(result) &&
                             sameVector(local.position, expectedLocalPosition) &&
                             sameVector(local.scale, expectedLocalScale),
                         "Scaled parent conversion did not recover local TRS");
    }

    return passed;
}

bool runRotationAndMultiLevelTests() {
    bool passed = true;
    TestScene scene;
    GameObject* root = addObject<GameObject>(scene, "multi-root");
    GameObject* parent = addObject<GameObject>(scene, "multi-parent");
    GameObject* child = addObject<GameObject>(scene, "multi-child");
    if (!root || !parent || !child ||
        !scene.reparentGameObject(*parent, root,
                                  Scene::ReparentMode::PreserveLocal) ||
        !scene.reparentGameObject(*child, parent,
                                  Scene::ReparentMode::PreserveLocal)) {
        return expect(false, "Could not create multi-level conversion fixture");
    }

    root->position = {30.0f, -10.0f, 4.0f};
    root->rotation = {10.0f, 25.0f, 35.0f};
    root->scale = {1.5f, 2.0f, 0.75f};
    parent->position = {-4.0f, 6.0f, 2.0f};
    parent->rotation = {-15.0f, 20.0f, 40.0f};
    parent->scale = {0.75f, 1.25f, 1.5f};
    child->position = {2.0f, 3.0f, 4.0f};
    child->rotation = {15.0f, -20.0f, 25.0f};
    child->scale = {-1.0f, 1.5f, 2.0f};

    const glm::vec3 expectedLocalPosition{-5.0f, 7.0f, 1.0f};
    const glm::vec3 expectedLocalRotation{35.0f, 10.0f, -15.0f};
    const glm::vec3 expectedLocalScale{-1.25f, 0.8f, 1.75f};
    const glm::mat4 candidateLocal = transform_math::makeModelMatrix(
        expectedLocalPosition, expectedLocalRotation, expectedLocalScale);
    const glm::mat4 candidateWorld =
        root->worldTransformMatrix() * parent->localTransformMatrix() *
        candidateLocal;

    transform_math::DecomposedTransform local;
    const Result result = editor_transform::deriveLocalTransformFromWorld(
        *child, candidateWorld, local);
    passed &= expect(static_cast<bool>(result),
                     "Multi-level rotation conversion failed");
    passed &= expect(sameMatrix(
                         parent->worldTransformMatrix() *
                             transform_math::makeModelMatrix(
                                 local.position, local.rotation, local.scale),
                         candidateWorld),
                     "Multi-level conversion did not use the complete parent chain");
    passed &= expect(sameVector(local.position, expectedLocalPosition) &&
                         sameVector(local.rotation, expectedLocalRotation) &&
                         sameVector(local.scale, expectedLocalScale),
                     "Matrix rotation conversion changed the expected local TRS");
    return passed;
}

bool runFailureAndTransactionalTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "shear-parent");
        GameObject* child = addObject<GameObject>(scene, "shear-child");
        if (!parent || !child ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create shear fixture");
        }
        parent->rotation = {0.0f, 0.0f, 45.0f};
        parent->scale = {2.0f, 1.0f, 1.0f};
        child->position = {2.0f, 3.0f, 4.0f};
        child->rotation = {10.0f, 20.0f, 30.0f};
        child->scale = {1.0f, 1.5f, 2.0f};

        const glm::vec3 positionBefore = child->position;
        const glm::vec3 rotationBefore = child->rotation;
        const glm::vec3 scaleBefore = child->scale;
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, glm::mat4(1.0f), local);
        passed &= expect(!result,
                         "World-to-local conversion accepted meaningful shear");
        passed &= expect(child->position == positionBefore &&
                             child->rotation == rotationBefore &&
                             child->scale == scaleBefore,
                         "Shear rejection changed authored local state");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "singular-parent");
        auto attachedObject = std::make_unique<AttachedCameraObject>();
        AttachedCameraObject* child = attachedObject.get();
        child->persistentId = "singular-attached-child";
        if (!parent || !scene.addGameObject(std::move(attachedObject)) ||
            !scene.reparentGameObject(*child, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create singular attached fixture");
        }
        parent->scale = {0.0f, 1.0f, 1.0f};
        child->position = {2.0f, 3.0f, 4.0f};
        child->rotation = {5.0f, 6.0f, 7.0f};
        child->scale = {1.0f, 2.0f, 3.0f};
        child->camera.position = {7.0f, 8.0f, 9.0f};
        child->camera.front = {0.0f, 0.0f, -1.0f};
        child->camera.up = {0.0f, 1.0f, 0.0f};

        const glm::vec3 positionBefore = child->position;
        const glm::vec3 rotationBefore = child->rotation;
        const glm::vec3 scaleBefore = child->scale;
        const glm::vec3 cameraPositionBefore = child->camera.position;
        const glm::vec3 cameraFrontBefore = child->camera.front;
        const glm::vec3 cameraUpBefore = child->camera.up;
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            *child, child->worldTransformMatrix(), local);
        passed &= expect(!result,
                         "World-to-local conversion accepted a singular parent");
        passed &= expect(child->position == positionBefore &&
                             child->rotation == rotationBefore &&
                             child->scale == scaleBefore &&
                             child->camera.position == cameraPositionBefore &&
                             child->camera.front == cameraFrontBefore &&
                             child->camera.up == cameraUpBefore,
                         "Failed gizmo conversion changed attached Camera state");
    }

    {
        GameObject object;
        object.position = {1.0f, 2.0f, 3.0f};
        object.rotation = {4.0f, 5.0f, 6.0f};
        object.scale = {2.0f, 3.0f, 4.0f};
        const glm::vec3 positionBefore = object.position;
        const glm::vec3 rotationBefore = object.rotation;
        const glm::vec3 scaleBefore = object.scale;
        glm::mat4 candidateWorld = object.worldTransformMatrix();
        candidateWorld[0][0] = std::numeric_limits<float>::quiet_NaN();
        transform_math::DecomposedTransform local;
        const Result result = editor_transform::deriveLocalTransformFromWorld(
            object, candidateWorld, local);
        passed &= expect(!result && object.position == positionBefore &&
                             object.rotation == rotationBefore &&
                             object.scale == scaleBefore,
                         "Nonfinite gizmo conversion was not transactional");
    }

    return passed;
}

bool runEditorWorldDataTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "light-parent");
        PointLight* point = addObject<PointLight>(scene, "point-child");
        DirectionalLight* directional =
            addObject<DirectionalLight>(scene, "directional-child");
        if (!parent || !point || !directional ||
            !scene.reparentGameObject(*point, parent,
                                      Scene::ReparentMode::PreserveLocal) ||
            !scene.reparentGameObject(*directional, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create world-light helper fixture");
        }
        parent->position = {100.0f, 50.0f, 0.0f};
        parent->rotation = {0.0f, 0.0f, 90.0f};
        parent->scale = {2.0f, 3.0f, 1.0f};
        point->position = {10.0f, 5.0f, 0.0f};
        directional->position = {-4.0f, 8.0f, 2.0f};
        directional->rotation = {30.0f, -10.0f, 40.0f};

        glm::vec3 pointWorldPosition;
        glm::vec3 directionalWorldPosition;
        glm::vec3 worldDirection;
        passed &= expect(
            editor_picking::deriveWorldPosition(*point, pointWorldPosition) &&
                sameVector(pointWorldPosition, {85.0f, 70.0f, 0.0f}) &&
                editor_picking::deriveWorldPosition(
                    *directional, directionalWorldPosition) &&
                sameVector(directionalWorldPosition,
                           glm::vec3(directional->worldTransformMatrix()[3])) &&
                directional->calculateWorldDirection(worldDirection),
            "Light editor helper world data did not use hierarchy transforms");
        passed &= expect(
            sameVector(worldDirection,
                       glm::normalize(glm::vec3(
                           directional->worldTransformMatrix() *
                           glm::vec4(0.0f, -1.0f, 0.0f, 0.0f)))),
            "Directional helper did not use the hierarchy-aware direction");

        PointLight rootPoint;
        rootPoint.position = {1.0f, 2.0f, 3.0f};
        glm::vec3 rootPosition;
        passed &= expect(editor_picking::deriveWorldPosition(
                             rootPoint, rootPosition) &&
                             sameVector(rootPosition, rootPoint.position),
                         "Root PointLight helper changed position semantics");
    }

    {
        TestScene scene;
        GameObject* parent = addObject<GameObject>(scene, "camera-parent");
        Camera* camera = addObject<Camera>(scene, "camera-child");
        if (!parent || !camera ||
            !scene.reparentGameObject(*camera, parent,
                                      Scene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not create camera helper fixture");
        }
        parent->position = {100.0f, 20.0f, -5.0f};
        parent->rotation = {10.0f, 20.0f, 30.0f};
        parent->scale = {2.0f, 3.0f, 1.5f};
        camera->position = {4.0f, 5.0f, 6.0f};
        camera->front = {0.2f, 0.7f, -1.0f};
        camera->up = {0.1f, 1.0f, 0.2f};

        CameraWorldPose expected;
        CameraWorldPose visualization;
        const bool expectedResult = camera->calculateWorldPose(expected);
        const bool visualizationResult =
            editor_picking::calculateCameraVisualizationPose(
                *camera, camera, visualization);
        passed &= expect(expectedResult && visualizationResult &&
                             sameVector(visualization.position,
                                        expected.position) &&
                             sameVector(visualization.front, expected.front) &&
                             sameVector(visualization.up, expected.up),
                         "Standalone Camera helper did not use Camera world pose");

        AttachedCameraObject attached;
        attached.camera.position = {7.0f, 8.0f, 9.0f};
        attached.camera.front = {0.0f, 0.0f, -1.0f};
        attached.camera.up = {0.0f, 1.0f, 0.0f};
        CameraWorldPose attachedPose;
        passed &= expect(
            editor_picking::calculateCameraVisualizationPose(
                attached.camera, &attached, attachedPose) &&
                sameVector(attachedPose.position, attached.camera.position) &&
                sameVector(attachedPose.front, attached.camera.front) &&
                sameVector(attachedPose.up, attached.camera.up),
            "Attached Camera helper was transformed a second time");

        Camera rootCamera;
        rootCamera.position = {12.0f, 13.0f, 14.0f};
        CameraWorldPose rootPose;
        passed &= expect(
            editor_picking::calculateCameraVisualizationPose(
                rootCamera, &rootCamera, rootPose) &&
                sameVector(rootPose.position, rootCamera.position),
            "Root Camera helper changed world position semantics");
    }

    return passed;
}

}  // namespace

int main() {
    return runRootConversionTests() && runChildTranslationTests() &&
                   runRotatedAndScaledParentTests() &&
                   runRotationAndMultiLevelTests() &&
                   runFailureAndTransactionalTests() &&
                   runEditorWorldDataTests()
               ? 0
               : 1;
}
