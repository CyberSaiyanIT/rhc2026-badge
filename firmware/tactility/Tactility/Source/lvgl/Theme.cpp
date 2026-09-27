#include <Tactility/lvgl/Theme.h>

#include <lvgl.h>

namespace tt::lvgl {

namespace {

struct Palette {
    uint32_t primary;
    uint32_t secondary;
    bool dark;
};

Palette getPalette(settings::display::ThemeType type) {
    switch (type) {
        using enum settings::display::ThemeType;
        case RomHack:
            return { 0xE80C60, 0xB0000D, true };  // Badge magenta over badge red
        case NordLight:
            return { 0x5E81AC, 0xBF616A, false }; // Nord frost / aurora red
        case NordDark:
            return { 0x88C0D0, 0xBF616A, true };
        case TokyoNight:
            return { 0x7AA2F7, 0xBB9AF7, true };
        case SolarisedDark:
            return { 0x268BD2, 0xB58900, true };
        case Tactility:
        default:
            // Matches what lv_display_create() applies on its own, so this entry is a true reset.
            return { 0x2196F3, 0xF44336, true };
    }
}

/**
 * Mirrors lv_theme_default.c's own constants, so an app's surfaces match the widgets the theme
 * paints. Read from there rather than probed off a live widget: the accessors below are called
 * while a window is being built, before there is anything themed to probe.
 */
lv_color_t surfaceFor(bool dark) {
    return dark ? lv_color_hex(0x15171A) : lv_palette_lighten(LV_PALETTE_GREY, 4);
}

lv_color_t surfaceRaisedFor(bool dark) {
    return dark ? lv_color_hex(0x282B30) : lv_color_white();
}

lv_color_t textFor(bool dark) {
    return dark ? lv_palette_lighten(LV_PALETTE_GREY, 5) : lv_palette_darken(LV_PALETTE_GREY, 4);
}

lv_color_t greyFor(bool dark) {
    return dark ? lv_color_hex(0x2F3237) : lv_palette_lighten(LV_PALETTE_GREY, 2);
}

/** The last palette applied, so the accessors answer without re-deriving it from the theme. */
Palette active = getPalette(settings::display::ThemeType::Tactility);

} // namespace

lv_color_t getThemeAccent() { return lv_color_hex(active.primary); }
lv_color_t getThemeSurface() { return surfaceFor(active.dark); }
lv_color_t getThemeSurfaceRaised() { return surfaceRaisedFor(active.dark); }
lv_color_t getThemeText() { return textFor(active.dark); }
lv_color_t getThemeGrey() { return greyFor(active.dark); }

lv_color_t getContrastingText(lv_color_t background) {
    // Rec. 601 luma, which tracks perceived brightness closely enough to choose between two inks.
    const uint32_t luma = (background.red * 299u + background.green * 587u + background.blue * 114u) / 1000u;
    return luma > 140 ? lv_color_black() : lv_color_white();
}

void applyTheme(settings::display::ThemeType type) {
    auto* display = lv_display_get_default();
    if (display == nullptr) {
        return;
    }

    const auto palette = getPalette(type);
    active = palette;
    auto* theme = lv_theme_default_init(
        display,
        lv_color_hex(palette.primary),
        lv_color_hex(palette.secondary),
        palette.dark,
        LV_FONT_DEFAULT
    );
    lv_display_set_theme(display, theme);
}

} // namespace tt::lvgl
