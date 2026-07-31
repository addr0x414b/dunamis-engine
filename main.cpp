#include "core/dunamis.h"

#include "spdlog/spdlog.h"

#include <cstdlib>
#include <exception>

int main() {
    try {
        Dunamis engine;

        const Result initializationResult = engine.initialize();
        if (!initializationResult) {
            spdlog::critical("Dunamis Engine initialization failed: {}",
                             initializationResult.error());
            return EXIT_FAILURE;
        }

        const Result runResult = engine.run();
        const bool shutdownSucceeded = engine.shutdown();
        if (!runResult) {
            spdlog::critical("Dunamis Engine stopped due to an error: {}",
                             runResult.error());
        }
        if (!shutdownSucceeded) {
            spdlog::critical(
                "Dunamis Engine could not complete Vulkan shutdown");
        }

        if (!runResult || !shutdownSucceeded) {
            return EXIT_FAILURE;
        }

        return EXIT_SUCCESS;
    } catch (const std::exception& exception) {
        spdlog::critical("Dunamis Engine terminated with an exception: {}",
                         exception.what());
        return EXIT_FAILURE;
    } catch (...) {
        spdlog::critical(
            "Dunamis Engine terminated with an unknown exception");
        return EXIT_FAILURE;
    }
}
