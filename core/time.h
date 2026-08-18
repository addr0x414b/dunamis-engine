#ifndef TIME_H
#define TIME_H

class Dunamis;
class TimeTestAccess;

class Time final {
public:
    // Returns the elapsed simulation time for the current frame, in seconds.
    // Before the first run-loop frame, and during the first frame after the
    // engine resets timing, this returns 0.0f. Simulation-facing values are
    // capped at 0.1 seconds.
    [[nodiscard]] static float deltaTime() noexcept;

private:
    friend class Dunamis;
    friend class TimeTestAccess;

    static void initialize() noexcept;
    static void update() noexcept;
    static float normalizeDeltaTime(float measuredDeltaTime) noexcept;
};

#endif
