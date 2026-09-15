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

} // namespace

void applyTheme(settings::display::ThemeType type) {
    auto* display = lv_display_get_default();
    if (display == nullptr) {
        return;
    }

    const auto palette = getPalette(type);
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
