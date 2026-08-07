#include "scene.h"

#include <cmath>
#include <exception>
#include <stdexcept>
#include <typeinfo>
#include <utility>

namespace {

constexpr float minDirectionalDirectionLengthSquared = 1.0e-8f;

bool isValidColorComponent(float component) {
    return std::isfinite(component) && component >= 0.0f &&
           component <= 1.0f;
}

bool isFiniteVector(const glm::vec3& vector) {
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
           std::isfinite(vector.z);
}

Result validateDirectionalLightState(const DirectionalLight& light) {
    if (!isFiniteVector(light.direction)) {
        return Result::failure("Directional light direction must be finite");
    }

    const float directionLengthSquared = glm::dot(
        light.direction, light.direction);
    if (!std::isfinite(directionLengthSquared)) {
        return Result::failure(
            "Directional light direction must have finite length");
    }
    if (directionLengthSquared <= minDirectionalDirectionLengthSquared) {
        return Result::failure(
            "Directional light direction must have nonzero length");
    }

    const DirectionalShadowSettings& shadow = light.shadow;
    if (!isFiniteVector(shadow.focus) || !std::isfinite(shadow.halfExtent) ||
        !std::isfinite(shadow.lightDistance) || !std::isfinite(shadow.nearPlane) ||
        !std::isfinite(shadow.farPlane) || shadow.halfExtent <= 0.0f ||
        shadow.lightDistance <= 0.0f || shadow.nearPlane < 0.0f ||
        shadow.farPlane <= shadow.nearPlane) {
        return Result::failure("Directional shadow settings must be finite and valid");
    }

    if (!isFiniteVector(light.color) || light.color.r < 0.0f ||
        light.color.g < 0.0f || light.color.b < 0.0f) {
        return Result::failure(
            "Directional light color must be finite and nonnegative");
    }
    if (!std::isfinite(light.intensity) || light.intensity < 0.0f) {
        return Result::failure(
            "Directional light intensity must be finite and nonnegative");
    }

    return Result::success();
}

}  // namespace

Result Scene::addGameObject(std::unique_ptr<GameObject> gameObject) {
    if (!gameObject) {
        return Result::failure("Cannot add a null game object to a scene");
    }
    if (active_) {
        return Result::failure(
            "Cannot add game objects while the scene is active");
    }

    const bool isPointLight =
        dynamic_cast<PointLight*>(gameObject.get()) != nullptr;
    const bool isDirectionalLight =
        dynamic_cast<DirectionalLight*>(gameObject.get()) != nullptr;
    if (isDirectionalLight && directionalLightIndex_.has_value()) {
        return Result::failure(
            "A scene can contain only one directional light");
    }
    if (isPointLight &&
        pointLightCount_ >= scene_limits::maxPointLights) {
        return Result::failure(
            "Cannot add more than " +
            std::to_string(scene_limits::maxPointLights) +
            " point lights to a scene");
    }

    const std::size_t objectIndex = gameObjects_.size();
    try {
        gameObjects_.push_back(std::move(gameObject));
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to add game object: " + std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to add game object with an unknown error");
    }

    if (isPointLight) {
        pointLightIndices_[pointLightCount_] = objectIndex;
        ++pointLightCount_;
    }
    if (isDirectionalLight) {
        directionalLightIndex_ = objectIndex;
    }

    return Result::success();
}

Result Scene::setActiveCamera(std::shared_ptr<Camera> camera) {
    if (!camera) {
        return Result::failure("Cannot set a null camera as active");
    }

    activeCamera_ = std::move(camera);
    return Result::success();
}

Result Scene::validateForActivation() const {
    if (active_) {
        return Result::failure("Scene is already active");
    }
    if (pointLightCount_ > scene_limits::maxPointLights) {
        return Result::failure(
            "Scene exceeds the maximum point-light count");
    }

    for (std::size_t index = 0; index < gameObjects_.size(); ++index) {
        if (!gameObjects_[index]) {
            return Result::failure(
                "Scene contains a null game object at index " +
                std::to_string(index));
        }
    }

    std::size_t directionalLightCount = 0;
    for (const auto& gameObject : gameObjects_) {
        if (dynamic_cast<const DirectionalLight*>(gameObject.get()) !=
            nullptr) {
            ++directionalLightCount;
        }
    }

    if (!directionalLightIndex_.has_value()) {
        if (directionalLightCount != 0) {
            return Result::failure(
                "Scene contains an unregistered directional light");
        }
    } else {
        const std::size_t objectIndex = *directionalLightIndex_;
        if (objectIndex >= gameObjects_.size()) {
            return Result::failure(
                "Scene contains an invalid directional-light registration");
        }

        const auto* directionalLight = dynamic_cast<const DirectionalLight*>(
            gameObjects_[objectIndex].get());
        if (directionalLight == nullptr) {
            return Result::failure(
                "Scene contains an invalid directional-light registration");
        }
        if (directionalLightCount != 1) {
            return Result::failure(
                "Scene contains a duplicate directional-light registration");
        }

        Result lightResult = validateDirectionalLightState(*directionalLight);
        if (!lightResult) {
            return lightResult;
        }
    }

    for (std::size_t lightIndex = 0;
         lightIndex < pointLightCount_; ++lightIndex) {
        const std::size_t objectIndex = pointLightIndices_[lightIndex];
        if (objectIndex >= gameObjects_.size() ||
            dynamic_cast<const PointLight*>(
                gameObjects_[objectIndex].get()) == nullptr) {
            return Result::failure(
                "Scene contains an invalid point-light registration");
        }

        for (std::size_t previous = 0; previous < lightIndex; ++previous) {
            if (pointLightIndices_[previous] == objectIndex) {
                return Result::failure(
                    "Scene contains a duplicate point-light registration");
            }
        }
    }

    return Result::success();
}

Result Scene::copyAuthoringStateTo(Scene& runtimeScene) const {
    if (this == &runtimeScene) {
        return Result::failure(
            "Authoring state destination must be a different scene");
    }
    const auto& editorObjects = gameObjects_;
    const auto& runtimeObjects = runtimeScene.gameObjects_;
    if (editorObjects.size() != runtimeObjects.size()) {
        return Result::failure(
            "Editor and runtime scene object counts do not match");
    }

    for (std::size_t index = 0; index < editorObjects.size(); ++index) {
        if (!editorObjects[index] || !runtimeObjects[index]) {
            return Result::failure(
                "Editor/runtime topology contains a null object at index " +
                std::to_string(index));
        }
        if (typeid(*editorObjects[index]) !=
            typeid(*runtimeObjects[index])) {
            return Result::failure(
                "Editor/runtime object types do not match at index " +
                std::to_string(index));
        }

        const bool editorHasAttachedCamera =
            editorObjects[index]->attachedCamera() != nullptr;
        const bool runtimeHasAttachedCamera =
            runtimeObjects[index]->attachedCamera() != nullptr;
        if (editorHasAttachedCamera != runtimeHasAttachedCamera) {
            return Result::failure(
                "Editor/runtime attached-camera topology does not match at "
                "index " + std::to_string(index));
        }
    }

    Result result = runtimeScene.setBackgroundColor(backgroundColor_);
    if (!result) {
        return Result::failure(
            "Failed to copy editor background color: " + result.error());
    }
    result = runtimeScene.setAmbientLight(ambientColor_, ambientIntensity_);
    if (!result) {
        return Result::failure(
            "Failed to copy editor ambient light: " + result.error());
    }
    runtimeScene.name = name;

    for (std::size_t index = 0; index < editorObjects.size(); ++index) {
        const GameObject& editorObject = *editorObjects[index];
        GameObject& runtimeObject = *runtimeObjects[index];
        runtimeObject.name = editorObject.name;
        runtimeObject.position = editorObject.position;
        runtimeObject.rotation = editorObject.rotation;
        runtimeObject.scale = editorObject.scale;

        if (const auto* editorPointLight =
                dynamic_cast<const PointLight*>(&editorObject)) {
            auto& runtimePointLight =
                static_cast<PointLight&>(runtimeObject);
            runtimePointLight.color = editorPointLight->color;
            runtimePointLight.intensity = editorPointLight->intensity;
        } else if (const auto* editorDirectionalLight =
                       dynamic_cast<const DirectionalLight*>(&editorObject)) {
            auto& runtimeDirectionalLight =
                static_cast<DirectionalLight&>(runtimeObject);
            runtimeDirectionalLight.direction =
                editorDirectionalLight->direction;
            runtimeDirectionalLight.color = editorDirectionalLight->color;
            runtimeDirectionalLight.intensity =
                editorDirectionalLight->intensity;
            runtimeDirectionalLight.shadow = editorDirectionalLight->shadow;
        }

        if (const auto* editorCamera =
                dynamic_cast<const Camera*>(&editorObject)) {
            auto& runtimeCamera = static_cast<Camera&>(runtimeObject);
            runtimeCamera.front = editorCamera->front;
            runtimeCamera.up = editorCamera->up;
        }

        const Camera* editorAttachedCamera = editorObject.attachedCamera();
        Camera* runtimeAttachedCamera = runtimeObject.attachedCamera();
        if (editorAttachedCamera != nullptr && runtimeAttachedCamera != nullptr) {
            runtimeAttachedCamera->position = editorAttachedCamera->position;
            runtimeAttachedCamera->front = editorAttachedCamera->front;
            runtimeAttachedCamera->up = editorAttachedCamera->up;
        }
    }

    return Result::success();
}

const std::vector<std::unique_ptr<GameObject>>& Scene::gameObjects()
    const noexcept {
    return gameObjects_;
}

std::size_t Scene::pointLightCount() const noexcept {
    return pointLightCount_;
}

const PointLight& Scene::pointLightAt(std::size_t index) const {
    if (index >= pointLightCount_) {
        throw std::out_of_range("Point-light index is out of range");
    }
    return static_cast<const PointLight&>(
        *gameObjects_.at(pointLightIndices_[index]));
}

const DirectionalLight* Scene::directionalLight() const noexcept {
    if (!directionalLightIndex_.has_value() ||
        *directionalLightIndex_ >= gameObjects_.size() ||
        !gameObjects_[*directionalLightIndex_]) {
        return nullptr;
    }

    return dynamic_cast<const DirectionalLight*>(
        gameObjects_[*directionalLightIndex_].get());
}

const Camera* Scene::activeCamera() const noexcept {
    return activeCamera_.get();
}

bool Scene::isActive() const noexcept {
    return active_;
}

Result Scene::setBackgroundColor(const glm::vec4& color) {
    if (!isValidColorComponent(color.r) ||
        !isValidColorComponent(color.g) ||
        !isValidColorComponent(color.b) ||
        !isValidColorComponent(color.a)) {
        return Result::failure(
            "Background color must be finite and in [0, 1]");
    }

    backgroundColor_ = color;
    return Result::success();
}

Result Scene::setAmbientLight(const glm::vec3& color, float intensity) {
    if (!isValidColorComponent(color.r) ||
        !isValidColorComponent(color.g) ||
        !isValidColorComponent(color.b)) {
        return Result::failure(
            "Ambient color must be finite and in [0, 1]");
    }
    if (!std::isfinite(intensity) || intensity < 0.0f) {
        return Result::failure(
            "Ambient intensity must be finite and nonnegative");
    }

    ambientColor_ = color;
    ambientIntensity_ = intensity;
    return Result::success();
}

const glm::vec4& Scene::backgroundColor() const noexcept {
    return backgroundColor_;
}

const glm::vec3& Scene::ambientColor() const noexcept {
    return ambientColor_;
}

float Scene::ambientIntensity() const noexcept {
    return ambientIntensity_;
}

Result Scene::activate() {
    Result result = validateForActivation();
    if (!result) {
        return result;
    }

    active_ = true;
    return Result::success();
}

void Scene::deactivate() noexcept {
    active_ = false;
}
