#include "scene.h"

#include "../math/transform_math.h"
#include "persistent_id.h"

#include <algorithm>
#include <cmath>
#include <exception>
#include <limits>
#include <stdexcept>
#include <unordered_set>
#include <utility>

namespace {

bool isValidColorComponent(float component) {
    return std::isfinite(component) && component >= 0.0f &&
           component <= 1.0f;
}

bool isFiniteVector(const glm::vec3& vector) {
    return std::isfinite(vector.x) && std::isfinite(vector.y) &&
           std::isfinite(vector.z);
}

bool isValidCameraState(const Camera& camera) {
    double yaw = 0.0;
    double pitch = 0.0;
    const glm::vec3 cross = glm::cross(camera.front, camera.up);
    return isFiniteVector(camera.position) && isFiniteVector(camera.up) &&
           camera.deriveYawPitchDegrees(yaw, pitch) &&
           std::isfinite(glm::dot(cross, cross)) &&
           glm::dot(cross, cross) > 1.0e-8f;
}

Result validateDirectionalLightState(const DirectionalLight& light) {
    glm::vec3 direction;
    if (!light.calculateWorldDirection(direction)) {
        return Result::failure(
            "Directional light rotation does not produce a valid direction");
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
    GameObject* ignored = nullptr;
    return addGameObjectInternal(std::move(gameObject), false, ignored);
}

Result Scene::reparentGameObject(GameObject& child, GameObject* newParent,
                                 ReparentMode mode) {
    if (!ownsGameObject(&child)) {
        return Result::failure(
            "Cannot reparent a game object that is not owned by this scene");
    }
    if (newParent != nullptr && !ownsGameObject(newParent)) {
        return Result::failure(
            "Cannot use a parent that is not owned by this scene");
    }
    if (newParent == &child) {
        return Result::failure("A game object cannot be its own parent");
    }

    switch (mode) {
    case ReparentMode::PreserveWorld:
    case ReparentMode::PreserveLocal:
        break;
    default:
        return Result::failure("Unknown game object reparent mode");
    }

    if (child.parent_ == newParent) {
        return Result::success();
    }

    std::unordered_set<GameObject*> ancestorChain;
    for (GameObject* ancestor = newParent; ancestor != nullptr;
         ancestor = ancestor->parent_) {
        if (!ownsGameObject(ancestor)) {
            return Result::failure(
                "Existing hierarchy parent is not owned by this scene");
        }
        if (ancestor == &child) {
            return Result::failure(
                "Cannot reparent a game object beneath its descendant");
        }
        if (!ancestorChain.insert(ancestor).second) {
            return Result::failure("Cannot reparent into a hierarchy cycle");
        }
    }

    GameObject* oldParent = child.parent_;
    std::unordered_set<GameObject*> existingParentChain;
    for (GameObject* ancestor = oldParent; ancestor != nullptr;
         ancestor = ancestor->parent_) {
        if (!ownsGameObject(ancestor) || ancestor == &child ||
            !existingParentChain.insert(ancestor).second) {
            return Result::failure(
                "Existing hierarchy contains an invalid cycle");
        }
    }

    std::vector<GameObject*>::iterator oldChild = {};
    if (oldParent != nullptr) {
        if (!ownsGameObject(oldParent)) {
            return Result::failure(
                "Existing hierarchy parent is not owned by this scene");
        }
        const auto oldBegin = oldParent->children_.begin();
        const auto oldEnd = oldParent->children_.end();
        if (std::count(oldBegin, oldEnd, &child) != 1) {
            return Result::failure(
                "Existing parent/child relationship is inconsistent");
        }
        oldChild = std::find(oldBegin, oldEnd, &child);
    }

    for (const auto& object : gameObjects_) {
        if (!object) {
            return Result::failure(
                "Cannot reparent within a scene containing a null game object");
        }
        const std::size_t occurrences = static_cast<std::size_t>(std::count(
            object->children_.begin(), object->children_.end(), &child));
        if (object.get() == oldParent) {
            continue;
        }
        if (occurrences != 0) {
            return Result::failure(
                "Existing parent/child relationship is inconsistent");
        }
    }

    if (newParent != nullptr &&
        std::count(newParent->children_.begin(), newParent->children_.end(),
                   &child) != 0) {
        return Result::failure(
            "The new parent already contains this child");
    }

    transform_math::DecomposedTransform newLocalTransform;
    if (mode == ReparentMode::PreserveWorld) {
        const glm::mat4 oldWorld = child.worldTransformMatrix();
        if (!transform_math::isFiniteMatrix(oldWorld)) {
            return Result::failure(
                "PreserveWorld requires a finite current world transform");
        }

        glm::mat4 newParentWorld(1.0f);
        if (newParent != nullptr) {
            newParentWorld = newParent->worldTransformMatrix();
        }
        if (!transform_math::isFiniteMatrix(newParentWorld)) {
            return Result::failure(
                "PreserveWorld requires a finite new-parent transform");
        }

        const glm::mat3 linearPart(newParentWorld);
        float maximumLinearMagnitude = 0.0f;
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                maximumLinearMagnitude = std::max(
                    maximumLinearMagnitude,
                    std::fabs(linearPart[column][row]));
            }
        }
        const float determinant = glm::determinant(linearPart);
        const float determinantTolerance =
            128.0f * std::numeric_limits<float>::epsilon() *
            maximumLinearMagnitude * maximumLinearMagnitude *
            maximumLinearMagnitude;
        if (!std::isfinite(determinant) ||
            !std::isfinite(maximumLinearMagnitude) ||
            maximumLinearMagnitude <= 0.0f ||
            std::fabs(determinant) <= determinantTolerance) {
            return Result::failure(
                "PreserveWorld requires an invertible new-parent transform");
        }

        const glm::mat4 inverseParent = glm::inverse(newParentWorld);
        if (!transform_math::isFiniteMatrix(inverseParent)) {
            return Result::failure(
                "PreserveWorld could not safely invert the new-parent transform");
        }

        const glm::mat4 candidateLocal = inverseParent * oldWorld;
        if (!transform_math::isFiniteMatrix(candidateLocal)) {
            return Result::failure(
                "PreserveWorld produced a non-finite local transform");
        }
        const Result decomposition = transform_math::decomposeModelMatrix(
            candidateLocal, newLocalTransform);
        if (!decomposition) {
            return Result::failure(
                "PreserveWorld cannot represent the required local transform: " +
                decomposition.error());
        }
    }

    // Add first so a possible allocation failure leaves the old relationship
    // untouched. Erasing from the old vector and assigning parent_ cannot
    // throw after this point.
    if (newParent != nullptr) {
        try {
            newParent->children_.push_back(&child);
        } catch (const std::exception& exception) {
            return Result::failure(
                "Failed to add child to the new parent: " +
                std::string(exception.what()));
        } catch (...) {
            return Result::failure(
                "Failed to add child to the new parent");
        }
    }
    if (oldParent != nullptr) oldParent->children_.erase(oldChild);
    child.parent_ = newParent;
    if (mode == ReparentMode::PreserveWorld) {
        child.position = newLocalTransform.position;
        child.rotation = newLocalTransform.rotation;
        child.scale = newLocalTransform.scale;
    }
    return Result::success();
}

Result Scene::validateEditorGameObjectInsertion(
    const GameObject& gameObject) const {
    if (!active_) {
        return Result::failure(
            "Controlled editor insertion requires an active scene");
    }
    return validateGameObjectInsertion(gameObject);
}

Result Scene::addGameObjectForEditor(
    std::unique_ptr<GameObject> gameObject, GameObject*& inserted) {
    inserted = nullptr;
    return addGameObjectInternal(std::move(gameObject), true, inserted);
}

Result Scene::addGameObjectInternal(
    std::unique_ptr<GameObject> gameObject, bool allowActive,
    GameObject*& inserted) {
    inserted = nullptr;
    if (!gameObject) {
        return Result::failure("Cannot add a null game object to a scene");
    }
    if (active_ && !allowActive) {
        return Result::failure(
            "Cannot add game objects while the scene is active");
    }

    if (allowActive && !active_) {
        return Result::failure(
            "Controlled editor insertion requires an active scene");
    }

    Result validation = validateGameObjectInsertion(*gameObject);
    if (!validation) return validation;

    const bool isPointLight =
        dynamic_cast<PointLight*>(gameObject.get()) != nullptr;
    const bool isDirectionalLight =
        dynamic_cast<DirectionalLight*>(gameObject.get()) != nullptr;

    if (gameObject->persistentId.empty()) {
        constexpr std::size_t maxGenerationAttempts = 64;
        std::string generatedId;
        bool foundUniqueId = false;
        try {
            for (std::size_t attempt = 0; attempt < maxGenerationAttempts;
                 ++attempt) {
                generatedId = persistent_id::generate();
                if (!generatedId.empty() &&
                    findGameObject(generatedId) == nullptr) {
                    foundUniqueId = true;
                    break;
                }
            }
        } catch (const std::exception& exception) {
            return Result::failure(
                "Failed to generate a persistent ID: " +
                std::string(exception.what()));
        } catch (...) {
            return Result::failure(
                "Failed to generate a persistent ID with an unknown error");
        }

        if (!foundUniqueId) {
            return Result::failure(
                "Failed to generate a unique persistent ID for the game object");
        }
        gameObject->persistentId = std::move(generatedId);
    }

    const std::size_t objectIndex = gameObjects_.size();
    GameObject* insertedObject = gameObject.get();
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
    inserted = insertedObject;
    return Result::success();
}

Result Scene::validateGameObjectInsertion(
    const GameObject& gameObject) const {

    if (gameObject.parent_ != nullptr || !gameObject.children_.empty()) {
        return Result::failure(
            "Cannot add a game object with an existing hierarchy relationship");
    }

    const bool isPointLight =
        dynamic_cast<const PointLight*>(&gameObject) != nullptr;
    const bool isDirectionalLight =
        dynamic_cast<const DirectionalLight*>(&gameObject) != nullptr;
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

    if (!gameObject.persistentId.empty() &&
        findGameObject(gameObject.persistentId) != nullptr) {
        return Result::failure("Scene contains duplicate persistent ID '" +
                               gameObject.persistentId + "'");
    }

    return Result::success();
}

Result Scene::removeGameObjectForEditor(
    GameObject* gameObject, std::unique_ptr<GameObject>& removed) noexcept {
    removed.reset();
    if (!active_) {
        return Result::failure(
            "Controlled editor rollback requires an active scene");
    }
    if (gameObjects_.empty() || gameObjects_.back().get() != gameObject) {
        return Result::failure(
            "Controlled editor rollback can remove only the last inserted object");
    }
    if (gameObject->parent_ != nullptr || !gameObject->children_.empty()) {
        return Result::failure(
            "Controlled editor rollback cannot remove a related game object");
    }

    const std::size_t objectIndex = gameObjects_.size() - 1;
    const bool isPointLight =
        dynamic_cast<PointLight*>(gameObject) != nullptr;
    const bool isDirectionalLight =
        dynamic_cast<DirectionalLight*>(gameObject) != nullptr;
    removed = std::move(gameObjects_.back());
    gameObjects_.pop_back();
    if (isPointLight) {
        if (pointLightCount_ > 0 &&
            pointLightIndices_[pointLightCount_ - 1] == objectIndex) {
            --pointLightCount_;
            pointLightIndices_[pointLightCount_] = 0;
        }
    }
    if (isDirectionalLight && directionalLightIndex_ == objectIndex) {
        directionalLightIndex_.reset();
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

Result Scene::setActiveCameraReference(Camera* camera) {
    if (!camera) return Result::failure("Cannot set a null camera as active");
    activeCamera_ = std::shared_ptr<Camera>(camera, [](Camera*) {});
    return Result::success();
}

Result Scene::validateForActivation() const {
    if (active_) {
        return Result::failure("Scene is already active");
    }
    return validateAuthoredState();
}

Result Scene::validateAuthoredState() const {
    if (pointLightCount_ > scene_limits::maxPointLights) {
        return Result::failure(
            "Scene exceeds the maximum point-light count");
    }

    std::unordered_set<const GameObject*> ownedObjects;
    ownedObjects.reserve(gameObjects_.size());
    for (std::size_t index = 0; index < gameObjects_.size(); ++index) {
        if (!gameObjects_[index]) {
            return Result::failure(
                "Scene contains a null game object at index " +
                std::to_string(index));
        }
        ownedObjects.insert(gameObjects_[index].get());
    }

    const auto isOwned = [&ownedObjects](const GameObject* object) {
        return object != nullptr && ownedObjects.find(object) !=
               ownedObjects.end();
    };

    std::unordered_set<std::string> persistentIds;
    for (std::size_t index = 0; index < gameObjects_.size(); ++index) {
        const std::string& id = gameObjects_[index]->persistentId;
        if (!id.empty() && !persistentIds.insert(id).second) {
            return Result::failure("Scene contains duplicate persistent ID '" +
                                   id + "'");
        }
        const GameObject& object = *gameObjects_[index];
        if (object.parent_ == &object) {
            return Result::failure("GameObject cannot be its own parent");
        }
        if (object.parent_ != nullptr && !isOwned(object.parent_)) {
            return Result::failure(
                "GameObject parent is not owned by this scene");
        }
        if (object.parent_ != nullptr &&
            std::count(object.parent_->children_.begin(),
                       object.parent_->children_.end(), &object) != 1) {
            return Result::failure(
                "GameObject parent/child relationship is inconsistent");
        }
        for (const GameObject* child : object.children_) {
            if (!isOwned(child)) {
                return Result::failure(
                    "GameObject child is not owned by this scene");
            }
            if (child == &object) {
                return Result::failure("GameObject cannot be its own child");
            }
            if (child->parent_ != &object) {
                return Result::failure(
                    "GameObject child/parent relationship is inconsistent");
            }
            if (std::count(object.children_.begin(), object.children_.end(),
                           child) != 1) {
                return Result::failure(
                    "GameObject child appears more than once in its parent");
            }
        }

        std::unordered_set<const GameObject*> ancestors;
        for (const GameObject* ancestor = object.parent_;
             ancestor != nullptr; ancestor = ancestor->parent_) {
            if (!ancestors.insert(ancestor).second || ancestor == &object) {
                return Result::failure("Scene hierarchy contains a cycle");
            }
        }

        if (!isFiniteVector(object.position) ||
            !isFiniteVector(object.rotation) ||
            !isFiniteVector(object.scale)) {
            if (dynamic_cast<const DirectionalLight*>(&object) != nullptr &&
                !isFiniteVector(object.rotation)) {
                return Result::failure(
                    "Directional light rotation must be finite");
            }
            return Result::failure("GameObject transform must be finite");
        }
        if (const auto* camera = dynamic_cast<const Camera*>(&object)) {
            if (!isValidCameraState(*camera)) {
                return Result::failure("Camera state must contain a valid finite orientation");
            }
        }
        if (const Camera* attached = object.attachedCamera()) {
            if (!isValidCameraState(*attached)) {
                return Result::failure("Attached camera state must contain a valid finite orientation");
            }
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

        const PointLight& pointLight = static_cast<const PointLight&>(
            *gameObjects_[objectIndex]);
        if (!isFiniteVector(pointLight.color) || pointLight.color.r < 0.0f ||
            pointLight.color.g < 0.0f || pointLight.color.b < 0.0f ||
            !std::isfinite(pointLight.intensity) || pointLight.intensity < 0.0f) {
            return Result::failure(
                "Point light color and intensity must be finite and nonnegative");
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

bool Scene::ownsGameObject(const GameObject* gameObject) const noexcept {
    if (gameObject == nullptr) return false;
    for (const auto& owned : gameObjects_) {
        if (owned.get() == gameObject) return true;
    }
    return false;
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

GameObject* Scene::findGameObject(const std::string& persistentId) noexcept {
    for (const auto& object : gameObjects_) {
        if (object && object->persistentId == persistentId) return object.get();
    }
    return nullptr;
}

const GameObject* Scene::findGameObject(
    const std::string& persistentId) const noexcept {
    for (const auto& object : gameObjects_) {
        if (object && object->persistentId == persistentId) return object.get();
    }
    return nullptr;
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
