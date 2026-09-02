#include "dunamis.h"

#include "../game/level_1.h"

#include "time.h"

#include "spdlog/spdlog.h"

#include <chrono>
#include <exception>
#include <filesystem>
#include <optional>
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

    spdlog::info("Dunamis Engine v0.0.3");

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

        sceneManager_.setCurrentScenePath(
            std::filesystem::path(DUNAMIS_SOURCE_DIR) /
            "game/scenes/level_1/level_1.scene.json");
        result = sceneManager_.initialize<Level1>("Level 1", inputManager);
        if (!result) {
            (void)shutdown();
            return Result::failure("Scene Manager initialization failed: " +
                                   result.error());
        }

        result = physicsServer_.initialize();
        if (!result) {
            (void)shutdown();
            return Result::failure("Physics Server initialization failed: " +
                                   result.error());
        }

        const auto& restoredCamera = sceneManager_.editingEditorCamera();
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

        result = visualServer.initialize(platform.window(),
                                         sceneManager_.editingScene(),
                                         editorSession_,
                                         &physicsServer_);
        if (!result) {
            (void)shutdown();
            return Result::failure("Visual Server initialization failed: " +
                                   result.error());
        }
        editorSession_.setRenderColliderIds(sceneManager_.editorRenderColliders());
        editorSession_.setRunState(SceneRunState::Editing);
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
        Time::initialize();
        while (running) {
            Time::update();

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
                if (togglesGameplayMouse &&
                    usesGameplayCamera(editorSession_.runState())) {
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

                if (editorToolsEnabled(editorSession_.runState()) &&
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

                if (editorToolsEnabled(editorSession_.runState()) &&
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

            if (editorToolsEnabled(editorSession_.runState())) {
                editorCameraController.update(*inputManager);
            }
            if (runtimeSceneRunning(editorSession_.runState())) {
                Scene* runtimeScene = sceneManager_.runtimeScene();
                if (!runtimeScene ||
                    sceneManager_.activeScene() != runtimeScene) {
                    return Result::failure(
                        "Runtime execution requires an active runtime scene");
                }
                if (const std::optional<RuntimeTransformEdit> edit =
                        editorSession_.consumeRuntimeTransformEdit()) {
                    if (edit->object != nullptr) {
                        physicsServer_.applyRuntimeTransform(
                            *edit->object, edit->position, edit->rotation,
                            edit->manipulating);
                    }
                }
                runtimeScene->update();
                physicsServer_.update();
            }

            Scene* activeScene = sceneManager_.activeScene();
            if (!activeScene) {
                return Result::failure("No active scene is available");
            }
            Result result = visualServer.run(
                activeScene, renderCamera(), editorSession_.runState());
            if (!result) {
                return Result::failure("Rendering failed: " + result.error());
            }

            const EditorAction action = editorSession_.consumeEditorAction();
            const EditorCommand command = action.command;
            const auto captureColliderVisibility = [&] {
                sceneManager_.setEditorRenderColliders(
                    editorSession_.renderColliderIds());
            };
            if (command == EditorCommand::Play) {
                result = beginRuntimeSession(SceneRunState::Playing);
            } else if (command == EditorCommand::Simulate) {
                result = beginRuntimeSession(SceneRunState::Simulating);
            } else if (command == EditorCommand::Stop) {
                result = stopRuntimeSession();
            } else if (command == EditorCommand::SaveScene) {
                captureColliderVisibility();
                result = sceneManager_.saveEditingScene(
                    editorCameraController.camera());
                reportPersistenceResult(result, "Scene saved");
                result = Result::success();
            } else if (command == EditorCommand::SaveSceneAs) {
                editorSession_.clearPendingSaveAsPath();
                if (editorSession_.runState() != SceneRunState::Editing) {
                    reportPersistenceResult(
                        Result::failure("Scene Save As is available only while Editing"),
                        {});
                } else if (action.path.empty()) {
                    reportPersistenceResult(
                        Result::failure("Save As path is empty"), {});
                } else {
                    std::error_code pathQueryError;
                    const bool destinationExists = std::filesystem::exists(
                        action.path, pathQueryError);
                    if (pathQueryError) {
                        reportPersistenceResult(
                            Result::failure(
                                "Failed to query Save As destination '" +
                                action.path.string() + "': " +
                                pathQueryError.message()),
                            {});
                    } else if (destinationExists) {
                        const bool destinationIsDirectory =
                            std::filesystem::is_directory(
                                action.path, pathQueryError);
                        if (pathQueryError) {
                            reportPersistenceResult(
                                Result::failure(
                                    "Failed to query Save As destination '" +
                                    action.path.string() + "': " +
                                    pathQueryError.message()),
                                {});
                        } else if (destinationIsDirectory) {
                            reportPersistenceResult(
                                Result::failure(
                                    "Save As destination '" +
                                    action.path.string() +
                                    "' is an existing directory"),
                                {});
                        } else {
                            editorSession_.setPendingSaveAsPath(action.path);
                            visualServer.requestSaveAsOverwriteConfirmation(
                                action.path.string());
                        }
                    } else {
                        captureColliderVisibility();
                        result = sceneManager_.saveEditingSceneAs(
                            action.path,
                            editorCameraController.camera());
                        if (result) {
                            visualServer.setCurrentScenePath(
                                sceneManager_.currentScenePath().string());
                        }
                        reportPersistenceResult(
                            result,
                            "Scene saved as '" +
                                action.path.string() + "'");
                    }
                }
                result = Result::success();
            } else if (command == EditorCommand::ConfirmSaveSceneAsOverwrite) {
                const std::filesystem::path pendingSaveAsPath =
                    editorSession_.pendingSaveAsPath();
                if (editorSession_.runState() != SceneRunState::Editing) {
                    reportPersistenceResult(
                        Result::failure("Scene Save As is available only while Editing"),
                        {});
                } else if (pendingSaveAsPath.empty()) {
                    reportPersistenceResult(
                        Result::failure("No Save As destination is pending"),
                        {});
                } else {
                    captureColliderVisibility();
                    result = sceneManager_.saveEditingSceneAs(
                        pendingSaveAsPath, editorCameraController.camera());
                    if (result) {
                        visualServer.setCurrentScenePath(
                            sceneManager_.currentScenePath().string());
                    }
                    reportPersistenceResult(
                        result,
                        "Scene saved as '" + pendingSaveAsPath.string() + "'");
                }
                editorSession_.clearPendingSaveAsPath();
                result = Result::success();
            } else if (command == EditorCommand::CancelSaveSceneAs) {
                editorSession_.clearPendingSaveAsPath();
                result = Result::success();
            } else if (command == EditorCommand::LoadScene) {
                if (sceneManager_.hasUnsavedChanges()) {
                    editorSession_.setPendingLoadPath(action.path);
                    visualServer.requestLoadConfirmation();
                    result = Result::success();
                } else {
                    editorSession_.clearPendingLoadPath();
                    result = loadEditingScene(action.path);
                    reportPersistenceResult(result, "Scene loaded");
                    result = Result::success();
                }
            } else if (command == EditorCommand::SaveAndLoad) {
                const std::filesystem::path pendingLoadPath =
                    editorSession_.pendingLoadPath();
                captureColliderVisibility();
                result = sceneManager_.saveEditingScene(
                    editorCameraController.camera());
                if (result) result = loadEditingScene(pendingLoadPath);
                reportPersistenceResult(result, "Scene saved and loaded");
                if (result) editorSession_.clearPendingLoadPath();
                result = Result::success();
            } else if (command == EditorCommand::DiscardAndLoad) {
                const std::filesystem::path pendingLoadPath =
                    editorSession_.pendingLoadPath();
                result = loadEditingScene(pendingLoadPath);
                reportPersistenceResult(result, "Scene loaded");
                if (result) editorSession_.clearPendingLoadPath();
                result = Result::success();
            } else if (command == EditorCommand::SaveAndQuit) {
                captureColliderVisibility();
                result = sceneManager_.saveEditingScene(
                    editorCameraController.camera());
                if (result) running = false;
                else {
                    editorSession_.setQuitConfirmationPending(false);
                    reportPersistenceResult(result, {});
                }
                result = Result::success();
            } else if (command == EditorCommand::DiscardAndQuit) {
                running = false;
                result = Result::success();
            } else if (command == EditorCommand::Cancel) {
                editorSession_.clearPendingLoadPath();
                editorSession_.setQuitConfirmationPending(false);
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
    if (editorSession_.runState() != SceneRunState::Editing) {
        return Result::failure("Scene loading is available only while Editing");
    }
    Result result = sceneManager_.prepareEditingSceneLoad(path);
    if (!result) return result;
    Scene* candidate = sceneManager_.preparedEditingScene();
    const auto restoredCamera = sceneManager_.preparedEditorCamera();
    const auto restoredColliderIds = sceneManager_.preparedRenderColliders();
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
    editorSession_.setRenderColliderIds(restoredColliderIds);
    visualServer.setCurrentScenePath(sceneManager_.currentScenePath().string());
    for (const std::string& warning : sceneManager_.persistenceWarnings()) {
        spdlog::warn("{}", warning);
    }
    return Result::success();
}

void Dunamis::requestQuit(bool& running) {
    if (editorSession_.quitConfirmationPending()) return;
    if (!sceneManager_.hasUnsavedChanges()) {
        running = false;
        return;
    }
    editorSession_.setQuitConfirmationPending(true);
    if (usesGameplayCamera(editorSession_.runState()) && inputManager &&
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
    if (editorSession_.runState() != SceneRunState::Editing) {
        return Result::failure(
            "A runtime session can begin only while editing");
    }
    if (!runtimeSceneRunning(targetState)) {
        return Result::failure("Invalid runtime session target state");
    }

    using Clock = std::chrono::steady_clock;
    const Clock::time_point totalStart = Clock::now();
    Clock::duration scenePreparation{};
    Clock::duration physicsCreation{};
    Clock::duration renderingResources{};
    Clock::duration inputTransition{};
    Clock::duration sceneSwitch{};
    Clock::duration sceneCommit{};
    Clock::duration runtimeStart{};
    visualServer.clearEditorSelection();
    const auto rollbackRuntimeSession = [this](const std::string& error) {
        editorSession_.setRunState(SceneRunState::Editing);
        Scene* runtimeScene = sceneManager_.runtimeScene();
        // Runtime bodies retain GameObject pointers; clear them before this
        // transactional path can release the disposable runtime Scene.
        physicsServer_.endRuntimeSession();
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

    Clock::time_point stageStart = Clock::now();
    Result result = sceneManager_.prepareRuntimeScene();
    scenePreparation = Clock::now() - stageStart;
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to prepare runtime scene: " + result.error());
    }

    Scene* runtimeScene = sceneManager_.runtimeScene();
    stageStart = Clock::now();
    result = physicsServer_.beginRuntimeSession(*runtimeScene);
    physicsCreation = Clock::now() - stageStart;
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to prepare runtime physics: " + result.error());
    }

    stageStart = Clock::now();
    result = visualServer.loadSceneResources(runtimeScene);
    renderingResources = Clock::now() - stageStart;
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to prepare runtime rendering: " + result.error());
    }

    stageStart = Clock::now();
    if (targetState == SceneRunState::Playing) {
        result = inputManager->beginGameplaySession();
    } else {
        result = inputManager->enterEditorInteractive();
    }
    inputTransition = Clock::now() - stageStart;
    if (!result) {
        return rollbackRuntimeSession(
            std::string(targetState == SceneRunState::Playing
                            ? "Failed to enter gameplay input: "
                            : "Failed to enter editor input for simulation: ") +
            result.error());
    }

    synchronizeImGuiInput();
    stageStart = Clock::now();
    result = visualServer.switchScene(runtimeScene);
    sceneSwitch = Clock::now() - stageStart;
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to switch rendering to the runtime scene: " +
            result.error());
    }

    stageStart = Clock::now();
    result = sceneManager_.commitRuntimeScene();
    sceneCommit = Clock::now() - stageStart;
    if (!result) {
        return rollbackRuntimeSession(
            "Failed to commit the runtime scene: " + result.error());
    }

    stageStart = Clock::now();
    try {
        runtimeScene->start();
    } catch (const std::exception& exception) {
        return rollbackRuntimeSession("Failed to start runtime scene: " +
                                      std::string(exception.what()));
    } catch (...) {
        return rollbackRuntimeSession(
            "Failed to start runtime scene with an unknown error");
    }
    runtimeStart = Clock::now() - stageStart;

    editorSession_.setRunState(targetState);
    synchronizeImGuiInput();
    const auto asMilliseconds = [](Clock::duration duration) {
        return std::chrono::duration<double, std::milli>(duration).count();
    };
    spdlog::info("Runtime startup: scene preparation {:.2f} ms, physics creation {:.2f} ms, rendering resources {:.2f} ms, input transition {:.2f} ms, scene switch {:.2f} ms, scene commit {:.2f} ms, runtime start {:.2f} ms, total {:.2f} ms",
                 asMilliseconds(scenePreparation), asMilliseconds(physicsCreation),
                 asMilliseconds(renderingResources), asMilliseconds(inputTransition),
                 asMilliseconds(sceneSwitch), asMilliseconds(sceneCommit),
                 asMilliseconds(runtimeStart),
                 asMilliseconds(Clock::now() - totalStart));
    return Result::success();
}

Result Dunamis::stopRuntimeSession() {
    if (!runtimeSceneRunning(editorSession_.runState())) {
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

    editorSession_.setRunState(SceneRunState::Editing);
    // The runtime scene remains alive until after renderer resources unload.
    // Release its Jolt body-to-GameObject mappings first.
    physicsServer_.endRuntimeSession();
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
    if (usesGameplayCamera(editorSession_.runState())) {
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
    // This is harmless for Editing/partial initialization and guarantees no
    // physics mapping survives the later SceneManager teardown.
    physicsServer_.endRuntimeSession();
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
    physicsServer_.shutdown();
    inputManager.reset();

    platform.shutdown();
    editorSession_.setRunState(SceneRunState::Editing);
    editorSession_.clearPendingLoadPath();
    editorSession_.clearPendingSaveAsPath();
    editorSession_.setQuitConfirmationPending(false);
    initialized = false;
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
