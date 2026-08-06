#include "dunamis.h"

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

        // Initialize the scene. Eventually replace with a scene manager.
        level1.name = "Level 1";
        level1.init();

        inputManager = std::make_shared<InputManager>();
        inputManager->window = platform.window();

        result = inputManager->setInputMode(InputMode::GameplayCaptured);
        if (!result) {
            (void)shutdown();
            return Result::failure("Failed to initialize gameplay input: " +
                                   result.error());
        }

        level1.inputManager = inputManager;

        result = visualServer.initialize(platform.window(), &level1);
        if (!result) {
            (void)shutdown();
            return Result::failure("Visual Server initialization failed: " +
                                   result.error());
        }
        visualServer.setInputMode(inputManager->inputMode());

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
        level1.start();

        while (running) {
            SDL_Event e;
            while (SDL_PollEvent(&e)) {
                const bool togglesInputMode =
                    e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
                    e.key.key == SDLK_LALT;
                if (togglesInputMode) {
                    inputManager->toggleInputMode();
                    visualServer.setInputMode(inputManager->inputMode());
                    continue;
                }

                visualServer.processEvent(e);

                if (e.type == SDL_EVENT_QUIT ||
                    e.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                    running = false;
                } else if (e.type == SDL_EVENT_KEY_DOWN) {
                    if (e.key.key == SDLK_ESCAPE) {
                        running = false;
                    }
                }

                inputManager->handleEvent(e);
            }
            level1.update();

            Result result = visualServer.run();
            if (!result) {
                return Result::failure("Rendering failed: " + result.error());
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

bool Dunamis::shutdown() noexcept {
    if (!visualServer.shutdown()) {
        return false;
    }

    if (inputManager) {
        inputManager->window = nullptr;
    }
    level1.inputManager.reset();
    inputManager.reset();

    platform.shutdown();
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
