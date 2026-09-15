#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * A switched power rail behind an I2C expander pin, consumer-counted and mirroring the firmware's
 * romhack,power-rail driver. No lock of its own: callers are serialised by rg_audio's device lock,
 * and anything else needs its own or concurrent pairs corrupt `consumers`.
 */
typedef struct rg_power_rail_s
{
    int pin;                        // Expander pin in rg_i2c.c's flat numbering (P0.x = x, P1.x = 8 + x)
    bool active_low;                // Drive the pin low, not high, to switch the rail on
    uint32_t startup_delay_us;      // Settling time after the rail is switched on
    struct rg_power_rail_s *supply; // Upstream rail held while this one is on, or NULL
    uint32_t consumers;             // Runtime; must start at 0
} rg_power_rail_t;

/** Takes a hold on `rail`, switching it (and its supply) on if this is the first one. */
bool rg_power_rail_enable(rg_power_rail_t *rail);

/**
 * Drops a hold on `rail`, switching it (and its supply) off if it was the last one.
 * A hold is dropped even when the switch write fails, so one I2C error cannot strand the rail on.
 */
bool rg_power_rail_disable(rg_power_rail_t *rail);

#ifdef RG_POWER_RAIL_SUPPLY
extern rg_power_rail_t rg_power_rail_supply;
#endif
#ifdef RG_POWER_RAIL_SPEAKER
extern rg_power_rail_t rg_power_rail_speaker;
#endif
