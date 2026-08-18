#ifndef PHYSICS_UNITS_H
#define PHYSICS_UNITS_H

#include <glm/vec3.hpp>

namespace physics {

// Dunamis scene and render transforms are authored in Dunamis units. Jolt
// exclusively receives and returns meters at this backend boundary.
constexpr float dunamisUnitsPerMeter = 100.0f;
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
