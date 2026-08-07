#include "dunamis.h"

#include "../game/level_1.h"

#include "spdlog/spdlog.h"

#include <exception>
#include <string>

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

        result = visualServer.initialize(platform.window(),
                                         sceneManager_.editingScene());
        if (!result) {
            (void)shutdown();
            return Result::failure("Visual Server initialization failed: " +
                                   result.error());
        }
        runState = SceneRunState::Editing;
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
                    running = false;
                } else if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.key == SDLK_ESCAPE) {
                        running = false;
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
