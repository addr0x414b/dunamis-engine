#ifndef EDITOR_TRANSFORM_UNDO_HISTORY_H
#define EDITOR_TRANSFORM_UNDO_HISTORY_H

#include "../core/result.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>

class GameObject;
class Scene;

inline constexpr std::size_t maxTransformUndoActions = 100;

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

// This history contains authored transform data only. It deliberately keeps
// no pointers to Scene, GameObject, or Camera instances.
class TransformUndoHistory final {
public:
    void clear() noexcept;
    [[nodiscard]] Result capture(
        const Scene& scene, const std::vector<GameObject*>& objects,
        TransformUndoSnapshot& snapshot) const;
    [[nodiscard]] Result commit(
        const Scene& scene, const TransformUndoSnapshot& snapshot);
    [[nodiscard]] Result undo(Scene& scene);

    [[nodiscard]] bool canUndo() const noexcept;
    [[nodiscard]] std::size_t actionCount() const noexcept;

private:
    struct ObjectChange {
        TransformUndoObjectState before;
        TransformUndoObjectState after;
        glm::mat4 expectedWorldBefore{1.0f};
        glm::mat4 expectedWorldAfter{1.0f};
    };

    struct Action {
        std::uint64_t sceneLifetimeId = 0;
        std::vector<ObjectChange> objects;
        std::size_t estimatedBytes = 0;
    };

    std::deque<Action> actions_;
    std::size_t estimatedBytes_ = 0;
};

}  // namespace editor_undo

#endif
