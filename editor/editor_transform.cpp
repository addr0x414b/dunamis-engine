#include "editor_transform.h"

#include "editor_session.h"
#include "../scene/camera.h"
#include "../scene/game_object.h"
#include "../scene/scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <glm/gtc/matrix_inverse.hpp>

namespace editor_transform {
namespace {

constexpr float affineTolerance = 1.0e-4f;

[[nodiscard]] bool isAffineMatrix(const glm::mat4& matrix) noexcept {
    if (!transform_math::isFiniteMatrix(matrix)) {
        return false;
    }
    return std::fabs(matrix[0][3]) <= affineTolerance &&
           std::fabs(matrix[1][3]) <= affineTolerance &&
           std::fabs(matrix[2][3]) <= affineTolerance &&
           std::fabs(matrix[3][3] - 1.0f) <= affineTolerance;
}

[[nodiscard]] bool matricesExactlyEqual(const glm::mat4& first,
                                        const glm::mat4& second) noexcept {
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            if (first[column][row] != second[column][row]) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool matricesEquivalentWithinTransformTolerance(
    const glm::mat4& first, const glm::mat4& second) noexcept {
    if (!transform_math::isFiniteMatrix(first) ||
        !transform_math::isFiniteMatrix(second)) {
        return false;
    }

    float maximumLinearMagnitude = 0.0f;
    for (const glm::mat4* matrix : {&first, &second}) {
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                maximumLinearMagnitude = std::max(
                    maximumLinearMagnitude, std::fabs((*matrix)[column][row]));
            }
        }
    }
    const float linearComparisonScale =
        maximumLinearMagnitude > 0.0f ? maximumLinearMagnitude : 1.0f;
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const float difference =
                std::fabs(first[column][row] - second[column][row]);
            const float comparisonScale =
                column < 3 && row < 3
                    ? linearComparisonScale
                    : std::max({1.0f, std::fabs(first[column][row]),
                                std::fabs(second[column][row])});
            if (difference > affineTolerance * comparisonScale) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool calculateSafeInverse(const glm::mat4& matrix,
                                        glm::mat4& inverse) noexcept {
    inverse = glm::mat4(1.0f);
    if (!transform_math::isFiniteMatrix(matrix)) {
        return false;
    }

    const glm::mat3 linearPart(matrix);
    float maximumLinearMagnitude = 0.0f;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            maximumLinearMagnitude = std::max(
                maximumLinearMagnitude, std::fabs(linearPart[column][row]));
        }
    }
    const float determinant = glm::determinant(linearPart);
    const float determinantTolerance =
        128.0f * std::numeric_limits<float>::epsilon() *
        maximumLinearMagnitude * maximumLinearMagnitude *
        maximumLinearMagnitude;
    if (!std::isfinite(maximumLinearMagnitude) ||
        maximumLinearMagnitude <= 0.0f || !std::isfinite(determinant) ||
        std::fabs(determinant) <= determinantTolerance) {
        return false;
    }

    inverse = glm::inverse(matrix);
    return transform_math::isFiniteMatrix(inverse);
}

[[nodiscard]] bool ownsObject(const std::vector<GameObject*>& sceneObjects,
                              const GameObject* object) noexcept {
    return object != nullptr &&
           std::find(sceneObjects.begin(), sceneObjects.end(), object) !=
               sceneObjects.end();
}

[[nodiscard]] std::size_t countChildLinks(const GameObject* parent,
                                          const GameObject* child) noexcept {
    if (parent == nullptr || child == nullptr) {
        return 0;
    }
    return static_cast<std::size_t>(std::count(
        parent->children().begin(), parent->children().end(), child));
}

[[nodiscard]] const TransformObjectSnapshot* findObjectSnapshot(
    const TransformDragSnapshot& snapshot, const GameObject* object) noexcept {
    for (const TransformObjectSnapshot& candidate : snapshot.objects) {
        if (candidate.object == object) {
            return &candidate;
        }
    }
    return nullptr;
}

[[nodiscard]] Result validateSnapshot(const TransformDragSnapshot& snapshot) {
    if (!snapshot.valid || snapshot.scene == nullptr ||
        snapshot.editorSession == nullptr || snapshot.active == nullptr ||
        snapshot.activeTransformRoot == nullptr ||
        snapshot.selectedObjects.empty() || snapshot.objects.empty()) {
        return Result::failure("Transform transaction snapshot is invalid.");
    }

    const Scene& scene = *snapshot.scene;
    const auto& currentObjects = scene.gameObjects();
    if (currentObjects.size() != snapshot.sceneObjects.size() ||
        currentObjects.size() != snapshot.sceneObjectPersistentIds.size() ||
        currentObjects.size() != snapshot.sceneObjectParents.size()) {
        return Result::failure(
            "Scene objects changed during the transform transaction.");
    }
    std::unordered_set<GameObject*> sceneObjectSet;
    sceneObjectSet.reserve(snapshot.sceneObjects.size());
    for (std::size_t index = 0; index < currentObjects.size(); ++index) {
        if (currentObjects[index] == nullptr ||
            currentObjects[index].get() != snapshot.sceneObjects[index] ||
            currentObjects[index]->persistentId !=
                snapshot.sceneObjectPersistentIds[index] ||
            !sceneObjectSet.insert(currentObjects[index].get()).second) {
            return Result::failure(
                "Scene objects changed during the transform transaction.");
        }
    }
    for (std::size_t index = 0; index < currentObjects.size(); ++index) {
        if (currentObjects[index]->parent() != snapshot.sceneObjectParents[index] ||
            (snapshot.sceneObjectParents[index] != nullptr &&
             sceneObjectSet.count(snapshot.sceneObjectParents[index]) == 0)) {
            return Result::failure(
                "Scene hierarchy changed during the transform transaction.");
        }
    }

    if (snapshot.editorSession->selectionScene() != snapshot.scene ||
        snapshot.editorSession->activeGameObject() != snapshot.active ||
        snapshot.editorSession->transformTool() != snapshot.tool ||
        snapshot.editorSession->transformSpace() != snapshot.space) {
        return Result::failure(
            "Editor transform state changed during the transaction.");
    }
    const std::vector<GameObject*>& selected =
        snapshot.editorSession->selectedGameObjects();
    if (selected.size() != snapshot.selectedObjects.size()) {
        return Result::failure(
            "Selection changed during the transform transaction.");
    }
    for (std::size_t index = 0; index < selected.size(); ++index) {
        if (selected[index] != snapshot.selectedObjects[index]) {
            return Result::failure(
                "Selection changed during the transform transaction.");
        }
    }

    std::unordered_set<GameObject*> affectedObjects;
    affectedObjects.reserve(snapshot.objects.size());
    for (const TransformObjectSnapshot& objectSnapshot : snapshot.objects) {
        if (objectSnapshot.object == nullptr ||
            sceneObjectSet.count(objectSnapshot.object) == 0 ||
            !affectedObjects.insert(objectSnapshot.object).second) {
            return Result::failure(
                "Affected hierarchy changed during the transform transaction.");
        }
        if (objectSnapshot.object->parent() != objectSnapshot.parent) {
            return Result::failure(
                "Affected hierarchy changed during the transform transaction.");
        }
        if (objectSnapshot.parent != nullptr &&
            sceneObjectSet.count(objectSnapshot.parent) == 0) {
            return Result::failure(
                "Affected hierarchy changed during the transform transaction.");
        }

        editor_mutation::CameraTransformState currentCamera;
        const Result cameraResult = editor_mutation::captureCameraTransformState(
            *objectSnapshot.object, currentCamera);
        if (!cameraResult || currentCamera.camera !=
                                  objectSnapshot.originalCamera.camera) {
            return Result::failure(
                "Camera association changed during the transform transaction.");
        }
    }

    for (const TransformObjectSnapshot& objectSnapshot : snapshot.objects) {
        const std::vector<GameObject*>& children =
            objectSnapshot.object->children();
        if (children.size() != objectSnapshot.originalChildren.size() ||
            !std::equal(children.begin(), children.end(),
                        objectSnapshot.originalChildren.begin())) {
            return Result::failure(
                "Affected hierarchy changed during the transform transaction.");
        }
        for (GameObject* child : children) {
            if (child == nullptr || sceneObjectSet.count(child) == 0 ||
                child->parent() != objectSnapshot.object) {
                return Result::failure(
                    "Affected hierarchy changed during the transform transaction.");
            }
        }
    }

    for (const TransformExternalParentSnapshot& parentSnapshot :
         snapshot.externalParents) {
        if (parentSnapshot.object == nullptr ||
            sceneObjectSet.count(parentSnapshot.object) == 0 ||
            parentSnapshot.object->parent() != parentSnapshot.parent ||
            (parentSnapshot.parent != nullptr &&
             (sceneObjectSet.count(parentSnapshot.parent) == 0 ||
              countChildLinks(parentSnapshot.parent, parentSnapshot.object) !=
                  1))) {
            return Result::failure(
                "External hierarchy changed during the transform transaction.");
        }
        const glm::mat4 currentWorld =
            parentSnapshot.object->worldTransformMatrix();
        if (!isAffineMatrix(currentWorld) ||
            !matricesExactlyEqual(currentWorld, parentSnapshot.originalWorld)) {
            return Result::failure(
                "External parent changed during the transform transaction.");
        }
    }

    if (sceneObjectSet.count(snapshot.active) == 0 ||
        sceneObjectSet.count(snapshot.activeTransformRoot) == 0 ||
        affectedObjects.count(snapshot.active) == 0 ||
        affectedObjects.count(snapshot.activeTransformRoot) == 0) {
        return Result::failure(
            "Active GameObject changed during the transform transaction.");
    }
    return Result::success();
}

[[nodiscard]] Result deriveLocalTransformFromParentWorld(
    const glm::mat4& parentWorld, const glm::vec3& authoredScaleHint,
    const glm::mat4& candidateWorld,
    transform_math::DecomposedTransform& local) {
    local = {};
    if (!isAffineMatrix(parentWorld) ||
        !transform_math::isFiniteVector(authoredScaleHint) ||
        !isAffineMatrix(candidateWorld)) {
        return Result::failure(
            "Gizmo transform inputs must be finite affine matrices.");
    }

    glm::mat4 inverseParent;
    if (!calculateSafeInverse(parentWorld, inverseParent)) {
        return Result::failure(
            "Gizmo candidate parent world transform is not safely invertible.");
    }
    const glm::mat4 candidateLocal = inverseParent * candidateWorld;
    if (!isAffineMatrix(candidateLocal)) {
        return Result::failure(
            "Gizmo world transform produced a non-finite local transform.");
    }

    const Result decomposition =
        transform_math::decomposeModelMatrixAllowingZeroScale(
            candidateLocal, authoredScaleHint, local);
    if (!decomposition) {
        local = {};
        return Result::failure(
            "Gizmo world transform cannot be represented as local TRS: " +
            decomposition.error());
    }
    return Result::success();
}

[[nodiscard]] glm::mat4 identityOrientation() noexcept {
    return glm::mat4(1.0f);
}

}  // namespace

Result calculateSharedPivot(
    const std::vector<GameObject*>& topLevelSelectedRoots,
    glm::vec3& pivot) {
    pivot = {};
    if (topLevelSelectedRoots.empty()) {
        return Result::failure(
            "At least one top-level selected root is required for a pivot.");
    }

    glm::dvec3 sum(0.0);
    for (const GameObject* root : topLevelSelectedRoots) {
        if (root == nullptr) {
            return Result::failure("A selected root is null.");
        }
        const glm::mat4 world = root->worldTransformMatrix();
        if (!isAffineMatrix(world)) {
            return Result::failure(
                "A selected root world transform is not finite affine data.");
        }
        const glm::vec3 position = glm::vec3(world[3]);
        if (!transform_math::isFiniteVector(position)) {
            return Result::failure("A selected root position is not finite.");
        }
        sum += glm::dvec3(position);
    }

    const glm::dvec3 average =
        sum / static_cast<double>(topLevelSelectedRoots.size());
    pivot = glm::vec3(average);
    if (!transform_math::isFiniteVector(pivot)) {
        pivot = {};
        return Result::failure("The shared gizmo pivot is not finite.");
    }
    return Result::success();
}

Result resolveActiveTransformRoot(
    GameObject* active, const std::vector<GameObject*>& selectedObjects,
    GameObject*& root) {
    root = nullptr;
    if (active == nullptr || selectedObjects.empty()) {
        return Result::failure(
            "An active selected object is required for a transform root.");
    }

    std::unordered_set<GameObject*> selected;
    selected.reserve(selectedObjects.size());
    for (GameObject* object : selectedObjects) {
        if (object == nullptr || !selected.insert(object).second) {
            return Result::failure(
                "The transform selection contains an invalid object.");
        }
    }
    if (selected.count(active) == 0) {
        return Result::failure(
            "The active object is not a member of the transform selection.");
    }

    root = active;
    std::unordered_set<GameObject*> visited;
    visited.reserve(selectedObjects.size() + 1);
    visited.insert(active);
    for (GameObject* ancestor = active->parent(); ancestor != nullptr;
         ancestor = ancestor->parent()) {
        if (!visited.insert(ancestor).second) {
            root = nullptr;
            return Result::failure("The transform hierarchy contains a cycle.");
        }
        if (selected.count(ancestor) != 0) {
            root = ancestor;
        }
    }
    return Result::success();
}

Result calculateWorldOrientation(const glm::mat4& worldTransform,
                                 glm::mat4& orientation) {
    orientation = identityOrientation();
    if (!isAffineMatrix(worldTransform)) {
        return Result::failure(
            "World orientation source must be a finite affine matrix.");
    }

    glm::vec3 xAxis = glm::vec3(worldTransform[0]);
    glm::vec3 yAxis = glm::vec3(worldTransform[1]);
    const glm::vec3 zSource = glm::vec3(worldTransform[2]);
    const float xLength = glm::length(xAxis);
    if (!std::isfinite(xLength) || xLength <= 1.0e-6f) {
        return Result::failure("World orientation has a zero X axis.");
    }
    xAxis /= xLength;

    yAxis -= glm::dot(yAxis, xAxis) * xAxis;
    const float yLength = glm::length(yAxis);
    if (!std::isfinite(yLength) || yLength <= 1.0e-6f) {
        return Result::failure("World orientation has a degenerate Y axis.");
    }
    yAxis /= yLength;

    glm::vec3 zAxis = glm::cross(xAxis, yAxis);
    const float zLength = glm::length(zAxis);
    if (!std::isfinite(zLength) || zLength <= 1.0e-6f ||
        !transform_math::isFiniteVector(zSource)) {
        return Result::failure("World orientation has a degenerate Z axis.");
    }
    zAxis /= zLength;

    // A proper orthonormal frame is required by ImGuizmo. Reflection signs
    // remain authored scale data rather than becoming gizmo orientation.
    if (glm::dot(zAxis, zSource) < 0.0f) {
        xAxis = -xAxis;
        zAxis = glm::cross(xAxis, yAxis);
        const float correctedLength = glm::length(zAxis);
        if (!std::isfinite(correctedLength) || correctedLength <= 1.0e-6f) {
            return Result::failure("World orientation cannot be normalized.");
        }
        zAxis /= correctedLength;
    }

    orientation[0] = glm::vec4(xAxis, 0.0f);
    orientation[1] = glm::vec4(yAxis, 0.0f);
    orientation[2] = glm::vec4(zAxis, 0.0f);
    orientation[3] = glm::vec4(0.0f, 0.0f, 0.0f, 1.0f);
    return transform_math::isFiniteMatrix(orientation)
               ? Result::success()
               : Result::failure("World orientation is not finite.");
}

Result makeGizmoFrame(const glm::vec3& pivot, const glm::mat4& orientation,
                      glm::mat4& frame) {
    frame = glm::mat4(1.0f);
    if (!transform_math::isFiniteVector(pivot) ||
        !isAffineMatrix(orientation)) {
        return Result::failure(
            "Gizmo frame requires a finite pivot and orientation.");
    }
    frame = orientation;
    frame[3] = glm::vec4(pivot, 1.0f);
    if (!isAffineMatrix(frame)) {
        frame = glm::mat4(1.0f);
        return Result::failure("Gizmo frame is not finite affine data.");
    }
    glm::mat4 unusedInverse;
    if (!calculateSafeInverse(frame, unusedInverse)) {
        frame = glm::mat4(1.0f);
        return Result::failure("Gizmo frame orientation is not invertible.");
    }
    return Result::success();
}

Result calculateSelectionGizmoFrame(const EditorSession& editorSession,
                                    TransformSpace space, glm::mat4& frame) {
    frame = glm::mat4(1.0f);
    const std::vector<GameObject*> roots =
        editorSession.topLevelSelectedRoots();
    GameObject* active = editorSession.activeGameObject();
    if (roots.empty() || active == nullptr) {
        return Result::failure(
            "A non-empty selection and Active Object are required for a gizmo frame.");
    }

    glm::vec3 pivot;
    const Result pivotResult = calculateSharedPivot(roots, pivot);
    if (!pivotResult) {
        return pivotResult;
    }

    glm::mat4 orientation = identityOrientation();
    if (space == TransformSpace::Local) {
        GameObject* activeRoot = nullptr;
        const Result rootResult = resolveActiveTransformRoot(
            active, editorSession.selectedGameObjects(), activeRoot);
        if (!rootResult) {
            return rootResult;
        }
        const Result orientationResult = calculateWorldOrientation(
            activeRoot->worldTransformMatrix(), orientation);
        if (!orientationResult) {
            return orientationResult;
        }
    }
    return makeGizmoFrame(pivot, orientation, frame);
}

Result captureTransformDragSnapshot(
    Scene& scene, EditorSession& editorSession, TransformSpace space,
    TransformDragSnapshot& snapshot) {
    snapshot = {};
    if (editorSession.selectionScene() != &scene) {
        return Result::failure(
            "The selected scene does not match the transform scene.");
    }

    const std::vector<GameObject*>& selected =
        editorSession.selectedGameObjects();
    GameObject* active = editorSession.activeGameObject();
    if (selected.empty() || active == nullptr) {
        return Result::failure(
            "A non-empty selection and Active Object are required.");
    }

    snapshot.scene = &scene;
    snapshot.editorSession = &editorSession;
    snapshot.active = active;
    snapshot.space = space;
    snapshot.tool = editorSession.transformTool();
    snapshot.selectedObjects = selected;

    snapshot.sceneObjects.reserve(scene.gameObjects().size());
    snapshot.sceneObjectPersistentIds.reserve(scene.gameObjects().size());
    snapshot.sceneObjectParents.reserve(scene.gameObjects().size());
    for (const std::unique_ptr<GameObject>& owner : scene.gameObjects()) {
        if (owner == nullptr) {
            snapshot = {};
            return Result::failure("The scene contains a null GameObject.");
        }
        snapshot.sceneObjects.push_back(owner.get());
        snapshot.sceneObjectPersistentIds.push_back(owner->persistentId);
        snapshot.sceneObjectParents.push_back(owner->parent());
    }

    std::unordered_set<GameObject*> ownedObjects;
    ownedObjects.reserve(snapshot.sceneObjects.size());
    for (GameObject* object : snapshot.sceneObjects) {
        if (!ownedObjects.insert(object).second) {
            snapshot = {};
            return Result::failure("The scene contains a duplicate GameObject.");
        }
    }
    for (std::size_t index = 0; index < snapshot.sceneObjects.size(); ++index) {
        GameObject* parent = snapshot.sceneObjectParents[index];
        if (parent != nullptr && ownedObjects.count(parent) == 0) {
            snapshot = {};
            return Result::failure("The scene hierarchy has an invalid parent link.");
        }
    }

    std::unordered_set<GameObject*> selectedSet;
    selectedSet.reserve(selected.size());
    for (GameObject* object : selected) {
        if (object == nullptr || ownedObjects.count(object) == 0 ||
            !selectedSet.insert(object).second) {
            snapshot = {};
            return Result::failure(
                "The transform selection contains a stale or duplicate object.");
        }
    }
    if (selectedSet.count(active) == 0) {
        snapshot = {};
        return Result::failure("The Active Object is not selected.");
    }

    snapshot.topLevelSelectedRoots = editorSession.topLevelSelectedRoots();
    if (snapshot.topLevelSelectedRoots.empty()) {
        snapshot = {};
        return Result::failure("The selection has no top-level transform root.");
    }

    std::unordered_set<GameObject*> rootSet;
    rootSet.reserve(snapshot.topLevelSelectedRoots.size());
    for (GameObject* root : snapshot.topLevelSelectedRoots) {
        if (root == nullptr || ownedObjects.count(root) == 0 ||
            selectedSet.count(root) == 0 || !rootSet.insert(root).second) {
            snapshot = {};
            return Result::failure("The top-level transform roots are invalid.");
        }
    }

    std::vector<GameObject*> pending;
    pending.reserve(snapshot.topLevelSelectedRoots.size());
    for (auto iterator = snapshot.topLevelSelectedRoots.rbegin();
         iterator != snapshot.topLevelSelectedRoots.rend(); ++iterator) {
        pending.push_back(*iterator);
    }
    std::unordered_set<GameObject*> affectedSet;
    affectedSet.reserve(snapshot.sceneObjects.size());
    while (!pending.empty()) {
        GameObject* object = pending.back();
        pending.pop_back();
        if (object == nullptr || ownedObjects.count(object) == 0 ||
            !affectedSet.insert(object).second) {
            snapshot = {};
            return Result::failure(
                "The selected-root hierarchy overlaps or is malformed.");
        }

        if (object->parent() != nullptr &&
            ownedObjects.count(object->parent()) == 0) {
            snapshot = {};
            return Result::failure("The affected hierarchy has an invalid parent link.");
        }

        std::unordered_set<GameObject*> ancestorChain;
        for (GameObject* ancestor = object; ancestor != nullptr;
             ancestor = ancestor->parent()) {
            if (ownedObjects.count(ancestor) == 0 ||
                !ancestorChain.insert(ancestor).second) {
                snapshot = {};
                return Result::failure(
                    "The affected hierarchy contains a cycle or foreign parent.");
            }
        }

        std::unordered_set<GameObject*> childSet;
        childSet.reserve(object->children().size());
        for (GameObject* child : object->children()) {
            if (child == nullptr || ownedObjects.count(child) == 0 ||
                child->parent() != object || !childSet.insert(child).second) {
                snapshot = {};
                return Result::failure("The affected hierarchy has an invalid child link.");
            }
            pending.push_back(child);
        }
    }

    for (GameObject* object : selected) {
        if (affectedSet.count(object) == 0) {
            snapshot = {};
            return Result::failure(
                "A selected object is outside the affected root hierarchy.");
        }
    }

    // Capture all unchanged ancestor worlds used at transform boundaries.
    std::unordered_set<GameObject*> externalSet;
    for (GameObject* root : snapshot.topLevelSelectedRoots) {
        GameObject* child = root;
        std::unordered_set<GameObject*> ancestorChain;
        for (GameObject* parent = root->parent(); parent != nullptr;
             parent = parent->parent()) {
            if (ownedObjects.count(parent) == 0 ||
                !ancestorChain.insert(parent).second ||
                countChildLinks(parent, child) != 1) {
                snapshot = {};
                return Result::failure(
                    "The external parent hierarchy is malformed.");
            }
            if (affectedSet.count(parent) != 0) {
                break;
            }
            if (externalSet.insert(parent).second) {
                TransformExternalParentSnapshot parentSnapshot;
                parentSnapshot.object = parent;
                parentSnapshot.parent = parent->parent();
                parentSnapshot.originalWorld = parent->worldTransformMatrix();
                if (!isAffineMatrix(parentSnapshot.originalWorld)) {
                    snapshot = {};
                    return Result::failure(
                        "An external parent world transform is invalid.");
                }
                snapshot.externalParents.push_back(parentSnapshot);
            }
            child = parent;
        }
    }

    // The traversal order is parent-before-child, which lets the solver
    // preserve same-category locals without recursively mutating parents.
    snapshot.objects.reserve(affectedSet.size());
    pending.clear();
    for (auto iterator = snapshot.topLevelSelectedRoots.rbegin();
         iterator != snapshot.topLevelSelectedRoots.rend(); ++iterator) {
        pending.push_back(*iterator);
    }
    while (!pending.empty()) {
        GameObject* object = pending.back();
        pending.pop_back();
        TransformObjectSnapshot objectSnapshot;
        objectSnapshot.object = object;
        objectSnapshot.parent = object->parent();
        objectSnapshot.selected = selectedSet.count(object) != 0;
        objectSnapshot.originalChildren = object->children();
        objectSnapshot.originalLocal.position = object->position;
        objectSnapshot.originalLocal.rotation = object->rotation;
        objectSnapshot.originalLocal.scale = object->scale;
        if (!transform_math::isFiniteVector(objectSnapshot.originalLocal.position) ||
            !transform_math::isFiniteVector(objectSnapshot.originalLocal.rotation) ||
            !transform_math::isFiniteVector(objectSnapshot.originalLocal.scale)) {
            snapshot = {};
            return Result::failure(
                "An affected GameObject has non-finite authored transform data.");
        }
        objectSnapshot.originalWorld = object->worldTransformMatrix();
        if (!isAffineMatrix(objectSnapshot.originalWorld)) {
            snapshot = {};
            return Result::failure("An affected GameObject world transform is invalid.");
        }
        const Result cameraResult = editor_mutation::captureCameraTransformState(
            *object, objectSnapshot.originalCamera);
        if (!cameraResult) {
            snapshot = {};
            return cameraResult;
        }
        if (objectSnapshot.originalCamera.camera != nullptr) {
            for (const TransformObjectSnapshot& previous : snapshot.objects) {
                if (previous.originalCamera.camera ==
                    objectSnapshot.originalCamera.camera) {
                    snapshot = {};
                    return Result::failure(
                        "One auxiliary Camera is associated with multiple affected owners.");
                }
            }
        }
        snapshot.objects.push_back(objectSnapshot);
        for (auto iterator = objectSnapshot.originalChildren.rbegin();
             iterator != objectSnapshot.originalChildren.rend(); ++iterator) {
            pending.push_back(*iterator);
        }
    }

    const Result rootResult = resolveActiveTransformRoot(
        active, snapshot.selectedObjects, snapshot.activeTransformRoot);
    if (!rootResult || rootSet.count(snapshot.activeTransformRoot) == 0) {
        snapshot = {};
        return rootResult
                   ? Result::failure(
                         "The Active Object does not belong to a selected root.")
                   : rootResult;
    }

    const Result pivotResult =
        calculateSharedPivot(snapshot.topLevelSelectedRoots, snapshot.pivot);
    if (!pivotResult) {
        snapshot = {};
        return pivotResult;
    }

    if (space == TransformSpace::Local) {
        const TransformObjectSnapshot* rootSnapshot = findObjectSnapshot(
            snapshot, snapshot.activeTransformRoot);
        if (rootSnapshot == nullptr) {
            snapshot = {};
            return Result::failure(
                "The Active transform root is not in the snapshot.");
        }
        const Result orientationResult = calculateWorldOrientation(
            rootSnapshot->originalWorld, snapshot.gizmoOrientation);
        if (!orientationResult) {
            snapshot = {};
            return orientationResult;
        }
    } else {
        snapshot.gizmoOrientation = identityOrientation();
    }

    const Result frameResult = makeGizmoFrame(
        snapshot.pivot, snapshot.gizmoOrientation,
        snapshot.originalGizmoWorld);
    if (!frameResult) {
        snapshot = {};
        return frameResult;
    }
    snapshot.valid = true;
    return Result::success();
}

Result solveTransformDrag(const TransformDragSnapshot& snapshot,
                          const glm::mat4& currentGizmoWorld,
                          std::vector<TransformCandidate>& candidates) {
    candidates.clear();
    const Result snapshotResult = validateSnapshot(snapshot);
    if (!snapshotResult) {
        return snapshotResult;
    }
    if (!isAffineMatrix(currentGizmoWorld)) {
        return Result::failure("Current gizmo frame must be finite affine data.");
    }

    glm::mat4 inverseOriginalGizmo;
    if (!calculateSafeInverse(snapshot.originalGizmoWorld,
                              inverseOriginalGizmo)) {
        return Result::failure("Original gizmo frame is not invertible.");
    }
    const glm::mat4 sharedDelta = currentGizmoWorld * inverseOriginalGizmo;
    if (!isAffineMatrix(sharedDelta)) {
        return Result::failure("Shared gizmo delta is not finite affine data.");
    }

    std::unordered_map<GameObject*, std::size_t> objectIndices;
    objectIndices.reserve(snapshot.objects.size());
    for (std::size_t index = 0; index < snapshot.objects.size(); ++index) {
        if (!objectIndices.emplace(snapshot.objects[index].object, index).second) {
            return Result::failure("Transform snapshot contains a duplicate object.");
        }
    }

    std::unordered_map<GameObject*, glm::mat4> desiredWorlds;
    desiredWorlds.reserve(snapshot.objects.size());
    for (const TransformObjectSnapshot& objectSnapshot : snapshot.objects) {
        const glm::mat4 desiredWorld = objectSnapshot.selected
                                           ? sharedDelta *
                                                 objectSnapshot.originalWorld
                                           : objectSnapshot.originalWorld;
        if (!isAffineMatrix(desiredWorld) ||
            !desiredWorlds.emplace(objectSnapshot.object, desiredWorld).second) {
            return Result::failure("A transform candidate world matrix is invalid.");
        }
    }

    std::unordered_map<GameObject*, const TransformExternalParentSnapshot*>
        externalParents;
    externalParents.reserve(snapshot.externalParents.size());
    for (const TransformExternalParentSnapshot& parentSnapshot :
         snapshot.externalParents) {
        externalParents.emplace(parentSnapshot.object, &parentSnapshot);
    }

    std::vector<TransformCandidate> solvedCandidates;
    solvedCandidates.reserve(snapshot.objects.size());
    for (const TransformObjectSnapshot& objectSnapshot : snapshot.objects) {
        TransformCandidate candidate;
        candidate.object = objectSnapshot.object;
        candidate.desiredWorld =
            desiredWorlds.find(objectSnapshot.object)->second;

        const auto parentIndex = objectIndices.find(objectSnapshot.parent);
        const bool parentIsAffected = parentIndex != objectIndices.end();
        glm::mat4 originalParentWorld(1.0f);
        glm::mat4 candidateParentWorld(1.0f);
        if (parentIsAffected) {
            originalParentWorld = snapshot.objects[parentIndex->second].originalWorld;
            candidateParentWorld = desiredWorlds.find(objectSnapshot.parent)
                                       ->second;
        } else if (objectSnapshot.parent != nullptr) {
            const auto external = externalParents.find(objectSnapshot.parent);
            if (external == externalParents.end()) {
                return Result::failure(
                    "A transform boundary parent is missing from the snapshot.");
            }
            originalParentWorld = external->second->originalWorld;
            candidateParentWorld = originalParentWorld;
        }
        const bool preserveLocal =
            parentIsAffected &&
            snapshot.objects[parentIndex->second].selected ==
                objectSnapshot.selected;
        if (preserveLocal) {
            candidate.local = objectSnapshot.originalLocal;
        } else {
            const Result localResult = deriveLocalTransformFromParentWorld(
                candidateParentWorld, objectSnapshot.originalLocal.scale,
                candidate.desiredWorld, candidate.local);
            if (!localResult) {
                return localResult;
            }

            if (snapshot.tool == TransformTool::Rotate) {
                const glm::vec3 decomposedScale = candidate.local.scale;
                candidate.local.scale = objectSnapshot.originalLocal.scale;
                const glm::mat4 stabilizedLocal =
                    transform_math::makeModelMatrix(
                        candidate.local.position, candidate.local.rotation,
                        candidate.local.scale);
                const glm::mat4 stabilizedWorld =
                    candidateParentWorld * stabilizedLocal;
                if (!isAffineMatrix(stabilizedLocal) ||
                    !isAffineMatrix(stabilizedWorld) ||
                    !matricesEquivalentWithinTransformTolerance(
                        stabilizedWorld, candidate.desiredWorld)) {
                    candidate.local.scale = decomposedScale;
                }
            }
        }

        const glm::mat4 rebuiltLocal = transform_math::makeModelMatrix(
            candidate.local.position, candidate.local.rotation,
            candidate.local.scale);
        if (!isAffineMatrix(rebuiltLocal) ||
            !transform_math::isFiniteVector(candidate.local.position) ||
            !transform_math::isFiniteVector(candidate.local.rotation) ||
            !transform_math::isFiniteVector(candidate.local.scale)) {
            return Result::failure("A transform candidate local TRS is invalid.");
        }

        if (objectSnapshot.originalCamera.camera != nullptr) {
            const Result cameraResult =
                editor_mutation::prepareCameraTransformState(
                    *objectSnapshot.object, objectSnapshot.originalWorld,
                    candidate.desiredWorld,
                    originalParentWorld, candidateParentWorld,
                    objectSnapshot.originalLocal.rotation,
                    candidate.local.rotation, objectSnapshot.originalCamera,
                    candidate.camera,
                    !objectSnapshot.selected && parentIsAffected &&
                        snapshot.objects[parentIndex->second].selected);
            if (!cameraResult) {
                return cameraResult;
            }
        }
        solvedCandidates.push_back(candidate);
    }
    candidates = std::move(solvedCandidates);
    return Result::success();
}

Result commitTransformCandidates(
    const TransformDragSnapshot& snapshot,
    const std::vector<TransformCandidate>& candidates) {
    const Result snapshotResult = validateSnapshot(snapshot);
    if (!snapshotResult) {
        return snapshotResult;
    }
    if (candidates.size() != snapshot.objects.size()) {
        return Result::failure("Transform candidate count does not match the snapshot.");
    }

    std::unordered_set<GameObject*> committedObjects;
    committedObjects.reserve(candidates.size());
    for (std::size_t index = 0; index < candidates.size(); ++index) {
        const TransformCandidate& candidate = candidates[index];
        const TransformObjectSnapshot& objectSnapshot = snapshot.objects[index];
        if (candidate.object != objectSnapshot.object ||
            !committedObjects.insert(candidate.object).second ||
            !isAffineMatrix(candidate.desiredWorld) ||
            !transform_math::isFiniteVector(candidate.local.position) ||
            !transform_math::isFiniteVector(candidate.local.rotation) ||
            !transform_math::isFiniteVector(candidate.local.scale) ||
            !transform_math::isFiniteMatrix(transform_math::makeModelMatrix(
                candidate.local.position, candidate.local.rotation,
                candidate.local.scale))) {
            return Result::failure("Transform candidate validation failed.");
        }
        if (objectSnapshot.originalCamera.camera != candidate.camera.camera) {
            return Result::failure(
                "Camera candidate association does not match the snapshot.");
        }
        if (candidate.camera.camera != nullptr &&
            (!transform_math::isFiniteVector(candidate.camera.position) ||
             !transform_math::isFiniteVector(candidate.camera.front) ||
             !transform_math::isFiniteVector(candidate.camera.up))) {
            return Result::failure("Camera candidate state is not finite.");
        }
    }

    // Every possible failure has been checked before the first authored write.
    for (const TransformCandidate& candidate : candidates) {
        candidate.object->position = candidate.local.position;
        candidate.object->rotation = candidate.local.rotation;
        candidate.object->scale = candidate.local.scale;
    }
    for (const TransformCandidate& candidate : candidates) {
        if (candidate.camera.camera != nullptr) {
            // A standalone Camera's position is GameObject::position itself;
            // the authored write above already committed it. Attached
            // cameras have an independent auxiliary position.
            if (dynamic_cast<Camera*>(candidate.object) == nullptr) {
                candidate.camera.camera->position = candidate.camera.position;
            }
            candidate.camera.camera->front = candidate.camera.front;
            candidate.camera.camera->up = candidate.camera.up;
        }
    }
    return Result::success();
}

bool restoreTransformDragSnapshot(
    const TransformDragSnapshot& snapshot) noexcept {
    if (!snapshot.valid || snapshot.scene == nullptr ||
        snapshot.sceneObjects.size() != snapshot.scene->gameObjects().size() ||
        snapshot.sceneObjectPersistentIds.size() != snapshot.sceneObjects.size() ||
        snapshot.sceneObjectParents.size() != snapshot.sceneObjects.size()) {
        return false;
    }
    for (std::size_t index = 0; index < snapshot.sceneObjects.size(); ++index) {
        const auto& owner = snapshot.scene->gameObjects()[index];
        if (owner == nullptr || owner.get() != snapshot.sceneObjects[index] ||
            owner->persistentId != snapshot.sceneObjectPersistentIds[index]) {
            return false;
        }
    }
    for (std::size_t index = 0; index < snapshot.sceneObjects.size(); ++index) {
        if (snapshot.sceneObjects[index]->parent() !=
                snapshot.sceneObjectParents[index] ||
            (snapshot.sceneObjectParents[index] != nullptr &&
             !ownsObject(snapshot.sceneObjects,
                         snapshot.sceneObjectParents[index]))) {
            return false;
        }
    }
    for (const TransformObjectSnapshot& objectSnapshot : snapshot.objects) {
        if (objectSnapshot.object == nullptr ||
            !ownsObject(snapshot.sceneObjects, objectSnapshot.object) ||
            objectSnapshot.object->parent() != objectSnapshot.parent ||
            (objectSnapshot.parent != nullptr &&
             !ownsObject(snapshot.sceneObjects, objectSnapshot.parent))) {
            return false;
        }
        const std::vector<GameObject*>& children =
            objectSnapshot.object->children();
        if (children.size() != objectSnapshot.originalChildren.size() ||
            !std::equal(children.begin(), children.end(),
                        objectSnapshot.originalChildren.begin())) {
            return false;
        }
        for (GameObject* child : children) {
            if (child == nullptr || !ownsObject(snapshot.sceneObjects, child) ||
                child->parent() != objectSnapshot.object) {
                return false;
            }
        }
        editor_mutation::CameraTransformState currentCamera;
        if (!editor_mutation::captureCameraTransformState(
                *objectSnapshot.object, currentCamera) ||
            currentCamera.camera != objectSnapshot.originalCamera.camera) {
            return false;
        }
    }
    for (const TransformExternalParentSnapshot& parentSnapshot :
         snapshot.externalParents) {
        if (parentSnapshot.object == nullptr ||
            !ownsObject(snapshot.sceneObjects, parentSnapshot.object) ||
            parentSnapshot.object->parent() != parentSnapshot.parent ||
            (parentSnapshot.parent != nullptr &&
             (!ownsObject(snapshot.sceneObjects, parentSnapshot.parent) ||
              countChildLinks(parentSnapshot.parent, parentSnapshot.object) !=
                  1))) {
            return false;
        }
        const glm::mat4 currentWorld =
            parentSnapshot.object->worldTransformMatrix();
        if (!isAffineMatrix(currentWorld) ||
            !matricesExactlyEqual(currentWorld, parentSnapshot.originalWorld)) {
            return false;
        }
    }

    for (const TransformObjectSnapshot& objectSnapshot : snapshot.objects) {
        objectSnapshot.object->position = objectSnapshot.originalLocal.position;
        objectSnapshot.object->rotation = objectSnapshot.originalLocal.rotation;
        objectSnapshot.object->scale = objectSnapshot.originalLocal.scale;
    }
    for (const TransformObjectSnapshot& objectSnapshot : snapshot.objects) {
        if (objectSnapshot.originalCamera.camera != nullptr) {
            if (dynamic_cast<Camera*>(objectSnapshot.object) == nullptr) {
                objectSnapshot.originalCamera.camera->position =
                    objectSnapshot.originalCamera.position;
            }
            objectSnapshot.originalCamera.camera->front =
                objectSnapshot.originalCamera.front;
            objectSnapshot.originalCamera.camera->up =
                objectSnapshot.originalCamera.up;
        }
    }
    return true;
}

Result deriveLocalTransformFromWorld(
    const GameObject& object, const glm::mat4& candidateWorld,
    transform_math::DecomposedTransform& local) {
    glm::mat4 parentWorld(1.0f);
    if (const GameObject* parent = object.parent()) {
        parentWorld = parent->worldTransformMatrix();
    }
    return deriveLocalTransformFromParentWorld(
        parentWorld, object.scale, candidateWorld, local);
}

Result deriveLocalTransformFromWorld(
    const glm::mat4& parentWorld, const glm::vec3& authoredScaleHint,
    const glm::mat4& candidateWorld,
    transform_math::DecomposedTransform& local) {
    return deriveLocalTransformFromParentWorld(
        parentWorld, authoredScaleHint, candidateWorld, local);
}

}  // namespace editor_transform
