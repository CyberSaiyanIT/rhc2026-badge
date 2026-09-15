#pragma once

#include <Tactility/settings/DisplaySettings.h>

namespace tt::lvgl {

/**
 * Re-initialise the default LVGL theme with the palette for `type`. Affects only widgets created
 * afterwards, so the caller rebuilds any window already on screen.
 *
 * @pre Caller must hold the LVGL lock
 */
void applyTheme(settings::display::ThemeType type);

} // namespace tt::lvgl
