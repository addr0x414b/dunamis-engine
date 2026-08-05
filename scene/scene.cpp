#include "scene.h"

#include <cmath>
#include <exception>
#include <stdexcept>
#include <utility>

namespace {

bool isValidColorComponent(float component) {
    return std::isfinite(component) && component >= 0.0f &&
           component <= 1.0f;
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
    if (!activeCamera_) {
        return Result::failure("Scene does not have an active camera");
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
