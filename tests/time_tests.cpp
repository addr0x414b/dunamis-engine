#include "../core/time.h"

#include <cmath>
#include <iostream>
#include <limits>

class TimeTestAccess {
public:
    static float normalize(float measuredDeltaTime) noexcept {
        return Time::normalizeDeltaTime(measuredDeltaTime);
    }
};

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        return false;
    }
    return true;
}

bool approximatelyEqual(float first, float second, float tolerance = 1.0e-6f) {
    return std::fabs(first - second) <= tolerance;
}

bool runInitialStateTests() {
    return expect(Time::deltaTime() == 0.0f,
                  "Time delta was not zero before the first frame");
}

bool runNormalizationTests() {
    constexpr float maxDeltaTime = 0.1f;
    bool passed = true;
    passed &= expect(approximatelyEqual(TimeTestAccess::normalize(0.016f),
                                        0.016f),
                     "Normal frame delta was changed");
    passed &= expect(approximatelyEqual(TimeTestAccess::normalize(0.05f),
                                        0.05f),
                     "Long valid frame delta was changed");
    passed &= expect(approximatelyEqual(TimeTestAccess::normalize(0.004f),
                                        0.004f),
                     "Small valid frame delta was artificially increased");
    passed &= expect(TimeTestAccess::normalize(0.5f) == maxDeltaTime,
                     "Large frame delta was not clamped");
    passed &= expect(TimeTestAccess::normalize(-1.0f) == 0.0f,
                     "Negative frame delta was accepted");
    passed &= expect(TimeTestAccess::normalize(
                         std::numeric_limits<float>::quiet_NaN()) == 0.0f,
                     "NaN frame delta was accepted");
    passed &= expect(TimeTestAccess::normalize(
                         std::numeric_limits<float>::infinity()) == 0.0f,
                     "Infinite frame delta was accepted");
    return passed;
}

float distanceAtFrameRate(int frameCount) {
    constexpr float speedUnitsPerSecond = 10.0f;
    const float frameDelta = 1.0f / static_cast<float>(frameCount);
    float distance = 0.0f;
    for (int frame = 0; frame < frameCount; ++frame) {
        distance += speedUnitsPerSecond * frameDelta;
    }
    return distance;
}

bool runFrameRateIndependenceTests() {
    const float distanceAt60Hz = distanceAtFrameRate(60);
    const float distanceAt144Hz = distanceAtFrameRate(144);
    return expect(approximatelyEqual(distanceAt60Hz, 10.0f, 1.0e-4f) &&
                      approximatelyEqual(distanceAt144Hz, 10.0f, 1.0e-4f) &&
                      approximatelyEqual(distanceAt60Hz, distanceAt144Hz,
                                         1.0e-4f),
                  "Rate-based movement was not frame-rate independent");
}

}  // namespace

int main() {
    return runInitialStateTests() && runNormalizationTests() &&
                   runFrameRateIndependenceTests()
               ? 0
               : 1;
}
