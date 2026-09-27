#include <Tactility/lvgl/Theme.h>
#include <Tactility/service/neopixel/NeoPixel.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/check.h>

#include <lvgl.h>

#include <algorithm>
#include <cstdint>

namespace tt::app::lighting {

extern const ::AppManifest manifest;

namespace {

namespace np = service::neopixel;

// How long a change waits before it reaches flash. Long enough to coalesce a slider drag,
// short enough that a badge unplugged mid-session keeps what was set.
constexpr uint32_t SETTINGS_FLUSH_MS = 2000;

// Long enough for the service's dispatcher to have applied a reset before the page reads it
// back. The dispatcher is shared, and its own documentation warns it can be busy for a while.
constexpr uint32_t RESET_SETTLE_MS = 250;

// How many values each cycle steps through. The enums are contiguous from 0, so deriving the
// count here means adding an entry cannot leave a cycle stepping over it.
constexpr uint8_t ORIGIN_CHOICES = (uint8_t) np::VuOrigin::Cycle + 1;
constexpr uint8_t PALETTE_CHOICES = (uint8_t) np::VuPalette::Cycle + 1;
constexpr uint8_t BEAT_SOURCE_CHOICES = (uint8_t) np::VuBeatSource::Treble + 1;

// Order matches np::Animation, which is what the dropdown index is cast to.
constexpr auto* ANIMATION_OPTIONS =
    "Off\n"
    "Solid\n"
    "Breathing\n"
    "Rainbow\n"
    "Rainbow cycle\n"
    "Comet\n"
    "Scanner\n"
    "Theater chase\n"
    "Sparkle\n"
    "Twinkle\n"
    "Fire\n"
    "Wave\n"
    "Confetti\n"
    "Colour wipe\n"
    "Pulse\n"
    "Alternate";

// Order matches np::SleepAnimation.
constexpr auto* SLEEP_OPTIONS = "Off\nBeacon\nDot\nSweep\nTwinkle\nEnds\nPulse\nComet\nDrift\nSpark";

constexpr auto* SLEEP_AFTER_OPTIONS = "At once\n1 minute\n2 minutes\n5 minutes\n15 minutes\n30 minutes\n1 hour";
constexpr uint8_t SLEEP_AFTER_MINUTES[] = { 0, 1, 2, 5, 15, 30, 60 };

// Order matches np::Transition.
constexpr auto* TRANSITION_OPTIONS = "Off\nWipe\nBloom\nFade\nComet";

constexpr auto* SLEEP_TIME_OPTIONS = "2 seconds\n3 seconds\n5 seconds\n10 seconds\n30 seconds\n1 minute";
constexpr uint8_t SLEEP_TIME_SECONDS[] = { 2, 3, 5, 10, 30, 60 };

constexpr auto* VU_INACTIVE_OPTIONS = "15 minutes\n30 minutes\n45 minutes\n1 hour\n2 hours\nNever";
constexpr uint16_t VU_INACTIVE_SECONDS[] = { 900, 1800, 2700, 3600, 7200, 0 };

enum class Page : uint8_t { Home, General, Active, Sleep, Standby, Vu, Beat };

/** @return the page a back press returns to. Home is the only one that closes the app. */
Page parentOf(Page page) {
    switch (page) {
        case Page::General: return Page::Home;
        case Page::Vu: return Page::Home;
        case Page::Beat: return Page::Vu;
        case Page::Home: return Page::Home;
        default: return Page::General;
    }
}

/** Which setting a cycle row steps through. Rows carry this in their widget user data. */
enum class Cycle : uint8_t { Origin, Palette, BeatSource, ActiveColor, StandbyColor, SleepColor };

/** Which setting a dropdown row picks. Rows carry this in their widget user data. */
enum class Choice : uint8_t {
    ActiveAnimation, StandbyAnimation, SleepAnimation,
    WakeTransition, SleepTransition,
    SleepAfter, SleepTime, VuInactive
};

/** How long a preview holds the strip before the badge goes back to what it was doing. */
constexpr uint16_t PREVIEW_SECONDS = 20;

/** Which subpage a navigation row opens. */
enum class Nav : uint8_t { General, Active, Sleep, Standby, Vu, Beat };

struct Context {
    uint32_t appInstanceId = 0;
    Page page = Page::Home;
    lv_timer_t* rebuildTimer = nullptr;
    Page rebuildPage = Page::Home;
    lv_obj_t* root = nullptr;
    /** The awake animation's dropdown, so a refused strip supply can be reflected on it. */
    lv_obj_t* activeAnimationDropdown = nullptr;
    lv_timer_t* animationSyncTimer = nullptr;
};

lv_color_t accent() { return lvgl::getThemeAccent(); }
lv_color_t surface() { return lvgl::getThemeSurface(); }
lv_color_t surfaceRaised() { return lvgl::getThemeSurfaceRaised(); }

void populate(lv_obj_t* root, void* userData);
void showPage(Context* ctx, Page page);
void rebuildAfterDispatch(Context* ctx, Page page);

// Focus reads as a ring drawn inside the widget: the theme's default outline is painted outside
// it and gets clipped by the parent on the first and last rows.
void styleFocusRing(lv_obj_t* object) {
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(object, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(object, lvgl::getThemeText(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(object, LV_OPA_90, LV_STATE_FOCUSED);
}

lv_obj_t* createLabel(lv_obj_t* parent, const char* text, bool dim) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    if (dim) {
        lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
        lv_obj_set_style_text_opa(label, LV_OPA_60, LV_PART_MAIN);
    }
    return label;
}

/** A plain full-width row that lays its children out left to right. */
lv_obj_t* createRow(lv_obj_t* parent) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

/** A full-width button styled as a list row, with its label on the left. */
lv_obj_t* createButtonRow(lv_obj_t* parent, const char* text) {
    auto* button = lv_button_create(parent);
    lv_obj_set_size(button, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_text_color(button, lvgl::getContrastingText(surfaceRaised()), LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    styleFocusRing(button);
    createLabel(button, text, false);
    return button;
}

void onSliderChanged(lv_event_t* event);

/**
 * Name and current value on one line, the slider under it. The read-out is kept in the slider's
 * own user data so one callback can update any of them.
 */
lv_obj_t* createSlider(lv_obj_t* parent, const char* name, int min, int max, int value,
                       lv_event_cb_t callback, Context* ctx) {
    auto* header = createRow(parent);
    createLabel(header, name, false);
    auto* value_label = lv_label_create(header);
    lv_label_set_text_fmt(value_label, "%d", value);
    lv_obj_set_style_text_color(value_label, accent(), LV_PART_MAIN);

    auto* slider = lv_slider_create(parent);
    lv_obj_set_width(slider, LV_PCT(100));
    lv_obj_set_height(slider, 6);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_user_data(slider, value_label);
    lv_obj_set_style_bg_color(slider, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, accent(), LV_PART_KNOB);
    lv_obj_set_style_outline_width(slider, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
    // The knob grows on focus, which reads clearly without painting outside the row.
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(slider, lvgl::getThemeText(), LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_add_event_cb(slider, onSliderChanged, LV_EVENT_VALUE_CHANGED, nullptr);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, ctx);
    return slider;
}

lv_obj_t* createSwitch(lv_obj_t* parent, const char* name, bool on, lv_event_cb_t callback, Context* ctx) {
    auto* row = createRow(parent);
    createLabel(row, name, false);
    auto* toggle = lv_switch_create(row);
    if (on) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    lv_obj_set_style_bg_color(toggle, accent(), LV_PART_INDICATOR | LV_STATE_CHECKED);
    styleFocusRing(toggle);
    lv_obj_add_event_cb(toggle, callback, LV_EVENT_VALUE_CHANGED, ctx);
    return toggle;
}

/** Name on the left, a picker on the right. Used where a cycle row would need too many taps. */
lv_obj_t* createDropdown(lv_obj_t* parent, const char* name, const char* options, uint32_t selected,
                         Choice choice, lv_event_cb_t callback, Context* ctx) {
    auto* row = createRow(parent);
    createLabel(row, name, false);
    auto* dropdown = lv_dropdown_create(row);
    lv_dropdown_set_options(dropdown, options);
    lv_dropdown_set_selected(dropdown, selected);
    lv_obj_set_style_bg_color(dropdown, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_text_color(dropdown, accent(), LV_PART_MAIN);
    styleFocusRing(dropdown);
    lv_obj_set_user_data(dropdown, (void*) (uintptr_t) choice);
    lv_obj_add_event_cb(dropdown, callback, LV_EVENT_VALUE_CHANGED, ctx);
    return dropdown;
}

const char* originText(np::VuOrigin origin) {
    switch (origin) {
        case np::VuOrigin::BothLeft: return "Both left";
        case np::VuOrigin::BothRight: return "Both right";
        case np::VuOrigin::Center: return "Center";
        default: return "Cycle";
    }
}

const char* paletteText(np::VuPalette palette) {
    switch (palette) {
        case np::VuPalette::Solid: return "Solid";
        case np::VuPalette::Rainbow: return "Rainbow";
        case np::VuPalette::Spectrum: return "Spectrum";
        case np::VuPalette::Cycle: return "Cycle";
        default: return "Classic";
    }
}

const char* colorModeText(np::ColorMode mode) {
    return mode == np::ColorMode::Cycle ? "Cycle" : "Static";
}

/** @return the mode a stage's colour card steps to when tapped */
np::ColorMode nextColorMode(np::ColorMode mode) {
    return mode == np::ColorMode::Cycle ? np::ColorMode::Static : np::ColorMode::Cycle;
}

const char* beatSourceText(np::VuBeatSource source) {
    switch (source) {
        case np::VuBeatSource::Bass: return "Bass";
        case np::VuBeatSource::Treble: return "Treble";
        default: return "Mix";
    }
}

/** @return the text a cycle row shows for its setting's current value */
const char* cycleText(Cycle cycle) {
    switch (cycle) {
        case Cycle::ActiveColor: return colorModeText(np::getActiveColorMode());
        case Cycle::StandbyColor: return colorModeText(np::getStandbyColorMode());
        case Cycle::SleepColor: return colorModeText(np::getSleepColorMode());
        case Cycle::Origin: return originText(np::getVuOrigin());
        case Cycle::Palette: return paletteText(np::getVuPalette());
        default: return beatSourceText(np::getVuBeatSource());
    }
}

/**
 * Set when a control has moved, cleared once the service has written the file. At namespace scope
 * because it tracks the service's state, which is one thing however many apps are open.
 */
bool settingsDirty = false;

void onSliderChanged(lv_event_t* event) {
    auto* slider = lv_event_get_target_obj(event);
    if (auto* value_label = static_cast<lv_obj_t*>(lv_obj_get_user_data(slider))) {
        lv_label_set_text_fmt(value_label, "%d", (int) lv_slider_get_value(slider));
    }
}

// Every slider and switch handler reads its value through these, so marking the settings dirty
// here covers all of them without a line in each.
int32_t sliderValue(lv_event_t* event) {
    settingsDirty = true;
    return lv_slider_get_value(lv_event_get_target_obj(event));
}

bool switchValue(lv_event_t* event) {
    settingsDirty = true;
    return lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED);
}

/**
 * The service falls back to Off when the strip's supply refuses. The dropdown is set optimistically
 * and corrected here once the dispatched change has had a frame.
 */
void onAnimationSettled(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    ctx->animationSyncTimer = nullptr;
    if (ctx->activeAnimationDropdown == nullptr) {
        return;
    }
    const auto animation = (uint32_t) np::getActiveAnimation();
    if (lv_dropdown_get_selected(ctx->activeAnimationDropdown) != animation) {
        lv_dropdown_set_selected(ctx->activeAnimationDropdown, animation);
        rebuildAfterDispatch(ctx, Page::Active);
    }
}

void scheduleAnimationSync(Context* ctx) {
    if (ctx->animationSyncTimer == nullptr) {
        ctx->animationSyncTimer = lv_timer_create(onAnimationSettled, RESET_SETTLE_MS, ctx);
        lv_timer_set_repeat_count(ctx->animationSyncTimer, 1);
    }
}

/**
 * Gives a running preview of @a stage its full window back, so stepping through patterns keeps
 * showing each one. Does not start one: a preview is only ever asked for by tapping.
 */
void refreshPreview(np::Stage stage) {
    if (np::isPreviewActive() && np::getPreviewStage() == stage) {
        np::startPreview(stage, PREVIEW_SECONDS);
    }
}

void onChoiceChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* dropdown = lv_event_get_target_obj(event);
    const uint32_t selected = lv_dropdown_get_selected(dropdown);
    settingsDirty = true;

    switch ((Choice) (uintptr_t) lv_obj_get_user_data(dropdown)) {
        case Choice::ActiveAnimation:
            np::setActiveAnimation((np::Animation) selected);
            refreshPreview(np::Stage::Active);
            scheduleAnimationSync(ctx);
            // Off drops the rest of the page, and a pattern that lights few pixels is allowed a
            // brighter maximum, so the brightness slider has to be rebuilt around the new one.
            rebuildAfterDispatch(ctx, Page::Active);
            break;
        case Choice::StandbyAnimation:
            np::setStandbyAnimation((np::Animation) selected);
            refreshPreview(np::Stage::Standby);
            rebuildAfterDispatch(ctx, Page::Standby);
            break;
        case Choice::SleepAnimation:
            np::setSleepAnimation((np::SleepAnimation) selected);
            refreshPreview(np::Stage::Sleep);
            rebuildAfterDispatch(ctx, Page::Sleep);
            break;
        case Choice::SleepAfter:
            if (selected < sizeof(SLEEP_AFTER_MINUTES)) {
                np::setSleepMinutes(SLEEP_AFTER_MINUTES[selected]);
            }
            break;
        case Choice::WakeTransition:
            np::setWakeTransition((np::Transition) selected);
            break;
        case Choice::SleepTransition:
            np::setSleepTransition((np::Transition) selected);
            break;
        case Choice::SleepTime:
            if (selected < sizeof(SLEEP_TIME_SECONDS)) {
                np::setSleepIntervalSeconds(SLEEP_TIME_SECONDS[selected]);
            }
            break;
        case Choice::VuInactive:
            if (selected < sizeof(VU_INACTIVE_SECONDS) / sizeof(VU_INACTIVE_SECONDS[0])) {
                np::setVuInactiveSeconds(VU_INACTIVE_SECONDS[selected]);
            }
            break;
    }
}

/**
 * Steps a setting to its next value. Labelled from the value being sent, never from reading the
 * service back: setters queue onto the dispatcher, so a read would trail one press behind.
 */
void onCycleClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* button = lv_event_get_target_obj(event);
    settingsDirty = true;
    const auto cycle = (Cycle) (uintptr_t) lv_obj_get_user_data(button);

    const char* text = nullptr;
    switch (cycle) {
        // The channel sliders appear and disappear with Static, so these rebuild rather than
        // relabel; the page is redrawn under them either way.
        case Cycle::ActiveColor: {
            const auto next = nextColorMode(np::getActiveColorMode());
            np::setActiveColorMode(next);
            text = colorModeText(next);
            rebuildAfterDispatch(ctx, Page::Active);
            break;
        }
        case Cycle::StandbyColor: {
            const auto next = nextColorMode(np::getStandbyColorMode());
            np::setStandbyColorMode(next);
            text = colorModeText(next);
            rebuildAfterDispatch(ctx, Page::Standby);
            break;
        }
        case Cycle::SleepColor: {
            const auto next = nextColorMode(np::getSleepColorMode());
            np::setSleepColorMode(next);
            text = colorModeText(next);
            rebuildAfterDispatch(ctx, Page::Sleep);
            break;
        }
        case Cycle::Origin: {
            const auto next = (np::VuOrigin) (((uint8_t) np::getVuOrigin() + 1) % ORIGIN_CHOICES);
            np::setVuOrigin(next);
            text = originText(next);
            break;
        }
        case Cycle::Palette: {
            const auto current = np::getVuPalette();
            const auto next = (np::VuPalette) (((uint8_t) current + 1) % PALETTE_CHOICES);
            np::setVuPalette(next);
            text = paletteText(next);
            // Only Solid paints a colour of its own, so crossing it adds or removes its sliders.
            if ((next == np::VuPalette::Solid) != (current == np::VuPalette::Solid)) {
                rebuildAfterDispatch(ctx, Page::Vu);
            }
            break;
        }
        case Cycle::BeatSource: {
            const auto next = (np::VuBeatSource) (((uint8_t) np::getVuBeatSource() + 1) % BEAT_SOURCE_CHOICES);
            np::setVuBeatSource(next);
            text = beatSourceText(next);
            break;
        }
    }
    // Child 1 is the value; child 0 is the name.
    lv_label_set_text(lv_obj_get_child(button, 1), text);
}

void onNavClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    switch ((Nav) (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(event))) {
        case Nav::General: showPage(ctx, Page::General); break;
        case Nav::Active: showPage(ctx, Page::Active); break;
        case Nav::Standby: showPage(ctx, Page::Standby); break;
        case Nav::Sleep: showPage(ctx, Page::Sleep); break;
        case Nav::Vu: showPage(ctx, Page::Vu); break;
        case Nav::Beat: showPage(ctx, Page::Beat); break;
    }
}

/** @return the stage a page configures */
np::Stage stageOf(Page page) {
    switch (page) {
        case Page::Standby: return np::Stage::Standby;
        case Page::Sleep: return np::Stage::Sleep;
        default: return np::Stage::Active;
    }
}

/** Starts this page's stage on the strip, or stops one that is already showing. */
void onPreviewClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const auto stage = stageOf(ctx->page);
    if (np::isPreviewActive() && np::getPreviewStage() == stage) {
        np::stopPreview();
    } else {
        np::startPreview(stage, PREVIEW_SECONDS);
    }
    rebuildAfterDispatch(ctx, ctx->page);
}

void onBackClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    showPage(ctx, parentOf(ctx->page));
}

void onActiveBrightness(lv_event_t* e) { np::setActiveBrightness(sliderValue(e)); }
void onActiveSpeed(lv_event_t* e) { np::setActiveSpeed(sliderValue(e)); }
void onStandbyBrightness(lv_event_t* e) { np::setStandbyBrightness(sliderValue(e)); }
void onStandbySpeed(lv_event_t* e) { np::setStandbySpeed(sliderValue(e)); }

// The service takes the three channels as one value, so the other two are read back rather than
// shadowed here.
void onActiveRed(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getActiveColor(&r, &g, &b);
    np::setActiveColor(sliderValue(e), g, b);
}

void onActiveGreen(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getActiveColor(&r, &g, &b);
    np::setActiveColor(r, sliderValue(e), b);
}

void onActiveBlue(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getActiveColor(&r, &g, &b);
    np::setActiveColor(r, g, sliderValue(e));
}

void onStandbyRed(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getStandbyColor(&r, &g, &b);
    np::setStandbyColor(sliderValue(e), g, b);
}

void onStandbyGreen(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getStandbyColor(&r, &g, &b);
    np::setStandbyColor(r, sliderValue(e), b);
}

void onStandbyBlue(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getStandbyColor(&r, &g, &b);
    np::setStandbyColor(r, g, sliderValue(e));
}

void onVuRed(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getVuColor(&r, &g, &b);
    np::setVuColor(sliderValue(e), g, b);
}

void onVuGreen(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getVuColor(&r, &g, &b);
    np::setVuColor(r, sliderValue(e), b);
}

void onVuBlue(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getVuColor(&r, &g, &b);
    np::setVuColor(r, g, sliderValue(e));
}

void onSleepBrightness(lv_event_t* e) { np::setSleepBrightness(sliderValue(e)); }
void onSleepSpeed(lv_event_t* e) { np::setSleepSpeed(sliderValue(e)); }

void onSleepRed(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getSleepColor(&r, &g, &b);
    np::setSleepColor(sliderValue(e), g, b);
}

void onSleepGreen(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getSleepColor(&r, &g, &b);
    np::setSleepColor(r, sliderValue(e), b);
}

void onSleepBlue(lv_event_t* e) {
    uint8_t r = 0, g = 0, b = 0;
    np::getSleepColor(&r, &g, &b);
    np::setSleepColor(r, g, sliderValue(e));
}

void onVuSpeed(lv_event_t* e) { np::setVuSpeed(sliderValue(e)); }
void onVuBrightness(lv_event_t* e) { np::setVuBrightness(sliderValue(e)); }
void onVuSensitivity(lv_event_t* e) { np::setVuSensitivity(sliderValue(e)); }
void onVuPeakBrightness(lv_event_t* e) { np::setVuPeakBrightness(sliderValue(e)); }
void onVuAutoGain(lv_event_t* e) { np::setVuAutoGainEnabled(switchValue(e)); }
void onVuBassOnly(lv_event_t* e) { np::setVuBassOnlyEnabled(switchValue(e)); }
void onVuDecay(lv_event_t* e) { np::setVuDecayEnabled(switchValue(e)); }
void onVuPeakHold(lv_event_t* e) { np::setVuPeakHoldEnabled(switchValue(e)); }

void onBeatEnabled(lv_event_t* e) { np::setVuBeatFlashEnabled(switchValue(e)); }
void onBeatSensitivity(lv_event_t* e) { np::setVuBeatSensitivity(sliderValue(e)); }

// The manual slider stops being shown while this is on, so the page is rebuilt around it.
void onBeatAuto(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    np::setVuBeatAutoEnabled(switchValue(e));
    rebuildAfterDispatch(ctx, Page::Beat);
}
void onFlashAttack(lv_event_t* e) { np::setVuFlashAttack(sliderValue(e)); }
void onFlashDecay(lv_event_t* e) { np::setVuFlashDecay(sliderValue(e)); }

lv_obj_t* createCycleRow(lv_obj_t* parent, const char* name, Cycle cycle, Context* ctx) {
    auto* button = createButtonRow(parent, name);
    auto* value_label = lv_label_create(button);
    lv_label_set_text(value_label, cycleText(cycle));
    lv_obj_set_style_text_color(value_label, accent(), LV_PART_MAIN);
    lv_obj_set_user_data(button, (void*) (uintptr_t) cycle);
    lv_obj_add_event_cb(button, onCycleClicked, LV_EVENT_CLICKED, ctx);
    return button;
}

void createNavRow(lv_obj_t* parent, const char* name, Nav nav, Context* ctx) {
    auto* button = createButtonRow(parent, name);
    createLabel(button, LV_SYMBOL_RIGHT, true);
    lv_obj_set_user_data(button, (void*) (uintptr_t) nav);
    lv_obj_add_event_cb(button, onNavClicked, LV_EVENT_CLICKED, ctx);
}

void onRebuildSettled(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    // A one-shot timer deletes itself once it has run.
    ctx->rebuildTimer = nullptr;
    showPage(ctx, ctx->rebuildPage);
}

/**
 * Redraws @a page once the service has caught up. Setters queue onto the dispatcher while getters
 * read straight through, so rebuilding in the same callback reads back the old value.
 */
void rebuildAfterDispatch(Context* ctx, Page page) {
    ctx->rebuildPage = page;
    if (ctx->rebuildTimer == nullptr) {
        ctx->rebuildTimer = lv_timer_create(onRebuildSettled, RESET_SETTLE_MS, ctx);
        lv_timer_set_repeat_count(ctx->rebuildTimer, 1);
    }
}

void onResetClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    np::resetSettings();
    settingsDirty = false;
    rebuildAfterDispatch(ctx, Page::Home);
}

void createResetRow(lv_obj_t* parent, Context* ctx) {
    auto* button = createButtonRow(parent, "Reset to defaults");
    lv_obj_add_event_cb(button, onResetClicked, LV_EVENT_CLICKED, ctx);
}

void createPreviewRow(lv_obj_t* parent, Context* ctx) {
    const bool showing = np::isPreviewActive() && np::getPreviewStage() == stageOf(ctx->page);
    auto* button = createButtonRow(parent, showing ? "Stop preview" : "Preview");
    lv_obj_add_event_cb(button, onPreviewClicked, LV_EVENT_CLICKED, ctx);
}

void createBackRow(lv_obj_t* parent, Context* ctx) {
    auto* button = createButtonRow(parent, LV_SYMBOL_LEFT "  Back");
    lv_obj_add_event_cb(button, onBackClicked, LV_EVENT_CLICKED, ctx);
}

void createHeading(lv_obj_t* parent, const char* text) {
    auto* label = createLabel(parent, text, true);
    lv_obj_set_style_pad_top(label, 4, LV_PART_MAIN);
}

/** @return the standby-after dropdown index that @a minutes was picked from */
uint32_t sleepAfterIndex(uint8_t minutes) {
    for (uint32_t i = 0; i < sizeof(SLEEP_AFTER_MINUTES); i++) {
        if (SLEEP_AFTER_MINUTES[i] == minutes) {
            return i;
        }
    }
    return 0;
}

/** @return the sleep-time dropdown index that @a seconds was picked from */
uint32_t sleepTimeIndex(uint8_t seconds) {
    for (uint32_t i = 0; i < sizeof(SLEEP_TIME_SECONDS); i++) {
        if (SLEEP_TIME_SECONDS[i] == seconds) {
            return i;
        }
    }
    return 1;
}

/** @return the inactive-timeout dropdown index that @a seconds was picked from */
uint32_t vuInactiveIndex(uint16_t seconds) {
    constexpr uint32_t COUNT = sizeof(VU_INACTIVE_SECONDS) / sizeof(VU_INACTIVE_SECONDS[0]);
    for (uint32_t i = 0; i < COUNT; i++) {
        if (VU_INACTIVE_SECONDS[i] == seconds) {
            return i;
        }
    }
    return 1;
}

/**
 * A brightness slider that runs to the stage's allowance rather than to 100, so the user cannot
 * set a number the strip refuses to draw. The stored value is left alone for when the limit lifts.
 */
void createBrightnessSlider(lv_obj_t* parent, np::Stage stage, uint8_t value,
                            lv_event_cb_t callback, Context* ctx) {
    const uint8_t allowed = np::getAllowedBrightness(stage);
    createSlider(parent, "Brightness", 0, allowed, std::min(value, allowed), callback, ctx);
}

/** The colour rows a stage shares: how it picks a colour, and the colour itself when it is fixed. */
void createColorRows(lv_obj_t* parent, np::ColorMode mode, Cycle cycle,
                     void (*getColor)(uint8_t*, uint8_t*, uint8_t*),
                     lv_event_cb_t onRed, lv_event_cb_t onGreen, lv_event_cb_t onBlue, Context* ctx) {
    createCycleRow(parent, "Colour", cycle, ctx);
    // Cycle picks its own hue, so the channels below would have nothing to act on.
    if (mode != np::ColorMode::Cycle) {
        uint8_t r = 0, g = 0, b = 0;
        getColor(&r, &g, &b);
        createSlider(parent, "Red", 0, 255, r, onRed, ctx);
        createSlider(parent, "Green", 0, 255, g, onGreen, ctx);
        createSlider(parent, "Blue", 0, 255, b, onBlue, ctx);
    }
}

void buildHomePage(Context* ctx, lv_obj_t* parent) {
    createNavRow(parent, "General", Nav::General, ctx);
    createNavRow(parent, "VU", Nav::Vu, ctx);
    createResetRow(parent, ctx);
}

// The stages the badge moves through on its own. The meter is not among them: it takes the strip
// whenever music plays, whichever of these would otherwise be showing.
void buildGeneralPage(Context* ctx, lv_obj_t* parent) {
    createNavRow(parent, "Run", Nav::Active, ctx);
    createNavRow(parent, "Standby", Nav::Standby, ctx);
    createNavRow(parent, "Sleep", Nav::Sleep, ctx);

    createHeading(parent, "Standby starts on the display auto-off timeout");
    createDropdown(parent, "Sleep after", SLEEP_AFTER_OPTIONS,
        sleepAfterIndex(np::getSleepMinutes()), Choice::SleepAfter, onChoiceChanged, ctx);

    createHeading(parent, "Transitions");
    createDropdown(parent, "Wake", TRANSITION_OPTIONS, (uint32_t) np::getWakeTransition(),
        Choice::WakeTransition, onChoiceChanged, ctx);
    createDropdown(parent, "Sleep", TRANSITION_OPTIONS, (uint32_t) np::getSleepTransition(),
        Choice::SleepTransition, onChoiceChanged, ctx);

    createBackRow(parent, ctx);
}

void buildActivePage(Context* ctx, lv_obj_t* parent) {
    ctx->activeAnimationDropdown = createDropdown(parent, "Animation", ANIMATION_OPTIONS,
        (uint32_t) np::getActiveAnimation(), Choice::ActiveAnimation, onChoiceChanged, ctx);
    if (np::getActiveAnimation() != np::Animation::Off) {
        createBrightnessSlider(parent, np::Stage::Active, np::getActiveBrightness(), onActiveBrightness, ctx);
        createSlider(parent, "Speed", 1, 20, np::getActiveSpeed(), onActiveSpeed, ctx);
        createColorRows(parent, np::getActiveColorMode(), Cycle::ActiveColor, np::getActiveColor,
            onActiveRed, onActiveGreen, onActiveBlue, ctx);
    }

    createPreviewRow(parent, ctx);
    createBackRow(parent, ctx);
}

void buildStandbyPage(Context* ctx, lv_obj_t* parent) {
    createDropdown(parent, "Animation", ANIMATION_OPTIONS, (uint32_t) np::getStandbyAnimation(),
        Choice::StandbyAnimation, onChoiceChanged, ctx);
    if (np::getStandbyAnimation() != np::Animation::Off) {
        createBrightnessSlider(parent, np::Stage::Standby, np::getStandbyBrightness(), onStandbyBrightness, ctx);
        createSlider(parent, "Speed", 1, 20, np::getStandbySpeed(), onStandbySpeed, ctx);
        createColorRows(parent, np::getStandbyColorMode(), Cycle::StandbyColor, np::getStandbyColor,
            onStandbyRed, onStandbyGreen, onStandbyBlue, ctx);
    }

    createPreviewRow(parent, ctx);
    createBackRow(parent, ctx);
}

void buildSleepPage(Context* ctx, lv_obj_t* parent) {
    createDropdown(parent, "Animation", SLEEP_OPTIONS, (uint32_t) np::getSleepAnimation(),
        Choice::SleepAnimation, onChoiceChanged, ctx);
    if (np::getSleepAnimation() != np::SleepAnimation::Off) {
        createBrightnessSlider(parent, np::Stage::Sleep, np::getSleepBrightness(),
            onSleepBrightness, ctx);
        createSlider(parent, "Speed", 1, 20, np::getSleepSpeed(), onSleepSpeed, ctx);
        createColorRows(parent, np::getSleepColorMode(), Cycle::SleepColor, np::getSleepColor,
            onSleepRed, onSleepGreen, onSleepBlue, ctx);
    }

    createHeading(parent, "Wakes for a moment, then the supply is released");
    createDropdown(parent, "Delay", SLEEP_TIME_OPTIONS,
        sleepTimeIndex(np::getSleepIntervalSeconds()), Choice::SleepTime, onChoiceChanged, ctx);

    createPreviewRow(parent, ctx);
    createBackRow(parent, ctx);
}

void buildVuPage(Context* ctx, lv_obj_t* parent) {
    createBrightnessSlider(parent, np::Stage::Vu, np::getVuBrightness(), onVuBrightness, ctx);
    createSlider(parent, "Sensitivity", 0, 100, np::getVuSensitivity(), onVuSensitivity, ctx);
    createCycleRow(parent, "Pattern", Cycle::Origin, ctx);
    createCycleRow(parent, "Colour", Cycle::Palette, ctx);
    if (np::getVuPalette() == np::VuPalette::Solid) {
        uint8_t r = 0, g = 0, b = 0;
        np::getVuColor(&r, &g, &b);
        createSlider(parent, "Red", 0, 255, r, onVuRed, ctx);
        createSlider(parent, "Green", 0, 255, g, onVuGreen, ctx);
        createSlider(parent, "Blue", 0, 255, b, onVuBlue, ctx);
    }
    // Only the Cycle entries above are paced by it, so it sits with them.
    createSlider(parent, "Cycle speed", 1, 20, np::getVuSpeed(), onVuSpeed, ctx);

    createHeading(parent, "Response");
    createSwitch(parent, "Auto gain", np::isVuAutoGainEnabled(), onVuAutoGain, ctx);
    createSwitch(parent, "Beats only", np::isVuBassOnlyEnabled(), onVuBassOnly, ctx);
    createSwitch(parent, "Decay", np::isVuDecayEnabled(), onVuDecay, ctx);

    createHeading(parent, "Peak marker");
    createSwitch(parent, "Peak hold", np::isVuPeakHoldEnabled(), onVuPeakHold, ctx);

    createHeading(parent, "Gives the strip back this long after the badge was last touched");
    createDropdown(parent, "Inactive timeout", VU_INACTIVE_OPTIONS,
        vuInactiveIndex(np::getVuInactiveSeconds()), Choice::VuInactive, onChoiceChanged, ctx);

    createNavRow(parent, "Beat flash", Nav::Beat, ctx);
    createBackRow(parent, ctx);
}

void buildBeatPage(Context* ctx, lv_obj_t* parent) {
    createSwitch(parent, "Flash on beat", np::isVuBeatFlashEnabled(), onBeatEnabled, ctx);
    createCycleRow(parent, "Detect from", Cycle::BeatSource, ctx);
    createSwitch(parent, "Auto sensitivity", np::isVuBeatAutoEnabled(), onBeatAuto, ctx);
    // Nothing reads it while the threshold is being derived from the track, so showing it would
    // only invite the user to move a control that does nothing.
    if (!np::isVuBeatAutoEnabled()) {
        createSlider(parent, "Sensitivity", 0, 100, np::getVuBeatSensitivity(), onBeatSensitivity, ctx);
    }

    createHeading(parent, "Flash shape");
    createSlider(parent, "Attack", 0, 100, np::getVuFlashAttack(), onFlashAttack, ctx);
    createSlider(parent, "Decay", 0, 100, np::getVuFlashDecay(), onFlashDecay, ctx);

    createHeading(parent, "Peak marker");
    createSlider(parent, "Peak brightness", 0, 100, np::getVuPeakBrightness(), onVuPeakBrightness, ctx);

    createBackRow(parent, ctx);
}

// Switching pages rebuilds the children rather than hiding them, so no hidden widget is ever
// left behind in the focus group.
void populate(lv_obj_t* root, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->root = root;

    lv_obj_clean(root);
    ctx->activeAnimationDropdown = nullptr;
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_bg_color(root, surface(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(root, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(root, 6, LV_PART_MAIN);
    lv_obj_add_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLL_WITH_ARROW);

    switch (ctx->page) {
        case Page::General: buildGeneralPage(ctx, root); break;
        case Page::Active: buildActivePage(ctx, root); break;
        case Page::Standby: buildStandbyPage(ctx, root); break;
        case Page::Sleep: buildSleepPage(ctx, root); break;
        case Page::Vu: buildVuPage(ctx, root); break;
        case Page::Beat: buildBeatPage(ctx, root); break;
        default: buildHomePage(ctx, root); break;
    }
}

void showPage(Context* ctx, Page page) {
    ctx->page = page;
    populate(ctx->root, ctx);
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = appInstanceId;

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(appInstanceId, populate, &ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        // A bounded wait so the settings get written while the app is still open: the badge is
        // unplugged rather than shut down, so saving only on close would rarely happen at all.
        task_event_group_wait_any(&event_group, nullptr, pdMS_TO_TICKS(SETTINGS_FLUSH_MS));

        if (settingsDirty) {
            settingsDirty = false;
            np::saveSettings();
        }

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                // Back arrives as a close, not a key, so on a subpage it must mean "back to the
                // list" and only Home closes the app. Held across the ctx.page read.
                lvgl_lock();
                const bool on_subpage = ctx.page != Page::Home;
                if (on_subpage) {
                    showPage(&ctx, parentOf(ctx.page));
                }
                lvgl_unlock();

                if (!on_subpage) {
                    shouldClose = true;
                    break;
                }
            }
        }
    }

    if (settingsDirty) {
        settingsDirty = false;
        np::saveSettings();
    }

    if (ctx.rebuildTimer != nullptr) {
        lv_timer_delete(ctx.rebuildTimer);
        ctx.rebuildTimer = nullptr;
    }

    if (ctx.animationSyncTimer != nullptr) {
        lv_timer_delete(ctx.animationSyncTimer);
        ctx.animationSyncTimer = nullptr;
    }

    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);
    return 0;
}

}

extern const ::AppManifest manifest = {
    .id = "Lighting",
    .name = "Lighting",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
};

}
