#ifndef EDITOR_EDITOR_TRANSFORM_H
#define EDITOR_EDITOR_TRANSFORM_H

#include "../core/result.h"
#include "../math/transform_math.h"
#include "editor_mutation.h"
#include "editor_state.h"

#include <glm/mat4x4.hpp>
#include <string>
#include <vector>

class GameObject;
class Scene;
class EditorSession;

namespace editor_transform {

struct TransformObjectSnapshot {
    GameObject* object = nullptr;
    GameObject* parent = nullptr;
    bool selected = false;
    glm::mat4 originalWorld{1.0f};
    std::vector<GameObject*> originalChildren;
    transform_math::DecomposedTransform originalLocal;
    editor_mutation::CameraTransformState originalCamera;
};

struct TransformExternalParentSnapshot {
    GameObject* object = nullptr;
    GameObject* parent = nullptr;
    glm::mat4 originalWorld{1.0f};
};

struct TransformCandidate {
    GameObject* object = nullptr;
    glm::mat4 desiredWorld{1.0f};
    transform_math::DecomposedTransform local;
    editor_mutation::CameraTransformState camera;
};

// All state needed to solve one ImGuizmo drag. It is intentionally transient:
// the ImGui layer owns one instance only while a drag is active.
struct TransformDragSnapshot {
    Scene* scene = nullptr;
    EditorSession* editorSession = nullptr;
    GameObject* active = nullptr;
    GameObject* activeTransformRoot = nullptr;
    TransformSpace space = TransformSpace::World;
    TransformTool tool = TransformTool::Translate;
    std::vector<GameObject*> selectedObjects;
    std::vector<GameObject*> topLevelSelectedRoots;
    std::vector<GameObject*> sceneObjects;
    std::vector<std::string> sceneObjectPersistentIds;
    std::vector<GameObject*> sceneObjectParents;
    std::vector<TransformObjectSnapshot> objects;
    std::vector<TransformExternalParentSnapshot> externalParents;
    glm::vec3 pivot{0.0f};
    glm::mat4 gizmoOrientation{1.0f};
    glm::mat4 originalGizmoWorld{1.0f};
    bool valid = false;
};

// Pure hierarchy/selection geometry helpers used by the snapshot builder and
// by editor tests without requiring ImGui.
[[nodiscard]] Result calculateSharedPivot(
    const std::vector<GameObject*>& topLevelSelectedRoots,
    glm::vec3& pivot);
[[nodiscard]] Result resolveActiveTransformRoot(
    GameObject* active, const std::vector<GameObject*>& selectedObjects,
    GameObject*& root);
[[nodiscard]] Result calculateWorldOrientation(
    const glm::mat4& worldTransform, glm::mat4& orientation);
[[nodiscard]] Result makeGizmoFrame(const glm::vec3& pivot,
                                    const glm::mat4& orientation,
                                    glm::mat4& frame);
[[nodiscard]] Result calculateSelectionGizmoFrame(
    const EditorSession& editorSession, TransformSpace space,
    glm::mat4& frame);

// Captures the authoritative EditorSession selection and every object in the
// selected-root subtrees. No GameObject is mutated.
[[nodiscard]] Result captureTransformDragSnapshot(
    Scene& scene, EditorSession& editorSession, TransformSpace space,
    TransformDragSnapshot& snapshot);

// Returns whether an object belongs to the immutable affected-object set of
// an active Scale transaction. The caller owns the transaction lifetime.
[[nodiscard]] bool isScalePreviewParticipant(
    const TransformDragSnapshot& snapshot, const Scene* scene,
    const GameObject* object) noexcept;

// Computes all candidate local TRS and auxiliary Camera state from the
// immutable snapshot and the current manipulated gizmo frame. No mutation is
// performed until commitTransformCandidates succeeds.
[[nodiscard]] Result solveTransformDrag(
    const TransformDragSnapshot& snapshot,
    const glm::mat4& currentGizmoWorld,
    std::vector<TransformCandidate>& candidates);

// Commits a completely validated candidate set in one batch.
[[nodiscard]] Result commitTransformCandidates(
    const TransformDragSnapshot& snapshot,
    const std::vector<TransformCandidate>& candidates);

// Restores the original authored locals and auxiliary Camera state. This is
// noexcept so an interrupted UI drag can roll back from input/shutdown paths.
[[nodiscard]] bool restoreTransformDragSnapshot(
    const TransformDragSnapshot& snapshot) noexcept;

// Converts a world-space gizmo result into the selected object's authored
// local TRS. The output is only populated on success; the GameObject is never
// mutated by this helper.
[[nodiscard]] Result deriveLocalTransformFromWorld(
    const GameObject& object, const glm::mat4& candidateWorld,
    transform_math::DecomposedTransform& local);

// Variant used by the batch solver when the candidate parent world transform
// is a snapshot result rather than the hierarchy's currently committed one.
[[nodiscard]] Result deriveLocalTransformFromWorld(
    const glm::mat4& parentWorld, const glm::vec3& authoredScaleHint,
    const glm::mat4& candidateWorld,
    transform_math::DecomposedTransform& local);

}  // namespace editor_transform

#endif
