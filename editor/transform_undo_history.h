#ifndef EDITOR_TRANSFORM_UNDO_HISTORY_H
#define EDITOR_TRANSFORM_UNDO_HISTORY_H

#include "../core/result.h"
#include "../scene/game_object.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class Scene;

inline constexpr std::size_t maxEditorUndoActions = 100;
inline constexpr std::size_t maxTransformUndoActions = maxEditorUndoActions;

namespace editor_undo {

struct TransformUndoCameraState {
    bool present = false;
    bool standalone = false;
    std::uint64_t instanceId = 0;
    glm::vec3 position{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

struct TransformUndoObjectState {
    std::string persistentId;
    std::uint64_t instanceId = 0;
    std::string parentPersistentId;
    std::uint64_t parentInstanceId = 0;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
    TransformUndoCameraState camera;
};

struct TransformUndoSnapshot {
    std::uint64_t sceneLifetimeId = 0;
    std::vector<TransformUndoObjectState> objects;
    std::vector<glm::mat4> worldTransforms;
    bool valid = false;
};

// A delete action owns the original detached objects until restoration. The
// plannedObject field is used only while a delete transaction is staged; it is
// cleared before that action becomes visible to undo().
struct DeleteUndoObject {
    std::unique_ptr<GameObject> retainedObject;
    GameObject* plannedObject = nullptr;
    std::string persistentId;
    std::uint64_t instanceId = 0;
    std::string parentPersistentId;
    std::uint64_t parentInstanceId = 0;
    std::size_t owningIndex = 0;
    std::size_t siblingIndex = 0;
    std::size_t hierarchyDepth = 0;
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    glm::vec3 scale{1.0f};
};

struct DeleteUndoPromotion {
    std::string persistentId;
    std::uint64_t instanceId = 0;
    std::string originalParentPersistentId;
    std::uint64_t originalParentInstanceId = 0;
    std::size_t originalSiblingIndex = 0;
    std::size_t hierarchyDepth = 0;
    glm::vec3 originalPosition{0.0f};
    glm::vec3 originalRotation{0.0f};
    glm::vec3 originalScale{1.0f};
    std::string finalParentPersistentId;
    std::uint64_t finalParentInstanceId = 0;
    std::size_t finalSiblingIndex = 0;
    glm::vec3 finalPosition{0.0f};
    glm::vec3 finalRotation{0.0f};
    glm::vec3 finalScale{1.0f};
};

struct DeleteUndoAction {
    std::uint64_t sceneLifetimeId = 0;
    std::vector<DeleteUndoObject> objects;
    std::vector<DeleteUndoPromotion> promotions;
    std::vector<std::string> selectionOrder;
    std::string activeSelectionId;
    std::vector<std::string> renderColliderIds;
    std::string activeCameraOwnerId;
    std::uint64_t activeCameraOwnerInstanceId = 0;
    std::uint64_t activeCameraInstanceId = 0;
    bool activeCameraStandalone = false;
    std::size_t estimatedBytes = 0;
    bool staged = false;
};

// One chronological, RAM-only history for authored transforms and deletes.
class EditorUndoHistory final {
public:
    EditorUndoHistory() = default;
    ~EditorUndoHistory();
    EditorUndoHistory(const EditorUndoHistory&) = delete;
    EditorUndoHistory& operator=(const EditorUndoHistory&) = delete;

    void clear() noexcept;
    [[nodiscard]] Result capture(
        const Scene& scene, const std::vector<GameObject*>& objects,
        TransformUndoSnapshot& snapshot) const;
    [[nodiscard]] Result commit(
        const Scene& scene, const TransformUndoSnapshot& snapshot);
    using DeleteRestoreHandler =
        std::function<Result(Scene&, DeleteUndoAction&)>;
    [[nodiscard]] Result undo(
        Scene& scene, const DeleteRestoreHandler& restoreDelete = {});

    // stageDelete performs every history allocation and memory-budget check
    // before Scene or renderer state changes. The pending payload is committed
    // only after the deletion transaction has transferred object ownership.
    [[nodiscard]] Result stageDelete(Scene& scene, DeleteUndoAction&& action);
    [[nodiscard]] DeleteUndoAction* stagedDeleteAction() noexcept;
    [[nodiscard]] Result commitStagedDelete() noexcept;
    void cancelStagedDelete() noexcept;

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] std::size_t actionCount() const noexcept;

private:
    struct ObjectChange {
        TransformUndoObjectState before;
        TransformUndoObjectState after;
        glm::mat4 expectedWorldBefore{1.0f};
        glm::mat4 expectedWorldAfter{1.0f};
    };

    struct TransformAction {
        std::uint64_t sceneLifetimeId = 0;
        std::vector<ObjectChange> objects;
        std::size_t estimatedBytes = 0;
    };

    using Action = std::variant<TransformAction, DeleteUndoAction>;
    [[nodiscard]] static std::size_t actionBytes(const Action& action) noexcept;
    void enforceLimits() noexcept;

    std::deque<Action> actions_;
    std::size_t estimatedBytes_ = 0;
    bool stagedDeletePending_ = false;
};

// Source compatibility for existing transform editor code and tests. The
// underlying history is shared with delete actions.
using TransformUndoHistory = EditorUndoHistory;

}  // namespace editor_undo

#endif
