#include "scene_manager.h"

#include <exception>
#include <string>
#include <utility>

Result SceneManager::constructInitializedScene(
    std::unique_ptr<Scene>& scene) const {
    if (!sceneConstructor_) {
        return Result::failure("Scene constructor is not configured");
    }
    if (!inputManager_) {
        return Result::failure("Scene input manager is not initialized");
    }

    try {
        std::unique_ptr<Scene> candidate = sceneConstructor_();
        if (!candidate) {
            return Result::failure("Scene constructor returned a null scene");
        }
        candidate->name = sceneName_;
        candidate->inputManager = inputManager_;
        candidate->init();

        Result result = candidate->validateForActivation();
        if (!result) {
            return Result::failure("Initialized scene is invalid: " +
                                   result.error());
        }
        scene = std::move(candidate);
        return Result::success();
    } catch (const std::exception& exception) {
        return Result::failure("Scene initialization failed: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure(
            "Scene initialization failed with an unknown error");
    }
}

Result SceneManager::initializeEditingScene() {
    if (!inputManager_) {
        return Result::failure(
            "Cannot initialize Scene Manager with a null Input Manager");
    }

    Result result = constructInitializedScene(editingScene_);
    if (!result) {
        return Result::failure("Failed to create editing scene: " +
                               result.error());
    }
    activeScene_ = editingScene_.get();
    return Result::success();
}

Result SceneManager::prepareRuntimeScene() {
    if (!initialized()) {
        return Result::failure("Scene Manager is not initialized");
    }
    if (runtimeScene_) {
        return Result::failure("Runtime scene already exists");
    }
    if (activeScene_ != editingScene_.get()) {
        return Result::failure(
            "Runtime scene can be prepared only from the editing scene");
    }

    std::unique_ptr<Scene> candidate;
    Result result = constructInitializedScene(candidate);
    if (!result) {
        return Result::failure("Failed to create runtime scene: " +
                               result.error());
    }

    result = editingScene_->copyAuthoringStateTo(*candidate);
    if (!result) {
        return Result::failure(
            "Failed to transfer editor-authored state: " + result.error());
    }
    result = candidate->validateForActivation();
    if (!result) {
        return Result::failure(
            "Runtime scene is invalid after authoring transfer: " +
            result.error());
    }

    runtimeScene_ = std::move(candidate);
    return Result::success();
}

Result SceneManager::commitRuntimeScene() {
    if (!runtimeScene_) {
        return Result::failure("No prepared runtime scene exists");
    }
    if (activeScene_ != editingScene_.get()) {
        return Result::failure(
            "Runtime scene cannot be committed from the current state");
    }
    activeScene_ = runtimeScene_.get();
    return Result::success();
}

void SceneManager::cancelPreparedRuntimeScene() noexcept {
    if (activeScene_ != runtimeScene_.get()) {
        runtimeScene_.reset();
    }
}

Result SceneManager::returnToEditingScene() {
    if (!editingScene_) {
        return Result::failure("Editing scene is unavailable");
    }
    if (!runtimeScene_ || activeScene_ != runtimeScene_.get()) {
        return Result::failure("Scene Manager has no active runtime scene");
    }
    activeScene_ = editingScene_.get();
    return Result::success();
}

Result SceneManager::destroyRuntimeScene() {
    if (!runtimeScene_) {
        return Result::success();
    }
    if (activeScene_ == runtimeScene_.get() || runtimeScene_->isActive()) {
        return Result::failure(
            "Cannot destroy the runtime scene while it is active");
    }
    runtimeScene_.reset();
    return Result::success();
}

Scene* SceneManager::editingScene() noexcept { return editingScene_.get(); }

const Scene* SceneManager::editingScene() const noexcept {
    return editingScene_.get();
}

Scene* SceneManager::runtimeScene() noexcept { return runtimeScene_.get(); }

const Scene* SceneManager::runtimeScene() const noexcept {
    return runtimeScene_.get();
}

Scene* SceneManager::activeScene() noexcept { return activeScene_; }

const Scene* SceneManager::activeScene() const noexcept {
    return activeScene_;
}

bool SceneManager::isRuntimeSceneActive() const noexcept {
    return runtimeScene_ && activeScene_ == runtimeScene_.get();
}

bool SceneManager::initialized() const noexcept {
    return editingScene_ && activeScene_;
}

void SceneManager::shutdown() noexcept {
    activeScene_ = nullptr;
    runtimeScene_.reset();
    editingScene_.reset();
    inputManager_.reset();
    sceneConstructor_ = {};
    sceneName_.clear();
}
