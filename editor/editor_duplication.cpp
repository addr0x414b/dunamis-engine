#include "editor_duplication.h"

#include "editor_session.h"
#include "editor_transform.h"
#include "../math/transform_math.h"
#include "../scene/camera.h"
#include "../scene/game_object.h"
#include "../scene/group.h"
#include "../scene/scene.h"
#include "../scene/scene_serializer.h"
#include "../scene/type_registry.h"

#include <charconv>
#include <cstdint>
#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace {

struct StructuralObjectSnapshot {
    GameObject* object = nullptr;
    GameObject* parent = nullptr;
    std::size_t siblingIndex = 0;
    glm::vec3 localPosition{0.0f};
    glm::vec3 localRotation{0.0f};
    glm::vec3 localScale{1.0f};
};

bool ownsObject(const Scene& scene, const GameObject* object) noexcept {
    if (object == nullptr) return false;
    for (const auto& owner : scene.gameObjects()) {
        if (owner.get() == object) return true;
    }
    return false;
}

std::vector<GameObject*> orderedTopLevelRoots(
    const Scene& scene, const EditorSession& editorSession) {
    const std::vector<GameObject*> unorderedRoots =
        editorSession.topLevelSelectedRoots();
    std::unordered_set<GameObject*> rootSet;
    rootSet.reserve(unorderedRoots.size());
    for (GameObject* root : unorderedRoots) {
        if (root != nullptr) rootSet.insert(root);
    }

    std::vector<GameObject*> ordered;
    ordered.reserve(unorderedRoots.size());
    std::vector<GameObject*> pending;
    pending.reserve(scene.gameObjects().size());
    for (auto iterator = scene.rootObjects().rbegin();
         iterator != scene.rootObjects().rend(); ++iterator) {
        pending.push_back(*iterator);
    }
    while (!pending.empty()) {
        GameObject* object = pending.back();
        pending.pop_back();
        if (object == nullptr) continue;
        if (rootSet.count(object) != 0) ordered.push_back(object);
        for (auto iterator = object->children().rbegin();
             iterator != object->children().rend(); ++iterator) {
            pending.push_back(*iterator);
        }
    }

    // A valid Scene makes the traversal above complete. Keep this fallback
    // deterministic if a caller presents a malformed selection before Scene
    // validation can report it.
    for (GameObject* root : unorderedRoots) {
        if (root != nullptr &&
            std::find(ordered.begin(), ordered.end(), root) == ordered.end()) {
            ordered.push_back(root);
        }
    }
    return ordered;
}

Result captureStructuralSnapshot(const Scene& scene, GameObject* object,
                                 StructuralObjectSnapshot& snapshot) {
    snapshot = {};
    if (!ownsObject(scene, object)) {
        return Result::failure(
            "Structural selection contains an object outside the Scene");
    }

    snapshot.object = object;
    snapshot.parent = object->parent();
    const auto& siblings = snapshot.parent == nullptr
                               ? scene.rootObjects()
                               : snapshot.parent->children();
    const auto sibling = std::find(siblings.begin(), siblings.end(), object);
    if (sibling == siblings.end()) {
        return Result::failure(
            "Structural selection object is missing from its sibling list");
    }
    snapshot.siblingIndex = static_cast<std::size_t>(
        std::distance(siblings.begin(), sibling));
    snapshot.localPosition = object->position;
    snapshot.localRotation = object->rotation;
    snapshot.localScale = object->scale;
    if (!transform_math::isFiniteVector(snapshot.localPosition) ||
        !transform_math::isFiniteVector(snapshot.localRotation) ||
        !transform_math::isFiniteVector(snapshot.localScale) ||
        !transform_math::isFiniteMatrix(object->worldTransformMatrix())) {
        return Result::failure(
            "Structural selection contains an invalid transform");
    }
    return Result::success();
}

Result validateStructuralContext(const Scene& scene,
                                 const EditorSession& editorSession) {
    if (editorSession.runState() != SceneRunState::Editing) {
        return Result::failure(
            "Structural hierarchy editing is available only while Editing");
    }
    if (editorSession.selectionScene() != &scene) {
        return Result::failure(
            "Structural selection does not belong to the requested Scene");
    }
    if (!scene.isActive()) {
        return Result::failure(
            "Structural hierarchy editing requires an active Scene");
    }
    return scene.validateAuthoredState();
}

Result makeUniqueGroupName(const Scene& scene, std::string& name) {
    std::unordered_set<std::string> names;
    names.reserve(scene.gameObjects().size());
    for (const auto& owner : scene.gameObjects()) {
        if (owner != nullptr) names.insert(owner->name);
    }
    if (names.count("Group") == 0) {
        name = "Group";
        return Result::success();
    }

    for (std::uint64_t index = 1;; ++index) {
        const std::string candidate = "Group (" + std::to_string(index) + ")";
        if (names.count(candidate) == 0) {
            name = candidate;
            return Result::success();
        }
        if (index == std::numeric_limits<std::uint64_t>::max()) {
            return Result::failure(
                "Could not generate a unique Group display name");
        }
    }
}

Result deriveStrictLocalTransform(const glm::mat4& parentWorld,
                                  const glm::mat4& desiredWorld,
                                  transform_math::DecomposedTransform& local) {
    local = {};
    if (!transform_math::isFiniteMatrix(parentWorld) ||
        !transform_math::isFiniteMatrix(desiredWorld)) {
        return Result::failure(
            "Group transform inputs must be finite matrices");
    }

    const glm::mat3 linearParent(parentWorld);
    float maximumLinearMagnitude = 0.0f;
    for (int column = 0; column < 3; ++column) {
        for (int row = 0; row < 3; ++row) {
            maximumLinearMagnitude = std::max(
                maximumLinearMagnitude,
                std::fabs(linearParent[column][row]));
        }
    }
    const float determinant = glm::determinant(linearParent);
    const float determinantTolerance =
        128.0f * std::numeric_limits<float>::epsilon() *
        maximumLinearMagnitude * maximumLinearMagnitude *
        maximumLinearMagnitude;
    if (!std::isfinite(determinant) ||
        !std::isfinite(maximumLinearMagnitude) ||
        maximumLinearMagnitude <= 0.0f ||
        std::fabs(determinant) <= determinantTolerance) {
        return Result::failure(
            "Group transform requires an invertible parent transform");
    }

    const glm::mat4 inverseParent = glm::inverse(parentWorld);
    if (!transform_math::isFiniteMatrix(inverseParent)) {
        return Result::failure(
            "Group transform parent inverse is not finite");
    }
    const glm::mat4 candidateLocal = inverseParent * desiredWorld;
    if (!transform_math::isFiniteMatrix(candidateLocal)) {
        return Result::failure(
            "Group transform produced a non-finite local transform");
    }
    const Result decomposition =
        transform_math::decomposeModelMatrix(candidateLocal, local);
    if (!decomposition) {
        return Result::failure(
            "Group transform cannot be represented as local TRS: " +
            decomposition.error());
    }
    return Result::success();
}

template <typename ReparentCallback>
Result restoreMovedObjects(
    const std::vector<StructuralObjectSnapshot>& snapshots,
    const std::vector<GameObject*>& movedObjects,
    ReparentCallback&& reparent) {
    // snapshots are in hierarchy traversal order, which is ascending within
    // each original sibling list. Restoring in that order reproduces the
    // captured indices even when several siblings were moved.
    for (const StructuralObjectSnapshot& snapshot : snapshots) {
        if (std::find(movedObjects.begin(), movedObjects.end(),
                      snapshot.object) == movedObjects.end()) {
            continue;
        }
        const Result result = reparent(
            *snapshot.object, snapshot.parent, Scene::ReparentMode::PreserveLocal,
            snapshot.siblingIndex);
        if (!result) {
            return Result::failure(
                "Failed to restore original hierarchy position for '" +
                snapshot.object->name + "': " + result.error());
        }
        snapshot.object->position = snapshot.localPosition;
        snapshot.object->rotation = snapshot.localRotation;
        snapshot.object->scale = snapshot.localScale;
    }
    return Result::success();
}

template <typename ReparentCallback, typename RemoveCallback>
Result rollbackGroup(Scene& scene, Group* group, std::size_t groupRootIndex,
                     const std::vector<StructuralObjectSnapshot>& snapshots,
                     const std::vector<GameObject*>& movedObjects,
                     ReparentCallback&& reparent,
                     RemoveCallback&& remove) {
    if (group == nullptr || !ownsObject(scene, group)) {
        return Result::failure("Group transaction rollback lost its Group object");
    }

    Result result = restoreMovedObjects(
        snapshots, movedObjects, std::forward<ReparentCallback>(reparent));
    if (!result) return result;

    if (group->parent() != nullptr) {
        result = reparent(
            *group, nullptr, Scene::ReparentMode::PreserveLocal,
            groupRootIndex);
        if (!result) {
            return Result::failure(
                "Failed to restore the temporary Group root: " +
                result.error());
        }
    }

    std::unique_ptr<GameObject> removed;
    result = remove(group, removed);
    if (!result) {
        return Result::failure(
            "Failed to remove the temporary Group: " + result.error());
    }
    return scene.validateAuthoredState();
}

bool parsePositiveDuplicateSuffix(const std::string& name,
                                  std::size_t& suffixStart) {
    suffixStart = std::string::npos;
    if (name.size() < 4 || name.back() != ')') {
        return false;
    }

    const std::size_t opening = name.rfind(" (");
    if (opening == std::string::npos || opening + 3 > name.size()) {
        return false;
    }

    const char* begin = name.data() + opening + 2;
    const char* end = name.data() + name.size() - 1;
    std::uint64_t value = 0;
    const auto parsed = std::from_chars(begin, end, value);
    if (parsed.ec != std::errc{} || parsed.ptr != end || value == 0) {
        return false;
    }

    suffixStart = opening;
    return true;
}

std::string makeNumberedName(const std::string& base,
                             std::uint64_t index) {
    return base + " (" + std::to_string(index) + ")";
}

Result copyAttachedCameraIfPresent(const GameObject& source,
                                   GameObject& duplicate,
                                   const TypeRegistry& registry) {
    const Camera* sourceCamera = source.attachedCamera();
    Camera* duplicateCamera = duplicate.attachedCamera();
    if ((sourceCamera != nullptr) != (duplicateCamera != nullptr)) {
        return Result::failure(
            "Attached-camera topology does not match the source object");
    }
    if (!sourceCamera) {
        return Result::success();
    }

    return SceneSerializer::copyAuthoredAttachedCameraState(
        *sourceCamera, *duplicateCamera, registry);
}

struct DuplicateBatchEntry {
    GameObject* source = nullptr;
    std::unique_ptr<GameObject> detached;
    GameObject* inserted = nullptr;
    GameObject* targetParent = nullptr;
};

struct DeleteHierarchyListPlan {
    GameObject* parent = nullptr;
    std::vector<GameObject*> objects;
};

struct DeletePromotionPlan {
    GameObject* object = nullptr;
    GameObject* originalParent = nullptr;
    GameObject* finalParent = nullptr;
    glm::mat4 originalWorld{1.0f};
    transform_math::DecomposedTransform finalLocal;
};

struct DeleteBatchPlan {
    std::vector<GameObject*> selected;
    std::unordered_set<GameObject*> selectedSet;
    std::vector<GameObject*> deletionOrder;
    std::vector<DeleteHierarchyListPlan> finalLists;
    std::vector<DeletePromotionPlan> promotions;
    std::vector<StructuralObjectSnapshot> promotionSnapshots;
};

void collectHierarchyOrder(
    GameObject* object, std::vector<GameObject*>& ordered,
    std::unordered_set<GameObject*>& visited) {
    if (object == nullptr || !visited.insert(object).second) return;
    ordered.push_back(object);
    for (GameObject* child : object->children()) {
        collectHierarchyOrder(child, ordered, visited);
    }
}

std::vector<GameObject*> orderedSelectedObjects(
    const Scene& scene, const EditorSession& editorSession) {
    std::vector<GameObject*> ordered;
    ordered.reserve(editorSession.selectedGameObjects().size());
    std::unordered_set<GameObject*> selected;
    selected.reserve(editorSession.selectedGameObjects().size());
    for (GameObject* object : editorSession.selectedGameObjects()) {
        if (object != nullptr) selected.insert(object);
    }

    std::unordered_set<GameObject*> visited;
    visited.reserve(scene.gameObjects().size());
    for (GameObject* root : scene.rootObjects()) {
        std::vector<GameObject*> hierarchy;
        collectHierarchyOrder(root, hierarchy, visited);
        for (GameObject* object : hierarchy) {
            if (selected.count(object) != 0) ordered.push_back(object);
        }
    }

    // A valid scene and selection make this fallback unnecessary. It keeps
    // the operation deterministic and diagnosable if a caller supplies a
    // malformed selection before context validation can reject it.
    for (GameObject* object : editorSession.selectedGameObjects()) {
        if (object != nullptr &&
            std::find(ordered.begin(), ordered.end(), object) == ordered.end()) {
            ordered.push_back(object);
        }
    }
    return ordered;
}

Result validateExactSelection(
    const Scene& scene, const EditorSession& editorSession,
    std::vector<GameObject*>& ordered) {
    ordered.clear();
    const std::vector<GameObject*>& selected =
        editorSession.selectedGameObjects();
    std::unordered_set<GameObject*> seen;
    try {
        seen.reserve(selected.size());
        for (GameObject* object : selected) {
            if (!ownsObject(scene, object)) {
                return Result::failure(
                    "Selection contains a GameObject outside the Scene");
            }
            if (!seen.insert(object).second) {
                return Result::failure(
                    "Selection contains a duplicate GameObject");
            }
        }
        ordered = orderedSelectedObjects(scene, editorSession);
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to order the exact selection: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to order the exact selection with an unknown error");
    }
    if (ordered.size() != selected.size()) {
        return Result::failure(
            "Selection could not be mapped to the Scene hierarchy");
    }
    const GameObject* active = editorSession.activeGameObjectForScene(&scene);
    if (active == nullptr || seen.count(const_cast<GameObject*>(active)) == 0) {
        return Result::failure(
            "Selection requires a selected Active GameObject");
    }
    return Result::success();
}

void appendAfterDeleting(
    GameObject* object, const std::unordered_set<GameObject*>& selected,
    std::vector<GameObject*>& output) {
    if (object == nullptr) return;
    if (selected.count(object) == 0) {
        output.push_back(object);
        return;
    }
    for (GameObject* child : object->children()) {
        appendAfterDeleting(child, selected, output);
    }
}

void appendFinalChildren(
    GameObject* parent, const std::unordered_set<GameObject*>& selected,
    DeleteHierarchyListPlan& list) {
    list.parent = parent;
    list.objects.clear();
    const std::vector<GameObject*>& children = parent->children();
    list.objects.reserve(children.size());
    for (GameObject* child : children) {
        appendAfterDeleting(child, selected, list.objects);
    }
}

void appendFinalRoots(
    const Scene& scene, const std::unordered_set<GameObject*>& selected,
    DeleteHierarchyListPlan& list) {
    list.parent = nullptr;
    list.objects.clear();
    list.objects.reserve(scene.rootObjects().size());
    for (GameObject* root : scene.rootObjects()) {
        appendAfterDeleting(root, selected, list.objects);
    }
}

std::size_t objectDepth(const GameObject* object) {
    std::size_t depth = 0;
    std::unordered_set<const GameObject*> visited;
    for (const GameObject* parent = object == nullptr ? nullptr : object->parent();
         parent != nullptr && visited.insert(parent).second;
         parent = parent->parent()) {
        ++depth;
    }
    return depth;
}

Result capturePromotionSnapshot(
    const Scene& scene, GameObject* object,
    StructuralObjectSnapshot& snapshot) {
    return captureStructuralSnapshot(scene, object, snapshot);
}

Result makeDeletePlan(
    Scene& scene, const EditorSession& editorSession,
    DeleteBatchPlan& plan) {
    plan = {};
    Result result = validateExactSelection(scene, editorSession, plan.selected);
    if (!result) return result;

    try {
        plan.selectedSet.reserve(plan.selected.size());
        for (GameObject* object : plan.selected) {
            plan.selectedSet.insert(object);
        }

        DeleteHierarchyListPlan roots;
        appendFinalRoots(scene, plan.selectedSet, roots);
        plan.finalLists.push_back(std::move(roots));

        std::vector<GameObject*> allObjects;
        allObjects.reserve(scene.gameObjects().size());
        std::unordered_set<GameObject*> visited;
        visited.reserve(scene.gameObjects().size());
        for (GameObject* root : scene.rootObjects()) {
            collectHierarchyOrder(root, allObjects, visited);
        }
        for (GameObject* object : allObjects) {
            if (plan.selectedSet.count(object) == 0) {
                DeleteHierarchyListPlan list;
                appendFinalChildren(object, plan.selectedSet, list);
                plan.finalLists.push_back(std::move(list));
            }
        }

        std::unordered_set<GameObject*> promotionSet;
        promotionSet.reserve(scene.gameObjects().size());
        for (const DeleteHierarchyListPlan& list : plan.finalLists) {
            for (GameObject* object : list.objects) {
                if (object == nullptr || object->parent() == list.parent) {
                    continue;
                }
                if (!promotionSet.insert(object).second) {
                    return Result::failure(
                        "Delete plan promoted a survivor more than once");
                }
                DeletePromotionPlan promotion;
                promotion.object = object;
                promotion.originalParent = object->parent();
                promotion.finalParent = list.parent;
                promotion.originalWorld = object->worldTransformMatrix();
                if (!transform_math::isFiniteMatrix(promotion.originalWorld)) {
                    return Result::failure(
                        "Delete plan contains a survivor with an invalid world transform");
                }
                glm::mat4 parentWorld(1.0f);
                if (promotion.finalParent != nullptr) {
                    parentWorld = promotion.finalParent->worldTransformMatrix();
                }
                result = deriveStrictLocalTransform(
                    parentWorld, promotion.originalWorld, promotion.finalLocal);
                if (!result) {
                    return Result::failure(
                        "Delete promotion for '" + object->name +
                        "' failed strict TRS preflight: " + result.error());
                }
                plan.promotions.push_back(std::move(promotion));
            }
        }

        plan.promotionSnapshots.reserve(plan.promotions.size());
        for (GameObject* object : allObjects) {
            if (promotionSet.count(object) == 0) continue;
            StructuralObjectSnapshot snapshot;
            result = capturePromotionSnapshot(scene, object, snapshot);
            if (!result) return result;
            plan.promotionSnapshots.push_back(snapshot);
        }

        plan.deletionOrder = plan.selected;
        std::stable_sort(
            plan.deletionOrder.begin(), plan.deletionOrder.end(),
            [](const GameObject* first, const GameObject* second) {
                return objectDepth(first) > objectDepth(second);
            });
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to build Delete plan: " + std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to build Delete plan with an unknown error");
    }
    return Result::success();
}

}  // namespace

namespace editor {

std::string makeDuplicateName(const std::string& sourceName,
                              const std::vector<std::string>& existingNames) {
    if (sourceName.empty()) {
        return {};
    }

    std::string baseName = sourceName;
    std::size_t suffixStart = std::string::npos;
    if (parsePositiveDuplicateSuffix(sourceName, suffixStart)) {
        baseName = sourceName.substr(0, suffixStart);
    }

    std::unordered_set<std::string> names;
    names.reserve(existingNames.size());
    for (const std::string& name : existingNames) {
        names.insert(name);
    }

    for (std::uint64_t index = 1;; ++index) {
        const std::string candidate = makeNumberedName(baseName, index);
        if (names.count(candidate) == 0) {
            return candidate;
        }
        if (index == std::numeric_limits<std::uint64_t>::max()) {
            // A Scene cannot realistically contain this many names, but keep
            // the loop defined if an adversarial test supplies that input.
            return sourceName;
        }
    }
}

Result duplicateAuthoredGameObject(
    const GameObject& source, const TypeRegistry& registry,
    std::unique_ptr<GameObject>& duplicate) {
    duplicate.reset();

    const TypeDescriptor* sourceType = registry.find(source);
    if (!sourceType) {
        return Result::failure(
            "Cannot duplicate an object with an unregistered C++ type");
    }
    if (!sourceType->factory) {
        return Result::failure("Registered type '" + sourceType->name +
                               "' has no factory");
    }

    try {
        duplicate = sourceType->factory();
    } catch (const std::exception& exception) {
        return Result::failure("Factory for type '" + sourceType->name +
                               "' threw: " + exception.what());
    } catch (...) {
        return Result::failure("Factory for type '" + sourceType->name +
                               "' threw an unknown exception");
    }
    if (!duplicate) {
        return Result::failure("Factory for type '" + sourceType->name +
                               "' returned null");
    }

    const TypeDescriptor* duplicateType = registry.find(*duplicate);
    if (!duplicateType || duplicateType->name != sourceType->name ||
        duplicateType->type != sourceType->type) {
        duplicate.reset();
        return Result::failure(
            "Factory for type '" + sourceType->name +
            "' returned a different registered concrete type");
    }

    try {
        const Result topologyResult =
            copyAttachedCameraIfPresent(source, *duplicate, registry);
        if (!topologyResult) {
            duplicate.reset();
            return topologyResult;
        }

        Result result = registry.copyAuthoredProperties(source, *duplicate);
        if (!result) {
            duplicate.reset();
            return result;
        }
    } catch (const std::exception& exception) {
        duplicate.reset();
        return Result::failure(
            "Authored GameObject copy threw: " + std::string(exception.what()));
    } catch (...) {
        duplicate.reset();
        return Result::failure(
            "Authored GameObject copy threw an unknown exception");
    }

    // Persistent identity is intentionally outside TypeRegistry authored
    // properties. Clear any factory default so Scene's centralized insertion
    // policy assigns the identity at commit time.
    duplicate->persistentId.clear();
    return Result::success();
}

Result EditorObjectCoordinator::duplicateIntoScene(
    Scene& scene, const GameObject& source, const TypeRegistry& registry,
    const RendererAttachment& attachRenderer, GameObject*& duplicate) {
    duplicate = nullptr;
    if (!attachRenderer) {
        return Result::failure(
            "Cannot duplicate an object without a renderer attachment path");
    }

    bool sourceBelongsToScene = false;
    for (const auto& owner : scene.gameObjects()) {
        if (owner.get() == &source) {
            sourceBelongsToScene = true;
            break;
        }
    }
    if (!sourceBelongsToScene) {
        return Result::failure(
            "Cannot duplicate an object that is not owned by the Scene");
    }

    std::unique_ptr<GameObject> detachedDuplicate;
    Result result = duplicateAuthoredGameObject(
        source, registry, detachedDuplicate);
    if (!result) {
        return result;
    }

    std::vector<std::string> existingNames;
    try {
        existingNames.reserve(scene.gameObjects().size());
        for (const auto& owner : scene.gameObjects()) {
            if (owner) {
                existingNames.push_back(owner->name);
            }
        }
        detachedDuplicate->name =
            makeDuplicateName(source.name, existingNames);
    } catch (const std::exception& exception) {
        return Result::failure("Failed to generate duplicate name: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to generate duplicate name with an unknown exception");
    }

    result = scene.validateEditorGameObjectInsertion(*detachedDuplicate);
    if (!result) {
        return result;
    }

    GameObject* inserted = nullptr;
    result = scene.addGameObjectForEditor(std::move(detachedDuplicate),
                                          inserted);
    if (!result) {
        return result;
    }

    try {
        result = attachRenderer(scene, *inserted);
    } catch (const std::exception& exception) {
        result = Result::failure("Renderer attachment threw: " +
                                 std::string(exception.what()));
    } catch (...) {
        result = Result::failure(
            "Renderer attachment threw an unknown exception");
    }
    if (!result) {
        std::unique_ptr<GameObject> removed;
        const Result rollback =
            scene.removeGameObjectForEditor(inserted, removed);
        if (!rollback) {
            return Result::failure(
                result.error() + "; Scene rollback failed: " +
                rollback.error());
        }
        return result;
    }

    duplicate = inserted;
    return Result::success();
}

Result EditorObjectCoordinator::duplicateSelectionIntoScene(
    Scene& scene, EditorSession& editorSession, const TypeRegistry& registry,
    const RendererAttachment& attachRenderer,
    const RendererBatchDetachment& detachRenderer) {
    if (editorSession.selectedGameObjects().empty()) {
        return Result::success();
    }
    if (!attachRenderer) {
        return Result::failure(
            "Cannot duplicate a selection without a renderer attachment path");
    }

    Result result = validateStructuralContext(scene, editorSession);
    if (!result) return result;

    std::vector<GameObject*> sources;
    result = validateExactSelection(scene, editorSession, sources);
    if (!result) return result;
    const GameObject* previousActive =
        editorSession.activeGameObjectForScene(&scene);

    std::size_t selectedPointLights = 0;
    std::size_t selectedDirectionalLights = 0;
    for (GameObject* source : sources) {
        if (dynamic_cast<PointLight*>(source) != nullptr) {
            ++selectedPointLights;
        }
        if (dynamic_cast<DirectionalLight*>(source) != nullptr) {
            ++selectedDirectionalLights;
        }
    }
    if (selectedPointLights >
        scene_limits::maxPointLights - scene.pointLightCount()) {
        return Result::failure(
            "Duplicating the selection would exceed the point-light limit");
    }
    if (selectedDirectionalLights > 0 && scene.directionalLight() != nullptr) {
        return Result::failure(
            "Duplicating the selection would create a second directional light");
    }

    std::vector<std::string> occupiedNames;
    std::vector<DuplicateBatchEntry> entries;
    std::unordered_map<GameObject*, GameObject*> sourceToDuplicate;
    try {
        occupiedNames.reserve(scene.gameObjects().size() + sources.size());
        for (const auto& owner : scene.gameObjects()) {
            if (owner != nullptr) occupiedNames.push_back(owner->name);
        }
        entries.reserve(sources.size());
        sourceToDuplicate.reserve(sources.size());
        for (GameObject* source : sources) {
            DuplicateBatchEntry entry;
            entry.source = source;
            result = duplicateAuthoredGameObject(
                *source, registry, entry.detached);
            if (!result) return result;

            entry.detached->name = makeDuplicateName(
                source->name, occupiedNames);
            if (!entry.detached->name.empty() &&
                std::find(occupiedNames.begin(), occupiedNames.end(),
                          entry.detached->name) != occupiedNames.end()) {
                return Result::failure(
                    "Could not generate a unique duplicate name for '" +
                    source->name + "'");
            }
            occupiedNames.push_back(entry.detached->name);

            result = scene.validateEditorGameObjectInsertion(*entry.detached);
            if (!result) return result;

            GameObject* detachedPointer = entry.detached.get();
            entries.push_back(std::move(entry));
            sourceToDuplicate.emplace(source, detachedPointer);
        }

        for (DuplicateBatchEntry& entry : entries) {
            const GameObject* sourceParent = entry.source->parent();
            const auto duplicateParent = sourceParent == nullptr
                                             ? sourceToDuplicate.end()
                                             : sourceToDuplicate.find(
                                                   const_cast<GameObject*>(
                                                       sourceParent));
            entry.targetParent = duplicateParent == sourceToDuplicate.end()
                                     ? entry.source->parent()
                                     : duplicateParent->second;
        }
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to prepare duplicate batch: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to prepare duplicate batch with an unknown error");
    }

    std::vector<GameObject*> inserted;
    std::vector<GameObject*> attached;
    std::vector<GameObject*> duplicates;
    try {
        inserted.reserve(entries.size());
        attached.reserve(entries.size());
        duplicates.reserve(entries.size());
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to reserve duplicate transaction storage: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to reserve duplicate transaction storage with an unknown error");
    }

    auto rollback = [&](const Result& failure) -> Result {
        Result rollbackResult = Result::success();
        if (!attached.empty()) {
            if (!detachRenderer) {
                rollbackResult = Result::failure(
                    "No renderer detachment path was supplied for rollback");
            } else {
                try {
                    rollbackResult = detachRenderer(scene, attached);
                } catch (const std::exception& exception) {
                    rollbackResult = Result::failure(
                        "Renderer rollback detachment threw: " +
                        std::string(exception.what()));
                } catch (...) {
                    rollbackResult = Result::failure(
                        "Renderer rollback detachment threw an unknown exception");
                }
            }
        }

        if (rollbackResult) {
            std::vector<GameObject*> removalOrder = inserted;
            std::stable_sort(
                removalOrder.begin(), removalOrder.end(),
                [](const GameObject* first, const GameObject* second) {
                    return objectDepth(first) > objectDepth(second);
                });
            for (GameObject* object : removalOrder) {
                std::unique_ptr<GameObject> removed;
                const Result removeResult =
                    scene.removeGameObjectForEditor(object, removed);
                if (!removeResult) {
                    rollbackResult = Result::failure(
                        "Failed to remove duplicate during rollback: " +
                        removeResult.error());
                    break;
                }
            }
        }
        if (rollbackResult) {
            rollbackResult = scene.validateAuthoredState();
        }
        if (!rollbackResult) {
            return Result::failure(
                failure.error() + "; duplicate transaction rollback failed: " +
                rollbackResult.error());
        }
        return failure;
    };

    for (DuplicateBatchEntry& entry : entries) {
        result = scene.addGameObjectForEditor(std::move(entry.detached),
                                              entry.inserted);
        if (!result) return rollback(result);
        if (entry.inserted == nullptr) {
            return rollback(Result::failure(
                "Scene insertion succeeded without returning a duplicate"));
        }
        inserted.push_back(entry.inserted);
    }

    for (DuplicateBatchEntry& entry : entries) {
        GameObject* duplicate = entry.inserted;
        const GameObject* sourceParent = entry.source->parent();
        const bool parentDuplicated =
            sourceParent != nullptr &&
            sourceToDuplicate.find(const_cast<GameObject*>(sourceParent)) !=
                sourceToDuplicate.end();
        if (parentDuplicated) {
            result = scene.reparentGameObjectAt(
                *duplicate, entry.targetParent, Scene::ReparentMode::PreserveLocal,
                entry.targetParent->children().size());
        } else if (entry.targetParent == nullptr) {
            const auto sourceRoot = std::find(
                scene.rootObjects().begin(), scene.rootObjects().end(),
                entry.source);
            if (sourceRoot == scene.rootObjects().end()) {
                result = Result::failure(
                    "Could not locate a source root for duplicate placement");
            } else {
                const std::size_t sourceIndex = static_cast<std::size_t>(
                    std::distance(scene.rootObjects().begin(), sourceRoot));
                result = scene.reorderGameObject(
                    *duplicate, nullptr, sourceIndex + 1);
            }
        } else {
            const auto sourceSibling = std::find(
                entry.targetParent->children().begin(),
                entry.targetParent->children().end(), entry.source);
            if (sourceSibling == entry.targetParent->children().end()) {
                result = Result::failure(
                    "Could not locate a source sibling for duplicate placement");
            } else {
                const std::size_t sourceIndex = static_cast<std::size_t>(
                    std::distance(entry.targetParent->children().begin(),
                                  sourceSibling));
                result = scene.reparentGameObjectAt(
                    *duplicate, entry.targetParent,
                    Scene::ReparentMode::PreserveLocal, sourceIndex + 1);
            }
        }
        if (!result) return rollback(result);
    }

    result = scene.validateAuthoredState();
    if (!result) return rollback(result);

    for (DuplicateBatchEntry& entry : entries) {
        try {
            result = attachRenderer(scene, *entry.inserted);
        } catch (const std::exception& exception) {
            result = Result::failure(
                "Renderer attachment threw: " +
                std::string(exception.what()));
        } catch (...) {
            result = Result::failure(
                "Renderer attachment threw an unknown exception");
        }
        if (!result) return rollback(result);
        attached.push_back(entry.inserted);
    }

    for (const DuplicateBatchEntry& entry : entries) {
        duplicates.push_back(entry.inserted);
    }
    GameObject* duplicateActive = nullptr;
    const auto activeMapping = previousActive == nullptr
                                   ? sourceToDuplicate.end()
                                   : sourceToDuplicate.find(
                                         const_cast<GameObject*>(previousActive));
    if (activeMapping != sourceToDuplicate.end()) {
        duplicateActive = activeMapping->second;
    }
    result = editorSession.setExactSelection(
        &scene, duplicates, duplicateActive);
    if (!result) return rollback(result);
    return Result::success();
}

Result EditorObjectCoordinator::duplicateSelectionIntoScene(
    Scene& scene, EditorSession& editorSession, const TypeRegistry& registry,
    const RendererAttachment& attachRenderer) {
    return duplicateSelectionIntoScene(
        scene, editorSession, registry, attachRenderer,
        [](Scene&, const std::vector<GameObject*>&) {
            return Result::success();
        });
}

Result EditorObjectCoordinator::deleteSelection(
    Scene& scene, EditorSession& editorSession,
    const RendererBatchDetachment& detachRenderer,
    const RendererAttachment& attachRenderer) {
    if (editorSession.selectedGameObjects().empty()) {
        return Result::success();
    }

    Result result = validateStructuralContext(scene, editorSession);
    if (!result) return result;

    DeleteBatchPlan plan;
    result = makeDeletePlan(scene, editorSession, plan);
    if (!result) return result;

    std::vector<std::pair<GameObject*, std::size_t>> requirements;
    std::vector<std::string> deletedColliderIds;
    try {
        requirements.reserve(plan.promotions.size());
        for (const DeletePromotionPlan& promotion : plan.promotions) {
            const auto requirement = std::find_if(
                requirements.begin(), requirements.end(),
                [&promotion](const auto& candidate) {
                    return candidate.first == promotion.finalParent;
                });
            if (requirement == requirements.end()) {
                requirements.emplace_back(promotion.finalParent, 1);
            } else {
                ++requirement->second;
            }
        }
        deletedColliderIds.reserve(plan.selected.size());
        for (GameObject* object : plan.selected) {
            if (!object->persistentId.empty() &&
                editorSession.renderColliderEnabled(*object)) {
                deletedColliderIds.push_back(object->persistentId);
            }
        }
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to prepare Delete transaction storage: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to prepare Delete transaction storage with an unknown error");
    }

    result = scene.reserveEditorHierarchyStorage(requirements);
    if (!result) return result;

    std::vector<GameObject*> movedPromotions;
    movedPromotions.reserve(plan.promotions.size());
    bool rendererDetached = false;

    auto rollback = [&](const Result& failure) -> Result {
        Result rollbackResult = Result::success();
        if (!movedPromotions.empty()) {
            const auto reparent = [&scene](GameObject& object,
                                           GameObject* parent,
                                           Scene::ReparentMode mode,
                                           std::size_t index) {
                return scene.reparentGameObjectAt(object, parent, mode, index);
            };
            rollbackResult = restoreMovedObjects(
                plan.promotionSnapshots, movedPromotions, reparent);
        }

        if (rollbackResult && rendererDetached && !plan.selected.empty()) {
            if (!attachRenderer) {
                rollbackResult = Result::failure(
                    "No renderer attachment path was supplied for Delete rollback");
            } else {
                for (GameObject* object : plan.selected) {
                    try {
                        const Result attachResult =
                            attachRenderer(scene, *object);
                        if (!attachResult && rollbackResult) {
                            rollbackResult = Result::failure(
                                "Failed to reattach '" + object->name +
                                "': " + attachResult.error());
                        }
                    } catch (const std::exception& exception) {
                        if (rollbackResult) {
                            rollbackResult = Result::failure(
                                "Renderer rollback attachment for '" +
                                object->name + "' threw: " +
                                std::string(exception.what()));
                        }
                    } catch (...) {
                        if (rollbackResult) {
                            rollbackResult = Result::failure(
                                "Renderer rollback attachment for '" +
                                object->name +
                                "' threw an unknown exception");
                        }
                    }
                }
            }
        }
        if (rollbackResult) {
            rollbackResult = scene.validateAuthoredState();
        }
        if (!rollbackResult) {
            return Result::failure(
                failure.error() + "; Delete transaction rollback failed: " +
                rollbackResult.error());
        }
        return failure;
    };

    if (detachRenderer) {
        try {
            result = detachRenderer(scene, plan.selected);
        } catch (const std::exception& exception) {
            result = Result::failure(
                "Renderer detachment threw: " +
                std::string(exception.what()));
        } catch (...) {
            result = Result::failure(
                "Renderer detachment threw an unknown exception");
        }
        if (!result) return result;
    }
    rendererDetached = static_cast<bool>(detachRenderer);

    for (const DeleteHierarchyListPlan& list : plan.finalLists) {
        for (std::size_t index = 0; index < list.objects.size(); ++index) {
            GameObject* object = list.objects[index];
            if (object->parent() == list.parent) continue;

            std::size_t destinationIndex =
                list.parent == nullptr ? scene.rootObjects().size()
                                       : list.parent->children().size();
            for (std::size_t later = index + 1; later < list.objects.size();
                 ++later) {
                GameObject* next = list.objects[later];
                if (next->parent() != list.parent) continue;
                const auto& siblings = list.parent == nullptr
                                           ? scene.rootObjects()
                                           : list.parent->children();
                const auto nextIt =
                    std::find(siblings.begin(), siblings.end(), next);
                if (nextIt != siblings.end()) {
                    destinationIndex = static_cast<std::size_t>(
                        std::distance(siblings.begin(), nextIt));
                    break;
                }
            }

            result = scene.reparentGameObjectAt(
                *object, list.parent, Scene::ReparentMode::PreserveWorld,
                destinationIndex);
            if (!result) return rollback(result);
            for (const DeletePromotionPlan& promotion : plan.promotions) {
                if (promotion.object == object) {
                    object->position = promotion.finalLocal.position;
                    object->rotation = promotion.finalLocal.rotation;
                    object->scale = promotion.finalLocal.scale;
                    break;
                }
            }
            movedPromotions.push_back(object);
        }
    }

    result = scene.validateAuthoredState();
    if (!result) return rollback(result);

    // Every remaining child of a selected object must itself be selected;
    // the deepest-first order below then makes each low-level removal
    // structurally safe without ever destroying an unselected survivor.
    for (GameObject* object : plan.selected) {
        for (GameObject* child : object->children()) {
            if (plan.selectedSet.count(child) == 0) {
                return rollback(Result::failure(
                    "Delete plan left an unselected child beneath '" +
                    object->name + "'"));
            }
        }
    }

    // Validate every ownership erase before the first one. The low-level
    // removal is intentionally noexcept, so this keeps a later precondition
    // failure from producing a partially deleted selection.
    for (GameObject* object : plan.deletionOrder) {
        result = scene.validateEditorGameObjectRemoval(object, false);
        if (!result) return rollback(result);
    }

    for (GameObject* object : plan.deletionOrder) {
        std::unique_ptr<GameObject> removed;
        result = scene.removeGameObjectForEditor(object, removed);
        if (!result) return rollback(result);
    }

    result = scene.validateAuthoredState();
    if (!result) {
        return Result::failure(
            "Delete committed but final Scene validation failed: " +
            result.error());
    }

    editorSession.removeRenderColliderIds(deletedColliderIds);
    editorSession.clearSelection();
    return Result::success();
}

Result EditorObjectCoordinator::deleteSelection(
    Scene& scene, EditorSession& editorSession,
    const RendererBatchDetachment& detachRenderer) {
    return deleteSelection(scene, editorSession, detachRenderer, {});
}

Result EditorObjectCoordinator::deleteSelection(
    Scene& scene, EditorSession& editorSession) {
    return deleteSelection(scene, editorSession, {}, {});
}

Result EditorObjectCoordinator::parentSelectionToActive(
    Scene& scene, const EditorSession& editorSession) {
    if (editorSession.selectedGameObjects().empty()) {
        return Result::success();
    }

    Result result = validateStructuralContext(scene, editorSession);
    if (!result) return result;

    GameObject* active = editorSession.activeGameObjectForScene(&scene)
                             ? const_cast<GameObject*>(
                                   editorSession.activeGameObjectForScene(&scene))
                             : nullptr;
    if (active == nullptr) {
        return Result::failure(
            "Parent selection requires a selected Active GameObject");
    }

    const std::vector<GameObject*> roots =
        orderedTopLevelRoots(scene, editorSession);
    std::vector<StructuralObjectSnapshot> snapshots;
    snapshots.reserve(roots.size());
    for (GameObject* root : roots) {
        if (root == active) continue;
        StructuralObjectSnapshot snapshot;
        result = captureStructuralSnapshot(scene, root, snapshot);
        if (!result) return result;
        snapshots.push_back(snapshot);
    }
    if (snapshots.empty()) {
        return Result::success();
    }

    // Every request is checked against the original hierarchy before the
    // first reparent is committed. This includes cycle and PreserveWorld
    // decomposition failures.
    for (const StructuralObjectSnapshot& snapshot : snapshots) {
        result = scene.validateReparentGameObject(
            *snapshot.object, active, Scene::ReparentMode::PreserveWorld,
            std::nullopt);
        if (!result) return result;
    }

    std::vector<GameObject*> moved;
    moved.reserve(snapshots.size());
    const auto reparent = [&scene](GameObject& child, GameObject* parent,
                                   Scene::ReparentMode mode,
                                   std::size_t index) {
        return scene.reparentGameObjectAt(child, parent, mode, index);
    };
    for (const StructuralObjectSnapshot& snapshot : snapshots) {
        result = scene.reparentGameObjectAt(
            *snapshot.object, active, Scene::ReparentMode::PreserveWorld,
            active->children().size());
        if (!result) {
            const Result rollback =
                restoreMovedObjects(snapshots, moved, reparent);
            if (!rollback) {
                return Result::failure(
                    result.error() + "; hierarchy rollback failed: " +
                    rollback.error());
            }
            return result;
        }
        moved.push_back(snapshot.object);
    }

    result = scene.validateAuthoredState();
    if (!result) {
        const Result rollback =
            restoreMovedObjects(snapshots, moved, reparent);
        if (!rollback) {
            return Result::failure(
                result.error() + "; hierarchy rollback failed: " +
                rollback.error());
        }
        return result;
    }
    return Result::success();
}

Result EditorObjectCoordinator::groupSelection(
    Scene& scene, EditorSession& editorSession, const TypeRegistry& registry,
    const RendererAttachment& attachRenderer, GameObject*& group) {
    group = nullptr;
    if (editorSession.selectedGameObjects().empty()) {
        return Result::success();
    }

    Result result = validateStructuralContext(scene, editorSession);
    if (!result) return result;

    const GameObject* activeConst =
        editorSession.activeGameObjectForScene(&scene);
    GameObject* active = const_cast<GameObject*>(activeConst);
    if (active == nullptr) {
        return Result::failure(
            "Grouping requires a selected Active GameObject");
    }

    const std::vector<GameObject*> roots =
        orderedTopLevelRoots(scene, editorSession);
    if (roots.empty()) {
        return Result::failure("Grouping requires at least one top-level root");
    }

    GameObject* activeRoot = nullptr;
    result = editor_transform::resolveActiveTransformRoot(
        active, editorSession.selectedGameObjects(), activeRoot);
    if (!result) return result;
    if (std::find(roots.begin(), roots.end(), activeRoot) == roots.end()) {
        return Result::failure(
            "Active GameObject does not belong to a selected hierarchy root");
    }

    std::vector<StructuralObjectSnapshot> snapshots;
    snapshots.reserve(roots.size());
    for (GameObject* root : roots) {
        StructuralObjectSnapshot snapshot;
        result = captureStructuralSnapshot(scene, root, snapshot);
        if (!result) return result;
        snapshots.push_back(snapshot);
    }

    glm::vec3 pivot;
    result = editor_transform::calculateSharedPivot(roots, pivot);
    if (!result) return result;
    glm::mat4 desiredWorld(1.0f);
    desiredWorld[3] = glm::vec4(pivot, 1.0f);

    const GameObject* groupParentConst = activeRoot->parent();
    GameObject* groupParent = const_cast<GameObject*>(groupParentConst);
    std::size_t activeRootIndex = 0;
    for (const StructuralObjectSnapshot& snapshot : snapshots) {
        if (snapshot.object == activeRoot) {
            groupParent = snapshot.parent;
            activeRootIndex = snapshot.siblingIndex;
            break;
        }
    }
    const std::size_t groupRootIndex = scene.rootObjects().size();

    glm::mat4 groupParentWorld(1.0f);
    if (groupParent != nullptr) {
        groupParentWorld = groupParent->worldTransformMatrix();
    }
    transform_math::DecomposedTransform groupLocal;
    result = deriveStrictLocalTransform(groupParentWorld, desiredWorld,
                                        groupLocal);
    if (!result) return result;

    std::string groupName;
    result = makeUniqueGroupName(scene, groupName);
    if (!result) return result;

    std::unique_ptr<Group> newGroup;
    try {
        newGroup = std::make_unique<Group>();
    } catch (const std::exception& exception) {
        return Result::failure("Failed to create Group: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure("Failed to create Group");
    }
    if (!newGroup) {
        return Result::failure("Group factory returned null");
    }
    const TypeDescriptor* groupType = registry.find(*newGroup);
    if (groupType == nullptr || groupType->name != "Group") {
        return Result::failure(
            "The Group type is not registered with the editor TypeRegistry");
    }
    newGroup->name = std::move(groupName);
    newGroup->position = groupLocal.position;
    newGroup->rotation = groupLocal.rotation;
    newGroup->scale = groupLocal.scale;

    if (!scene.isActive()) {
        return Result::failure(
            "Structural hierarchy editing requires an active Scene");
    }
    result = scene.validateEditorGameObjectInsertion(*newGroup);
    if (!result) return result;

    GameObject* inserted = nullptr;
    result = scene.addGameObjectForEditor(std::move(newGroup), inserted);
    if (!result) return result;
    Group* insertedGroup = dynamic_cast<Group*>(inserted);
    if (insertedGroup == nullptr) {
        std::unique_ptr<GameObject> removed;
        (void)scene.removeGameObjectForEditor(inserted, removed);
        return Result::failure(
            "Group insertion produced a different GameObject type");
    }

    std::vector<GameObject*> moved;
    moved.reserve(snapshots.size());
    const auto reparent = [&scene](GameObject& child, GameObject* parent,
                                   Scene::ReparentMode mode,
                                   std::size_t index) {
        return scene.reparentGameObjectAt(child, parent, mode, index);
    };
    const auto remove = [&scene](GameObject* object,
                                 std::unique_ptr<GameObject>& removed) {
        return scene.removeGameObjectForEditor(object, removed);
    };

    auto rollback = [&](const Result& failure) {
        const Result rollbackResult = rollbackGroup(
            scene, insertedGroup, groupRootIndex, snapshots, moved, reparent,
            remove);
        if (!rollbackResult) {
            return Result::failure(
                failure.error() + "; Group transaction rollback failed: " +
                rollbackResult.error());
        }
        return failure;
    };

    if (groupParent != nullptr) {
        result = scene.reparentGameObjectAt(
            *insertedGroup, groupParent, Scene::ReparentMode::PreserveLocal,
            activeRootIndex);
    } else {
        result = scene.reorderGameObject(
            *insertedGroup, nullptr, activeRootIndex);
    }
    if (!result) return rollback(result);

    for (const StructuralObjectSnapshot& snapshot : snapshots) {
        result = scene.validateReparentGameObject(
            *snapshot.object, insertedGroup, Scene::ReparentMode::PreserveWorld,
            std::nullopt);
        if (!result) return rollback(result);
    }

    for (std::size_t index = 0; index < snapshots.size(); ++index) {
        const StructuralObjectSnapshot& snapshot = snapshots[index];
        result = scene.reparentGameObjectAt(
            *snapshot.object, insertedGroup, Scene::ReparentMode::PreserveWorld,
            index);
        if (!result) return rollback(result);
        moved.push_back(snapshot.object);
    }

    result = scene.validateAuthoredState();
    if (!result) return rollback(result);

    if (attachRenderer) {
        try {
            result = attachRenderer(scene, *insertedGroup);
        } catch (const std::exception& exception) {
            result = Result::failure("Group renderer attachment threw: " +
                                     std::string(exception.what()));
        } catch (...) {
            result = Result::failure(
                "Group renderer attachment threw an unknown exception");
        }
        if (!result) return rollback(result);
    }

    try {
        editorSession.applySelection(
            &scene, insertedGroup, SelectionOperation::ToggleExact);
    } catch (const std::exception& exception) {
        return rollback(Result::failure(
            "Failed to update selection for Group: " +
            std::string(exception.what())));
    } catch (...) {
        return rollback(
            Result::failure("Failed to update selection for Group"));
    }

    group = insertedGroup;
    return Result::success();
}

Result EditorObjectCoordinator::groupSelection(
    Scene& scene, EditorSession& editorSession, const TypeRegistry& registry,
    GameObject*& group) {
    return groupSelection(scene, editorSession, registry, {}, group);
}

}  // namespace editor
