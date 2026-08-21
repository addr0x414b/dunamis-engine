#ifndef GAME_OBJECT_H
#define GAME_OBJECT_H

#include <glm/glm.hpp>
#include <memory>
#include <string>
#include "../core/result.h"

class Scene;
class Camera;
class ModelRenderable;

class GameObject {
public:
    GameObject();
    GameObject(const GameObject& other) = delete;
    GameObject& operator=(const GameObject& other) = delete;

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
    // Called by PhysicsServer after it writes a physics-resolved transform.
    // Most objects have nothing else to synchronize.
    virtual void onPhysicsTransformResolved() noexcept {}
    virtual Camera* attachedCamera() noexcept { return nullptr; }
    virtual const Camera* attachedCamera() const noexcept {
        return nullptr;
    }
    virtual ~GameObject();

    [[nodiscard]] ModelRenderable& modelRenderable() noexcept;
    [[nodiscard]] const ModelRenderable& modelRenderable() const noexcept;

    // Transitional compatibility inputs/mirrors for ModelRenderable's
    // authored path state. The GameObject wrappers rebind them after every
    // delegated operation, including failures.
    const char* modelPath = nullptr;
    // texturePath is only used if the textures are not baked into the model file
    const char* texturePath = nullptr;

    // Call after setting modelPath (and texturePath if needed)
    // Loads the model and sets up the renderable's meshes and materials.
    [[nodiscard]] Result loadModel();
    [[nodiscard]] std::string authoredModelPath() const;
    [[nodiscard]] Result setAuthoredModelPath(std::string path);
    [[nodiscard]] std::string authoredTexturePath() const;
    [[nodiscard]] Result setAuthoredTexturePath(std::string path);

private:
    friend class Scene;

    std::unique_ptr<ModelRenderable> modelRenderable_;
};

#endif
