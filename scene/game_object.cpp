#include "game_object.h"

#include "../math/transform_math.h"
#include "model_renderable.h"

#include <utility>

GameObject::GameObject()
    : modelRenderable_(std::make_unique<ModelRenderable>()) {}

GameObject::~GameObject() = default;

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

GameObject* GameObject::parent() noexcept {
    return parent_;
}

const GameObject* GameObject::parent() const noexcept {
    return parent_;
}

const std::vector<GameObject*>& GameObject::children() const noexcept {
    return children_;
}
