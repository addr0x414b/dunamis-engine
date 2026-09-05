#include "scene/scene.h"
#include "scene/scene_manager.h"
#include "scene/scene_serializer.h"
#include "scene/type_registry.h"
#include "scene/model_renderable.h"
#include "math/transform_math.h"
#include "game/level_1.h"
#include "input/input_manager.h"
#include "rendering/utils/vulkan_utils.h"

#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <limits>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>

class PlayerTestAccess {
public:
    static double yaw(const Player& player) { return player.yaw; }
    static double pitch(const Player& player) { return player.pitch; }
};

class InputManagerTestAccess {
public:
    static void setMode(InputManager& input, InputMode mode) {
        input.inputMode_ = mode;
    }
};

class GameObjectTestAccess {
public:
    static void setParent(GameObject& object, GameObject* parent) {
        object.parent_ = parent;
    }

    static void addChild(GameObject& object, GameObject* child) {
        object.children_.push_back(child);
    }
};

namespace {

class TestScene final : public Scene {
public:
    void buildDefaults() override {}
    void start() override {}
    void update() override {}
};

class DerivedGameObject final : public GameObject {};

class ManagedObject final : public GameObject {
public:
    ManagedObject() : instanceId(++nextInstanceId) { ++liveCount; }
    ~ManagedObject() override { --liveCount; }

    inline static int liveCount = 0;
    inline static int nextInstanceId = 0;
    const int instanceId;
};

class ManagedScene final : public Scene {
public:
    ManagedScene() { ++constructionCount; }

    static Result registerTypes(TypeRegistry& registry) {
        return registry.registerType<ManagedObject>(
            "ManagedObject", "GameObject",
            [] { return std::make_unique<ManagedObject>(); });
    }

    void buildDefaults() override {
        ++defaultBuildCount;
        auto object = std::make_unique<ManagedObject>();
        Result result = addGameObject(std::move(object));
        if (!result) {
            throw std::runtime_error(result.error());
        }
    }
    void start() override { ++startCount; }
    void update() override { ++updateCount; }

    inline static int constructionCount = 0;
    inline static int defaultBuildCount = 0;
    inline static int startCount = 0;
    inline static int updateCount = 0;
};

Result registerInvariantTypes(TypeRegistry& registry) {
    Result result = registerEngineTypes(registry);
    if (!result) return result;
    result = Level1::registerTypes(registry);
    if (!result) return result;
    return registry.registerType<DerivedGameObject>(
        "DerivedGameObject", "GameObject",
        [] { return std::make_unique<DerivedGameObject>(); });
}

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

Player* findPlayer(Scene& scene) {
    for (const auto& object : scene.gameObjects()) {
        if (auto* player = dynamic_cast<Player*>(object.get())) {
            return player;
        }
    }
    return nullptr;
}

bool nearlyEqual(float actual, float expected) {
    return std::fabs(actual - expected) < 0.0001f;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second) {
    return glm::length(first - second) < 1.0e-5f;
}

bool sameMatrix(const glm::mat4& first, const glm::mat4& second,
                float tolerance = 1.0e-4f) {
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

GameObject* addHierarchyObject(TestScene& scene, const char* persistentId) {
    auto object = std::make_unique<GameObject>();
    object->persistentId = persistentId;
    GameObject* pointer = object.get();
    if (!scene.addGameObject(std::move(object))) return nullptr;
    return pointer;
}

bool containsChild(const GameObject& parent, const GameObject* child) {
    return std::count(parent.children().begin(), parent.children().end(), child) !=
           0;
}

std::size_t childCount(const GameObject& parent, const GameObject* child) {
    return static_cast<std::size_t>(std::count(
        parent.children().begin(), parent.children().end(), child));
}

bool runCameraWorldPoseTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* root = addHierarchyObject(scene, "camera-pose-root");
        GameObject* parent = addHierarchyObject(scene, "camera-pose-parent");
        auto cameraObject = std::make_unique<Camera>();
        Camera* camera = cameraObject.get();
        camera->persistentId = "camera-pose-camera";
        if (!root || !parent || !scene.addGameObject(std::move(cameraObject))) {
            return expect(false, "Could not create Camera world-pose fixture");
        }

        root->position = {100.0f, 50.0f, -20.0f};
        root->rotation = {10.0f, 25.0f, 35.0f};
        root->scale = {2.0f, 3.0f, 1.5f};
        parent->position = {10.0f, 5.0f, 2.0f};
        parent->rotation = {-15.0f, 20.0f, 40.0f};
        parent->scale = {0.75f, 2.0f, 1.25f};
        camera->position = {10.0f, 5.0f, 0.0f};
        camera->rotation = {35.0f, -20.0f, 15.0f};
        camera->scale = {1.5f, 0.5f, 2.0f};
        camera->front = {0.2f, 0.7f, -1.0f};
        camera->up = {0.1f, 1.0f, 0.2f};

        const Result parentingResult = scene.reparentGameObject(
            *parent, root, TestScene::ReparentMode::PreserveLocal);
        const Result cameraParentingResult = scene.reparentGameObject(
            *camera, parent, TestScene::ReparentMode::PreserveLocal);
        CameraWorldPose pose;
        const bool poseResult = camera->calculateWorldPose(pose);

        const glm::mat4 ancestorWorld = parent->worldTransformMatrix();
        const glm::vec3 expectedPosition =
            glm::vec3(camera->worldTransformMatrix()[3]);
        const glm::vec3 transformedFront = glm::vec3(
            ancestorWorld * glm::vec4(camera->front, 0.0f));
        const glm::vec3 transformedUp = glm::vec3(
            ancestorWorld * glm::vec4(camera->up, 0.0f));
        const glm::vec3 expectedFront = glm::normalize(transformedFront);
        const glm::vec3 expectedRight = glm::normalize(
            glm::cross(expectedFront, glm::normalize(transformedUp)));
        const glm::vec3 expectedUp = glm::normalize(
            glm::cross(expectedRight, expectedFront));

        passed &= expect(parentingResult && cameraParentingResult && poseResult,
                         "Parented Camera world-pose calculation failed");
        passed &= expect(sameVector(pose.position, expectedPosition),
                         "Parented Camera position did not use its world transform");
        passed &= expect(sameVector(pose.front, expectedFront) &&
                             sameVector(pose.up, expectedUp),
                         "Parented Camera orientation did not use ancestor transforms only");
        passed &= expect(transform_math::isFiniteVector(pose.front) &&
                             transform_math::isFiniteVector(pose.up) &&
                             nearlyEqual(glm::length(pose.front), 1.0f) &&
                             nearlyEqual(glm::length(pose.up), 1.0f) &&
                             std::fabs(glm::dot(pose.front, pose.up)) < 1.0e-5f,
                         "Parented Camera world basis was not robust and normalized");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "invalid-camera-parent");
        auto cameraObject = std::make_unique<Camera>();
        Camera* camera = cameraObject.get();
        if (!parent || !scene.addGameObject(std::move(cameraObject))) {
            return expect(false, "Could not create invalid Camera pose fixture");
        }
        parent->scale = glm::vec3(0.0f);
        if (!scene.reparentGameObject(
                *camera, parent, TestScene::ReparentMode::PreserveLocal)) {
            return expect(false, "Could not parent invalid Camera pose fixture");
        }

        CameraWorldPose pose;
        pose.position = glm::vec3(std::numeric_limits<float>::quiet_NaN());
        const bool result = camera->calculateWorldPose(pose);
        passed &= expect(!result &&
                             transform_math::isFiniteVector(pose.position) &&
                             transform_math::isFiniteVector(pose.front) &&
                             transform_math::isFiniteVector(pose.up),
                         "Degenerate Camera world pose did not fail safely");
    }

    return passed;
}

bool runHierarchyTransformTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* root = addHierarchyObject(scene, "root-transform");
        if (!root) return expect(false, "Could not create root transform fixture");
        root->position = {2.0f, 3.0f, 4.0f};
        root->rotation = {10.0f, 20.0f, 30.0f};
        root->scale = {2.0f, 3.0f, 4.0f};
        const UniformBufferObject renderUbo = makeUniformBufferObject(
            root->worldTransformMatrix(), glm::mat4(1.0f), glm::mat4(1.0f),
            glm::vec3(0.0f));
        passed &= expect(sameMatrix(root->localTransformMatrix(),
                                    root->worldTransformMatrix()) &&
                             sameMatrix(renderUbo.model,
                                        transform_math::makeModelMatrix(
                                            root->position, root->rotation,
                                            root->scale)),
                         "A root world transform differed from its local transform");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "translation-parent");
        GameObject* child = addHierarchyObject(scene, "translation-child");
        if (!parent || !child) {
            return expect(false, "Could not create translation hierarchy fixture");
        }
        parent->position = {10.0f, 0.0f, 0.0f};
        child->position = {5.0f, 0.0f, 0.0f};
        const Result result = scene.reparentGameObject(
            *child, parent, TestScene::ReparentMode::PreserveLocal);
        const glm::mat4 expectedWorld =
            parent->localTransformMatrix() * child->localTransformMatrix();
        const UniformBufferObject renderUbo = makeUniformBufferObject(
            child->worldTransformMatrix(), glm::mat4(1.0f), glm::mat4(1.0f),
            glm::vec3(0.0f));
        passed &= expect(result && child->parent() == parent &&
                             sameVector(glm::vec3(child->worldTransformMatrix()[3]),
                                         {15.0f, 0.0f, 0.0f}) &&
                             sameMatrix(renderUbo.model, expectedWorld) &&
                             sameMatrix(renderUbo.model,
                                        child->worldTransformMatrix()),
                         "Parent translation did not compose with child local position");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "rotation-parent");
        GameObject* child = addHierarchyObject(scene, "rotation-child");
        if (!parent || !child) {
            return expect(false, "Could not create rotation hierarchy fixture");
        }
        parent->rotation = {0.0f, 0.0f, 90.0f};
        parent->scale = {2.0f, 3.0f, 4.0f};
        child->position = {1.0f, 0.0f, 0.0f};
        child->rotation = {10.0f, 20.0f, 30.0f};
        child->scale = {1.5f, 0.5f, 2.0f};
        const glm::mat4 localBefore = child->localTransformMatrix();
        passed &= expect(static_cast<bool>(scene.reparentGameObject(
                             *child, parent, TestScene::ReparentMode::PreserveLocal)),
                         "Parent rotation hierarchy setup failed");
        const UniformBufferObject renderUbo = makeUniformBufferObject(
            child->worldTransformMatrix(), glm::mat4(1.0f), glm::mat4(1.0f),
            glm::vec3(0.0f));
        passed &= expect(sameMatrix(child->localTransformMatrix(), localBefore) &&
                             sameMatrix(child->worldTransformMatrix(),
                                        parent->localTransformMatrix() * localBefore) &&
                             sameMatrix(renderUbo.model,
                                        child->worldTransformMatrix()),
                         "Parent rotation did not compose as a matrix");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "scale-parent");
        GameObject* child = addHierarchyObject(scene, "scale-child");
        if (!parent || !child) {
            return expect(false, "Could not create scale hierarchy fixture");
        }
        parent->scale = {2.0f, 3.0f, 4.0f};
        child->position = {1.0f, 2.0f, 3.0f};
        passed &= expect(static_cast<bool>(scene.reparentGameObject(
                             *child, parent, TestScene::ReparentMode::PreserveLocal)),
                         "Parent scale hierarchy setup failed");
        passed &= expect(sameVector(glm::vec3(child->worldTransformMatrix()[3]),
                                    {2.0f, 6.0f, 12.0f}),
                         "Parent scale did not affect child world translation");
    }

    {
        TestScene scene;
        GameObject* root = addHierarchyObject(scene, "point-root");
        auto pointLight = std::make_unique<PointLight>();
        pointLight->persistentId = "point-root-light";
        PointLight* rootLight = pointLight.get();
        if (!root || !scene.addGameObject(std::move(pointLight))) {
            return expect(false, "Could not create root point-light fixture");
        }
        rootLight->position = {4.0f, 5.0f, 6.0f};
        LightData uploadedLight{};
        uploadedLight.position =
            glm::vec3(rootLight->worldTransformMatrix()[3]);
        passed &= expect(sameVector(uploadedLight.position, {4.0f, 5.0f, 6.0f}),
                         "A root point light did not retain its world position");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "point-parent");
        auto pointLight = std::make_unique<PointLight>();
        pointLight->persistentId = "point-child";
        PointLight* childLight = pointLight.get();
        if (!parent || !scene.addGameObject(std::move(pointLight))) {
            return expect(false, "Could not create parented point-light fixture");
        }
        parent->position = {100.0f, 50.0f, 0.0f};
        parent->rotation = {0.0f, 0.0f, 90.0f};
        parent->scale = {2.0f, 3.0f, 1.0f};
        childLight->position = {10.0f, 5.0f, 0.0f};
        const Result result = scene.reparentGameObject(
            *childLight, parent, TestScene::ReparentMode::PreserveLocal);
        const glm::vec3 worldPosition =
            glm::vec3(childLight->worldTransformMatrix()[3]);
        LightData uploadedLight{};
        uploadedLight.position = worldPosition;
        passed &= expect(result &&
                             sameVector(uploadedLight.position,
                                        {85.0f, 70.0f, 0.0f}) &&
                             sameVector(glm::vec3(childLight->position),
                                        {10.0f, 5.0f, 0.0f}),
                         "A parented point light did not derive world position from its matrix");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "multi-parent");
        GameObject* child = addHierarchyObject(scene, "multi-child");
        GameObject* grandchild = addHierarchyObject(scene, "multi-grandchild");
        if (!parent || !child || !grandchild) {
            return expect(false, "Could not create multi-level hierarchy fixture");
        }
        parent->position = {3.0f, 4.0f, 5.0f};
        parent->rotation = {10.0f, 20.0f, 30.0f};
        parent->scale = {2.0f, 2.0f, 2.0f};
        child->position = {6.0f, 7.0f, 8.0f};
        child->rotation = {-5.0f, 12.0f, 18.0f};
        grandchild->position = {-2.0f, 1.0f, 4.0f};
        passed &= expect(
            scene.reparentGameObject(*child, parent,
                                     TestScene::ReparentMode::PreserveLocal) &&
                scene.reparentGameObject(*grandchild, child,
                                         TestScene::ReparentMode::PreserveLocal),
            "Multi-level hierarchy setup failed");
        passed &= expect(
            sameMatrix(grandchild->worldTransformMatrix(),
                       parent->localTransformMatrix() *
                           child->localTransformMatrix() *
                           grandchild->localTransformMatrix()),
            "Grandchild world transform did not compose all local transforms");
    }

    return passed;
}

bool runReparentModeTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parentA = addHierarchyObject(scene, "local-parent-a");
        GameObject* parentB = addHierarchyObject(scene, "local-parent-b");
        GameObject* child = addHierarchyObject(scene, "local-child");
        if (!parentA || !parentB || !child) {
            return expect(false, "Could not create PreserveLocal fixture");
        }
        parentA->position = {100.0f, 20.0f, 0.0f};
        parentB->position = {500.0f, -10.0f, 2.0f};
        child->position = {10.0f, 1.0f, 2.0f};
        child->rotation = {10.0f, 20.0f, 30.0f};
        child->scale = {2.0f, 3.0f, 4.0f};
        passed &= expect(
            static_cast<bool>(scene.reparentGameObject(
                *child, parentA, TestScene::ReparentMode::PreserveLocal)),
            "Initial PreserveLocal parenting failed");
        const glm::vec3 localPosition = child->position;
        const glm::vec3 localRotation = child->rotation;
        const glm::vec3 localScale = child->scale;
        passed &= expect(
            static_cast<bool>(scene.reparentGameObject(
                *child, parentB, TestScene::ReparentMode::PreserveLocal)),
            "PreserveLocal reparent failed");
        passed &= expect(child->parent() == parentB &&
                             child->position == localPosition &&
                             child->rotation == localRotation &&
                             child->scale == localScale &&
                             !containsChild(*parentA, child) &&
                             childCount(*parentB, child) == 1 &&
                             sameMatrix(child->worldTransformMatrix(),
                                        parentB->localTransformMatrix() *
                                            child->localTransformMatrix()),
                         "PreserveLocal changed authored local transform or topology incorrectly");
    }

    {
        TestScene scene;
        GameObject* parentA = addHierarchyObject(scene, "world-parent-a");
        GameObject* parentB = addHierarchyObject(scene, "world-parent-b");
        GameObject* child = addHierarchyObject(scene, "world-child");
        if (!parentA || !parentB || !child) {
            return expect(false, "Could not create PreserveWorld fixture");
        }
        parentA->position = {3.0f, 4.0f, 5.0f};
        parentA->rotation = {0.0f, 20.0f, 0.0f};
        parentA->scale = {2.0f, 2.0f, 2.0f};
        parentB->position = {-7.0f, 8.0f, 9.0f};
        parentB->rotation = {10.0f, 0.0f, 30.0f};
        parentB->scale = {0.5f, 0.5f, 0.5f};
        child->position = {1.0f, 2.0f, 3.0f};
        child->rotation = {15.0f, -25.0f, 35.0f};
        child->scale = {1.0f, 2.0f, 3.0f};
        passed &= expect(
            static_cast<bool>(scene.reparentGameObject(
                *child, parentA, TestScene::ReparentMode::PreserveLocal)),
            "Initial PreserveWorld parenting failed");
        const glm::mat4 worldBefore = child->worldTransformMatrix();
        const glm::mat4 localBefore = child->localTransformMatrix();
        passed &= expect(
            static_cast<bool>(scene.reparentGameObject(
                *child, parentB, TestScene::ReparentMode::PreserveWorld)),
            "Representable PreserveWorld reparent failed");
        passed &= expect(child->parent() == parentB &&
                             sameMatrix(child->worldTransformMatrix(), worldBefore,
                                        2.0e-4f) &&
                             !sameMatrix(child->localTransformMatrix(), localBefore) &&
                             !containsChild(*parentA, child) &&
                             childCount(*parentB, child) == 1,
                         "PreserveWorld did not preserve world transform and topology");

        const glm::mat4 worldBeforeUnparent = child->worldTransformMatrix();
        passed &= expect(
            static_cast<bool>(scene.reparentGameObject(
                *child, nullptr, TestScene::ReparentMode::PreserveWorld)),
            "PreserveWorld unparenting failed");
        passed &= expect(child->parent() == nullptr &&
                             sameMatrix(child->worldTransformMatrix(),
                                        worldBeforeUnparent, 2.0e-4f) &&
                             !containsChild(*parentB, child),
                         "PreserveWorld unparenting changed world transform or topology");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "repeat-parent");
        GameObject* child = addHierarchyObject(scene, "repeat-child");
        if (!parent || !child) {
            return expect(false, "Could not create duplicate-child fixture");
        }
        passed &= expect(
            scene.reparentGameObject(*child, parent,
                                     TestScene::ReparentMode::PreserveLocal) &&
                scene.reparentGameObject(*child, parent,
                                         TestScene::ReparentMode::PreserveWorld),
            "Repeated same-parent reparenting was not a safe no-op");
        passed &= expect(childCount(*parent, child) == 1,
                         "Repeated reparenting duplicated a child pointer");
    }

    {
        TestScene scene;
        GameObject* a = addHierarchyObject(scene, "cycle-a");
        GameObject* b = addHierarchyObject(scene, "cycle-b");
        GameObject* c = addHierarchyObject(scene, "cycle-c");
        if (!a || !b || !c) {
            return expect(false, "Could not create cycle-rejection fixture");
        }
        passed &= expect(
            scene.reparentGameObject(*b, a,
                                     TestScene::ReparentMode::PreserveLocal) &&
                scene.reparentGameObject(*c, b,
                                         TestScene::ReparentMode::PreserveLocal),
            "Cycle-rejection hierarchy setup failed");
        const Result result = scene.reparentGameObject(
            *a, c, TestScene::ReparentMode::PreserveLocal);
        passed &= expect(!result && a->parent() == nullptr && b->parent() == a &&
                             c->parent() == b && childCount(*a, b) == 1 &&
                             childCount(*b, c) == 1 && !containsChild(*c, a),
                         "Descendant-cycle rejection mutated hierarchy state");
    }

    {
        TestScene sceneA;
        TestScene sceneB;
        GameObject* childA = addHierarchyObject(sceneA, "cross-child-a");
        GameObject* parentB = addHierarchyObject(sceneB, "cross-parent-b");
        GameObject* childB = addHierarchyObject(sceneB, "cross-child-b");
        if (!childA || !parentB || !childB) {
            return expect(false, "Could not create cross-scene fixture");
        }
        const Result foreignChild = sceneA.reparentGameObject(
            *childB, childA, TestScene::ReparentMode::PreserveLocal);
        const Result foreignParent = sceneA.reparentGameObject(
            *childA, parentB, TestScene::ReparentMode::PreserveLocal);
        passed &= expect(!foreignChild && !foreignParent &&
                             childA->parent() == nullptr &&
                             childB->parent() == nullptr &&
                             !containsChild(*childA, childB) &&
                             !containsChild(*parentB, childA),
                         "Cross-scene parenting changed either scene");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "self-parent");
        if (!parent) return expect(false, "Could not create self-parent fixture");
        const Result result = scene.reparentGameObject(
            *parent, parent, TestScene::ReparentMode::PreserveLocal);
        passed &= expect(!result && parent->parent() == nullptr &&
                             parent->children().empty(),
                         "Self-parenting was accepted or mutated state");
    }

    return passed;
}

bool runHierarchyValidationTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "invalid-parent");
        GameObject* child = addHierarchyObject(scene, "invalid-child");
        if (!parent || !child) {
            return expect(false, "Could not create inconsistent-hierarchy fixture");
        }
        GameObjectTestAccess::setParent(*child, parent);
        const Result result = scene.validateAuthoredState();
        passed &= expect(!result &&
                             result.error().find("inconsistent") !=
                                 std::string::npos,
                         "Validation missed a one-way parent relationship");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "duplicate-parent");
        GameObject* child = addHierarchyObject(scene, "duplicate-child");
        if (!parent || !child) {
            return expect(false, "Could not create duplicate-hierarchy fixture");
        }
        GameObjectTestAccess::setParent(*child, parent);
        GameObjectTestAccess::addChild(*parent, child);
        GameObjectTestAccess::addChild(*parent, child);
        const Result result = scene.validateAuthoredState();
        passed &= expect(!result &&
                             result.error().find("more than once") !=
                                 std::string::npos,
                         "Validation missed a duplicate child pointer");
    }

    {
        TestScene scene;
        GameObject* a = addHierarchyObject(scene, "invalid-cycle-a");
        GameObject* b = addHierarchyObject(scene, "invalid-cycle-b");
        if (!a || !b) {
            return expect(false, "Could not create malformed-cycle fixture");
        }
        GameObjectTestAccess::setParent(*a, b);
        GameObjectTestAccess::setParent(*b, a);
        GameObjectTestAccess::addChild(*a, b);
        GameObjectTestAccess::addChild(*b, a);
        const Result result = scene.validateAuthoredState();
        passed &= expect(!result &&
                             result.error().find("cycle") != std::string::npos,
                         "Validation missed a hierarchy cycle");
    }

    return passed;
}

bool runHierarchyFailureTests() {
    bool passed = true;

    {
        TestScene scene;
        GameObject* oldParent = addHierarchyObject(scene, "shear-old-parent");
        GameObject* newParent = addHierarchyObject(scene, "shear-new-parent");
        GameObject* child = addHierarchyObject(scene, "shear-child");
        if (!oldParent || !newParent || !child) {
            return expect(false, "Could not create shear-rejection fixture");
        }
        newParent->rotation = {0.0f, 0.0f, 45.0f};
        newParent->scale = {2.0f, 1.0f, 1.0f};
        child->position = {1.0f, 2.0f, 3.0f};
        child->rotation = {10.0f, 20.0f, 30.0f};
        child->scale = {1.0f, 1.5f, 2.0f};
        passed &= expect(
            static_cast<bool>(scene.reparentGameObject(
                *child, oldParent, TestScene::ReparentMode::PreserveLocal)),
            "Shear-rejection hierarchy setup failed");
        const glm::vec3 positionBefore = child->position;
        const glm::vec3 rotationBefore = child->rotation;
        const glm::vec3 scaleBefore = child->scale;
        const Result result = scene.reparentGameObject(
            *child, newParent, TestScene::ReparentMode::PreserveWorld);
        passed &= expect(!result && child->parent() == oldParent &&
                             !containsChild(*newParent, child) &&
                             childCount(*oldParent, child) == 1 &&
                             child->position == positionBefore &&
                             child->rotation == rotationBefore &&
                             child->scale == scaleBefore,
                         "PreserveWorld shear rejection was not transactional");
    }

    {
        TestScene scene;
        GameObject* oldParent = addHierarchyObject(scene, "singular-old-parent");
        GameObject* newParent = addHierarchyObject(scene, "singular-new-parent");
        GameObject* child = addHierarchyObject(scene, "singular-child");
        if (!oldParent || !newParent || !child) {
            return expect(false, "Could not create singular-parent fixture");
        }
        newParent->scale = {0.0f, 1.0f, 1.0f};
        passed &= expect(
            static_cast<bool>(scene.reparentGameObject(
                *child, oldParent, TestScene::ReparentMode::PreserveLocal)),
            "Singular-parent hierarchy setup failed");
        const Result result = scene.reparentGameObject(
            *child, newParent, TestScene::ReparentMode::PreserveWorld);
        passed &= expect(!result && child->parent() == oldParent &&
                             !containsChild(*newParent, child) &&
                             childCount(*oldParent, child) == 1,
                         "PreserveWorld singular-parent rejection was not transactional");
    }

    return passed;
}

bool hasDirectionalLightError(const Result& result) {
    return result.error().find("Directional light") != std::string::npos;
}

bool isUuidStyle(const std::string& value) {
    if (value.size() != 36 || value[8] != '-' || value[13] != '-' ||
        value[18] != '-' || value[23] != '-') {
        return false;
    }

    const auto isHex = [](char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f');
    };
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (index == 8 || index == 13 || index == 18 || index == 23) {
            continue;
        }
        if (!isHex(value[index])) return false;
    }
    return value[14] == '4' &&
           (value[19] == '8' || value[19] == '9' || value[19] == 'a' ||
            value[19] == 'b');
}

bool runPersistentIdentityTests() {
    bool passed = true;
    TestScene scene;

    auto first = std::make_unique<GameObject>();
    first->name = "Pot";
    GameObject* firstPointer = first.get();
    const Result firstResult = scene.addGameObject(std::move(first));
    const std::string firstId = firstResult ? firstPointer->persistentId : "";

    auto second = std::make_unique<GameObject>();
    second->name = "Pot";
    GameObject* secondPointer = second.get();
    const Result secondResult = scene.addGameObject(std::move(second));
    const std::string secondId =
        secondResult ? secondPointer->persistentId : "";

    passed &= expect(firstResult && secondResult && !firstId.empty() &&
                         !secondId.empty() && firstId != secondId &&
                         isUuidStyle(firstId) && isUuidStyle(secondId),
                     "Empty persistent IDs were not generated as distinct UUID-style IDs");
    if (!firstResult || !secondResult) return false;
    passed &= expect(firstPointer->name == secondPointer->name,
                     "Duplicate display names were not allowed");
    passed &= expect(static_cast<bool>(scene.validateAuthoredState()),
                     "Distinct persistent IDs with duplicate names failed validation");

    auto authored = std::make_unique<GameObject>();
    authored->persistentId = "existing-authored-id";
    authored->name = "Authored";
    GameObject* authoredPointer = authored.get();
    const Result authoredResult = scene.addGameObject(std::move(authored));
    passed &= expect(authoredResult &&
                         authoredPointer->persistentId ==
                             "existing-authored-id",
                     "Explicit persistent ID was not preserved exactly");
    if (!authoredResult) return false;

    const std::size_t objectCountBeforeDuplicate = scene.gameObjects().size();
    auto duplicate = std::make_unique<GameObject>();
    duplicate->persistentId = "existing-authored-id";
    const Result duplicateResult = scene.addGameObject(std::move(duplicate));
    passed &= expect(!duplicateResult &&
                         scene.gameObjects().size() == objectCountBeforeDuplicate &&
                         scene.findGameObject("existing-authored-id") ==
                             authoredPointer,
                     "Duplicate persistent ID insertion changed scene state");

    secondPointer->persistentId = firstId;
    const Result invalidStateResult = scene.validateAuthoredState();
    passed &= expect(!invalidStateResult &&
                         invalidStateResult.error().find(
                             "duplicate persistent ID") != std::string::npos,
                     "Scene validation no longer detects duplicate identity");
    return passed;
}

bool runTypeRegistryHierarchyTests() {
    bool passed = true;
    TypeRegistry registry;
    const Result registration = registerInvariantTypes(registry);
    passed &= expect(static_cast<bool>(registration),
                     "Could not register TypeRegistry hierarchy types");

    const TypeDescriptor* gameObject = registry.find("GameObject");
    const TypeDescriptor* character = registry.find("Character");
    const TypeDescriptor* player = registry.find("Player");
    passed &= expect(character != nullptr,
                     "Character was not registered in the engine TypeRegistry");
    passed &= expect(character && character->parentName == "GameObject" &&
                         player && player->parentName == "Character",
                     "Character/Player TypeRegistry parents do not match C++ inheritance");
    passed &= expect(character && gameObject && registry.isA(*character, "GameObject") &&
                         player && registry.isA(*player, "Character") &&
                         registry.isA(*player, "GameObject"),
                     "TypeRegistry hierarchy relationships are incorrect");

    const auto contains = [](const auto& properties, const std::string& name) {
        return std::any_of(
            properties.begin(), properties.end(),
            [&name](const PropertyDescriptor* property) {
                return property->name == name;
            });
    };
    const auto characterPersisted = character
        ? registry.persistedProperties(*character)
        : std::vector<const PropertyDescriptor*>{};
    const auto characterRuntimeTransfer = character
        ? registry.runtimeTransferProperties(*character)
        : std::vector<const PropertyDescriptor*>{};
    const auto playerRuntimeTransfer = player
        ? registry.runtimeTransferProperties(*player)
        : std::vector<const PropertyDescriptor*>{};
    const PropertyDescriptor* capsuleHeight = character
        ? registry.findProperty(*character, "capsuleHeight")
        : nullptr;
    const PropertyDescriptor* capsuleRadius = character
        ? registry.findProperty(*character, "capsuleRadius")
        : nullptr;
    passed &= expect(
        capsuleHeight && capsuleRadius &&
            capsuleHeight->lifecycle == PropertyLifecycle::Persisted &&
            capsuleRadius->lifecycle == PropertyLifecycle::Persisted &&
            contains(characterPersisted, "capsuleHeight") &&
            contains(characterPersisted, "capsuleRadius") &&
            contains(characterRuntimeTransfer, "capsuleHeight") &&
            contains(characterRuntimeTransfer, "capsuleRadius") &&
            contains(playerRuntimeTransfer, "capsuleHeight") &&
            contains(playerRuntimeTransfer, "capsuleRadius"),
        "Character capsule properties have incorrect lifecycle or inheritance metadata");

    const bool simulationStateUnregistered =
        character && player &&
        registry.findProperty(*character, "desiredVelocity") == nullptr &&
        registry.findProperty(*character, "grounded") == nullptr &&
        registry.findProperty(*player, "desiredVelocity") == nullptr &&
        registry.findProperty(*player, "grounded") == nullptr;
    passed &= expect(simulationStateUnregistered,
                     "Character simulation state was registered as authored metadata");
    return passed;
}

class CurrentPathGuard {
public:
    explicit CurrentPathGuard(const std::filesystem::path& path)
        : original_(std::filesystem::current_path()) {
        std::filesystem::current_path(path);
    }

    ~CurrentPathGuard() { std::filesystem::current_path(original_); }

private:
    std::filesystem::path original_;
};

bool runLevel1FailurePropagationTest() {
    const auto missingAssetDirectory = std::filesystem::temp_directory_path() /
        ("dunamis-level1-missing-assets-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(missingAssetDirectory);
    Result result = Result::failure("Level1 initialization was not attempted");
    bool initialized = true;
    {
        CurrentPathGuard guard(missingAssetDirectory);
        SceneManager manager;
        result = manager.initialize<Level1>(
            "Level 1", std::make_shared<InputManager>());
        initialized = manager.initialized();
        manager.shutdown();
    }
    std::error_code error;
    std::filesystem::remove_all(missingAssetDirectory, error);
    return expect(!result && !initialized &&
                      result.error().find("Failed to load Sponza") !=
                          std::string::npos,
                  "Level1 did not propagate a required model-load failure");
}

bool runLevel1JsonLifecycleTests() {
    bool passed = true;
    TypeRegistry registry;
    Result result = registerEngineTypes(registry);
    if (result) result = Level1::registerTypes(registry);
    passed &= expect(static_cast<bool>(result),
                     "Could not register Level1 JSON lifecycle types");
    if (!result) return passed;

    Level1 source;
    source.name = "JSON Level 1";
    auto player = std::make_unique<Player>();
    Player* playerPointer = player.get();
    playerPointer->init();
    result = source.addGameObject(std::move(player));
    if (result) result = source.setActiveCamera(playerPointer->camera);
    nlohmann::json document;
    if (result) result = SceneSerializer::serializeAuthored(
        source, registry, document);
    passed &= expect(static_cast<bool>(result),
                     "Could not serialize the Level1 JSON lifecycle fixture");
    if (!result) return passed;

    const auto directory = std::filesystem::temp_directory_path() /
        ("dunamis-level1-json-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "level_1.scene.json";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (!error) {
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream << document.dump(2) << '\n';
        if (!stream) error = std::make_error_code(std::errc::io_error);
    }
    passed &= expect(!error, "Level1 JSON lifecycle fixture could not be written");
    if (error) {
        std::filesystem::remove_all(directory, error);
        return passed;
    }

    {
        CurrentPathGuard guard(directory);
        SceneManager manager;
        manager.setCurrentScenePath(path);
        result = manager.initialize<Level1>(
            "Level 1", std::make_shared<InputManager>());
        Player* loadedPlayer = result ? findPlayer(*manager.editingScene()) : nullptr;
        passed &= expect(result && manager.editingScene()->gameObjects().size() == 1 &&
                             loadedPlayer && loadedPlayer->persistentId ==
                                 playerPointer->persistentId &&
                             manager.editingScene()->activeCamera() ==
                                 loadedPlayer->attachedCamera(),
                         "JSON-loaded Level1 topology was not reconstructed from a blank scene");
        if (result) {
            try {
                manager.editingScene()->start();
                manager.editingScene()->update();
            } catch (const std::exception& exception) {
                passed &= expect(false,
                                 (std::string("JSON-loaded Level1 could not start: ") +
                                  exception.what()).c_str());
            }
        }
        manager.shutdown();
    }

    std::filesystem::remove_all(directory, error);
    return passed;
}

bool runAuthoringTransferTests() {
    bool passed = true;
    TypeRegistry registry;
    const Result registration = registerInvariantTypes(registry);
    passed &= expect(static_cast<bool>(registration),
                     "Could not register authoring-transfer types");
    GameObject baseObject;
    const GameObject& constBaseObject = baseObject;
    passed &= expect(baseObject.attachedCamera() == nullptr &&
                         constBaseObject.attachedCamera() == nullptr,
                     "Base GameObject unexpectedly reports an attached camera");

    TestScene editing;
    TestScene runtime;
    editing.name = "Edited Level";
    passed &= expect(static_cast<bool>(
                         editing.setBackgroundColor({0.1f, 0.2f, 0.3f, 0.4f})),
                     "Could not configure authoring background color");
    passed &= expect(static_cast<bool>(
                         editing.setAmbientLight({0.4f, 0.5f, 0.6f}, 3.0f)),
                     "Could not configure authoring ambient light");

    auto editingObject = std::make_unique<GameObject>();
    GameObject* editingObjectPointer = editingObject.get();
    editingObject->persistentId = "object";
    editingObject->name = "Edited object";
    editingObject->position = {1.0f, 2.0f, 3.0f};
    editingObject->rotation = {4.0f, 5.0f, 6.0f};
    editingObject->scale = {7.0f, 8.0f, 9.0f};
    editingObject->physics.enabled = true;
    editingObject->physics.motionType = GameObject::PhysicsMotionType::Dynamic;
    editingObject->physics.colliderType = GameObject::PhysicsColliderType::Sphere;
    editingObject->physics.sphereRadius = 0.75f;
    auto runtimeObject = std::make_unique<GameObject>();
    GameObject* runtimeObjectPointer = runtimeObject.get();
    runtimeObject->persistentId = "object";
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingObject))),
                     "Could not add editing object for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimeObject))),
                     "Could not add runtime object for transfer test");

    auto editingPoint = std::make_unique<PointLight>();
    PointLight* editingPointPointer = editingPoint.get();
    editingPoint->persistentId = "point";
    editingPoint->color = {0.2f, 0.3f, 0.4f};
    editingPoint->intensity = 11.0f;
    auto runtimePoint = std::make_unique<PointLight>();
    PointLight* runtimePointPointer = runtimePoint.get();
    runtimePoint->persistentId = "point";
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingPoint))),
                     "Could not add editing point light for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimePoint))),
                     "Could not add runtime point light for transfer test");

    auto editingDirectional = std::make_unique<DirectionalLight>();
    DirectionalLight* editingDirectionalPointer = editingDirectional.get();
    editingDirectional->persistentId = "directional";
    editingDirectional->rotation = {23.0f, -41.0f, 17.0f};
    editingDirectional->color = {0.7f, 0.8f, 0.9f};
    editingDirectional->intensity = 12.0f;
    editingDirectional->shadow.focus = {4.0f, 5.0f, 6.0f};
    editingDirectional->shadow.halfExtent = 123.0f;
    editingDirectional->shadow.lightDistance = 234.0f;
    editingDirectional->shadow.nearPlane = 2.0f;
    editingDirectional->shadow.farPlane = 456.0f;
    auto runtimeDirectional = std::make_unique<DirectionalLight>();
    DirectionalLight* runtimeDirectionalPointer = runtimeDirectional.get();
    runtimeDirectional->persistentId = "directional";
    passed &= expect(static_cast<bool>(editing.addGameObject(
                         std::move(editingDirectional))),
                     "Could not add editing directional light");
    passed &= expect(static_cast<bool>(runtime.addGameObject(
                         std::move(runtimeDirectional))),
                     "Could not add runtime directional light");

    auto editingCamera = std::make_unique<Camera>();
    Camera* editingCameraPointer = editingCamera.get();
    editingCamera->persistentId = "camera";
    editingCamera->position = {31.0f, 32.0f, 33.0f};
    editingCamera->rotation = {34.0f, 35.0f, 36.0f};
    editingCamera->scale = {37.0f, 38.0f, 39.0f};
    editingCamera->front = {0.5f, 0.25f, -0.75f};
    editingCamera->up = {0.0f, 0.5f, 0.5f};
    passed &= expect(editingCamera->setFov(90.0f),
                     "Could not configure standalone camera FOV");
    auto runtimeCamera = std::make_unique<Camera>();
    Camera* runtimeCameraPointer = runtimeCamera.get();
    runtimeCamera->persistentId = "camera";
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingCamera))),
                     "Could not add editing camera for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimeCamera))),
                     "Could not add runtime camera for transfer test");

    auto editingPlayer = std::make_unique<Player>();
    Player* editingPlayerPointer = editingPlayer.get();
    editingPlayer->persistentId = "player";
    editingPlayer->init();
    editingPlayer->position = {10.0f, 20.0f, 30.0f};
    editingPlayer->capsuleHeight = 211.0f;
    editingPlayer->capsuleRadius = 41.0f;
    editingPlayer->desiredVelocity = {12.0f, 34.0f, 56.0f};
    editingPlayer->grounded = true;
    editingPlayer->camera->position = {11.0f, 22.0f, 33.0f};
    editingPlayer->camera->front = {0.25f, 0.5f, -0.75f};
    editingPlayer->camera->up = {0.0f, 0.8f, 0.2f};
    passed &= expect(editingPlayer->camera->setFov(75.0f),
                     "Could not configure attached camera FOV");
    const glm::vec3 editingPlayerCameraPosition =
        editingPlayer->camera->position;
    const glm::vec3 editingPlayerCameraFront = editingPlayer->camera->front;
    const glm::vec3 editingPlayerCameraUp = editingPlayer->camera->up;
    passed &= expect(
        editingPlayer->attachedCamera() == editingPlayer->camera.get() &&
            static_cast<const Player&>(*editingPlayer).attachedCamera() ==
                editingPlayer->camera.get(),
        "Player does not report its attached camera through both accessors");

    auto runtimePlayer = std::make_unique<Player>();
    Player* runtimePlayerPointer = runtimePlayer.get();
    runtimePlayer->persistentId = "player";
    runtimePlayer->init();
    runtimePlayer->camera->position = {-1.0f, -2.0f, -3.0f};
    runtimePlayer->camera->front = {1.0f, 0.0f, 0.0f};
    runtimePlayer->camera->up = {0.0f, 0.0f, 1.0f};
    passed &= expect(static_cast<bool>(
                         editing.addGameObject(std::move(editingPlayer))),
                     "Could not add editing Player for transfer test");
    passed &= expect(static_cast<bool>(
                         runtime.addGameObject(std::move(runtimePlayer))),
                     "Could not add runtime Player for transfer test");

    const Result transfer = SceneSerializer::copyAuthoredState(
        editing, runtime, registry);
    passed &= expect(static_cast<bool>(transfer),
                     "Matching authoring topology was rejected");
    passed &= expect(runtime.name == editing.name &&
                         runtime.backgroundColor() == editing.backgroundColor() &&
                         runtime.ambientColor() == editing.ambientColor() &&
                         runtime.ambientIntensity() == editing.ambientIntensity(),
                     "Scene authoring properties were not transferred");
    passed &= expect(runtimeObjectPointer->name == "Edited object" &&
                         runtimeObjectPointer->position == glm::vec3(1.0f, 2.0f, 3.0f) &&
                         runtimeObjectPointer->rotation == glm::vec3(4.0f, 5.0f, 6.0f) &&
                         runtimeObjectPointer->scale == glm::vec3(7.0f, 8.0f, 9.0f) &&
                         runtimeObjectPointer->physics.enabled &&
                         runtimeObjectPointer->physics.motionType ==
                             GameObject::PhysicsMotionType::Dynamic &&
                         runtimeObjectPointer->physics.colliderType ==
                             GameObject::PhysicsColliderType::Sphere &&
                         nearlyEqual(runtimeObjectPointer->physics.sphereRadius, 0.75f),
                     "GameObject authoring transform was not transferred");
    passed &= expect(runtimePointPointer->color == editingPointPointer->color &&
                         runtimePointPointer->intensity == 11.0f,
                     "Point-light authoring state was not transferred");
    glm::vec3 editingDirectionalDirection;
    glm::vec3 runtimeDirectionalDirection;
    const bool calculatedEditingDirection =
        editingDirectionalPointer->calculateWorldDirection(
            editingDirectionalDirection);
    const bool calculatedRuntimeDirection =
        runtimeDirectionalPointer->calculateWorldDirection(
            runtimeDirectionalDirection);
    passed &= expect(calculatedEditingDirection && calculatedRuntimeDirection &&
                         sameVector(runtimeDirectionalDirection,
                                    editingDirectionalDirection) &&
                         runtimeDirectionalPointer->color ==
                             editingDirectionalPointer->color &&
                         runtimeDirectionalPointer->intensity == 12.0f &&
                         runtimeDirectionalPointer->shadow.focus ==
                             editingDirectionalPointer->shadow.focus &&
                         runtimeDirectionalPointer->shadow.halfExtent == 123.0f &&
                         runtimeDirectionalPointer->shadow.lightDistance == 234.0f &&
                         runtimeDirectionalPointer->shadow.nearPlane == 2.0f &&
                         runtimeDirectionalPointer->shadow.farPlane == 456.0f,
                     "Directional-light authoring state was not transferred");
    passed &= expect(runtimeCameraPointer->position == editingCameraPointer->position &&
                         runtimeCameraPointer->rotation == editingCameraPointer->rotation &&
                         runtimeCameraPointer->scale == editingCameraPointer->scale &&
                         runtimeCameraPointer->front == editingCameraPointer->front &&
                         runtimeCameraPointer->up == editingCameraPointer->up &&
                         runtimeCameraPointer->fov() == editingCameraPointer->fov(),
                     "Standalone Camera authoring state was not transferred");
    passed &= expect(
        runtimePlayerPointer->attachedCamera()->position ==
                editingPlayerCameraPosition &&
            runtimePlayerPointer->attachedCamera()->front ==
                editingPlayerCameraFront &&
            runtimePlayerPointer->attachedCamera()->up ==
                editingPlayerCameraUp &&
            runtimePlayerPointer->attachedCamera()->fov() ==
                editingPlayerPointer->attachedCamera()->fov(),
        "Attached camera authoring state was not transferred");
    passed &= expect(
        runtimePlayerPointer->capsuleHeight == 211.0f &&
            runtimePlayerPointer->capsuleRadius == 41.0f &&
            runtimePlayerPointer->desiredVelocity == glm::vec3(0.0f) &&
            !runtimePlayerPointer->grounded,
        "Character authored dimensions were not transferred or simulation state was copied");
    passed &= expect(
        editingPlayerPointer->attachedCamera()->position ==
                editingPlayerCameraPosition &&
            editingPlayerPointer->attachedCamera()->front ==
                editingPlayerCameraFront &&
            editingPlayerPointer->attachedCamera()->up ==
                editingPlayerCameraUp,
        "Attached camera transfer mutated the editing camera");
    passed &= expect(editingObjectPointer->position == glm::vec3(1.0f, 2.0f, 3.0f),
                     "Authoring transfer mutated the editing scene");

    TestScene identitySource;
    TestScene identityDestination;
    auto firstSourceObject = std::make_unique<GameObject>();
    firstSourceObject->persistentId = "first";
    firstSourceObject->position = {1.0f, 2.0f, 3.0f};
    auto secondSourceObject = std::make_unique<GameObject>();
    secondSourceObject->persistentId = "second";
    secondSourceObject->position = {4.0f, 5.0f, 6.0f};
    auto secondDestinationObject = std::make_unique<GameObject>();
    secondDestinationObject->persistentId = "second";
    secondDestinationObject->position = {91.0f, 92.0f, 93.0f};
    auto firstDestinationObject = std::make_unique<GameObject>();
    firstDestinationObject->persistentId = "first";
    firstDestinationObject->position = {81.0f, 82.0f, 83.0f};
    passed &= expect(
        identitySource.addGameObject(std::move(firstSourceObject)) &&
            identitySource.addGameObject(std::move(secondSourceObject)) &&
            identityDestination.addGameObject(std::move(secondDestinationObject)) &&
            identityDestination.addGameObject(std::move(firstDestinationObject)),
        "Could not configure persistent-identity transfer test");
    const Result identityTransfer = SceneSerializer::copyAuthoredState(
        identitySource, identityDestination, registry);
    passed &= expect(
        identityTransfer &&
            identityDestination.findGameObject("first")->position ==
                glm::vec3(1.0f, 2.0f, 3.0f) &&
            identityDestination.findGameObject("second")->position ==
                glm::vec3(4.0f, 5.0f, 6.0f),
        "Runtime transfer did not pair objects by persistent ID");

    TestScene typeSource;
    TestScene typeDestination;
    auto typeSourceObject = std::make_unique<GameObject>();
    typeSourceObject->persistentId = "type-mismatch";
    auto typeDestinationObject = std::make_unique<DerivedGameObject>();
    GameObject* typeDestinationPointer = typeDestinationObject.get();
    typeDestinationObject->persistentId = "type-mismatch";
    typeDestinationPointer->position = {81.0f, 82.0f, 83.0f};
    passed &= expect(typeSource.addGameObject(std::move(typeSourceObject)) &&
                         typeDestination.addGameObject(
                             std::move(typeDestinationObject)),
                     "Could not configure object-type mismatch test");
    const Result typeResult = SceneSerializer::copyAuthoredState(
        typeSource, typeDestination, registry);
    passed &= expect(!typeResult &&
                         typeDestinationPointer->position ==
                             glm::vec3(81.0f, 82.0f, 83.0f),
                     "Object-type mismatch was accepted or mutated the destination");

    TestScene attachedSource;
    TestScene attachedDestination;
    attachedSource.name = "Attached source";
    attachedDestination.name = "Attached destination";
    auto attachedSourcePlayer = std::make_unique<Player>();
    attachedSourcePlayer->init();
    attachedSourcePlayer->persistentId = "attached";
    Player* attachedSourcePointer = attachedSourcePlayer.get();
    auto attachedDestinationPlayer = std::make_unique<Player>();
    attachedDestinationPlayer->init();
    attachedDestinationPlayer->persistentId = "attached";
    attachedDestinationPlayer->camera.reset();
    Player* attachedDestinationPointer = attachedDestinationPlayer.get();
    attachedDestinationPointer->position = {71.0f, 72.0f, 73.0f};
    attachedDestinationPointer->rotation = {74.0f, 75.0f, 76.0f};
    attachedDestinationPointer->scale = {77.0f, 78.0f, 79.0f};
    passed &= expect(static_cast<bool>(attachedSource.addGameObject(
                         std::move(attachedSourcePlayer))) &&
                         static_cast<bool>(attachedDestination.addGameObject(
                             std::move(attachedDestinationPlayer))),
                     "Could not configure attached-camera mismatch test");
    const Result attachedTopologyResult = SceneSerializer::copyAuthoredState(
        attachedSource, attachedDestination, registry);
    passed &= expect(
        !attachedTopologyResult && attachedSourcePointer->attachedCamera() != nullptr &&
            attachedDestinationPointer->attachedCamera() == nullptr &&
            attachedTopologyResult.error().find("attached") != std::string::npos,
        "Attached-camera topology mismatch was not rejected");
    return passed;
}

bool runSceneManagerTests() {
    bool passed = true;
    ManagedScene::constructionCount = 0;
    ManagedScene::defaultBuildCount = 0;
    ManagedScene::startCount = 0;
    ManagedScene::updateCount = 0;
    ManagedObject::liveCount = 0;
    ManagedObject::nextInstanceId = 0;
    auto input = std::make_shared<InputManager>();
    SceneManager manager;
    passed &= expect(static_cast<bool>(
                         manager.initialize<ManagedScene>("Managed", input)),
                     "Scene Manager initialization failed");
    Scene* editing = manager.editingScene();
    GameObject* editingObject = editing->gameObjects().front().get();
    passed &= expect(manager.activeScene() == editing &&
                         !manager.isRuntimeSceneActive() &&
                         ManagedScene::constructionCount == 1 &&
                         ManagedScene::defaultBuildCount == 1 &&
                         ManagedScene::startCount == 0 &&
                         ManagedScene::updateCount == 0 &&
                         ManagedObject::liveCount == 1,
                     "Scene Manager did not retain one editing scene");
    const std::string editingObjectId = editingObject->persistentId;
    passed &= expect(!editingObjectId.empty() && isUuidStyle(editingObjectId),
                     "Scene Manager default object did not receive an automatic persistent ID");

    passed &= expect(static_cast<bool>(manager.prepareRuntimeScene()),
                     "Scene Manager could not prepare a runtime scene");
    Scene* firstRuntime = manager.runtimeScene();
    GameObject* firstRuntimeObject = firstRuntime->gameObjects().front().get();
    const int firstRuntimeObjectId =
        static_cast<ManagedObject*>(firstRuntimeObject)->instanceId;
    passed &= expect(firstRuntime != editing &&
                         firstRuntimeObject != editingObject &&
                         firstRuntimeObject->persistentId == editingObjectId &&
                         manager.activeScene() == editing &&
                         ManagedScene::defaultBuildCount == 1 &&
                         ManagedScene::startCount == 0 &&
                         ManagedScene::updateCount == 0 &&
                         ManagedObject::liveCount == 2,
                     "Prepared runtime scene ownership is incorrect");
    passed &= expect(manager.commitRuntimeScene() &&
                         manager.activeScene() == firstRuntime &&
                         manager.isRuntimeSceneActive() &&
                         ManagedScene::startCount == 0 &&
                         manager.returnToEditingScene() &&
                         !manager.isRuntimeSceneActive(),
                     "Scene Manager could not complete a runtime lifecycle");
    firstRuntime->start();
    firstRuntime->update();
    passed &= expect(ManagedScene::startCount == 1 &&
                         ManagedScene::updateCount == 1,
                     "Runtime lifecycle hooks did not remain runtime-only");
    passed &= expect(static_cast<bool>(manager.destroyRuntimeScene()),
                     "Scene Manager could not destroy the completed runtime scene");
    passed &= expect(manager.editingScene() == editing &&
                         manager.runtimeScene() == nullptr &&
                         ManagedObject::liveCount == 1,
                     "Destroying runtime scene affected the editing scene");

    passed &= expect(static_cast<bool>(manager.prepareRuntimeScene()),
                     "Scene Manager could not prepare a second runtime scene");
    GameObject* secondRuntimeObject =
        manager.runtimeScene()->gameObjects().front().get();
    passed &= expect(
                         static_cast<ManagedObject*>(secondRuntimeObject)
                                 ->instanceId != firstRuntimeObjectId &&
                         secondRuntimeObject != editingObject &&
                         secondRuntimeObject->persistentId == editingObjectId &&
                         ManagedScene::constructionCount == 3 &&
                         ManagedScene::defaultBuildCount == 1 &&
                         ManagedScene::startCount == 1 &&
                         ManagedScene::updateCount == 1,
                     "Repeated Play did not construct fresh runtime objects");
    manager.runtimeScene()->start();
    manager.runtimeScene()->update();
    passed &= expect(ManagedScene::startCount == 2 &&
                         ManagedScene::updateCount == 2,
                     "A runtime session did not invoke start/update once");
    manager.cancelPreparedRuntimeScene();
    manager.shutdown();
    passed &= expect(ManagedObject::liveCount == 0,
                     "Scene Manager shutdown leaked owned scenes");

    const auto directory = std::filesystem::temp_directory_path() /
        ("dunamis-managed-scene-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count()));
    const auto path = directory / "managed.scene.json";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    passed &= expect(!error, "Managed scene test directory could not be created");

    SceneManager bootstrapManager;
    bootstrapManager.setCurrentScenePath(path);
    ManagedScene::defaultBuildCount = 0;
    Result bootstrapResult = bootstrapManager.initialize<ManagedScene>(
        "Managed", std::make_shared<InputManager>());
    passed &= expect(bootstrapResult && ManagedScene::defaultBuildCount == 1,
                     "No-file scene did not call buildDefaults exactly once");
    Camera editorCamera;
    if (bootstrapResult) {
        bootstrapResult = bootstrapManager.saveEditingScene(editorCamera);
    }
    const std::string savedObjectId = bootstrapResult
        ? bootstrapManager.editingScene()->gameObjects().front()->persistentId
        : std::string{};
    passed &= expect(bootstrapResult && !savedObjectId.empty(),
                     "Managed bootstrap scene could not be saved");
    bootstrapManager.shutdown();

    ManagedScene::defaultBuildCount = 0;
    SceneManager loadedManager;
    loadedManager.setCurrentScenePath(path);
    Result loadedResult = loadedManager.initialize<ManagedScene>(
        "Managed", std::make_shared<InputManager>());
    const GameObject* loadedObject = loadedResult
        ? loadedManager.editingScene()->gameObjects().front().get()
        : nullptr;
    passed &= expect(loadedResult && ManagedScene::defaultBuildCount == 0 &&
                         loadedObject &&
                         loadedObject->persistentId == savedObjectId,
                     "JSON loading rebuilt topology through buildDefaults or lost identity");
    loadedManager.shutdown();
    std::filesystem::remove_all(directory, error);
    return passed;
}

class PlayerTransferScene final : public Scene {
public:
    static Result registerTypes(TypeRegistry& registry) {
        return Level1::registerTypes(registry);
    }

    void buildDefaults() override {
        auto player = std::make_unique<Player>();
        Player* playerPointer = player.get();
        playerPointer->init();
        Result result = addGameObject(std::move(player));
        if (!result) throw std::runtime_error(result.error());
        result = setActiveCamera(playerPointer->camera);
        if (!result) throw std::runtime_error(result.error());
    }

    void start() override {}
    void update() override {}
};

bool runPlayerRuntimeTransferTests() {
    bool passed = true;
    SceneManager manager;
    Result result = manager.initialize<PlayerTransferScene>(
        "Player Transfer", std::make_shared<InputManager>());
    passed &= expect(static_cast<bool>(result),
                     "Player runtime-transfer scene initialization failed");
    if (!result) {
        manager.shutdown();
        return passed;
    }

    auto* editingPlayer = findPlayer(*manager.editingScene());
    passed &= expect(editingPlayer != nullptr,
                     "Player runtime-transfer scene did not create its Player");
    if (!editingPlayer) {
        manager.shutdown();
        return passed;
    }

    editingPlayer->capsuleHeight = 211.0f;
    editingPlayer->capsuleRadius = 41.0f;
    editingPlayer->desiredVelocity = {12.0f, 34.0f, 56.0f};
    editingPlayer->grounded = true;

    result = manager.prepareRuntimeScene();
    auto* runtimePlayer = result ? findPlayer(*manager.runtimeScene()) : nullptr;
    passed &= expect(
        result && runtimePlayer && runtimePlayer->capsuleHeight == 211.0f &&
            runtimePlayer->capsuleRadius == 41.0f &&
            runtimePlayer->desiredVelocity == glm::vec3(0.0f) &&
            !runtimePlayer->grounded && runtimePlayer->camera &&
            runtimePlayer->attachedCamera() == runtimePlayer->camera.get(),
        "SceneManager runtime transfer did not preserve Character configuration or fresh Player state");

    manager.cancelPreparedRuntimeScene();
    manager.shutdown();
    return passed;
}

bool runPlayerCameraInitializationTests() {
    bool passed = true;
    const glm::vec3 playerWorldUp{0.0f, 1.0f, 0.0f};
    const glm::vec3 directions[] = {
        {0.0f, 0.0f, -1.0f},
        {0.98f, 0.15f, 0.1f},
        {-0.98f, -0.1f, 0.15f},
        {0.2f, 0.96f, -0.1f},
        {0.1f, -0.95f, -0.2f},
        {0.02f, 0.999f, 0.01f},
        {0.02f, -0.999f, 0.01f},
        {0.0f, 1.0f, 0.0f},
        {0.0f, -1.0f, 0.0f},
    };

    for (const glm::vec3& sourceDirection : directions) {
        Camera expectedCamera;
        expectedCamera.front = glm::normalize(sourceDirection);
        double expectedYaw = 0.0;
        double expectedPitch = 0.0;
        const bool expectedDerived = expectedCamera.deriveYawPitchDegrees(
            expectedYaw, expectedPitch);
        expectedPitch = std::clamp(expectedPitch, -89.0, 89.0);
        const bool expectedSet = expectedCamera.setYawPitchDegrees(
            expectedYaw, expectedPitch, playerWorldUp);
        passed &= expect(expectedDerived && expectedSet,
                         "Could not configure expected Player camera state");

        Player player;
        player.init();
        const glm::vec3 normalizedSource = glm::normalize(sourceDirection);
        player.camera->front = normalizedSource;
        player.camera->up = playerWorldUp;
        player.start(nullptr);

        const double playerYaw = PlayerTestAccess::yaw(player);
        const double playerPitch = PlayerTestAccess::pitch(player);
        passed &= expect(std::isfinite(playerYaw) && std::isfinite(playerPitch),
                         "Player startup produced invalid look angles");
        passed &= expect(
            std::fabs(playerYaw - expectedYaw) < 1.0e-3 &&
                std::fabs(playerPitch - expectedPitch) < 1.0e-3,
            "Player startup did not consume the authored Camera direction");
        passed &= expect(sameVector(player.camera->front,
                                    expectedCamera.front),
                         "Player startup produced an unexpected Camera direction");
        passed &= expect(
            sameVector(player.camera->up, playerWorldUp),
            "Player look-angle synchronization did not canonicalize world up");
        passed &= expect(playerPitch >= -89.0 && playerPitch <= 89.0,
                         "Player look synchronization exceeded pitch limits");
    }

    Player rotatedCameraPlayer;
    rotatedCameraPlayer.init();
    const float authoredPitch = glm::radians(35.0f);
    rotatedCameraPlayer.camera->front = {
        0.0f, std::sin(authoredPitch), -std::cos(authoredPitch)};
    rotatedCameraPlayer.camera->up = {
        0.0f, std::cos(authoredPitch), std::sin(authoredPitch)};

    Camera expectedRotatedCamera;
    expectedRotatedCamera.front = rotatedCameraPlayer.camera->front;
    double expectedRotatedYaw = 0.0;
    double expectedRotatedPitch = 0.0;
    const bool expectedRotatedDerived =
        expectedRotatedCamera.deriveYawPitchDegrees(
            expectedRotatedYaw, expectedRotatedPitch);
    expectedRotatedPitch = std::clamp(expectedRotatedPitch, -89.0, 89.0);
    const bool expectedRotatedSet = expectedRotatedCamera.setYawPitchDegrees(
        expectedRotatedYaw, expectedRotatedPitch, playerWorldUp);
    rotatedCameraPlayer.start(nullptr);

    passed &= expect(
        expectedRotatedDerived && expectedRotatedSet &&
            sameVector(rotatedCameraPlayer.camera->front,
                       expectedRotatedCamera.front) &&
            sameVector(rotatedCameraPlayer.camera->up, playerWorldUp) &&
            glm::length(glm::cross(rotatedCameraPlayer.camera->front,
                                   rotatedCameraPlayer.camera->up)) > 1.0e-4f,
        "Editor-rotated Camera up contaminated the runtime Player basis");

    auto editorInput = std::make_shared<InputManager>();
    Player simulatedPlayer;
    simulatedPlayer.init();
    simulatedPlayer.camera->front = {0.3f, 0.2f, -0.9f};
    simulatedPlayer.camera->up = playerWorldUp;
    simulatedPlayer.start(editorInput);
    passed &= expect(!editorInput->gameplayInputEnabled() &&
                         editorInput->inputMode() ==
                             InputMode::EditorInteractive &&
                         std::isfinite(PlayerTestAccess::yaw(simulatedPlayer)) &&
                         std::isfinite(PlayerTestAccess::pitch(simulatedPlayer)),
                     "Player startup captured gameplay input during editor ownership");

    simulatedPlayer.position = {4.0f, 5.0f, 6.0f};
    simulatedPlayer.camera->position = {4.0f, 5.0f, 6.0f};
    SDL_Event moveEvent{};
    moveEvent.type = SDL_EVENT_KEY_DOWN;
    moveEvent.key.key = SDLK_W;
    editorInput->handleEvent(moveEvent);
    simulatedPlayer.update(editorInput);
    passed &= expect(simulatedPlayer.position == glm::vec3(4.0f, 5.0f, 6.0f) &&
                         simulatedPlayer.camera->position ==
                             glm::vec3(4.0f, 5.0f, 6.0f),
                     "Player moved while gameplay input was editor-owned");

    return passed;
}

bool runPlayerMovementInvariantTests() {
    bool passed = true;
    passed &= expect(Player::eyeHeightMeters == 1.5f &&
                         Player::walkSpeedMetersPerSecond == 4.0f &&
                         Player::sprintSpeedMetersPerSecond == 7.0f,
                     "Player movement constants are not meter-based");
    const Character defaultCharacter;
    passed &= expect(defaultCharacter.capsuleHeight == 1.8f &&
                         defaultCharacter.capsuleRadius == 0.35f,
                     "Character default capsule dimensions are not meter-based");
    auto gameplayInput = std::make_shared<InputManager>();
    InputManagerTestAccess::setMode(*gameplayInput,
                                    InputMode::GameplayInteractive);
    Player movementPlayer;
    movementPlayer.init();
    movementPlayer.camera->front = {0.0f, 0.0f, -1.0f};
    SDL_Event forwardEvent{};
    forwardEvent.type = SDL_EVENT_KEY_DOWN;
    forwardEvent.key.key = SDLK_W;
    gameplayInput->handleEvent(forwardEvent);
    movementPlayer.update(gameplayInput);
    passed &= expect(std::fabs(glm::length(movementPlayer.desiredVelocity) -
                               4.0f) <= 1.0e-5f &&
                         std::fabs(movementPlayer.desiredVelocity.z + 4.0f) <=
                             1.0e-5f,
                     "Player walking velocity is not 4 meters per second");
    SDL_Event sprintEvent{};
    sprintEvent.type = SDL_EVENT_KEY_DOWN;
    sprintEvent.key.key = SDLK_LSHIFT;
    gameplayInput->handleEvent(sprintEvent);
    movementPlayer.update(gameplayInput);
    passed &= expect(std::fabs(glm::length(movementPlayer.desiredVelocity) -
                               7.0f) <= 1.0e-5f &&
                         std::fabs(movementPlayer.desiredVelocity.z + 7.0f) <=
                             1.0e-5f,
                     "Player sprint velocity is not 7 meters per second");
    {
        Player player;
        player.init();
        player.position = {10.0f, 20.0f, 30.0f};
        player.onPhysicsTransformResolved();
        passed &= expect(
            sameVector(player.camera->position, glm::vec3(10.0f, 21.5f, 30.0f)),
            "Player camera did not follow the physics-resolved position at eye height");
        passed &= expect(player.desiredVelocity == glm::vec3(0.0f),
                         "Player starts with nonzero character movement intent");
    }

    {
        TestScene scene;
        GameObject* parent = addHierarchyObject(scene, "player-camera-parent");
        auto playerObject = std::make_unique<Player>();
        Player* player = playerObject.get();
        player->persistentId = "player-camera-child";
        player->init();
        if (!parent || !scene.addGameObject(std::move(playerObject))) {
            return expect(false, "Could not create parented Player camera fixture");
        }

        parent->position = {100.0f, 50.0f, -20.0f};
        parent->rotation = {10.0f, 20.0f, 90.0f};
        parent->scale = {2.0f, 3.0f, 4.0f};
        player->position = {10.0f, 5.0f, 6.0f};
        const glm::vec3 localPositionBefore = player->position;
        const glm::vec3 frontBefore = player->camera->front;
        const glm::vec3 upBefore = player->camera->up;
        const Result parentingResult = scene.reparentGameObject(
            *player, parent, TestScene::ReparentMode::PreserveLocal);
        player->onPhysicsTransformResolved();

        const glm::vec3 playerWorldPosition =
            glm::vec3(player->worldTransformMatrix()[3]);
        const glm::vec3 expectedCameraPosition =
            playerWorldPosition + 1.5f * glm::vec3(0.0f, 1.0f, 0.0f);
        passed &= expect(parentingResult &&
                             sameVector(player->camera->position,
                                        expectedCameraPosition),
                         "Parented Player camera did not use Player world position");
        passed &= expect(player->position == localPositionBefore,
                         "Player camera synchronization changed local Player position");
        passed &= expect(player->camera->front == frontBefore &&
                             player->camera->up == upBefore,
                         "Player camera synchronization changed gameplay orientation");
    }

    return passed;
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
              decltype(std::declval<ModelRenderable&>().meshInstances()),
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
    passed &= runCameraWorldPoseTests();
    passed &= runHierarchyTransformTests();
    passed &= runReparentModeTests();
    passed &= runHierarchyValidationTests();
    passed &= runHierarchyFailureTests();
    passed &= runLevel1FailurePropagationTest();
    passed &= runLevel1JsonLifecycleTests();
    passed &= runPersistentIdentityTests();
    passed &= runTypeRegistryHierarchyTests();
    passed &= runAuthoringTransferTests();
    passed &= runSceneManagerTests();
    passed &= runPlayerRuntimeTransferTests();
    passed &= runPlayerCameraInitializationTests();
    passed &= runPlayerMovementInvariantTests();

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

    const auto binding = getVulkanVertexBindingDescription();
    const auto attributes = getVulkanVertexAttributeDescriptions();
    passed &= expect(binding.binding == 0 &&
                         binding.stride == sizeof(Vertex) &&
                         binding.inputRate == VK_VERTEX_INPUT_RATE_VERTEX &&
                         attributes.size() == 5 &&
                         attributes[0].binding == 0 &&
                         attributes[0].location == 0 &&
                         attributes[0].format ==
                             VK_FORMAT_R32G32B32_SFLOAT &&
                         attributes[0].offset == offsetof(Vertex, pos) &&
                         attributes[1].binding == 0 &&
                         attributes[1].location == 1 &&
                         attributes[1].format ==
                             VK_FORMAT_R32G32B32_SFLOAT &&
                         attributes[1].offset == offsetof(Vertex, color) &&
                         attributes[2].binding == 0 &&
                         attributes[2].location == 2 &&
                         attributes[2].format == VK_FORMAT_R32G32_SFLOAT &&
                         attributes[2].offset == offsetof(Vertex, texCoord) &&
                         attributes[3].binding == 0 &&
                         attributes[3].location == 3 &&
                         attributes[3].format ==
                             VK_FORMAT_R32G32B32_SFLOAT &&
                         attributes[3].offset == offsetof(Vertex, normal) &&
                         attributes[4].binding == 0 &&
                         attributes[4].location == 4 &&
                         attributes[4].format ==
                             VK_FORMAT_R32G32B32A32_SFLOAT &&
                         attributes[4].offset == offsetof(Vertex, tangent),
                     "Vulkan Vertex input layout is incorrect");

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
                         defaultMaterial.metallicRoughnessMapPath.empty() &&
                         defaultMaterial.metallicRoughnessMapPixels == nullptr &&
                         defaultMaterial.metallicRoughnessMapWidth == 0 &&
                         defaultMaterial.metallicRoughnessMapHeight == 0 &&
                         defaultMaterial.metallicRoughnessMapChannels == 0 &&
                         defaultMaterial.metallicRoughnessMapMipLevels == 0,
                     "Default CPU material state is invalid");

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
    glm::vec3 defaultDirectionalDirection;
    passed &= expect(defaultDirectionalLight.calculateWorldDirection(
                         defaultDirectionalDirection) &&
                         sameVector(defaultDirectionalDirection,
                                    glm::vec3(0.0f, -1.0f, 0.0f)) &&
                         nearlyEqual(glm::length(defaultDirectionalDirection),
                                     1.0f),
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
    passed &= expect(static_cast<bool>(scene.validateForActivation()),
                     "A scene without an active camera failed validation");
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
        "A non-finite directional-light rotation passed validation",
        [](DirectionalLight& light) {
            light.rotation.x = std::numeric_limits<float>::quiet_NaN();
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
    passed &= expect(ordinaryObject->modelRenderable().meshInstances().empty(),
                     "A new game object did not start without meshes");
    MeshInstance meshInstance{};
    meshInstance.mesh.vertices = {
        Vertex{{0.0f, 0.0f, 0.0f}}, Vertex{{1.0f, 0.0f, 0.0f}},
        Vertex{{0.0f, 1.0f, 0.0f}}};
    meshInstance.mesh.indices = {0, 1, 2};
    const Result meshResult = ordinaryObject->modelRenderable().addMeshInstance(
        std::move(meshInstance));
    passed &= expect(static_cast<bool>(meshResult),
                     "An inactive game object rejected a mesh instance");
    passed &= expect(ordinaryObject->modelRenderable().meshInstances().size() == 1 &&
                         ordinaryObject->modelRenderable().meshInstances().front()
                                 .mesh.indices.front() == 0,
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
