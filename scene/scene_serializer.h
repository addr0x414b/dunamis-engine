#ifndef SCENE_SERIALIZER_H
#define SCENE_SERIALIZER_H

#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "../core/result.h"
#include "camera.h"
#include "scene.h"
#include "type_registry.h"

struct EditorCameraState {
    glm::vec3 position{0.0f};
    glm::vec3 front{0.0f, 0.0f, -1.0f};
    glm::vec3 up{0.0f, 1.0f, 0.0f};
};

struct SceneLoadData {
    std::optional<EditorCameraState> editorCamera;
    std::vector<std::string> renderColliders;
    nlohmann::json authoredBaseline;
    std::vector<std::string> warnings;
};

class SceneSerializer {
public:
    static constexpr int formatVersion = 1;

    [[nodiscard]] static Result serializeAuthored(
        const Scene& scene, const TypeRegistry& registry,
        nlohmann::json& output);
    [[nodiscard]] static Result serializeFull(
        const Scene& scene, const TypeRegistry& registry,
        const Camera& editorCamera, nlohmann::json& output);
    [[nodiscard]] static Result serializeFull(
        const Scene& scene, const TypeRegistry& registry,
        const Camera& editorCamera, const std::vector<std::string>& renderColliders,
        nlohmann::json& output);
    [[nodiscard]] static Result applyDocument(
        const nlohmann::json& document, Scene& candidate,
        const TypeRegistry& registry, SceneLoadData& loadData);
    [[nodiscard]] static Result copyAuthoredState(
        const Scene& source, Scene& destination,
        const TypeRegistry& registry);

private:
    [[nodiscard]] static Result serializeObject(
        const GameObject& object, const TypeRegistry& registry,
        nlohmann::json& output);
};

#endif
