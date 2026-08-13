#include "dunamis.h"

#include "../game/level_1.h"

#include "spdlog/spdlog.h"

#include <exception>
#include <filesystem>
#include <string>
#include <system_error>

Result Dunamis::initialize() {
    if (initialized) {
        return Result::success();
    }
    if (initializationAttempted) {
        return Result::failure(
            "Dunamis Engine initialization cannot be retried after failure");
    }
    initializationAttempted = true;

    spdlog::info("Dunamis Engine v0.0.2");

    try {
        Result result = platform.initialize();
        if (!result) {
            (void)shutdown();
            return Result::failure("Platform initialization failed: " +
                                   result.error());
        }

        inputManager = std::make_shared<InputManager>();
        inputManager->window = platform.window();

        result = inputManager->enterEditorInteractive();
        if (!result) {
            (void)shutdown();
            return Result::failure("Failed to initialize editor input: " +
                                   result.error());
        }

        result = sceneManager_.initialize<Level1>("Level 1", inputManager);
        if (!result) {
            (void)shutdown();
            return Result::failure("Scene Manager initialization failed: " +
                                   result.error());
        }

        sceneManager_.setCurrentScenePath(
            std::filesystem::path(DUNAMIS_SOURCE_DIR) /
            "game/scenes/level_1/level_1.scene.json");
        const std::filesystem::path startupScenePath =
            sceneManager_.currentScenePath();
        std::error_code sceneFileQueryError;
        const bool sceneFileExists = std::filesystem::exists(
            startupScenePath, sceneFileQueryError);
        if (sceneFileQueryError) {
            (void)shutdown();
            return Result::failure(
                "Failed to query startup scene file '" +
                startupScenePath.string() + "': " +
                sceneFileQueryError.message());
        }
        if (sceneFileExists) {
            result = sceneManager_.prepareEditingSceneLoad(startupScenePath);
            if (!result) {
                (void)shutdown();
                return Result::failure("Startup scene restore failed: " +
                                       result.error());
            }
            const auto restoredCamera = sceneManager_.preparedEditorCamera();
            result = sceneManager_.commitPreparedEditingSceneLoad();
            if (!result) {
                (void)shutdown();
                return Result::failure("Startup scene commit failed: " +
                                       result.error());
            }
            sceneManager_.finishEditingSceneLoad();
            if (restoredCamera) {
                result = editorCameraController.restore(*restoredCamera);
                if (!result) {
                    (void)shutdown();
                    return Result::failure("Startup editor camera restore failed: " +
                                           result.error());
                }
            }
            for (const std::string& warning : sceneManager_.persistenceWarnings()) {
                spdlog::warn("{}", warning);
            }
        } else {
            spdlog::warn(
                "Scene file '{}' was not found; using C++ scene defaults",
                startupScenePath.string());
        }

        result = visualServer.initialize(platform.window(),
                                         sceneManager_.editingScene());
        if (!result) {
            (void)shutdown();
            return Result::failure("Visual Server initialization failed: " +
                                   result.error());
        }
        runState = SceneRunState::Editing;
        visualServer.setCurrentScenePath(
            sceneManager_.currentScenePath().string());
        synchronizeImGuiInput();

        initialized = true;
        return Result::success();
    } catch (const std::exception& exception) {
        (void)shutdown();
        return Result::failure("Engine initialization failed: " +
                               std::string(exception.what()));
    } catch (...) {
        (void)shutdown();
        return Result::failure(
            "Engine initialization failed with an unknown error");
    }
}

Result Dunamis::run() {
    if (!initialized) {
        return Result::failure("Dunamis Engine is not initialized");
    }

    spdlog::info("Begin running Dunamis Engine...");
    bool running = true;

    try {
        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                visualServer.processEvent(e);

                if (e.type == SDL_EVENT_QUIT ||
                    e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    requestQuit(running);
                } else if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.key == SDLK_ESCAPE) {
                        requestQuit(running);
                    }
                }

                const bool isAltEvent =
                    (e.type == SDL_EVENT_KEY_DOWN ||
                     e.type == SDL_EVENT_KEY_UP) &&
                    (e.key.key == SDLK_LALT || e.key.key == SDLK_RALT);
                if (!isAltEvent) {
                    inputManager->handleEvent(e);
                }

                if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
                    Result focusResult = Result::success();
                    if (inputManager->inputMode() ==
                        InputMode::EditorCameraCaptured) {
                        focusResult = inputManager->endEditorCameraCapture();
                    } else if (inputManager->inputMode() ==
                               InputMode::GameplayCaptured) {
                        focusResult =
                            inputManager->toggleGameplayMouseRelease();
                    } else {
                        inputManager->clearKeys();
                    }
                    if (!focusResult) {
                        return Result::failure(
                            "Failed to release captured input after focus "
                            "loss: " + focusResult.error());
                    }
                    synchronizeImGuiInput();
                    continue;
                }

                const bool togglesGameplayMouse =
                    e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                    (e.key.key == SDLK_LALT || e.key.key == SDLK_RALT);
                if (togglesGameplayMouse && usesGameplayCamera(runState)) {
                    Result toggleResult =
                        inputManager->toggleGameplayMouseRelease();
                    if (!toggleResult) {
                        return Result::failure(
                            "Failed to toggle gameplay mouse release: " +
                            toggleResult.error());
                    }
                    synchronizeImGuiInput();
                    continue;
                }

                if (editorToolsEnabled(runState) &&
                    e.type == SDL_EVENT_MOUSE_BUTTON_DOWN &&
                    e.button.button == SDL_BUTTON_RIGHT &&
                    inputManager->inputMode() ==
                        InputMode::EditorInteractive &&
                    visualServer.sceneInteractionAreaHovered()) {
                    Result captureResult =
                        inputManager->beginEditorCameraCapture();
                    if (!captureResult) {
                        return Result::failure(
                            "Failed to capture the editor camera: " +
                            captureResult.error());
                    }
                    synchronizeImGuiInput();
                    continue;
                }

                if (editorToolsEnabled(runState) &&
                    e.type == SDL_EVENT_MOUSE_BUTTON_UP &&
                    e.button.button == SDL_BUTTON_RIGHT &&
                    inputManager->inputMode() ==
                        InputMode::EditorCameraCaptured) {
                    Result releaseResult =
                        inputManager->endEditorCameraCapture();
                    if (!releaseResult) {
                        return Result::failure(
                            "Failed to release the editor camera: " +
                            releaseResult.error());
                    }
                    synchronizeImGuiInput();
                }
            }

            if (!running) {
                break;
            }

            if (editorToolsEnabled(runState)) {
                editorCameraController.update(*inputManager);
            }
            if (runtimeSceneRunning(runState)) {
                Scene* runtimeScene = sceneManager_.runtimeScene();
                if (!runtimeScene ||
                    sceneManager_.activeScene() != runtimeScene) {
                    return Result::failure(
                        "Runtime execution requires an active runtime scene");
                }
                runtimeScene->update();
            }

            Scene* activeScene = sceneManager_.activeScene();
            if (!activeScene) {
                return Result::failure("No active scene is available");
            }
            Result result = visualServer.run(
                activeScene, renderCamera(), runState);
            if (!result) {
                return Result::failure("Rendering failed: " + result.error());
            }

            const EditorCommand command =
                visualServer.consumeEditorCommand();
            if (command == EditorCommand::Play) {
                result = beginRuntimeSession(SceneRunState::Playing);
            } else if (command == EditorCommand::Simulate) {
                result = beginRuntimeSession(SceneRunState::Simulating);
            } else if (command == EditorCommand::Stop) {
                result = stopRuntimeSession();
            } else if (command == EditorCommand::SaveScene) {
                result = sceneManager_.saveEditingScene(
                    editorCameraController.camera());
                reportPersistenceResult(result, "Scene saved");
                result = Result::success();
            } else if (command == EditorCommand::SaveSceneAs) {
                pendingSaveAsPath_ = visualServer.requestedSaveAsPath();
                if (runState != SceneRunState::Editing) {
                    reportPersistenceResult(
                        Result::failure("Scene Save As is available only while Editing"),
                        {});
                    pendingSaveAsPath_.clear();
                } else if (pendingSaveAsPath_.empty()) {
                    reportPersistenceResult(
                        Result::failure("Save As path is empty"), {});
                    pendingSaveAsPath_.clear();
                } else {
                    std::error_code pathQueryError;
                    const bool destinationExists = std::filesystem::exists(
                        pendingSaveAsPath_, pathQueryError);
                    if (pathQueryError) {
                        reportPersistenceResult(
                            Result::failure(
                                "Failed to query Save As destination '" +
                                pendingSaveAsPath_.string() + "': " +
                                pathQueryError.message()),
                            {});
                        pendingSaveAsPath_.clear();
                    } else if (destinationExists) {
                        const bool destinationIsDirectory =
                            std::filesystem::is_directory(
                                pendingSaveAsPath_, pathQueryError);
                        if (pathQueryError) {
                            reportPersistenceResult(
                                Result::failure(
                                    "Failed to query Save As destination '" +
                                    pendingSaveAsPath_.string() + "': " +
                                    pathQueryError.message()),
                                {});
                            pendingSaveAsPath_.clear();
                        } else if (destinationIsDirectory) {
                            reportPersistenceResult(
                                Result::failure(
                                    "Save As destination '" +
                                    pendingSaveAsPath_.string() +
                                    "' is an existing directory"),
                                {});
                            pendingSaveAsPath_.clear();
                        } else {
                            visualServer.requestSaveAsOverwriteConfirmation(
                                pendingSaveAsPath_.string());
                        }
                    } else {
                        result = sceneManager_.saveEditingSceneAs(
                            pendingSaveAsPath_,
                            editorCameraController.camera());
                        if (result) {
                            visualServer.setCurrentScenePath(
                                sceneManager_.currentScenePath().string());
                        }
                        reportPersistenceResult(
                            result,
                            "Scene saved as '" +
                                pendingSaveAsPath_.string() + "'");
                        pendingSaveAsPath_.clear();
                    }
                }
                result = Result::success();
            } else if (command == EditorCommand::ConfirmSaveSceneAsOverwrite) {
                if (runState != SceneRunState::Editing) {
                    reportPersistenceResult(
                        Result::failure("Scene Save As is available only while Editing"),
                        {});
                } else if (pendingSaveAsPath_.empty()) {
                    reportPersistenceResult(
                        Result::failure("No Save As destination is pending"),
                        {});
                } else {
                    result = sceneManager_.saveEditingSceneAs(
                        pendingSaveAsPath_, editorCameraController.camera());
                    if (result) {
                        visualServer.setCurrentScenePath(
                            sceneManager_.currentScenePath().string());
                    }
                    reportPersistenceResult(
                        result,
                        "Scene saved as '" + pendingSaveAsPath_.string() + "'");
                    pendingSaveAsPath_.clear();
                }
                result = Result::success();
            } else if (command == EditorCommand::CancelSaveSceneAs) {
                pendingSaveAsPath_.clear();
                result = Result::success();
            } else if (command == EditorCommand::LoadScene) {
                pendingLoadPath_ = visualServer.requestedScenePath();
                if (sceneManager_.hasUnsavedChanges()) {
                    visualServer.requestLoadConfirmation();
                    result = Result::success();
                } else {
                    result = loadEditingScene(pendingLoadPath_);
                    reportPersistenceResult(result, "Scene loaded");
                    if (result) pendingLoadPath_.clear();
                    result = Result::success();
                }
            } else if (command == EditorCommand::SaveAndLoad) {
                result = sceneManager_.saveEditingScene(
                    editorCameraController.camera());
                if (result) result = loadEditingScene(pendingLoadPath_);
                reportPersistenceResult(result, "Scene saved and loaded");
                if (result) pendingLoadPath_.clear();
                result = Result::success();
            } else if (command == EditorCommand::DiscardAndLoad) {
                result = loadEditingScene(pendingLoadPath_);
                reportPersistenceResult(result, "Scene loaded");
                if (result) pendingLoadPath_.clear();
                result = Result::success();
            } else if (command == EditorCommand::SaveAndQuit) {
                result = sceneManager_.saveEditingScene(
                    editorCameraController.camera());
                if (result) running = false;
                else {
                    quitConfirmationPending_ = false;
                    reportPersistenceResult(result, {});
                }
                result = Result::success();
            } else if (command == EditorCommand::DiscardAndQuit) {
                running = false;
                result = Result::success();
            } else if (command == EditorCommand::Cancel) {
                pendingLoadPath_.clear();
                quitConfirmationPending_ = false;
                result = Result::success();
            }
            if (!result) {
                return Result::failure(
                    "Editor lifecycle transition failed: " + result.error());
            }

            inputManager->clearTransientInput();
        }
    } catch (const std::exception& exception) {
        return Result::failure("Engine run failed: " +
                               std::string(exception.what()));
    } catch (...) {
        return Result::failure("Engine run failed with an unknown error");
    }

    spdlog::info("Shutting down Dunamis Engine...");
    return Result::success();
}

Result Dunamis::loadEditingScene(const std::filesystem::path& path) {
    if (runState != SceneRunState::Editing) {
        return Result::failure("Scene loading is available only while Editing");
    }
    Result result = sceneManager_.prepareEditingSceneLoad(path);
    if (!result) return result;
    Scene* candidate = sceneManager_.preparedEditingScene();
    const auto restoredCamera = sceneManager_.preparedEditorCamera();
    Scene* oldScene = sceneManager_.editingScene();

    result = visualServer.loadSceneResources(candidate);
    if (!result) {
        sceneManager_.cancelPreparedEditingSceneLoad();
        return Result::failure("Failed to prepare loaded scene rendering: " + result.error());
    }
    result = visualServer.switchScene(candidate);
    if (!result) {
        (void)visualServer.unloadSceneResources(candidate);
        sceneManager_.cancelPreparedEditingSceneLoad();
        return Result::failure("Failed to switch to loaded scene: " + result.error());
    }
    result = visualServer.unloadSceneResources(oldScene);
    if (!result) {
        (void)visualServer.switchScene(oldScene);
        (void)visualServer.unloadSceneResources(candidate);
        sceneManager_.cancelPreparedEditingSceneLoad();
        return Result::failure(
            "Failed to retire old scene rendering; load was rolled back: " +
            result.error());
    }
    result = sceneManager_.commitPreparedEditingSceneLoad();
    if (!result) {
        (void)visualServer.loadSceneResources(oldScene);
        (void)visualServer.switchScene(oldScene);
        (void)visualServer.unloadSceneResources(candidate);
        sceneManager_.cancelPreparedEditingSceneLoad();
        return result;
    }
    if (restoredCamera) {
        result = editorCameraController.restore(*restoredCamera);
        if (!result) return Result::failure("Loaded scene committed but editor camera restore failed: " + result.error());
    }
    sceneManager_.finishEditingSceneLoad();
    visualServer.setCurrentScenePath(sceneManager_.currentScenePath().string());
    for (const std::string& warning : sceneManager_.persistenceWarnings()) {
        spdlog::warn("{}", warning);
    }
    return Result::success();
}

void Dunamis::requestQuit(bool& running) {
    if (quitConfirmationPending_) return;
    if (!sceneManager_.hasUnsavedChanges()) {
        running = false;
        return;
    }
    quitConfirmationPending_ = true;
    if (usesGameplayCamera(runState) && inputManager &&
        inputManager->inputMode() == InputMode::GameplayCaptured) {
        const Result release = inputManager->toggleGameplayMouseRelease();
        if (!release) {
            spdlog::error("Failed to release gameplay input for quit prompt: {}",
                          release.error());
        }
        synchronizeImGuiInput();
    }
    visualServer.requestQuitConfirmation();
}

void Dunamis::reportPersistenceResult(const Result& result,
                                      const std::string& successMessage) {
    if (!result) {
        spdlog::error("Scene persistence failed: {}", result.error());
    } else if (!successMessage.empty()) {
        spdlog::info("{}", successMessage);
    }
}

Result Dunamis::beginRuntimeSession(SceneRunState targetState) {
    if (runState != SceneRunState::Editing) {
        return Result::failure(
            "A runtime session can begin only while editing");
    }
    if (!runtimeSceneRunning(targetState)) {
        return Result::failure("Invalid runtime session target state");
    }

    visualServer.clearEditorSelection();
    const auto rollbackRuntimeSession = [this](const std::string& error) {
        runState = SceneRunState::Editing;
        Scene* runtimeScene = sceneManager_.runtimeScene();
        if (runtimeScene && visualServer.renderScene() == runtimeScene) {
            const Result switchResult = visualServer.switchScene(
                sceneManager_.editingScene());
            if (!switchResult) {
                synchronizeImGuiInput();
                return Result::failure(
                    error + "; failed to restore editing rendering: " +
                    switchResult.error());
            }
        }
        if (sceneManager_.isRuntimeSceneActive()) {
            const Result sceneResult = sceneManager_.returnToEditingScene();
            if (!sceneResult) {
                synchronizeImGuiInput();
                return Result::failure(
                    error + "; failed to restore editing scene ownership: " +
                    sceneResult.error());
            }
        }
        if (runtimeScene) {
            const Result unloadResult =
                visualServer.unloadSceneResources(runtimeScene);
            if (!unloadResult) {
                synchronizeImGuiInput();
                return Result::failure(
                    error + "; failed to unload runtime rendering: " +
                    unloadResult.error());
            }
        }
        sceneManager_.cancelPreparedRuntimeScene();
        const Result restoreResult = inputManager->enterEditorInteractive();
        synchronizeImGuiInput();
        if (!restoreResult) {
            return Result::failure(
                error + "; failed to restore editor input: " +
                restoreResult.error());
        }
        return Result::failure(error);
    };

    Result result = sceneManager_.prepareRuntimeScene();
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to prepare runtime scene: " + result.error());
    }

    Scene* runtimeScene = sceneManager_.runtimeScene();
    result = visualServer.loadSceneResources(runtimeScene);
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to prepare runtime rendering: " + result.error());
    }

    if (targetState == SceneRunState::Playing) {
        result = inputManager->beginGameplaySession();
    } else {
        result = inputManager->enterEditorInteractive();
    }
    if (!result) {
        return rollbackRuntimeSession(
            std::string(targetState == SceneRunState::Playing
                            ? "Failed to enter gameplay input: "
                            : "Failed to enter editor input for simulation: ") +
            result.error());
    }

    synchronizeImGuiInput();
    result = visualServer.switchScene(runtimeScene);
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to switch rendering to the runtime scene: " +
            result.error());
    }

    result = sceneManager_.commitRuntimeScene();
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to commit the runtime scene: " + result.error());
    }

    try {
        runtimeScene->start();
    } catch (const std::exception& exception) {
        return rollbackRuntimeSession("Failed to start runtime scene: " +
                                      std::string(exception.what()));
    } catch (...) {
        return rollbackRuntimeSession(
            "Failed to start runtime scene with an unknown error");
    }

    runState = targetState;
    synchronizeImGuiInput();
    return Result::success();
}

Result Dunamis::stopRuntimeSession() {
    if (!runtimeSceneRunning(runState)) {
        return Result::failure("A runtime session can stop only while running");
    }

    Result result = inputManager->enterEditorInteractive();
    if (!result) {
        synchronizeImGuiInput();
        return Result::failure("Failed to restore editor input: " +
                               result.error());
    }

    visualServer.clearEditorSelection();
    Scene* runtimeScene = sceneManager_.runtimeScene();
    result = visualServer.switchScene(sceneManager_.editingScene());
    if (!result) {
        synchronizeImGuiInput();
        return Result::failure(
            "Failed to switch rendering to the editing scene: " +
            result.error());
    }

    result = sceneManager_.returnToEditingScene();
    if (!result) {
        const Result rendererRestore = visualServer.switchScene(runtimeScene);
        synchronizeImGuiInput();
        if (!rendererRestore) {
            return Result::failure(
                "Failed to return to the editing scene: " + result.error() +
                "; failed to restore runtime rendering: " +
                rendererRestore.error());
        }
        return Result::failure("Failed to return to the editing scene: " +
                               result.error());
    }

    runState = SceneRunState::Editing;
    result = visualServer.unloadSceneResources(runtimeScene);
    if (!result) {
        synchronizeImGuiInput();
        return Result::failure(
            "Failed to destroy runtime rendering resources: " +
            result.error());
    }
    result = sceneManager_.destroyRuntimeScene();
    if (!result) {
        synchronizeImGuiInput();
        return Result::failure("Failed to destroy runtime scene: " +
                               result.error());
    }

    synchronizeImGuiInput();
    return Result::success();
}

const Camera& Dunamis::renderCamera() const noexcept {
    if (usesGameplayCamera(runState)) {
        if (const Scene* runtimeScene = sceneManager_.runtimeScene()) {
            if (const Camera* camera = runtimeScene->activeCamera()) {
                return *camera;
            }
        }
    }
    return editorCameraController.camera();
}

void Dunamis::synchronizeImGuiInput() noexcept {
    if (inputManager) {
        visualServer.setImGuiInputEnabled(
            inputManager->imguiInputEnabled());
    }
}

bool Dunamis::shutdown() noexcept {
    bool inputReleased = true;
    if (inputManager) {
        const Result result = inputManager->enterEditorInteractive();
        if (!result) {
            inputReleased = false;
            spdlog::error("Failed to release input during shutdown: {}",
                          result.error());
        } else {
            synchronizeImGuiInput();
        }
        inputManager->clearKeys();
    }
    const bool rendererShutdown = visualServer.shutdown();
    if (!rendererShutdown || !inputReleased) {
        return false;
    }

    if (inputManager) {
        inputManager->window = nullptr;
    }
    sceneManager_.shutdown();
    inputManager.reset();

    platform.shutdown();
    runState = SceneRunState::Editing;
    initialized = false;
    pendingLoadPath_.clear();
    pendingSaveAsPath_.clear();
    quitConfirmationPending_ = false;
    return true;
}

Dunamis::~Dunamis() noexcept {
    constexpr int shutdownAttempts = 3;
    for (int attempt = 0; attempt < shutdownAttempts; ++attempt) {
        if (shutdown()) {
            return;
        }
    }

    try {
        spdlog::critical(
            "Vulkan work did not become idle during final shutdown; retaining "
            "the SDL window and Vulkan resources for process teardown");
    } catch (...) {
        // Destructors must not throw while preserving the fatal cleanup state.
    }
    platform.abandon();
}
