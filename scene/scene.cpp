#include "scene.h"

#include "../math/transform_math.h"
#include "model_renderable.h"
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
    if (newParent != nullptr && !ownsGameObject(newParent)) {
        return Result::failure(
            "Cannot use a parent that is not owned by this scene");
    }
    if (ownsGameObject(&child) && child.parent_ == newParent) {
        return Result::success();
    }

    const std::size_t destinationIndex =
        newParent == nullptr ? rootObjects_.size()
                             : newParent->children_.size();
    return reparentGameObjectAt(child, newParent, mode, destinationIndex);
}

Result Scene::reorderGameObject(GameObject& object,
                                GameObject* expectedParent,
                                std::size_t siblingIndex) {
    if (!ownsGameObject(&object)) {
        return Result::failure(
            "Cannot reorder a game object that is not owned by this scene");
    }
    if (expectedParent != nullptr && !ownsGameObject(expectedParent)) {
        return Result::failure(
            "Cannot use a sibling parent that is not owned by this scene");
    }
    if (object.parent_ != expectedParent) {
        return Result::failure(
            "Hierarchy reorder cannot change an object's parent");
    }

    const Result validation = validateAuthoredState();
    if (!validation) {
        return Result::failure("Cannot reorder an invalid scene hierarchy: " +
                               validation.error());
    }

    std::vector<GameObject*>& siblings =
        expectedParent == nullptr ? rootObjects_ : expectedParent->children_;
    if (siblingIndex >= siblings.size()) {
        return Result::failure("Hierarchy sibling index is out of range");
    }

    const auto current = std::find(siblings.begin(), siblings.end(), &object);
    if (current == siblings.end()) {
        return Result::failure(
            "Hierarchy reorder could not find the object in its sibling list");
    }
    const std::size_t currentIndex =
        static_cast<std::size_t>(std::distance(siblings.begin(), current));
    if (currentIndex == siblingIndex) {
        return Result::success();
    }

    if (currentIndex > siblingIndex) {
        std::rotate(siblings.begin() + siblingIndex, current,
                    current + 1);
    } else {
        std::rotate(current, current + 1,
                    siblings.begin() + siblingIndex + 1);
    }
    return Result::success();
}

Result Scene::reorderGameObject(GameObject& object,
                                std::size_t siblingIndex) {
    return reorderGameObject(object, object.parent(), siblingIndex);
}

Result Scene::validateReparentGameObject(
    const GameObject& child, GameObject* newParent, ReparentMode mode,
    std::optional<std::size_t> destinationIndex) const {
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
        return Result::failure(
            "Reparenting to the current parent is not a hierarchy reorder");
    }

    const Result authoredState = validateAuthoredState();
    if (!authoredState) {
        return Result::failure("Cannot reparent within an invalid scene hierarchy: " +
                               authoredState.error());
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
    } else if (std::count(rootObjects_.begin(), rootObjects_.end(), &child) !=
               1) {
        return Result::failure(
            "Existing root hierarchy relationship is inconsistent");
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

    const std::size_t destinationSize =
        newParent == nullptr ? rootObjects_.size()
                             : newParent->children_.size();
    if (destinationIndex.has_value() &&
        *destinationIndex > destinationSize) {
        return Result::failure("Hierarchy insertion index is out of range");
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

    return Result::success();
}

Result Scene::reparentGameObjectAt(GameObject& child, GameObject* newParent,
                                   ReparentMode mode,
                                   std::size_t destinationIndex) {
    const Result validation = validateReparentGameObject(
        child, newParent, mode, destinationIndex);
    if (!validation) return validation;

    transform_math::DecomposedTransform newLocalTransform;
    if (mode == ReparentMode::PreserveWorld) {
        const glm::mat4 oldWorld = child.worldTransformMatrix();
        glm::mat4 newParentWorld(1.0f);
        if (newParent != nullptr) {
            newParentWorld = newParent->worldTransformMatrix();
        }

        const glm::mat4 linearParent(newParentWorld);
        const float determinant = glm::determinant(linearParent);
        float maximumLinearMagnitude = 0.0f;
        for (int column = 0; column < 3; ++column) {
            for (int row = 0; row < 3; ++row) {
                maximumLinearMagnitude = std::max(
                    maximumLinearMagnitude,
                    std::fabs(linearParent[column][row]));
            }
        }
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
        const glm::mat4 candidateLocal = inverseParent * oldWorld;
        const Result decomposition = transform_math::decomposeModelMatrix(
            candidateLocal, newLocalTransform);
        if (!decomposition) {
            return Result::failure(
                "PreserveWorld cannot represent the required local transform: " +
                decomposition.error());
        }
    }

    GameObject* oldParent = child.parent_;

    // Reserve before changing either side of the relationship. Once this
    // succeeds, pointer insertion/rotation/erasure cannot allocate or throw.
    try {
        if (newParent != nullptr) {
            newParent->children_.reserve(newParent->children_.size() + 1);
        } else {
            rootObjects_.reserve(rootObjects_.size() + 1);
        }
    } catch (const std::exception& exception) {
        return Result::failure("Failed to reserve hierarchy storage: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to reserve hierarchy storage");
    }

    std::vector<GameObject*>& destination =
        newParent == nullptr ? rootObjects_ : newParent->children_;
    destination.push_back(&child);
    if (destinationIndex != destination.size() - 1) {
        std::rotate(destination.begin() + destinationIndex,
                    destination.end() - 1, destination.end());
    }

    if (oldParent != nullptr) {
        const auto oldChild = std::find(oldParent->children_.begin(),
                                        oldParent->children_.end(), &child);
        oldParent->children_.erase(oldChild);
    } else {
        const auto oldRoot =
            std::find(rootObjects_.begin(), rootObjects_.end(), &child);
        rootObjects_.erase(oldRoot);
    }
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

Result Scene::reserveEditorHierarchyStorage(
    const std::vector<std::pair<GameObject*, std::size_t>>& requirements) {
    try {
        for (const auto& [parent, additionalChildren] : requirements) {
            if (parent != nullptr && !ownsGameObject(parent)) {
                return Result::failure(
                    "Cannot reserve hierarchy storage for a foreign parent");
            }
            std::vector<GameObject*>& siblings =
                parent == nullptr ? rootObjects_ : parent->children_;
            if (additionalChildren >
                std::numeric_limits<std::size_t>::max() - siblings.size()) {
                return Result::failure(
                    "Required hierarchy storage size is too large");
            }
            siblings.reserve(siblings.size() + additionalChildren);
        }
    } catch (const std::exception& exception) {
        return Result::failure(
            "Failed to reserve hierarchy storage: " +
            std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Failed to reserve hierarchy storage with an unknown error");
    }
    return Result::success();
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
        // All objects enter as roots. Reserve the presentation list before
        // ownership is committed so a root-order allocation failure cannot
        // leave an owned object without a root link.
        rootObjects_.reserve(rootObjects_.size() + 1);
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
    rootObjects_.push_back(insertedObject);
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

Result Scene::validateEditorGameObjectRemoval(GameObject* gameObject,
                                              bool requireNoChildren) const {
    if (!active_) {
        return Result::failure(
            "Controlled editor removal requires an active scene");
    }
    if (gameObject == nullptr || !ownsGameObject(gameObject)) {
        return Result::failure(
            "Controlled editor removal requires an object owned by the scene");
    }
    if (!gameObject->modelRenderable().renderTopologyMutable()) {
        return Result::failure(
            "Controlled editor removal requires renderer resources to be detached");
    }
    if (requireNoChildren && !gameObject->children_.empty()) {
        return Result::failure(
            "Controlled editor removal requires a game object without children");
    }

    const std::size_t objectIndex = static_cast<std::size_t>(
        std::distance(
            gameObjects_.begin(),
            std::find_if(gameObjects_.begin(), gameObjects_.end(),
                         [gameObject](const auto& object) {
                             return object.get() == gameObject;
                         })));
    if (objectIndex >= gameObjects_.size()) {
        return Result::failure(
            "Controlled editor removal could not locate the owned object");
    }

    if (gameObject->parent_ != nullptr) {
        if (!ownsGameObject(gameObject->parent_) ||
            std::count(gameObject->parent_->children_.begin(),
                       gameObject->parent_->children_.end(), gameObject) != 1) {
            return Result::failure(
                "Controlled editor removal found an inconsistent parent relationship");
        }
    } else if (std::count(rootObjects_.begin(), rootObjects_.end(), gameObject) !=
               1) {
        return Result::failure(
            "Controlled editor removal found an inconsistent root relationship");
    }

    if (pointLightCount_ > pointLightIndices_.size()) {
        return Result::failure(
            "Controlled editor removal found invalid point-light bookkeeping");
    }
    const bool isPointLight =
        dynamic_cast<PointLight*>(gameObject) != nullptr;
    if (isPointLight) {
        const auto pointLight = std::find(
            pointLightIndices_.begin(),
            pointLightIndices_.begin() + pointLightCount_, objectIndex);
        if (pointLight == pointLightIndices_.begin() + pointLightCount_) {
            return Result::failure(
                "Controlled editor removal found an unregistered point light");
        }
    }

    return Result::success();
}

Result Scene::removeGameObjectForEditor(
    GameObject* gameObject, std::unique_ptr<GameObject>& removed) noexcept {
    removed.reset();
    const Result validation = validateEditorGameObjectRemoval(gameObject);
    if (!validation) return validation;

    const std::size_t objectIndex = static_cast<std::size_t>(
        std::distance(
            gameObjects_.begin(),
            std::find_if(gameObjects_.begin(), gameObjects_.end(),
                         [gameObject](const auto& object) {
                             return object.get() == gameObject;
                         })));
    if (objectIndex >= gameObjects_.size()) {
        return Result::failure(
            "Controlled editor removal could not locate the owned object");
    }

    const bool isPointLight =
        dynamic_cast<PointLight*>(gameObject) != nullptr;
    if (isPointLight) {
        const std::size_t pointLightIndex = static_cast<std::size_t>(
            std::distance(
                pointLightIndices_.begin(),
                std::find(pointLightIndices_.begin(),
                          pointLightIndices_.begin() + pointLightCount_,
                          objectIndex)));
        for (std::size_t index = pointLightIndex + 1;
             index < pointLightCount_; ++index) {
            pointLightIndices_[index - 1] = pointLightIndices_[index];
        }
        --pointLightCount_;
        pointLightIndices_[pointLightCount_] = 0;
    }

    for (std::size_t index = 0; index < pointLightCount_; ++index) {
        if (pointLightIndices_[index] > objectIndex) {
            --pointLightIndices_[index];
        }
    }
    if (directionalLightIndex_.has_value()) {
        if (*directionalLightIndex_ == objectIndex) {
            directionalLightIndex_.reset();
        } else if (*directionalLightIndex_ > objectIndex) {
            --*directionalLightIndex_;
        }
    }

    const Camera* activeCamera = activeCamera_.get();
    const Camera* standaloneCamera = dynamic_cast<const Camera*>(gameObject);
    const Camera* attachedCamera = gameObject->attachedCamera();
    if (activeCamera == standaloneCamera ||
        (attachedCamera != nullptr && activeCamera == attachedCamera)) {
        activeCamera_.reset();
    }

    if (gameObject->parent_ != nullptr) {
        auto& siblings = gameObject->parent_->children_;
        siblings.erase(std::find(siblings.begin(), siblings.end(), gameObject));
    } else {
        rootObjects_.erase(
            std::find(rootObjects_.begin(), rootObjects_.end(), gameObject));
    }
    gameObject->parent_ = nullptr;

    removed = std::move(gameObjects_[objectIndex]);
    gameObjects_.erase(gameObjects_.begin() +
                       static_cast<std::ptrdiff_t>(objectIndex));
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

    std::unordered_set<const GameObject*> orderedRoots;
    orderedRoots.reserve(rootObjects_.size());
    for (std::size_t index = 0; index < rootObjects_.size(); ++index) {
        const GameObject* root = rootObjects_[index];
        if (!isOwned(root)) {
            return Result::failure(
                "Scene root list contains a null or foreign GameObject at index " +
                std::to_string(index));
        }
        if (!orderedRoots.insert(root).second) {
            return Result::failure(
                "Scene root list contains a duplicate GameObject");
        }
    }

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

        const std::size_t rootOccurrences = static_cast<std::size_t>(
            std::count(rootObjects_.begin(), rootObjects_.end(), &object));
        if (object.parent_ == nullptr) {
            if (rootOccurrences != 1) {
                return Result::failure(
                    "Parentless GameObject must appear exactly once in the Scene root list");
            }
        } else if (rootOccurrences != 0) {
            return Result::failure(
                "Child GameObject may not appear in the Scene root list");
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

const std::vector<GameObject*>& Scene::rootObjects() const noexcept {
    return rootObjects_;
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
