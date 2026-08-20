#include "game_object.h"

#include "model_renderable.h"

#include <utility>

GameObject::GameObject()
    : modelRenderable_(std::make_unique<ModelRenderable>()) {}

GameObject::GameObject(const GameObject& other)
    : name(other.name),
      position(other.position),
      rotation(other.rotation),
      scale(other.scale),
      persistentId(other.persistentId),
      physics(other.physics),
      modelPath(other.modelPath),
      texturePath(other.texturePath),
      modelRenderable_(std::make_unique<ModelRenderable>(
          *other.modelRenderable_)) {
    if (other.modelPath == other.modelRenderable_->modelPath) {
        modelPath = modelRenderable_->modelPath;
    }
    if (other.texturePath == other.modelRenderable_->texturePath) {
        texturePath = modelRenderable_->texturePath;
    }
}

GameObject& GameObject::operator=(const GameObject& other) {
    if (this == &other) {
        return *this;
    }

    const bool modelPathOwned =
        other.modelPath == other.modelRenderable_->modelPath;
    const bool texturePathOwned =
        other.texturePath == other.modelRenderable_->texturePath;
    name = other.name;
    position = other.position;
    rotation = other.rotation;
    scale = other.scale;
    persistentId = other.persistentId;
    physics = other.physics;
    *modelRenderable_ = *other.modelRenderable_;
    modelPath = modelPathOwned ? modelRenderable_->modelPath : other.modelPath;
    texturePath = texturePathOwned ? modelRenderable_->texturePath
                                   : other.texturePath;
    return *this;
}

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
    if (path == authoredModelPath()) {
        return Result::success();
    }

    ModelRenderable& renderable = modelRenderable();
    renderable.modelPath = modelPath;
    renderable.texturePath = texturePath;
    const Result result = renderable.setAuthoredModelPath(std::move(path));
    if (result) {
        modelPath = renderable.modelPath;
        texturePath = renderable.texturePath;
    }
    return result;
}

std::string GameObject::authoredTexturePath() const {
    return texturePath ? std::string(texturePath) : std::string{};
}

Result GameObject::setAuthoredTexturePath(std::string path) {
    if (path == authoredTexturePath()) {
        return Result::success();
    }

    ModelRenderable& renderable = modelRenderable();
    renderable.modelPath = modelPath;
    renderable.texturePath = texturePath;
    const Result result = renderable.setAuthoredTexturePath(std::move(path));
    if (result) {
        modelPath = renderable.modelPath;
        texturePath = renderable.texturePath;
    }
    return result;
}

Result GameObject::loadModel() {
    ModelRenderable& renderable = modelRenderable();
    renderable.modelPath = modelPath;
    renderable.texturePath = texturePath;
    const Result result = renderable.loadModel();
    if (result) {
        modelPath = renderable.modelPath;
        texturePath = renderable.texturePath;
    }
    return result;
}
