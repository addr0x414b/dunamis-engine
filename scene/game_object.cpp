#include "game_object.h"

#include "../math/transform_math.h"
#include "model_renderable.h"

#include <atomic>
#include <limits>
#include <utility>

namespace {

std::uint64_t nextGameObjectInstanceId() noexcept {
    static std::atomic<std::uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

}  // namespace

namespace {

std::size_t addBytes(std::size_t total, std::size_t value) noexcept {
    if (value > std::numeric_limits<std::size_t>::max() - total) {
        return std::numeric_limits<std::size_t>::max();
    }
    return total + value;
}

std::size_t multiplyBytes(std::size_t count,
                          std::size_t elementSize) noexcept {
    if (count != 0 &&
        elementSize > std::numeric_limits<std::size_t>::max() / count) {
        return std::numeric_limits<std::size_t>::max();
    }
    return count * elementSize;
}

}  // namespace

GameObject::GameObject()
    : instanceId_(nextGameObjectInstanceId()),
      modelRenderable_(std::make_unique<ModelRenderable>()) {}

GameObject::~GameObject() = default;

std::size_t GameObject::estimatedRetainedCpuMemoryBytes() const noexcept {
    std::size_t bytes = addBytes(sizeof(GameObject), 4096);
    bytes = addBytes(bytes, name.capacity());
    bytes = addBytes(bytes, persistentId.capacity());
    bytes = addBytes(bytes,
                     multiplyBytes(children_.capacity(), sizeof(GameObject*)));
    bytes = addBytes(bytes,
                     modelRenderable().estimatedPrivateCpuMemoryBytes());
    return bytes;
}

ModelRenderable& GameObject::modelRenderable() noexcept {
    return *modelRenderable_;
}

const ModelRenderable& GameObject::modelRenderable() const noexcept {
    return *modelRenderable_;
}

std::string GameObject::authoredModelPath() const {
    return modelPath ? std::string(modelPath) : std::string{};
}

Result GameObject::setAuthoredModelPath(std::string path) {
    ModelRenderable& renderable = modelRenderable();
    renderable.modelPath = modelPath;
    renderable.texturePath = texturePath;
    const Result result = renderable.setAuthoredModelPath(std::move(path));
    modelPath = renderable.modelPath;
    texturePath = renderable.texturePath;
    return result;
}

std::string GameObject::authoredTexturePath() const {
    return texturePath ? std::string(texturePath) : std::string{};
}

Result GameObject::setAuthoredTexturePath(std::string path) {
    ModelRenderable& renderable = modelRenderable();
    renderable.modelPath = modelPath;
    renderable.texturePath = texturePath;
    const Result result = renderable.setAuthoredTexturePath(std::move(path));
    modelPath = renderable.modelPath;
    texturePath = renderable.texturePath;
    return result;
}

ImportedAssetReference GameObject::importedAssetReference() const {
    return modelRenderable().importedAssetReference();
}

Result GameObject::setImportedAssetReference(
    ImportedAssetReference reference) {
    ModelRenderable& renderable = modelRenderable();
    renderable.modelPath = modelPath;
    renderable.texturePath = texturePath;
    const Result result =
        renderable.setImportedAssetReference(std::move(reference));
    modelPath = renderable.modelPath;
    texturePath = renderable.texturePath;
    return result;
}

Result GameObject::loadModel() {
    ModelRenderable& renderable = modelRenderable();
    renderable.modelPath = modelPath;
    renderable.texturePath = texturePath;
    const Result result = renderable.loadModel();
    modelPath = renderable.modelPath;
    texturePath = renderable.texturePath;
    return result;
}

glm::mat4 GameObject::localTransformMatrix() const noexcept {
    return transform_math::makeModelMatrix(position, rotation, scale);
}

glm::mat4 GameObject::worldTransformMatrix() const noexcept {
    glm::mat4 world = localTransformMatrix();
    for (const GameObject* ancestor = parent_; ancestor != nullptr;
         ancestor = ancestor->parent_) {
        world = ancestor->localTransformMatrix() * world;
    }
    return world;
}

std::uint64_t GameObject::instanceId() const noexcept {
    return instanceId_;
}

GameObject* GameObject::parent() noexcept {
    return parent_;
}

const GameObject* GameObject::parent() const noexcept {
    return parent_;
}

const std::vector<GameObject*>& GameObject::children() const noexcept {
    return children_;
}
