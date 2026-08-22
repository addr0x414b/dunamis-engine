#include "scene_manager.h"

#include <exception>
#include <algorithm>
#include <fstream>
#include <filesystem>
#include <system_error>
#include <string>
#include <utility>

SceneManager::SceneManager() {
    registryInitialization_ = registerEngineTypes(typeRegistry_);
}

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
    if (!registryInitialization_) {
        return Result::failure("Engine type registration failed: " +
                               registryInitialization_.error());
    }
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
    nlohmann::json baseline;
    result = SceneSerializer::serializeAuthored(*editingScene_, typeRegistry_, baseline);
    if (result) {
        authoredBaseline_ = std::move(baseline);
        authoredBaselineAvailable_ = true;
    }
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

    result = SceneSerializer::copyAuthoredState(
        *editingScene_, *candidate, typeRegistry_);
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

Result SceneManager::saveEditingScene(const Camera& editorCamera) {
    if (!editingScene_) return Result::failure("Editing scene is unavailable");
    if (currentScenePath_.empty()) return Result::failure("Current scene path is empty");
    return saveEditingSceneTo(currentScenePath_, editorCamera);
}

Result SceneManager::saveEditingSceneAs(
    const std::filesystem::path& path, const Camera& editorCamera) {
    if (path.empty()) return Result::failure("Save As path is empty");
    Result result = saveEditingSceneTo(path, editorCamera);
    if (!result) return result;
    currentScenePath_ = path;
    return Result::success();
}

Result SceneManager::saveEditingSceneTo(
    const std::filesystem::path& path, const Camera& editorCamera) {
    if (!editingScene_) return Result::failure("Editing scene is unavailable");
    if (path.empty()) return Result::failure("Scene path is empty");

    std::error_code error;
    const bool destinationExists = std::filesystem::exists(path, error);
    if (error) {
        return Result::failure("Failed to query scene destination '" +
                               path.string() + "': " + error.message());
    }
    if (destinationExists) {
        const bool destinationIsDirectory =
            std::filesystem::is_directory(path, error);
        if (error) {
            return Result::failure("Failed to query scene destination '" +
                                   path.string() + "': " + error.message());
        }
        if (destinationIsDirectory) {
            return Result::failure("Scene destination '" + path.string() +
                                   "' is an existing directory");
        }
    }

    nlohmann::json document;
    Result result = SceneSerializer::serializeFull(
        *editingScene_, typeRegistry_, editorCamera, editorRenderColliders_, document);
    if (!result) return Result::failure("Failed to serialize scene: " + result.error());

    const std::filesystem::path parent = path.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) return Result::failure("Failed to create scene directory '" +
                                          parent.string() + "': " + error.message());
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        if (!stream) return Result::failure("Failed to open temporary scene file '" +
                                            temporary.string() + "'");
        stream << document.dump(2) << '\n';
        stream.flush();
        if (!stream) return Result::failure("Failed to write temporary scene file '" +
                                            temporary.string() + "'");
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        const std::filesystem::path backup = path.string() + ".bak";
        std::error_code backupError;
        const bool destinationStillExists =
            std::filesystem::exists(path, backupError);
        if (backupError) {
            return Result::failure("Failed to query existing scene file '" +
                                   path.string() + "': " + backupError.message());
        }
        if (destinationStillExists) {
            std::filesystem::rename(path, backup, backupError);
        }
        if (backupError) return Result::failure("Failed to replace scene file '" +
                                                path.string() + "': " + backupError.message());
        error.clear();
        std::filesystem::rename(temporary, path, error);
        if (error) {
            std::error_code ignored;
            if (destinationStillExists) {
                std::filesystem::rename(backup, path, ignored);
            }
            return Result::failure("Failed to replace scene file '" +
                                   path.string() + "': " + error.message());
        }
        if (destinationStillExists) {
            std::filesystem::remove(backup, backupError);
        }
    }
    nlohmann::json authoredBaseline;
    result = SceneSerializer::serializeAuthored(
        *editingScene_, typeRegistry_, authoredBaseline);
    if (!result) return Result::failure("Scene was saved but baseline capture failed: " + result.error());
    authoredBaseline_ = std::move(authoredBaseline);
    authoredBaselineAvailable_ = true;
    persistenceWarnings_.clear();
    return Result::success();
}

Result SceneManager::prepareEditingSceneLoad(const std::filesystem::path& path) {
    if (!initialized()) return Result::failure("Scene Manager is not initialized");
    if (isRuntimeSceneActive() || runtimeScene_) {
        return Result::failure("Scene loading is unavailable during a runtime session");
    }
    if (path.empty()) return Result::failure("Scene path is empty");
    std::ifstream stream(path, std::ios::binary);
    if (!stream) return Result::failure("Failed to open scene file '" + path.string() + "'");
    nlohmann::json document;
    try { stream >> document; }
    catch (const std::exception& exception) {
        return Result::failure("Failed to parse scene file '" + path.string() + "': " + exception.what());
    }
    std::unique_ptr<Scene> candidate;
    Result result = constructInitializedScene(candidate);
    if (!result) return Result::failure("Failed to construct candidate scene: " + result.error());
    SceneLoadData loadData;
    result = SceneSerializer::applyDocument(document, *candidate, typeRegistry_, loadData);
    if (!result) return Result::failure("Failed to load scene '" + path.string() + "': " + result.error());
    preparedEditingScene_ = std::move(candidate);
    preparedScenePath_ = path;
    preparedLoadData_ = std::move(loadData);
    return Result::success();
}

Scene* SceneManager::preparedEditingScene() noexcept {
    return preparedEditingScene_.get();
}

const std::optional<EditorCameraState>&
SceneManager::preparedEditorCamera() const noexcept {
    return preparedLoadData_.editorCamera;
}

const std::vector<std::string>& SceneManager::preparedRenderColliders() const noexcept {
    return preparedLoadData_.renderColliders;
}

void SceneManager::setEditorRenderColliders(std::vector<std::string> ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    editorRenderColliders_ = std::move(ids);
}

Result SceneManager::commitPreparedEditingSceneLoad() {
    if (!preparedEditingScene_) return Result::failure("No prepared editing scene exists");
    if (previousEditingScene_) return Result::failure("A previous editing scene is still retained");
    previousEditingScene_ = std::move(editingScene_);
    editingScene_ = std::move(preparedEditingScene_);
    activeScene_ = editingScene_.get();
    currentScenePath_ = std::move(preparedScenePath_);
    authoredBaseline_ = std::move(preparedLoadData_.authoredBaseline);
    authoredBaselineAvailable_ = true;
    persistenceWarnings_ = std::move(preparedLoadData_.warnings);
    editorRenderColliders_ = std::move(preparedLoadData_.renderColliders);
    preparedLoadData_ = {};
    return Result::success();
}

void SceneManager::cancelPreparedEditingSceneLoad() noexcept {
    preparedEditingScene_.reset();
    preparedScenePath_.clear();
    preparedLoadData_ = {};
}

Scene* SceneManager::previousEditingScene() noexcept {
    return previousEditingScene_.get();
}

void SceneManager::finishEditingSceneLoad() noexcept {
    previousEditingScene_.reset();
}

bool SceneManager::hasUnsavedChanges() const {
    if (!editingScene_ || !authoredBaselineAvailable_) return false;
    nlohmann::json current;
    Result result = SceneSerializer::serializeAuthored(
        *editingScene_, typeRegistry_, current);
    return !result || current != authoredBaseline_;
}

Result SceneManager::captureCurrentAuthoredBaseline() {
    if (!editingScene_) return Result::failure("Editing scene is unavailable");
    Result result = SceneSerializer::serializeAuthored(
        *editingScene_, typeRegistry_, authoredBaseline_);
    if (result) authoredBaselineAvailable_ = true;
    return result;
}

void SceneManager::setCurrentScenePath(std::filesystem::path path) {
    currentScenePath_ = std::move(path);
}

const std::filesystem::path& SceneManager::currentScenePath() const noexcept {
    return currentScenePath_;
}

const std::vector<std::string>& SceneManager::persistenceWarnings() const noexcept {
    return persistenceWarnings_;
}

TypeRegistry& SceneManager::typeRegistry() noexcept { return typeRegistry_; }
const TypeRegistry& SceneManager::typeRegistry() const noexcept { return typeRegistry_; }

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
    preparedEditingScene_.reset();
    previousEditingScene_.reset();
    inputManager_.reset();
    sceneConstructor_ = {};
    sceneName_.clear();
    authoredBaseline_ = {};
    authoredBaselineAvailable_ = false;
    currentScenePath_.clear();
    preparedScenePath_.clear();
    preparedLoadData_ = {};
    persistenceWarnings_.clear();
    editorRenderColliders_.clear();
}
