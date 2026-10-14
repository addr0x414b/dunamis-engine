#ifndef MODEL_RENDERABLE_H
#define MODEL_RENDERABLE_H

#include <deque>
#include <cstddef>
#include <memory>
#include <optional>
#include <cstddef>
#include <string>
#include <vector>

#include "../core/result.h"
#include "../assets/model_asset.h"
#include "../scene/loading_cache_key.h"

namespace model_loading {
struct CachedCpuModel;
struct ImportedModelAsset;
}

class VulkanContext;

struct SharedCpuMemoryEstimate {
    const void* identity = nullptr;
    std::size_t bytes = 0;
    std::size_t referenceCount = 0;
};

class ModelRenderable {
public:
    ModelRenderable() = default;
    ModelRenderable(const ModelRenderable& other) = delete;
    ModelRenderable& operator=(const ModelRenderable& other) = delete;

    // These remain simple authoring inputs. Loaded CPU mesh/material data
    // lives here; renderer state is owned by the active renderer backend.
    const char* modelPath = nullptr;
    const char* texturePath = nullptr;

    [[nodiscard]] Result addMeshInstance(MeshInstance&& meshInstance);
    [[nodiscard]] const std::vector<MeshInstance>&
    meshInstances() const noexcept;

    // Call after setting modelPath (and texturePath if needed).
    [[nodiscard]] Result loadModel();
    [[nodiscard]] std::string authoredModelPath() const;
    [[nodiscard]] Result setAuthoredModelPath(std::string path);
    [[nodiscard]] std::string authoredTexturePath() const;
    [[nodiscard]] Result setAuthoredTexturePath(std::string path);
    [[nodiscard]] ImportedAssetReference importedAssetReference() const;
    [[nodiscard]] Result setImportedAssetReference(
        ImportedAssetReference reference);
    [[nodiscard]] std::optional<model_loading::ModelAssetCacheKey>
    gpuModelCacheKey() const;
    // Conservative CPU-side retention estimates used by RAM-only editor undo.
    // Shared immutable assets are reported separately so a delete action only
    // charges them when every reference is part of that same action.
    [[nodiscard]] std::size_t estimatedPrivateCpuMemoryBytes() const noexcept;
    void appendSharedCpuMemoryEstimates(
        std::vector<SharedCpuMemoryEstimate>& estimates) const;

private:
    friend class Scene;
    friend class VulkanContext;
    friend class GameObjectTestAccess;
    friend class GameObjectModelCacheTestAccess;

    enum class RenderTopologyState {
        Mutable,
        ResourcesAttached,
    };

    [[nodiscard]] Result markRenderResourcesAttached();
    void markRenderResourcesDetached() noexcept;
    [[nodiscard]] bool renderTopologyMutable() const noexcept;

    RenderTopologyState renderTopologyState_ = RenderTopologyState::Mutable;
    std::vector<MeshInstance> meshInstances_;
    std::deque<std::string> texturePathStorage_;
    std::string modelPathStorage_;
    std::string authoredTexturePathStorage_;
    std::shared_ptr<const model_loading::CachedCpuModel> loadedModelAsset_;
    ImportedAssetReference importedAssetReference_;
    std::shared_ptr<const model_loading::ImportedModelAsset>
        importedModelAsset_;
    std::size_t importedNodeIndex_ = 0;
};

#endif
