#include "scene/camera.h"

#include <cmath>
#include <iostream>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>

namespace {

bool expect(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
    }
    return condition;
}

bool sameVector(const glm::vec3& first, const glm::vec3& second) {
    return glm::length(first - second) < 1.0e-5f;
}

bool isFiniteVector(const glm::vec3& value) {
    return std::isfinite(value.x) && std::isfinite(value.y) &&
           std::isfinite(value.z);
}

bool isNormalized(const glm::vec3& value) {
    return isFiniteVector(value) &&
           std::fabs(glm::length(value) - 1.0f) < 1.0e-5f;
}

bool hasValidBasis(const Camera& camera) {
    const glm::vec3 cross = glm::cross(camera.front, camera.up);
    return isFiniteVector(cross) && glm::length(cross) > 1.0e-4f;
}

bool runDerivationTests() {
    bool passed = true;
    Camera camera;

    double yaw = 0.0;
    double pitch = 0.0;
    passed &= expect(camera.deriveYawPitchDegrees(yaw, pitch) &&
                         std::fabs(yaw + 90.0) < 1.0e-5 &&
                         std::fabs(pitch) < 1.0e-5,
                     "Default Camera did not derive yaw=-90, pitch=0");

    const glm::vec3 orientations[] = {
        glm::normalize(glm::vec3(0.98f, 0.1f, 0.15f)),
        glm::normalize(glm::vec3(-0.98f, -0.1f, 0.15f)),
        glm::normalize(glm::vec3(0.25f, 0.8f, -0.45f)),
        glm::normalize(glm::vec3(-0.25f, -0.8f, -0.45f)),
        glm::normalize(glm::vec3(0.01f, 0.9999f, -0.02f)),
        glm::normalize(glm::vec3(-0.01f, -0.9999f, 0.02f)),
    };

    for (const glm::vec3& orientation : orientations) {
        camera.front = orientation;
        passed &= expect(camera.deriveYawPitchDegrees(yaw, pitch) &&
                             std::isfinite(yaw) && std::isfinite(pitch),
                         "Camera failed to derive finite angles");
    }

    Camera invalidCamera;
    invalidCamera.front = glm::vec3(0.0f);
    double unchangedYaw = 123.0;
    double unchangedPitch = -456.0;
    passed &= expect(!invalidCamera.deriveYawPitchDegrees(
                         unchangedYaw, unchangedPitch) &&
                         unchangedYaw == 123.0 && unchangedPitch == -456.0,
                     "Zero Camera front produced a misleading derivation");

    invalidCamera.front.x = std::numeric_limits<float>::quiet_NaN();
    passed &= expect(!invalidCamera.deriveYawPitchDegrees(
                         unchangedYaw, unchangedPitch),
                     "Nonfinite Camera front was accepted for derivation");
    return passed;
}

bool runYawPitchMutationTests() {
    bool passed = true;
    const glm::vec3 worldUp{0.0f, 1.0f, 0.0f};

    Camera camera;
    passed &= expect(camera.setYawPitchDegrees(-90.0, 0.0, worldUp) &&
                         sameVector(camera.front, {0.0f, 0.0f, -1.0f}) &&
                         sameVector(camera.up, worldUp),
                     "Camera did not apply the default yaw/pitch convention");

    const glm::vec3 arbitraryUp = glm::normalize(glm::vec3(0.0f, 0.0f, 1.0f));
    passed &= expect(camera.setYawPitchDegrees(30.0, 20.0, arbitraryUp) &&
                         isNormalized(camera.front) &&
                         isNormalized(camera.up) &&
                         sameVector(camera.up, arbitraryUp) &&
                         hasValidBasis(camera),
                     "Camera rejected a valid arbitrary reference up");

    const double orientations[][2] = {
        {-135.0, -35.0}, {-90.0, 0.0}, {-15.0, 20.0},
        {30.0, -20.0},   {90.0, 35.0},  {170.0, 10.0},
    };
    for (const auto& orientation : orientations) {
        Camera roundTrip;
        passed &= expect(roundTrip.setYawPitchDegrees(
                             orientation[0], orientation[1], worldUp),
                         "Camera rejected a valid yaw/pitch pair");
        double derivedYaw = 0.0;
        double derivedPitch = 0.0;
        passed &= expect(roundTrip.deriveYawPitchDegrees(
                             derivedYaw, derivedPitch) &&
                             std::fabs(derivedYaw - orientation[0]) < 1.0e-3 &&
                             std::fabs(derivedPitch - orientation[1]) < 1.0e-3,
                         "Camera yaw/pitch round trip changed orientation");
    }

    const glm::vec3 originalFront = camera.front;
    const glm::vec3 originalUp = camera.up;
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    auto unchanged = [&camera, &originalFront, &originalUp]() {
        return sameVector(camera.front, originalFront) &&
               sameVector(camera.up, originalUp);
    };

    passed &= expect(!camera.setYawPitchDegrees(nan, 20.0, worldUp) &&
                         unchanged(),
                     "NaN yaw partially mutated Camera orientation");
    passed &= expect(!camera.setYawPitchDegrees(20.0, infinity, worldUp) &&
                         unchanged(),
                     "Infinite pitch partially mutated Camera orientation");
    passed &= expect(!camera.setYawPitchDegrees(20.0, 20.0, glm::vec3(0.0f)) &&
                         unchanged(),
                     "Zero reference up partially mutated Camera orientation");

    glm::vec3 nonfiniteUp = worldUp;
    nonfiniteUp.x = std::numeric_limits<float>::infinity();
    passed &= expect(!camera.setYawPitchDegrees(20.0, 20.0, nonfiniteUp) &&
                         unchanged(),
                     "Nonfinite reference up partially mutated Camera orientation");
    passed &= expect(!camera.setYawPitchDegrees(20.0, 90.0, worldUp) &&
                         unchanged(),
                     "Degenerate forward/up basis was accepted");

    return passed;
}

bool runOrientationDeltaTests() {
    bool passed = true;
    const glm::mat4 identity(1.0f);
    const glm::mat3 xRotation = glm::mat3(glm::rotate(
        identity, glm::radians(90.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    const glm::mat3 yRotation = glm::mat3(glm::rotate(
        identity, glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f)));

    Camera xRotated;
    passed &= expect(xRotated.applyOrientationDelta(xRotation) &&
                         sameVector(xRotated.front, {0.0f, 1.0f, 0.0f}) &&
                         sameVector(xRotated.up, {0.0f, 0.0f, 1.0f}) &&
                         isNormalized(xRotated.front) &&
                         isNormalized(xRotated.up) &&
                         hasValidBasis(xRotated),
                     "Camera X orientation delta was incorrect");

    Camera yRotated;
    passed &= expect(yRotated.applyOrientationDelta(yRotation) &&
                         sameVector(yRotated.front, {-1.0f, 0.0f, 0.0f}) &&
                         sameVector(yRotated.up, {0.0f, 1.0f, 0.0f}) &&
                         hasValidBasis(yRotated),
                     "Camera Y orientation delta was incorrect");

    const glm::mat3 combinedRotation = glm::mat3(glm::rotate(
        glm::rotate(identity, glm::radians(35.0f),
                    glm::vec3(0.0f, 1.0f, 0.0f)),
        glm::radians(-20.0f), glm::vec3(1.0f, 0.0f, 0.0f)));
    Camera combined;
    passed &= expect(combined.applyOrientationDelta(combinedRotation) &&
                         isNormalized(combined.front) &&
                         isNormalized(combined.up) &&
                         hasValidBasis(combined) &&
                         std::fabs(glm::dot(combined.front, combined.up)) <
                             1.0e-5f,
                     "Combined Camera orientation delta was invalid");

    const glm::vec3 originalFront = combined.front;
    const glm::vec3 originalUp = combined.up;
    glm::mat3 nonfiniteRotation(1.0f);
    nonfiniteRotation[1][1] = std::numeric_limits<float>::quiet_NaN();
    passed &= expect(!combined.applyOrientationDelta(nonfiniteRotation) &&
                         sameVector(combined.front, originalFront) &&
                         sameVector(combined.up, originalUp),
                     "Nonfinite orientation delta partially mutated Camera");

    const glm::mat3 zeroRotation(0.0f);
    passed &= expect(!combined.applyOrientationDelta(zeroRotation) &&
                         sameVector(combined.front, originalFront) &&
                         sameVector(combined.up, originalUp),
                     "Degenerate orientation delta partially mutated Camera");
    return passed;
}

}  // namespace

int main() {
    return runDerivationTests() && runYawPitchMutationTests() &&
                   runOrientationDeltaTests()
               ? 0
               : 1;
}
