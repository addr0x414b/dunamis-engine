#ifndef PHYSICS_UNITS_H
#define PHYSICS_UNITS_H

#include <glm/vec3.hpp>

namespace physics {

// Dunamis uses a single world-space unit convention: one Dunamis unit is one
// meter. Positions, distances, physical dimensions, and linear velocities are
// therefore expressed in meters (or meters per second). Scale is dimensionless
// and rotations, FOV, colors, and intensities are not unit-converted. Keep
// these helpers as the explicit Dunamis/Jolt ownership boundary; Jolt receives
// meters even while the current conversion is mathematically one-to-one.
constexpr float dunamisUnitsPerMeter = 1.0f;
constexpr float metersPerDunamisUnit = 1.0f / dunamisUnitsPerMeter;

constexpr float dunamisToMeters(float dunamisUnits) noexcept {
    return dunamisUnits * metersPerDunamisUnit;
}

constexpr float metersToDunamis(float meters) noexcept {
    return meters * dunamisUnitsPerMeter;
}

inline glm::vec3 dunamisToMeters(const glm::vec3& dunamisUnits) noexcept {
    return dunamisUnits * metersPerDunamisUnit;
}

inline glm::vec3 metersToDunamis(const glm::vec3& meters) noexcept {
    return meters * dunamisUnitsPerMeter;
}

}  // namespace physics

#endif
