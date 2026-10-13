#ifndef IMPORTED_MODEL_ASSET_H
#define IMPORTED_MODEL_ASSET_H

#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <glm/glm.hpp>

#include "model_asset.h"
#include "../core/result.h"

namespace model_loading {

struct ImportedMeshAsset {
    std::shared_ptr<const MeshGeometry> geometry;
    Material material;
    std::string texturePath;
    std::string assetIdentity;
    std::size_t sourceMeshIndex = 0;
    std::size_t sourceMaterialIndex = 0;
};

struct ImportedModelNode {
    static constexpr std::size_t noParent = static_cast<std::size_t>(-1);

    std::string identity;
    std::string name;
    bool nameWasEmpty = false;
    std::size_t parentIndex = noParent;
    // These are a rigid local transform. Scale, reflection, and shear are
    // carried by the node's immutable derived mesh geometry.
    glm::vec3 position{0.0f};
    glm::vec3 rotation{0.0f};
    std::vector<std::size_t> children;
    std::vector<std::shared_ptr<const ImportedMeshAsset>> meshes;
};

struct ImportedModelAsset {
    std::string sourcePath;
    std::string sourceIdentity;
    std::vector<ImportedModelNode> nodes;
    std::unordered_map<std::string, std::size_t> nodeByIdentity;
    std::size_t uniqueSourceMeshes = 0;
    std::size_t meshReferences = 0;
    std::size_t uniqueGeometryVariants = 0;
};

// Imports the scene graph once. The returned immutable asset owns all data
// needed after Assimp's temporary aiScene is destroyed.
[[nodiscard]] Result loadImportedModelAsset(
    const std::filesystem::path& sourcePath,
    const std::filesystem::path& textureFallbackPath,
    std::shared_ptr<const ImportedModelAsset>& output);

}  // namespace model_loading

#endif
