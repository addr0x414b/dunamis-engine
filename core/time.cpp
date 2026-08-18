#include "time.h"

#include <chrono>
#include <cmath>

namespace {

using Clock = std::chrono::steady_clock;

constexpr float maxDeltaTime = 0.1f;

Clock::time_point previousFrameTime{};
float currentDeltaTime = 0.0f;
bool hasPreviousFrameTime = false;

}  // namespace

float Time::deltaTime() noexcept { return currentDeltaTime; }

void Time::initialize() noexcept {
    previousFrameTime = Clock::now();
    currentDeltaTime = 0.0f;
    hasPreviousFrameTime = false;
}

void Time::update() noexcept {
    const Clock::time_point currentFrameTime = Clock::now();
    if (!hasPreviousFrameTime) {
        previousFrameTime = currentFrameTime;
        hasPreviousFrameTime = true;
        currentDeltaTime = 0.0f;
        return;
    }

    const float measuredDeltaTime =
        std::chrono::duration<float>(currentFrameTime - previousFrameTime)
            .count();
    previousFrameTime = currentFrameTime;
    currentDeltaTime = normalizeDeltaTime(measuredDeltaTime);
}

float Time::normalizeDeltaTime(float measuredDeltaTime) noexcept {
    if (!std::isfinite(measuredDeltaTime) || measuredDeltaTime <= 0.0f) {
        return 0.0f;
    }
    if (measuredDeltaTime > maxDeltaTime) {
        return maxDeltaTime;
    }
    return measuredDeltaTime;
}
