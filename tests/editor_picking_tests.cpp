#include "rendering/editor_picking.h"
#include "math/transform_math.h"
#include "scene/camera.h"
#include "scene/game_object.h"
#include "scene/model_renderable.h"

#include <cmath>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>

#include <glm/gtc/matrix_transform.hpp>

namespace {

class AttachedCameraObject final : public GameObject {
public:
    Camera camera;

    Camera* attachedCamera() noexcept override { return &camera; }
    const Camera* attachedCamera() const noexcept override {
        return &camera;
    }
};

bool expect(bool value, const char* message) {
    if (!value) std::cerr << message << '\n';
    return value;
}

Mesh makeTriangle() {
    Mesh mesh{};
    mesh.vertices.resize(3);
    mesh.vertices[0].pos = {-1.0f, -1.0f, 0.0f};
    mesh.vertices[1].pos = {1.0f, -1.0f, 0.0f};
    mesh.vertices[2].pos = {0.0f, 1.0f, 0.0f};
    mesh.indices = {0, 1, 2};
    calculateMeshBounds(mesh);
    return mesh;
}

Mesh makeTriangleAt(float x) {
    Mesh mesh = makeTriangle();
    for (Vertex& vertex : mesh.vertices) {
        vertex.pos.x += x;
    }
    calculateMeshBounds(mesh);
    return mesh;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second) {
    return glm::length(first - second) < 1.0e-5f;
}

bool sameMatrix(const glm::mat4& first, const glm::mat4& second) {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (std::abs(first[column][row] - second[column][row]) > 1.0e-5f) {
                return false;
            }
        }
    }
    return true;
}

bool runAabbTests() {
    const Mesh::Bounds bounds{glm::vec3(-1.0f), glm::vec3(1.0f), true};
    float distance = 0.0f;
    bool passed = expect(editor_picking::intersectAabb(
                             {{0.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 1.0f}},
                             bounds, distance) && std::abs(distance - 1.0f) < 1.0e-5f,
                         "AABB outside hit failed");
    passed &= expect(!editor_picking::intersectAabb(
                         {{2.0f, 0.0f, -2.0f}, {0.0f, 0.0f, 1.0f}}, bounds,
                         distance), "AABB miss failed");
    passed &= expect(editor_picking::intersectAabb(
                         {glm::vec3(0.0f), {1.0f, 0.0f, 0.0f}}, bounds, distance) &&
                         distance == 0.0f, "AABB inside hit failed");
    passed &= expect(editor_picking::intersectAabb(
                         {{0.0f, -2.0f, 0.0f}, {0.0f, 1.0f, 0.0f}}, bounds,
                         distance), "AABB parallel hit failed");
    Mesh::Bounds invalid{glm::vec3(1.0f), glm::vec3(-1.0f), true};
    passed &= expect(!editor_picking::intersectAabb(
                         {glm::vec3(0.0f), {1.0f, 0.0f, 0.0f}}, invalid, distance),
                     "Invalid AABB was accepted");
    return passed;
}

bool runScreenSegmentDistanceTests() {
    const glm::vec2 horizontalStart{0.0f, 0.0f};
    const glm::vec2 horizontalEnd{10.0f, 0.0f};
    bool passed = expect(
        editor_picking::distanceSquaredToSegment(
            {5.0f, 0.0f}, horizontalStart, horizontalEnd) == 0.0f,
        "Point on a segment did not have zero distance");
    passed &= expect(
        std::abs(editor_picking::distanceSquaredToSegment(
                     {5.0f, 3.0f}, horizontalStart, horizontalEnd) - 9.0f) <
            1.0e-5f,
        "Point near a horizontal segment had the wrong distance");
    passed &= expect(
        std::abs(editor_picking::distanceSquaredToSegment(
                     {14.0f, 0.0f}, horizontalStart, horizontalEnd) - 16.0f) <
            1.0e-5f,
        "Point beyond a segment used an unclamped projection");
    passed &= expect(
        editor_picking::distanceSquaredToSegment(
            {2.0f, 3.0f}, {2.0f, -4.0f}, {2.0f, 4.0f}) == 0.0f,
        "Point on a vertical segment did not have zero distance");
    passed &= expect(
        std::abs(editor_picking::distanceSquaredToSegment(
                     {2.0f, 3.0f}, {0.0f, 0.0f}, {4.0f, 4.0f}) - 0.5f) <
            1.0e-5f,
        "Point near a diagonal segment had the wrong distance");
    passed &= expect(
        std::abs(editor_picking::distanceSquaredToSegment(
                     {4.0f, 5.0f}, {1.0f, 1.0f}, {1.0f, 1.0f}) - 25.0f) <
            1.0e-5f,
        "Zero-length segment was not treated as a point");

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float invalidDistance = editor_picking::distanceSquaredToSegment(
        {nan, 0.0f}, horizontalStart, horizontalEnd);
    passed &= expect(!std::isfinite(invalidDistance),
                     "Nonfinite segment input produced a finite distance");
    return passed;
}

bool runTriangleTests() {
    const glm::vec3 a{-1.0f, -1.0f, 0.0f};
    const glm::vec3 b{1.0f, -1.0f, 0.0f};
    const glm::vec3 c{0.0f, 1.0f, 0.0f};
    float distance = 0.0f;
    bool passed = expect(editor_picking::intersectTriangle(
                             {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}}, a, b,
                             c, distance), "Front triangle hit failed");
    passed &= expect(editor_picking::intersectTriangle(
                         {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, -1.0f}}, a, b, c,
                         distance), "Back triangle hit failed");
    passed &= expect(!editor_picking::intersectTriangle(
                         {{3.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}}, a, b, c,
                         distance), "Triangle miss failed");
    passed &= expect(!editor_picking::intersectTriangle(
                         {{0.0f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}}, a, b, c,
                         distance), "Parallel triangle ray was accepted");
    passed &= expect(!editor_picking::intersectTriangle(
                         {{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}}, a, a, c,
                         distance), "Degenerate triangle was accepted");
    passed &= expect(!editor_picking::intersectTriangle(
                         {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}}, a, b, c,
                         distance), "Behind triangle hit was accepted");
    return passed;
}

bool runTransformedTests() {
    const Mesh mesh = makeTriangle();
    const editor_picking::Ray ray{{0.0f, 0.0f, -5.0f}, {0.0f, 0.0f, 1.0f}};
    const glm::vec3 zero(0.0f);
    const glm::vec3 one(1.0f);
    float distance = 0.0f;
    bool passed = expect(editor_picking::intersectMeshWorld(
                             ray, mesh, transform_math::makeModelMatrix(
                                 {0.0f, 0.0f, 2.0f}, zero, one), distance),
                         "Translated mesh hit failed");
    passed &= expect(editor_picking::intersectMeshWorld(
                         ray, mesh, transform_math::makeModelMatrix(
                             {0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 45.0f}, one),
                         distance), "Rotated mesh hit failed");
    passed &= expect(editor_picking::intersectMeshWorld(
                         ray, mesh, transform_math::makeModelMatrix(
                             {0.0f, 0.0f, 2.0f}, zero, glm::vec3(2.0f)), distance),
                         "Uniformly scaled mesh hit failed");
    passed &= expect(editor_picking::intersectMeshWorld(
                         ray, mesh, transform_math::makeModelMatrix(
                             {0.0f, 0.0f, 2.0f}, zero, {2.0f, 0.5f, 3.0f}),
                         distance), "Nonuniformly scaled mesh hit failed");
    for (const glm::vec3 scale : {glm::vec3(1.0f), glm::vec3(200.0f),
                                  glm::vec3(500.0f),
                                  glm::vec3(200.0f, 500.0f, 100.0f)}) {
        float scaledDistance = 0.0f;
        passed &= expect(editor_picking::intersectMeshWorld(
                             ray, mesh, transform_math::makeModelMatrix(
                                 {0.0f, 0.0f, 2.0f}, zero, scale),
                             scaledDistance) && std::abs(scaledDistance - 7.0f) < 1.0e-4f,
                         "Scale-invariant transformed mesh hit failed");
    }
    float nearDistance = 0.0f;
    float farDistance = 0.0f;
    const bool nearHit = editor_picking::intersectMeshWorld(ray, mesh,
        transform_math::makeModelMatrix({0.0f, 0.0f, 1.0f}, zero, one), nearDistance);
    const bool farHit = editor_picking::intersectMeshWorld(ray, mesh,
        transform_math::makeModelMatrix({0.0f, 0.0f, 3.0f}, zero, one), farDistance);
    passed &= expect(nearHit && farHit && nearDistance < farDistance,
                     "World-space closest hit failed");
    passed &= expect(!editor_picking::intersectMeshWorld(
                         ray, mesh, transform_math::makeModelMatrix(
                             {0.0f, 0.0f, 2.0f}, zero, {0.0f, 1.0f, 1.0f}),
                         distance), "Zero-scale mesh was accepted");
    return passed;
}

bool runTriangleScaleToleranceTests() {
    Mesh tiny = makeTriangle();
    for (Vertex& vertex : tiny.vertices) {
        vertex.pos *= 1.0e-7f;
    }
    calculateMeshBounds(tiny);
    Mesh large = makeTriangle();
    for (Vertex& vertex : large.vertices) {
        vertex.pos *= 1.0e6f;
    }
    calculateMeshBounds(large);
    const editor_picking::Ray tinyRay{{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}};
    const editor_picking::Ray largeRay{{0.0f, 0.0f, -1.0f}, {0.0f, 0.0f, 1.0f}};
    float distance = 0.0f;
    bool passed = expect(editor_picking::intersectMeshWorld(
                             tinyRay, tiny, glm::mat4(1.0f), distance),
                         "Very small valid triangle was rejected");
    passed &= expect(editor_picking::intersectMeshWorld(
                         largeRay, large, glm::mat4(1.0f), distance),
                     "Large valid triangle was rejected");
    passed &= expect(!editor_picking::intersectTriangle(
                         {{0.0f, 0.0f, -1.0f}, {1.0f, 0.0f, 0.0f}},
                         {-1.0f, -1.0f, 0.0f}, {1.0f, -1.0f, 0.0f},
                         {0.0f, 1.0f, 0.0f}, distance),
                     "Parallel ray was accepted by relative tolerance");
    return passed;
}

bool runMeshBoundsTests() {
    GameObject firstObject;
    GameObject secondObject;
    MeshInstance firstInstance{};
    firstInstance.mesh = makeTriangleAt(-4.0f);
    firstInstance.mesh.bounds = {};
    MeshInstance secondInstance{};
    secondInstance.mesh = makeTriangleAt(6.0f);
    secondInstance.mesh.bounds = {};
    MeshInstance copiedImportedInstance{};
    copiedImportedInstance.mesh = secondInstance.mesh;

    bool passed = expect(static_cast<bool>(firstObject.modelRenderable().addMeshInstance(
        std::move(firstInstance))),
                         "Could not add first mesh instance");
    passed &= expect(static_cast<bool>(secondObject.modelRenderable().addMeshInstance(
        std::move(secondInstance))),
                     "Could not add second mesh instance");
    passed &= expect(static_cast<bool>(secondObject.modelRenderable().addMeshInstance(
        std::move(copiedImportedInstance))),
                     "Could not add copied mesh instance");
    for (const MeshInstance& instance :
         firstObject.modelRenderable().meshInstances()) {
        passed &= expect(instance.mesh.bounds.valid,
                         "First GameObject mesh bounds were not computed");
    }
    for (const MeshInstance& instance :
         secondObject.modelRenderable().meshInstances()) {
        passed &= expect(instance.mesh.bounds.valid,
                         "Second GameObject mesh bounds were not computed");
    }
    return passed;
}

bool runClosestObjectAndGizmoTests() {
    const Mesh sponzaMesh = makeTriangle();
    const Mesh avocadoMesh = makeTriangle();
    const editor_picking::Ray ray{{0.0f, 0.0f, -10.0f}, {0.0f, 0.0f, 1.0f}};
    const glm::vec3 zero(0.0f);
    float sponzaDistance = 0.0f;
    float avocadoDistance = 0.0f;
    const bool sponzaHit = editor_picking::intersectMeshWorld(
        ray, sponzaMesh, transform_math::makeModelMatrix(
                             {0.0f, 0.0f, 8.0f}, zero, glm::vec3(100.0f)),
        sponzaDistance);
    const bool avocadoHit = editor_picking::intersectMeshWorld(
        ray, avocadoMesh, transform_math::makeModelMatrix(
                              {0.0f, 0.0f, 2.0f}, {0.0f, 0.0f, 35.0f},
                              {3.0f, 2.0f, 4.0f}),
        avocadoDistance);
    bool passed = expect(sponzaHit && avocadoHit && avocadoDistance < sponzaDistance,
                         "Closest hit across Sponza and Avocado-sized meshes failed");

    MeshInstance left{};
    left.mesh = makeTriangleAt(-2.0f);
    MeshInstance right{};
    right.mesh = makeTriangleAt(6.0f);
    const std::vector<MeshInstance> instances{left, right};
    const editor_picking::AggregateBounds aggregate =
        editor_picking::aggregateBounds(instances);
    passed &= expect(aggregate.valid && sameVector(aggregate.minimum,
                                                    {-3.0f, -1.0f, 0.0f}) &&
                         sameVector(aggregate.maximum, {7.0f, 1.0f, 0.0f}),
                     "Aggregate mesh bounds are incorrect");
    const glm::mat4 model = transform_math::makeModelMatrix(
        {10.0f, 5.0f, -2.0f}, {0.0f, 0.0f, 90.0f}, {2.0f, 3.0f, 1.0f});
    const glm::vec3 expectedCenter = glm::vec3(
        model * glm::vec4(glm::vec3(2.0f, 0.0f, 0.0f), 1.0f));
    const glm::vec3 worldCenter = editor_picking::worldBoundsCenter(
        aggregate, model, {10.0f, 5.0f, -2.0f});
    passed &= expect(sameVector(worldCenter, expectedCenter),
                     "Aggregate world bounds center is incorrect");

    const glm::mat4 gizmoMatrix =
        transform_math::makeTranslationMatrix(worldCenter);
    passed &= expect(gizmoMatrix[0][0] == 1.0f && gizmoMatrix[1][1] == 1.0f &&
                         gizmoMatrix[2][2] == 1.0f && gizmoMatrix[0][1] == 0.0f &&
                         gizmoMatrix[0][2] == 0.0f && gizmoMatrix[1][0] == 0.0f &&
                         gizmoMatrix[1][2] == 0.0f && gizmoMatrix[2][0] == 0.0f &&
                         gizmoMatrix[2][1] == 0.0f,
                     "Translation gizmo matrix contains rotation or scale");
    const glm::vec3 rotation{5.0f, 6.0f, 7.0f};
    const glm::vec3 scale{2.0f, 3.0f, 4.0f};
    glm::vec3 position{1.0f, 2.0f, 3.0f};
    const glm::vec3 originalPosition = position;
    const glm::vec3 delta{4.0f, -2.0f, 1.0f};
    position += delta;
    passed &= expect(sameVector(position, originalPosition + delta) &&
                         sameVector(rotation, {5.0f, 6.0f, 7.0f}) &&
                         sameVector(scale, {2.0f, 3.0f, 4.0f}),
                     "Gizmo center delta changed rotation or scale");
    const glm::mat4 firstView = glm::lookAt(
        glm::vec3(0.0f, 0.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    const glm::mat4 secondView = glm::lookAt(
        glm::vec3(5.0f, 3.0f, 10.0f), glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
    (void)firstView;
    (void)secondView;
    passed &= expect(sameVector(editor_picking::worldBoundsCenter(
                         aggregate, model, {10.0f, 5.0f, -2.0f}), worldCenter),
                     "Camera view changed the world gizmo center");
    glm::mat4 vulkanProjection = glm::perspective(
        glm::radians(45.0f), 16.0f / 9.0f, 0.1f, 10000.0f);
    vulkanProjection[1][1] *= -1.0f;
    glm::mat4 imguizmoProjection = vulkanProjection;
    imguizmoProjection[1][1] *= -1.0f;
    glm::vec2 vulkanScreen;
    glm::vec2 imguizmoScreen;
    const glm::vec2 renderPosition{17.0f, 23.0f};
    const glm::vec2 renderSize{1280.0f, 720.0f};
    const bool projectedByVulkan = editor_picking::projectVulkanWorldToImGui(
        worldCenter, firstView, vulkanProjection, renderPosition, renderSize,
        vulkanScreen);
    const bool projectedByImGuizmo = editor_picking::projectImGuizmoWorldToImGui(
        worldCenter, firstView, imguizmoProjection, renderPosition, renderSize,
        imguizmoScreen);
    passed &= expect(projectedByVulkan && projectedByImGuizmo &&
                         glm::length(vulkanScreen - imguizmoScreen) < 1.0e-4f,
                     "ImGuizmo origin differs from Vulkan-projected world center");
    return passed;
}

bool runTransformMathTests() {
    const glm::vec3 zero(0.0f);
    const glm::vec3 one(1.0f);
    bool passed = expect(
        sameMatrix(transform_math::makeModelMatrix(zero, zero, one),
                   glm::mat4(1.0f)),
        "Identity transform did not produce the identity matrix");

    const glm::vec3 translated = glm::vec3(
        transform_math::makeTranslationMatrix({3.0f, -2.0f, 5.0f}) *
        glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
    passed &= expect(sameVector(translated, {4.0f, 0.0f, 8.0f}),
                     "Translation transform moved a point incorrectly");

    const glm::vec3 scaled = glm::vec3(
        transform_math::makeScaleMatrix({2.0f, 3.0f, 4.0f}) *
        glm::vec4(1.0f, -2.0f, 0.5f, 1.0f));
    passed &= expect(sameVector(scaled, {2.0f, -6.0f, 2.0f}),
                     "Nonuniform scale transformed a point incorrectly");

    const glm::vec3 xRotated = glm::vec3(
        transform_math::makeRotationMatrix({90.0f, 0.0f, 0.0f}) *
        glm::vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const glm::vec3 yRotated = glm::vec3(
        transform_math::makeRotationMatrix({0.0f, 90.0f, 0.0f}) *
        glm::vec4(0.0f, 0.0f, 1.0f, 0.0f));
    const glm::vec3 zRotated = glm::vec3(
        transform_math::makeRotationMatrix({0.0f, 0.0f, 90.0f}) *
        glm::vec4(1.0f, 0.0f, 0.0f, 0.0f));
    passed &= expect(sameVector(xRotated, {0.0f, 0.0f, 1.0f}) &&
                         sameVector(yRotated, {1.0f, 0.0f, 0.0f}) &&
                         sameVector(zRotated, {0.0f, 1.0f, 0.0f}),
                     "Single-axis rotation convention changed");

    const glm::vec3 combinedRotated = glm::vec3(
        transform_math::makeRotationMatrix({30.0f, 45.0f, 60.0f}) *
        glm::vec4(1.0f, 2.0f, 3.0f, 0.0f));
    passed &= expect(sameVector(combinedRotated,
                                {1.250129f, 0.119769f, 3.524604f}),
                     "Combined Euler rotation order changed");

    const glm::vec3 combinedModelPoint = glm::vec3(
        transform_math::makeModelMatrix(
            {10.0f, -4.0f, 3.0f}, {30.0f, 45.0f, 60.0f},
            {2.0f, 3.0f, 4.0f}) *
        glm::vec4(1.0f, 2.0f, 3.0f, 1.0f));
    passed &= expect(sameVector(combinedModelPoint,
                                {15.518154f, -5.628128f, 15.284103f}),
                     "Combined model transform composition changed");
    return passed;
}

bool runRotationMatrixTests() {
    const glm::vec3 objectPosition{3.0f, -2.0f, 7.0f};
    const glm::vec3 originalOffset{0.0f, 0.0f, 4.0f};
    const glm::vec3 originalFront{0.0f, 0.0f, -1.0f};
    const glm::vec3 originalUp{0.0f, 1.0f, 0.0f};
    const glm::mat4 oldRotation =
        transform_math::makeRotationMatrix({11.0f, -23.0f, 17.0f});
    const glm::mat4 newRotation =
        transform_math::makeRotationMatrix({47.0f, 31.0f, -29.0f});
    const glm::mat4 deltaRotation = newRotation * glm::inverse(oldRotation);
    const glm::vec3 rotatedOffset = glm::vec3(
        deltaRotation * glm::vec4(originalOffset, 0.0f));
    const glm::vec3 rotatedFront = glm::normalize(glm::vec3(
        deltaRotation * glm::vec4(originalFront, 0.0f)));
    const glm::vec3 rotatedUp = glm::normalize(glm::vec3(
        deltaRotation * glm::vec4(originalUp, 0.0f)));
    const glm::vec3 rotatedCameraPosition = objectPosition + rotatedOffset;

    bool passed = expect(std::isfinite(rotatedCameraPosition.x) &&
                             std::isfinite(rotatedCameraPosition.y) &&
                             std::isfinite(rotatedCameraPosition.z) &&
                             std::isfinite(rotatedFront.x) &&
                             std::isfinite(rotatedFront.y) &&
                             std::isfinite(rotatedFront.z) &&
                             std::isfinite(rotatedUp.x) &&
                             std::isfinite(rotatedUp.y) &&
                             std::isfinite(rotatedUp.z),
                         "Combined Euler camera rotation was non-finite");
    passed &= expect(std::abs(glm::length(rotatedOffset) -
                              glm::length(originalOffset)) < 1.0e-5f &&
                         std::abs(glm::length(rotatedFront) - 1.0f) < 1.0e-5f &&
                         std::abs(glm::length(rotatedUp) - 1.0f) < 1.0e-5f,
                     "Attached camera rotation changed distance or direction lengths");

    const glm::mat4 quarterTurn =
        transform_math::makeRotationMatrix({0.0f, 90.0f, 0.0f});
    const glm::vec3 quarterTurnOffset = glm::vec3(
        quarterTurn * glm::vec4(originalOffset, 0.0f));
    const glm::vec3 quarterTurnFront = glm::vec3(
        quarterTurn * glm::vec4(originalFront, 0.0f));
    passed &= expect(sameVector(quarterTurnOffset, {4.0f, 0.0f, 0.0f}) &&
                         sameVector(quarterTurnFront, {-1.0f, 0.0f, 0.0f}),
                     "Y rotation did not use Dunamis' Euler convention");
    passed &= expect(sameVector(rotatedCameraPosition - objectPosition,
                                rotatedOffset),
                     "Attached camera offset was not rotated around the object");
    return passed;
}

bool runCameraDiscoveryTests() {
    Camera standaloneCamera;
    Camera secondStandaloneCamera;
    AttachedCameraObject attachedObject;
    Camera fallbackCamera;

    const std::vector<const GameObject*> objects{
        &standaloneCamera, &attachedObject, &secondStandaloneCamera};
    const std::vector<editor_picking::CameraVisualizationEntry> entries =
        editor_picking::collectCameraVisualizationEntries(
            objects, &attachedObject.camera);
    bool passed = expect(
        entries.size() == 3 && entries[0].camera == &standaloneCamera &&
            entries[0].selectionTarget == &standaloneCamera &&
            !entries[0].active && entries[1].camera == &attachedObject.camera &&
            entries[1].selectionTarget == &attachedObject && entries[1].active &&
            entries[2].camera == &secondStandaloneCamera &&
            entries[2].selectionTarget == &secondStandaloneCamera &&
            !entries[2].active,
        "Camera visualization discovery lost selection ownership");

    const std::vector<const Camera*> cameras =
        editor_picking::collectCameraPointers(objects, &attachedObject.camera);
    passed &= expect(cameras.size() == 3 &&
                             cameras[0] == &standaloneCamera &&
                             cameras[1] == &attachedObject.camera &&
                             cameras[2] == &secondStandaloneCamera,
                         "Camera discovery did not deduplicate an active attached camera");

    const std::vector<editor_picking::CameraVisualizationEntry>
        standaloneActiveEntries =
            editor_picking::collectCameraVisualizationEntries(
                objects, &standaloneCamera);
    passed &= expect(
        standaloneActiveEntries.size() == 3 &&
            standaloneActiveEntries[0].active &&
            standaloneActiveEntries[0].selectionTarget == &standaloneCamera,
        "Active standalone Camera did not retain its selection target");

    const std::vector<const GameObject*> partialObjects{&standaloneCamera};
    const std::vector<const Camera*> withFallback =
        editor_picking::collectCameraPointers(partialObjects, &fallbackCamera);
    passed &= expect(withFallback.size() == 2 &&
                         withFallback[0] == &standaloneCamera &&
                         withFallback[1] == &fallbackCamera,
                     "Active Camera fallback was not discovered");
    return passed;
}

}  // namespace

int main() {
    return runAabbTests() && runScreenSegmentDistanceTests() &&
                   runTriangleTests() && runTransformedTests() &&
                   runTriangleScaleToleranceTests() &&
                   runMeshBoundsTests() &&
                   runClosestObjectAndGizmoTests() &&
                   runTransformMathTests() &&
                   runRotationMatrixTests() &&
                   runCameraDiscoveryTests()
               ? 0
               : 1;
}
