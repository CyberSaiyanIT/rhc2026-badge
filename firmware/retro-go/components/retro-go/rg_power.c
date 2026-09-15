#include "rg_system.h"
#include "rg_power.h"

#ifdef RG_I2C_GPIO_DRIVER

// Rails are defined here rather than in the target header so there is exactly one instance of
// each, since the consumer count only means anything if every caller shares it.
#ifdef RG_POWER_RAIL_SUPPLY
rg_power_rail_t rg_power_rail_supply = RG_POWER_RAIL_SUPPLY;
#endif
#ifdef RG_POWER_RAIL_SPEAKER
rg_power_rail_t rg_power_rail_speaker = RG_POWER_RAIL_SPEAKER;
#endif


static bool rail_write(rg_power_rail_t *rail, bool on)
{
    return rg_i2c_gpio_set_level(rail->pin, (on != rail->active_low) ? 1 : 0) &&
           rg_i2c_gpio_set_direction(rail->pin, RG_GPIO_OUTPUT);
}

bool rg_power_rail_enable(rg_power_rail_t *rail)
{
    if (rail->consumers > 0)
    {
        rail->consumers++;
        return true;
    }

    if (rail->supply && !rg_power_rail_enable(rail->supply))
    {
        RG_LOGE("Supply of rail on pin %d is unavailable", rail->pin);
        return false;
    }

    if (!rail_write(rail, true))
    {
        RG_LOGE("Failed to switch on rail on pin %d", rail->pin);
        if (rail->supply)
            rg_power_rail_disable(rail->supply);
        return false;
    }

    if (rail->startup_delay_us)
        rg_usleep(rail->startup_delay_us);

    rail->consumers = 1;
    return true;
}

bool rg_power_rail_disable(rg_power_rail_t *rail)
{
    if (rail->consumers == 0)
    {
        RG_LOGW("Rail on pin %d disabled more often than it was enabled", rail->pin);
        return false;
    }

    if (--rail->consumers > 0)
        return true;

    bool ok = rail_write(rail, false); // the hold is already dropped, see rg_power.h
    if (!ok)
        RG_LOGE("Failed to switch off rail on pin %d", rail->pin);
    if (rail->supply && !rg_power_rail_disable(rail->supply))
        ok = false;
    return ok;
}

#endif
