#ifndef MODEL_IMPORTER_H
#define MODEL_IMPORTER_H

#include <cstddef>
#include <filesystem>
#include <string>

#include <glm/glm.hpp>

#include "../core/result.h"

class GameObject;
class Scene;

namespace model_import {

struct Options {
    std::string rootName;
    std::filesystem::path textureFallbackPath;
    glm::vec3 position{0.0f};
    // Authored in degrees, matching ordinary GameObject rotations.
    glm::vec3 rotation{0.0f};
    bool generateStaticColliders = false;
};

struct ImportResult {
    GameObject* root = nullptr;  // Owned by the target Scene.
    std::size_t objectCount = 0;
    std::size_t sourceNodeCount = 0;
    std::size_t uniqueSourceMeshCount = 0;
    std::size_t meshReferenceCount = 0;
    std::size_t uniqueGeometryVariantCount = 0;
};

class HierarchyImporter final {
public:
    // Import while scene construction is inactive (for example from
    // Scene::buildDefaults). The operation inserts the complete hierarchy or
    // leaves the Scene unchanged.
    [[nodiscard]] static Result import(
        Scene& scene, const std::filesystem::path& sourcePath,
        const Options& options, ImportResult& output);
};

[[nodiscard]] inline Result importHierarchy(
    Scene& scene, const std::filesystem::path& sourcePath,
    const Options& options, ImportResult& output) {
    return HierarchyImporter::import(scene, sourcePath, options, output);
}

}  // namespace model_import

#endif
