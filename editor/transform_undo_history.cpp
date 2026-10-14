#include "transform_undo_history.h"

#include "editor_mutation.h"
#include "../math/transform_math.h"
#include "../scene/camera.h"
#include "../scene/game_object.h"
#include "../scene/scene.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <utility>

namespace editor_undo {
namespace {

constexpr std::size_t maxEditorUndoHistoryBytes = 64u * 1024u * 1024u;
constexpr float noOpTolerance = 1.0e-5f;
constexpr float worldStateTolerance = 1.0e-4f;

[[nodiscard]] bool nearlyEqual(float first, float second,
                               float tolerance) noexcept {
    return std::fabs(first - second) <= tolerance;
}

[[nodiscard]] bool vectorsNear(const glm::vec3& first,
                               const glm::vec3& second,
                               float tolerance) noexcept {
    return nearlyEqual(first.x, second.x, tolerance) &&
           nearlyEqual(first.y, second.y, tolerance) &&
           nearlyEqual(first.z, second.z, tolerance);
}

[[nodiscard]] bool matricesNear(const glm::mat4& first,
                                const glm::mat4& second,
                                float tolerance) noexcept {
    if (!transform_math::isFiniteMatrix(first) ||
        !transform_math::isFiniteMatrix(second)) {
        return false;
    }
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            const float scale = std::max(
                {1.0f, std::fabs(first[column][row]),
                 std::fabs(second[column][row])});
            if (std::fabs(first[column][row] - second[column][row]) >
                tolerance * scale) {
                return false;
            }
        }
    }
    return true;
}

[[nodiscard]] bool vectorsExactlyEqual(const glm::vec3& first,
                                       const glm::vec3& second) noexcept {
    return first.x == second.x && first.y == second.y && first.z == second.z;
}

[[nodiscard]] bool isFinite(const TransformUndoCameraState& camera) noexcept {
    if (!camera.present) {
        return true;
    }
    if (camera.instanceId == 0 ||
        !transform_math::isFiniteVector(camera.position) ||
        !transform_math::isFiniteVector(camera.front) ||
        !transform_math::isFiniteVector(camera.up)) {
        return false;
    }
    const glm::vec3 cross = glm::cross(camera.front, camera.up);
    const float crossLengthSquared = glm::dot(cross, cross);
    return transform_math::isFiniteVector(cross) &&
           std::isfinite(crossLengthSquared) &&
           crossLengthSquared > 1.0e-8f;
}

[[nodiscard]] bool isFinite(const TransformUndoObjectState& state) noexcept {
    return !state.persistentId.empty() && state.instanceId != 0 &&
           ((state.parentPersistentId.empty() && state.parentInstanceId == 0) ||
            (!state.parentPersistentId.empty() && state.parentInstanceId != 0)) &&
           transform_math::isFiniteVector(state.position) &&
           transform_math::isFiniteVector(state.rotation) &&
           transform_math::isFiniteVector(state.scale) && isFinite(state.camera);
}

[[nodiscard]] bool sameCameraAssociation(
    const TransformUndoCameraState& first,
    const TransformUndoCameraState& second) noexcept {
    return first.present == second.present &&
           first.standalone == second.standalone &&
           first.instanceId == second.instanceId;
}

[[nodiscard]] bool sameCameraPose(const TransformUndoCameraState& first,
                                  const TransformUndoCameraState& second,
                                  bool exact) noexcept {
    if (!sameCameraAssociation(first, second)) {
        return false;
    }
    if (!first.present) {
        return true;
    }
    if (exact) {
        return vectorsExactlyEqual(first.position, second.position) &&
               vectorsExactlyEqual(first.front, second.front) &&
               vectorsExactlyEqual(first.up, second.up);
    }
    return vectorsNear(first.position, second.position, noOpTolerance) &&
           vectorsNear(first.front, second.front, noOpTolerance) &&
           vectorsNear(first.up, second.up, noOpTolerance);
}

[[nodiscard]] bool sameAuthoredTransform(
    const TransformUndoObjectState& first,
    const TransformUndoObjectState& second, bool exact) noexcept {
    if (first.persistentId != second.persistentId ||
        first.instanceId != second.instanceId ||
        first.parentPersistentId != second.parentPersistentId ||
        first.parentInstanceId != second.parentInstanceId) {
        return false;
    }
    if (exact) {
        return vectorsExactlyEqual(first.position, second.position) &&
               vectorsExactlyEqual(first.rotation, second.rotation) &&
               vectorsExactlyEqual(first.scale, second.scale) &&
               sameCameraPose(first.camera, second.camera, true);
    }
    return vectorsNear(first.position, second.position, noOpTolerance) &&
           vectorsNear(first.rotation, second.rotation, noOpTolerance) &&
           vectorsNear(first.scale, second.scale, noOpTolerance) &&
           sameCameraPose(first.camera, second.camera, false);
}

[[nodiscard]] Result captureObjectState(
    const Scene& scene, const GameObject& object,
    TransformUndoObjectState& state, glm::mat4* worldTransform = nullptr) {
    state = {};
    if (object.persistentId.empty() ||
        scene.findGameObject(object.persistentId) != &object) {
        return Result::failure(
            "Transform undo requires a GameObject with a valid persistent ID in the Scene.");
    }
    state.persistentId = object.persistentId;
    state.instanceId = object.instanceId();
    state.position = object.position;
    state.rotation = object.rotation;
    state.scale = object.scale;
    if (const GameObject* parent = object.parent()) {
        if (parent->persistentId.empty() ||
            scene.findGameObject(parent->persistentId) != parent) {
            return Result::failure(
                "Transform undo found an invalid parent identity.");
        }
        state.parentPersistentId = parent->persistentId;
        state.parentInstanceId = parent->instanceId();
    }

    editor_mutation::CameraTransformState cameraState;
    const Result cameraResult =
        editor_mutation::captureCameraTransformState(object, cameraState);
    if (!cameraResult) {
        return Result::failure("Failed to capture Camera transform state: " +
                               cameraResult.error());
    }
    if (cameraState.camera != nullptr) {
        state.camera.present = true;
        state.camera.standalone =
            dynamic_cast<const Camera*>(&object) != nullptr;
        state.camera.instanceId = cameraState.camera->instanceId();
        state.camera.position = cameraState.position;
        state.camera.front = cameraState.front;
        state.camera.up = cameraState.up;
    }
    if (!isFinite(state)) {
        return Result::failure(
            "Transform undo encountered non-finite authored or Camera state.");
    }
    const glm::mat4 currentWorld = object.worldTransformMatrix();
    if (!transform_math::isFiniteMatrix(currentWorld)) {
        return Result::failure(
            "Transform undo encountered a non-finite world transform.");
    }
    if (worldTransform != nullptr) {
        *worldTransform = currentWorld;
    }
    return Result::success();
}

[[nodiscard]] std::size_t saturatedAdd(std::size_t first,
                                       std::size_t second) noexcept {
    if (second > std::numeric_limits<std::size_t>::max() - first) {
        return std::numeric_limits<std::size_t>::max();
    }
    return first + second;
}

}  // namespace

EditorUndoHistory::~EditorUndoHistory() = default;

std::size_t EditorUndoHistory::actionBytes(const Action& action) noexcept {
    return std::visit([](const auto& concrete) {
        return concrete.estimatedBytes;
    }, action);
}

void EditorUndoHistory::enforceLimits() noexcept {
    while (actions_.size() > maxEditorUndoActions ||
           estimatedBytes_ > maxEditorUndoHistoryBytes) {
        estimatedBytes_ -= actionBytes(actions_.front());
        actions_.pop_front();
    }
}

void EditorUndoHistory::clear() noexcept {
    actions_.clear();
    estimatedBytes_ = 0;
    stagedDeletePending_ = false;
}

Result EditorUndoHistory::capture(
    const Scene& scene, const std::vector<GameObject*>& objects,
    TransformUndoSnapshot& snapshot) const {
    snapshot = {};
    if (objects.empty()) {
        return Result::failure(
            "Transform undo cannot capture an empty object set.");
    }

    try {
        TransformUndoSnapshot candidate;
        candidate.sceneLifetimeId = scene.lifetimeId();
        candidate.objects.reserve(objects.size());
        candidate.worldTransforms.reserve(objects.size());
        std::unordered_set<std::uint64_t> seenInstances;
        seenInstances.reserve(objects.size());
        std::unordered_set<std::uint64_t> seenCameraInstances;
        seenCameraInstances.reserve(objects.size());
        for (const GameObject* object : objects) {
            if (object == nullptr ||
                !seenInstances.insert(object->instanceId()).second) {
                return Result::failure(
                    "Transform undo capture contains a null or duplicate GameObject.");
            }
            TransformUndoObjectState state;
            glm::mat4 worldTransform(1.0f);
            const Result result = captureObjectState(
                scene, *object, state, &worldTransform);
            if (!result) {
                return result;
            }
            if (state.camera.present &&
                !seenCameraInstances.insert(state.camera.instanceId).second) {
                return Result::failure(
                    "Transform undo capture contains a Camera shared by multiple GameObjects.");
            }
            candidate.objects.push_back(std::move(state));
            candidate.worldTransforms.push_back(worldTransform);
        }
        candidate.valid = true;
        snapshot = std::move(candidate);
        return Result::success();
    } catch (const std::exception& exception) {
        snapshot = {};
        return Result::failure(
            "Failed to capture transform undo state: " +
            std::string(exception.what()));
    } catch (...) {
        snapshot = {};
        return Result::failure(
            "Failed to capture transform undo state with an unknown error.");
    }
}

Result EditorUndoHistory::commit(
    const Scene& scene, const TransformUndoSnapshot& snapshot) {
    if (!snapshot.valid || snapshot.sceneLifetimeId != scene.lifetimeId() ||
        snapshot.objects.empty() ||
        snapshot.worldTransforms.size() != snapshot.objects.size()) {
        return Result::failure(
            "Transform undo transaction does not match the active Scene.");
    }

    try {
        TransformAction action;
        action.sceneLifetimeId = snapshot.sceneLifetimeId;
        action.objects.reserve(snapshot.objects.size());
        std::unordered_set<std::uint64_t> seenInstances;
        seenInstances.reserve(snapshot.objects.size());
        std::unordered_set<std::uint64_t> seenCameraInstances;
        seenCameraInstances.reserve(snapshot.objects.size());
        bool changed = false;
        for (std::size_t index = 0; index < snapshot.objects.size(); ++index) {
            const TransformUndoObjectState& before = snapshot.objects[index];
            const glm::mat4& worldBefore = snapshot.worldTransforms[index];
            if (!isFinite(before) ||
                !transform_math::isFiniteMatrix(worldBefore) ||
                !seenInstances.insert(before.instanceId).second) {
                return Result::failure(
                    "Transform undo transaction contains invalid object state.");
            }
            const GameObject* object =
                scene.findGameObject(before.persistentId);
            if (object == nullptr || object->instanceId() != before.instanceId) {
                return Result::failure(
                    "A GameObject changed during the transform undo transaction.");
            }
            TransformUndoObjectState after;
            glm::mat4 worldAfter(1.0f);
            const Result result =
                captureObjectState(scene, *object, after, &worldAfter);
            if (!result) {
                return result;
            }
            if (before.parentPersistentId != after.parentPersistentId ||
                before.parentInstanceId != after.parentInstanceId ||
                !sameCameraAssociation(before.camera, after.camera)) {
                return Result::failure(
                    "Hierarchy or Camera association changed during the transform undo transaction.");
            }
            if (after.camera.present &&
                !seenCameraInstances.insert(after.camera.instanceId).second) {
                return Result::failure(
                    "Transform undo transaction contains a Camera shared by multiple GameObjects.");
            }
            changed = changed || !sameAuthoredTransform(before, after, false);
            action.objects.push_back(ObjectChange{
                before, std::move(after), worldBefore, worldAfter});
        }
        if (!changed) {
            return Result::success();
        }

        action.estimatedBytes = sizeof(Action);
        action.estimatedBytes = saturatedAdd(
            action.estimatedBytes,
            action.objects.capacity() * sizeof(ObjectChange));
        for (const ObjectChange& change : action.objects) {
            action.estimatedBytes = saturatedAdd(
                action.estimatedBytes, change.before.persistentId.capacity());
            action.estimatedBytes = saturatedAdd(
                action.estimatedBytes,
                change.before.parentPersistentId.capacity());
            action.estimatedBytes = saturatedAdd(
                action.estimatedBytes, change.after.persistentId.capacity());
            action.estimatedBytes = saturatedAdd(
                action.estimatedBytes,
                change.after.parentPersistentId.capacity());
        }
        if (action.estimatedBytes > maxEditorUndoHistoryBytes) {
            return Result::failure(
                "Transform action is too large for the in-memory undo budget.");
        }
        actions_.emplace_back(std::move(action));
        estimatedBytes_ += std::get<TransformAction>(actions_.back()).estimatedBytes;
        enforceLimits();
        return Result::success();
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to record transform undo action: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to record transform undo action with an unknown error.");
    }
}

Result EditorUndoHistory::stageDelete(Scene& scene,
                                      DeleteUndoAction&& action) {
    if (stagedDeletePending_) {
        return Result::failure(
            "Another Delete action is already being staged");
    }
    if (action.sceneLifetimeId != scene.lifetimeId() ||
        action.objects.empty() || action.estimatedBytes == 0) {
        return Result::failure(
            "Delete undo transaction does not match the active Scene");
    }
    if (action.estimatedBytes > maxEditorUndoHistoryBytes) {
        return Result::failure(
            "Delete is too large to retain safely in the in-memory undo budget");
    }
    for (const DeleteUndoObject& object : action.objects) {
        if (object.plannedObject == nullptr || object.retainedObject ||
            object.persistentId.empty() || object.instanceId == 0) {
            return Result::failure(
                "Delete undo transaction contains invalid ownership metadata");
        }
    }
    try {
        action.staged = true;
        actions_.emplace_back(std::move(action));
        stagedDeletePending_ = true;
        return Result::success();
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to reserve Delete undo history: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to reserve Delete undo history with an unknown error");
    }
}

DeleteUndoAction* EditorUndoHistory::stagedDeleteAction() noexcept {
    if (!stagedDeletePending_ || actions_.empty()) return nullptr;
    DeleteUndoAction* action = std::get_if<DeleteUndoAction>(&actions_.back());
    return action != nullptr && action->staged ? action : nullptr;
}

Result EditorUndoHistory::commitStagedDelete() noexcept {
    DeleteUndoAction* action = stagedDeleteAction();
    if (action == nullptr) {
        return Result::failure("No staged Delete undo action is available");
    }
    for (const DeleteUndoObject& object : action->objects) {
        const GameObject* source = object.retainedObject
                                       ? object.retainedObject.get()
                                       : object.plannedObject;
        if ((object.retainedObject && object.plannedObject != nullptr) ||
            source == nullptr || source->persistentId != object.persistentId ||
            source->instanceId() != object.instanceId) {
            return Result::failure(
                "Delete undo action lost a selected GameObject identity");
        }
    }
    action->staged = false;
    estimatedBytes_ += action->estimatedBytes;
    stagedDeletePending_ = false;
    enforceLimits();
    return Result::success();
}

void EditorUndoHistory::cancelStagedDelete() noexcept {
    if (!stagedDeletePending_ || actions_.empty()) return;
    const DeleteUndoAction* action =
        std::get_if<DeleteUndoAction>(&actions_.back());
    if (action != nullptr && action->staged) {
        actions_.pop_back();
    }
    stagedDeletePending_ = false;
}

Result EditorUndoHistory::undo(
    Scene& scene, const DeleteRestoreHandler& restoreDelete) {
    if (actions_.empty()) {
        return Result::success();
    }
    if (stagedDeletePending_) {
        return Result::failure(
            "Cannot undo while a Delete transaction is being committed");
    }
    const auto reject = [this](const char* message) {
        clear();
        return Result::failure(message);
    };

    if (DeleteUndoAction* deleteAction =
            std::get_if<DeleteUndoAction>(&actions_.back())) {
        if (deleteAction->staged ||
            deleteAction->sceneLifetimeId != scene.lifetimeId()) {
            return reject(
                "Delete undo history belongs to a different or replaced Scene; the history was cleared.");
        }
        if (!restoreDelete) {
            return Result::failure(
                "Delete undo requires the active editor restoration handler");
        }
        Result restoreResult = Result::failure(
            "Delete restoration handler did not complete");
        try {
            restoreResult = restoreDelete(scene, *deleteAction);
        } catch (const std::exception& exception) {
            restoreResult = Result::failure(
                "Delete restoration threw: " +
                std::string(exception.what()));
        } catch (...) {
            restoreResult = Result::failure(
                "Delete restoration threw an unknown exception");
        }
        if (!restoreResult) return restoreResult;
        for (const DeleteUndoObject& object : deleteAction->objects) {
            const GameObject* restored =
                scene.findGameObject(object.persistentId);
            if (object.retainedObject || object.plannedObject != nullptr ||
                restored == nullptr || restored->instanceId() != object.instanceId) {
                return Result::failure(
                    "Delete restoration did not transfer every original object back to the Scene");
            }
        }
        estimatedBytes_ -= deleteAction->estimatedBytes;
        actions_.pop_back();
        return Result::success();
    }

    const TransformAction& action = std::get<TransformAction>(actions_.back());
    if (action.sceneLifetimeId != scene.lifetimeId()) {
        return reject(
            "Transform undo history belongs to a different or replaced Scene; the history was cleared.");
    }

    struct ResolvedObject {
        GameObject* object = nullptr;
        Camera* camera = nullptr;
    };
    std::vector<ResolvedObject> resolved;
    try {
        resolved.reserve(action.objects.size());
        std::unordered_set<std::uint64_t> seenInstances;
        seenInstances.reserve(action.objects.size());
        for (const ObjectChange& change : action.objects) {
            const TransformUndoObjectState& before = change.before;
            const TransformUndoObjectState& after = change.after;
            if (!isFinite(before) || !isFinite(after) ||
                before.persistentId != after.persistentId ||
                before.instanceId != after.instanceId ||
                before.parentPersistentId != after.parentPersistentId ||
                before.parentInstanceId != after.parentInstanceId ||
                !sameCameraAssociation(before.camera, after.camera) ||
                !transform_math::isFiniteMatrix(change.expectedWorldBefore) ||
                !transform_math::isFiniteMatrix(change.expectedWorldAfter) ||
                !seenInstances.insert(after.instanceId).second) {
                return reject(
                    "Transform undo action contains invalid stored state; the history was cleared.");
            }

            GameObject* object = scene.findGameObject(after.persistentId);
            if (object == nullptr || object->instanceId() != after.instanceId ||
                object->persistentId != after.persistentId) {
                return reject(
                    "A transform undo target no longer exists with its recorded identity; the history was cleared.");
            }

            GameObject* expectedParent = nullptr;
            if (!after.parentPersistentId.empty()) {
                expectedParent =
                    scene.findGameObject(after.parentPersistentId);
                if (expectedParent == nullptr ||
                    expectedParent->instanceId() != after.parentInstanceId) {
                    return reject(
                        "A transform undo parent no longer exists with its recorded identity; the history was cleared.");
                }
            }
            if (object->parent() != expectedParent) {
                return reject(
                    "A transform undo target changed parent; the history was cleared.");
            }

            editor_mutation::CameraTransformState currentCameraState;
            const Result cameraResult =
                editor_mutation::captureCameraTransformState(
                    *object, currentCameraState);
            if (!cameraResult) {
                return reject(
                    "A transform undo Camera association is invalid; the history was cleared.");
            }
            const bool standalone = dynamic_cast<Camera*>(object) != nullptr;
            if (after.camera.present !=
                    (currentCameraState.camera != nullptr) ||
                (after.camera.present &&
                 (after.camera.standalone != standalone ||
                  after.camera.instanceId !=
                      currentCameraState.camera->instanceId()))) {
                return reject(
                    "A transform undo Camera association changed; the history was cleared.");
            }

            TransformUndoObjectState current;
            glm::mat4 currentWorld(1.0f);
            const Result currentResult =
                captureObjectState(scene, *object, current, &currentWorld);
            if (!currentResult || !sameAuthoredTransform(current, after, true) ||
                !matricesNear(currentWorld, change.expectedWorldAfter,
                              worldStateTolerance)) {
                return reject(
                    "A transform undo target changed since the action was recorded; the history was cleared.");
            }
            resolved.push_back({object, currentCameraState.camera});
        }
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to validate transform undo action; history was preserved: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to validate transform undo action; history was preserved after an unknown error.");
    }

    // All identities, hierarchy links, numeric values, and Camera poses have
    // been validated before the first write. These assignments cannot fail.
    for (std::size_t index = 0; index < action.objects.size(); ++index) {
        GameObject& object = *resolved[index].object;
        const TransformUndoObjectState& before = action.objects[index].before;
        object.position = before.position;
        object.rotation = before.rotation;
        object.scale = before.scale;
    }
    for (std::size_t index = 0; index < action.objects.size(); ++index) {
        Camera* camera = resolved[index].camera;
        if (camera == nullptr) {
            continue;
        }
        const TransformUndoCameraState& before = action.objects[index].before.camera;
        if (!before.standalone) {
            camera->position = before.position;
        }
        camera->front = before.front;
        camera->up = before.up;
    }

    estimatedBytes_ -= action.estimatedBytes;
    actions_.pop_back();
    return Result::success();
}

bool EditorUndoHistory::canUndo() const noexcept {
    return !actions_.empty() && !stagedDeletePending_;
}

std::size_t EditorUndoHistory::actionCount() const noexcept {
    return actions_.size();
}

}  // namespace editor_undo
