// SPDX-License-Identifier: Apache-2.0
#include <lvgl.h>
#include <string.h>
#include <lvgl/fonts.h>
#include <stdbool.h>
#include <tactility/log.h>
#include <tactility/check.h>

// The preprocessor definitions that are used below are defined in the CMakeLists.txt from this module.

extern const lv_font_t TT_LVGL_TEXT_FONT_SMALL_SYMBOL;
extern const lv_font_t TT_LVGL_TEXT_FONT_DEFAULT_SYMBOL;
extern const lv_font_t TT_LVGL_TEXT_FONT_LARGE_SYMBOL;

extern const lv_font_t TT_LVGL_LAUNCHER_FONT_ICON_SYMBOL;
extern const lv_font_t TT_LVGL_STATUSBAR_FONT_ICON_SYMBOL;
extern const lv_font_t TT_LVGL_SHARED_FONT_ICON_SYMBOL;

/**
 * The built-in Montserrat fonts cover 0x20-0x7F and are `const` in flash, so these copies carry the
 * fallback chain instead: Montserrat -> accents and punctuation -> emoji, per generated size.
 * Emoji are alpha bitmaps, so they draw in the label's text colour.
 */
static lv_font_t text_font_small;
static lv_font_t text_font_default;
static lv_font_t text_font_large;
static lv_font_t extras_font_small;
static lv_font_t extras_font_default;
static lv_font_t extras_font_large;

#ifdef TT_LVGL_TEXT_FONT_SMALL_FALLBACK_SYMBOL
extern const lv_font_t TT_LVGL_TEXT_FONT_SMALL_FALLBACK_SYMBOL;
#endif
#ifdef TT_LVGL_TEXT_FONT_DEFAULT_FALLBACK_SYMBOL
extern const lv_font_t TT_LVGL_TEXT_FONT_DEFAULT_FALLBACK_SYMBOL;
#endif
#ifdef TT_LVGL_TEXT_FONT_LARGE_FALLBACK_SYMBOL
extern const lv_font_t TT_LVGL_TEXT_FONT_LARGE_FALLBACK_SYMBOL;
#endif
#ifdef TT_LVGL_TEXT_FONT_SMALL_EMOJI_SYMBOL
extern const lv_font_t TT_LVGL_TEXT_FONT_SMALL_EMOJI_SYMBOL;
#endif
#ifdef TT_LVGL_TEXT_FONT_DEFAULT_EMOJI_SYMBOL
extern const lv_font_t TT_LVGL_TEXT_FONT_DEFAULT_EMOJI_SYMBOL;
#endif
#ifdef TT_LVGL_TEXT_FONT_LARGE_EMOJI_SYMBOL
extern const lv_font_t TT_LVGL_TEXT_FONT_LARGE_EMOJI_SYMBOL;
#endif

/** Builds one chain. @a emoji is last, so it needs no copy of its own. */
static void chain_font(lv_font_t* out, const lv_font_t* base, lv_font_t* extras, const lv_font_t* extrasSource, const lv_font_t* emoji) {
    const lv_font_t* tail = emoji;
    if (extrasSource != NULL) {
        *extras = *extrasSource;
        extras->fallback = tail;
        tail = extras;
    }
    *out = *base;
    out->fallback = tail;
}

static void init_text_fonts(void) {
    static bool initialised = false;
    if (initialised) {
        return;
    }

#ifdef TT_LVGL_TEXT_FONT_SMALL_FALLBACK_SYMBOL
    const lv_font_t* small_extras = &TT_LVGL_TEXT_FONT_SMALL_FALLBACK_SYMBOL;
#else
    const lv_font_t* small_extras = NULL;
#endif
#ifdef TT_LVGL_TEXT_FONT_DEFAULT_FALLBACK_SYMBOL
    const lv_font_t* default_extras = &TT_LVGL_TEXT_FONT_DEFAULT_FALLBACK_SYMBOL;
#else
    const lv_font_t* default_extras = NULL;
#endif
#ifdef TT_LVGL_TEXT_FONT_LARGE_FALLBACK_SYMBOL
    const lv_font_t* large_extras = &TT_LVGL_TEXT_FONT_LARGE_FALLBACK_SYMBOL;
#else
    const lv_font_t* large_extras = NULL;
#endif
#ifdef TT_LVGL_TEXT_FONT_SMALL_EMOJI_SYMBOL
    const lv_font_t* small_emoji = &TT_LVGL_TEXT_FONT_SMALL_EMOJI_SYMBOL;
#else
    const lv_font_t* small_emoji = NULL;
#endif
#ifdef TT_LVGL_TEXT_FONT_DEFAULT_EMOJI_SYMBOL
    const lv_font_t* default_emoji = &TT_LVGL_TEXT_FONT_DEFAULT_EMOJI_SYMBOL;
#else
    const lv_font_t* default_emoji = NULL;
#endif
#ifdef TT_LVGL_TEXT_FONT_LARGE_EMOJI_SYMBOL
    const lv_font_t* large_emoji = &TT_LVGL_TEXT_FONT_LARGE_EMOJI_SYMBOL;
#else
    const lv_font_t* large_emoji = NULL;
#endif

    chain_font(&text_font_small, &TT_LVGL_TEXT_FONT_SMALL_SYMBOL, &extras_font_small, small_extras, small_emoji);
    chain_font(&text_font_default, &TT_LVGL_TEXT_FONT_DEFAULT_SYMBOL, &extras_font_default, default_extras, default_emoji);
    chain_font(&text_font_large, &TT_LVGL_TEXT_FONT_LARGE_SYMBOL, &extras_font_large, large_extras, large_emoji);

    // Whether the fallbacks were linked in is a build-time question, answered here rather than
    // guessed from the UI. The glyph lookup needs LVGL up, which it is by the first draw.
    if (lv_is_initialized()) {
        lv_font_glyph_dsc_t probe;
        memset(&probe, 0, sizeof(probe));
        const bool accent = lv_font_get_glyph_dsc(&text_font_default, &probe, 0x00E0, 0);
        memset(&probe, 0, sizeof(probe));
        const bool emoji = lv_font_get_glyph_dsc(&text_font_default, &probe, 0x1F355, 0);
        LOG_I("lvgl", "Text font %d: extras=%s emoji=%s (a-grave=%s, pizza=%s)",
            (int) TT_LVGL_TEXT_FONT_DEFAULT_SIZE,
            default_extras != NULL ? "linked" : "ABSENT",
            default_emoji != NULL ? "linked" : "ABSENT",
            accent ? "ok" : "MISSING",
            emoji ? "ok" : "MISSING");
    } else {
        LOG_I("lvgl", "Text font %d: extras=%s emoji=%s",
            (int) TT_LVGL_TEXT_FONT_DEFAULT_SIZE,
            default_extras != NULL ? "linked" : "ABSENT",
            default_emoji != NULL ? "linked" : "ABSENT");
    }

    initialised = true;
}

uint32_t lvgl_get_text_font_height(enum LvglFontSize font_size) {
    switch (font_size) {
        case FONT_SIZE_SMALL: return TT_LVGL_TEXT_FONT_SMALL_SIZE;
        case FONT_SIZE_DEFAULT: return TT_LVGL_TEXT_FONT_DEFAULT_SIZE;
        case FONT_SIZE_LARGE: return TT_LVGL_TEXT_FONT_LARGE_SIZE;
        default: check(false);
    }
}
const lv_font_t* lvgl_get_text_font(enum LvglFontSize font_size) {
    init_text_fonts();
    switch (font_size) {
        case FONT_SIZE_SMALL: return &text_font_small;
        case FONT_SIZE_DEFAULT: return &text_font_default;
        case FONT_SIZE_LARGE: return &text_font_large;
        default: check(false);
    }
}

uint32_t lvgl_get_shared_icon_font_height() { return TT_LVGL_SHARED_FONT_ICON_SIZE; }

const lv_font_t* lvgl_get_shared_icon_font() { return &TT_LVGL_SHARED_FONT_ICON_SYMBOL; }

uint32_t lvgl_get_launcher_icon_font_height() { return TT_LVGL_LAUNCHER_FONT_ICON_SIZE; }

const lv_font_t* lvgl_get_launcher_icon_font() { return &TT_LVGL_LAUNCHER_FONT_ICON_SYMBOL; }

uint32_t lvgl_get_statusbar_icon_font_height() { return TT_LVGL_STATUSBAR_FONT_ICON_SIZE; }

const lv_font_t* lvgl_get_statusbar_icon_font() { return &TT_LVGL_STATUSBAR_FONT_ICON_SYMBOL; }
