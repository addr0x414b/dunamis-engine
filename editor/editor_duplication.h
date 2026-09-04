#ifndef EDITOR_DUPLICATION_H
#define EDITOR_DUPLICATION_H

#include "../core/result.h"

#include <functional>
#include <memory>
#include <string>
#include <vector>

class GameObject;
class Scene;
class EditorSession;
class TypeRegistry;

namespace editor {

// Returns the smallest available numbered suffix for an authored display
// name. An empty source name remains empty.
[[nodiscard]] std::string makeDuplicateName(
    const std::string& sourceName,
    const std::vector<std::string>& existingNames);

// Factory-create and copy one object's authored state without inserting it
// into a Scene. The returned object has no persistent identity yet.
[[nodiscard]] Result duplicateAuthoredGameObject(
    const GameObject& source, const TypeRegistry& registry,
    std::unique_ptr<GameObject>& duplicate);

class EditorObjectCoordinator final {
public:
    using RendererAttachment =
        std::function<Result(Scene&, GameObject&)>;

    // Duplicates source, inserts it through the controlled active-scene path,
    // and asks the renderer to attach the new object. The output is assigned
    // only after both subsystems have committed successfully.
    [[nodiscard]] static Result duplicateIntoScene(
        Scene& scene, const GameObject& source, const TypeRegistry& registry,
        const RendererAttachment& attachRenderer,
        GameObject*& duplicate);

    // Reparents every applicable top-level selected root beneath the Active
    // GameObject while preserving world transforms and selection state.
    [[nodiscard]] static Result parentSelectionToActive(
        Scene& scene, const EditorSession& editorSession);

    // Creates one authored Group, places it in the Active selected root's
    // former hierarchy slot, and groups the selected roots transactionally.
    [[nodiscard]] static Result groupSelection(
        Scene& scene, EditorSession& editorSession,
        const TypeRegistry& registry, const RendererAttachment& attachRenderer,
        GameObject*& group);
    [[nodiscard]] static Result groupSelection(
        Scene& scene, EditorSession& editorSession,
        const TypeRegistry& registry, GameObject*& group);
};

}  // namespace editor

#endif
