#ifndef MODEL_RENDERABLE_H
#define MODEL_RENDERABLE_H

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "../core/result.h"
#include "../rendering/utils/vulkan_utils.h"

namespace model_loading {
struct CachedCpuModel;
}

class VulkanContext;

class ModelRenderable {
public:
    ModelRenderable() = default;
    ModelRenderable(const ModelRenderable& other);
    ModelRenderable& operator=(const ModelRenderable& other);

    // These remain simple authoring inputs for the transitional API. The
    // loaded mesh data and all renderer-facing state live in this object.
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

private:
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

    void rebindPathPointersFrom(const ModelRenderable& source) noexcept;
};

#endif
