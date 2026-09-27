#pragma once

#include <Tactility/settings/DisplaySettings.h>

#include <lvgl.h>

namespace tt::lvgl {

/**
 * Re-initialise the default LVGL theme with the palette for `type`. Affects only widgets created
 * afterwards, so the caller rebuilds any window already on screen.
 *
 * @pre Caller must hold the LVGL lock
 */
void applyTheme(settings::display::ThemeType type);

/**
 * The active theme's colours, for apps that paint their own surfaces instead of leaving widgets on
 * the theme's defaults. Without these an app hardcodes one palette and stays dark under a light
 * theme. Safe to call before applyTheme(), which answers for the built-in default.
 */
lv_color_t getThemeAccent();
/** The window background. */
lv_color_t getThemeSurface();
/** One step up from the background, for cards, rows and chips. */
lv_color_t getThemeSurfaceRaised();
/** Normal text, and the colour a focus ring has to be visible against the surface. */
lv_color_t getThemeText();
/** Inactive fills: the empty part of a bar, a disabled control. */
lv_color_t getThemeGrey();
/**
 * Text or an icon that will sit on @a background, picked for contrast against it.
 *
 * LVGL's default theme gives every button white text, on the assumption its background stays the
 * theme's primary colour. An app that repaints a button has to answer for its foreground too.
 */
lv_color_t getContrastingText(lv_color_t background);

} // namespace tt::lvgl
