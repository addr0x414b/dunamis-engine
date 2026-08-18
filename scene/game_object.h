#ifndef GAME_OBJECT_H
#define GAME_OBJECT_H

#include <deque>
#include <glm/glm.hpp>
#include <memory>
#include <string>
#include <vector>
#include "spdlog/spdlog.h"
#include "../core/result.h"
#include "../rendering/utils/vulkan_utils.h"

class Scene;
class VulkanContext;
class Camera;
namespace model_loading {
struct CachedCpuModel;
}

class GameObject {
public:
    std::string name;
    glm::vec3 position = glm::vec3(0.0f);
    glm::vec3 rotation = glm::vec3(0.0f);
    glm::vec3 scale = glm::vec3(1.0f);
    // Stable authored identity. It is intentionally not exposed by the
    // inspector; display-name changes must not change persistence identity.
    std::string persistentId;

    // Authoring metadata only. PhysicsServer creates its Jolt bodies solely
    // for the disposable runtime scene; no backend state lives here.
    enum class PhysicsMotionType { Static, Dynamic };
    enum class PhysicsColliderType { Mesh, Sphere, ConvexHull };
    struct PhysicsBodySettings {
        bool enabled = false;
        PhysicsMotionType motionType = PhysicsMotionType::Static;
        PhysicsColliderType colliderType = PhysicsColliderType::Mesh;
        float sphereRadius = 0.5f;
    } physics;

    virtual void start() {}
    virtual void update() {}
    virtual Camera* attachedCamera() noexcept { return nullptr; }
    virtual const Camera* attachedCamera() const noexcept {
        return nullptr;
    }
    virtual ~GameObject();

    const char* modelPath = nullptr;
    // texturePath is only used if the textures are not baked into the model file
    const char* texturePath = nullptr;

    [[nodiscard]] Result addMeshInstance(MeshInstance&& meshInstance);
    [[nodiscard]] const std::vector<MeshInstance>&
    meshInstances() const noexcept;

    // Call after setting modelPath (and texturePath if needed)
    // Loads the model and sets up the mesh and materials
    [[nodiscard]] Result loadModel();
    [[nodiscard]] std::string authoredModelPath() const;
    [[nodiscard]] Result setAuthoredModelPath(std::string path);
    [[nodiscard]] std::string authoredTexturePath() const;
    [[nodiscard]] Result setAuthoredTexturePath(std::string path);

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
};

#endif
