#pragma once

namespace tt::power {

/** Below this the boot sequence shows the critical-battery screen instead of starting the launcher. */
constexpr int CRITICAL_BATTERY_MV = 2500;

/** Below this the power manager refuses to switch the 5V boost on. */
constexpr int BOOST_MINIMUM_MV = 2900;

/** Below this the statusbar battery icon turns red. */
constexpr int LOW_BATTERY_MV = 3000;

/**
 * The window a real cell can be in. The 358 kOhm sense divider is far above what the converter's
 * sample-and-hold wants, so anything outside the window is reported as no reading at all.
 */
constexpr int PLAUSIBLE_MIN_MV = 2200;
// Above a charger's ceiling rather than the cell's nominal 4.2 V: this pack settles at 4.22 V, and
// a correct reading of a full battery must not trip the window.
constexpr int PLAUSIBLE_MAX_MV = 4300;

/** @return whether a reading is in the window a real cell can be in */
constexpr bool isPlausible(int millivolts) {
    return millivolts >= PLAUSIBLE_MIN_MV && millivolts <= PLAUSIBLE_MAX_MV;
}

/** Installs the rail guard. Must run before any service that can bring a rail up is started. */
void init();

/**
 * Whether the user has switched the voltage limits off. The escape hatch for a board whose sensor
 * reads wrong, not the normal state.
 */
bool isLimitsOverridden();

void setLimitsOverridden(bool overridden);

/**
 * Reads the battery through the first supply that reports a voltage, dropping implausible samples
 * and returning the median: the divider sags under display and boost inrush.
 *
 * @return false when the board has no voltage-reporting power supply, or nothing it returned was
 *     a plausible cell voltage
 */
bool readBatteryMillivolts(int& outMillivolts);

} // namespace
