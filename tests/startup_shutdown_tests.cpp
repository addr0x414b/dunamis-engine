#include "core/platform.h"
#include "core/result.h"
#include "rendering/vulkan_context.h"

#include <SDL3/SDL.h>

#include <iostream>
#include <string>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

}  // namespace

int main() {
    bool passed = true;

    const Result success = Result::success();
    const Result failure = Result::failure("expected failure");
    passed &= expect(static_cast<bool>(success),
                     "A successful Result evaluated as failure");
    passed &= expect(!static_cast<bool>(failure),
                     "A failed Result evaluated as success");
    passed &= expect(failure.error() == "expected failure",
                     "A failed Result did not preserve its error");

    {
        VulkanContext context;
        const Result result = context.init(nullptr, nullptr);
        passed &= expect(!static_cast<bool>(result),
                         "VulkanContext accepted a null SDL window");

        passed &= expect(context.cleanup(),
                         "Virgin Vulkan cleanup did not complete");
        passed &= expect(context.cleanup(),
                         "Repeated virgin Vulkan cleanup did not complete");
    }

    passed &= expect(
        SDL_SetHintWithPriority(SDL_HINT_VIDEO_DRIVER, "dummy",
                                SDL_HINT_OVERRIDE),
        "Failed to select SDL's dummy video driver");
    {
        Platform platform;
        const Result result = platform.initialize();
        passed &= expect(!static_cast<bool>(result),
                         "Platform unexpectedly created a Vulkan window with "
                         "the dummy SDL driver");
        passed &= expect(platform.window() == nullptr,
                         "Platform retained a window after failed startup");
        passed &= expect(result.error().find("Failed to create") !=
                             std::string::npos,
                         "Platform did not reach the expected window-creation "
                         "failure");
        passed &= expect(SDL_WasInit(SDL_INIT_VIDEO) == 0,
                         "Platform retained SDL video after failed startup");

        platform.shutdown();
        platform.shutdown();
    }

    return passed ? 0 : 1;
}
