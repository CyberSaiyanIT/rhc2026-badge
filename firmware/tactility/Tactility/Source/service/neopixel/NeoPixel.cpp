#include <Tactility/service/neopixel/NeoPixel.h>

#include <Tactility/service/Service.h>
#include <Tactility/service/ServiceManifest.h>
#include <Tactility/service/ServiceRegistration.h>
#include <Tactility/Tactility.h>
#include <Tactility/CpuAffinity.h>
#include <Tactility/Thread.h>
#include <tactility/device.h>
#include <tactility/drivers/power_rail.h>
#include <tactility/log.h>
#include <tactility/time.h>

#include <led_strip.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <memory>

namespace tt::service::neopixel {

constexpr auto* TAG = "NeoPixelService";

extern const ServiceManifest manifest;

namespace {

// A frame every 50 ms: fast enough for a meter to read as continuous, slow enough that the
// RMT transfer never crowds the audio task.
constexpr uint32_t RENDER_INTERVAL_MS = 25;

// The VU_* constants were tuned at 50 ms. Kept an exact multiple of the render tick, so raising
// the frame rate does not re-tune the decays, beat window and refractory period with it.
constexpr uint32_t VU_TICK_DIVIDER = 50 / RENDER_INTERVAL_MS;

/**
 * Phase advanced per frame, per unit of speed. Animations run against phase, not time, so a given
 * speed keeps the same wall-clock rate whatever the render tick is.
 */
constexpr float PHASE_PER_FRAME = (float) RENDER_INTERVAL_MS / 50.0f;

/** The middle of the 1-20 speed range as far as the patterns are concerned, and every stage's
 *  starting value. A pattern that scales itself by speed treats this as its natural rate. */
constexpr uint8_t SPEED_DEFAULT = 5;

/**
 * Wraps rather than growing: a float holds integers exactly only to 2^24. 79200 is a multiple of
 * every period the patterns divide the phase by, so nothing jumps across the wrap.
 */
constexpr float PHASE_WRAP = 79200.0f;

// The strip is two horizontal rows of 11, one per channel.
constexpr int VU_ROW_LEDS = 11;
constexpr int VU_ROWS = 2;
constexpr int VU_LEDS = VU_ROW_LEDS * VU_ROWS;
// The second row is wired back to front: without this, a single origin sends the top row left
// and the bottom row right. Positions below are always counted from the left of the badge.
constexpr bool VU_SECOND_ROW_REVERSED = true;

// Per 50 ms frame. The bar catches a transient immediately and falls away over roughly 250 ms,
// which is what makes a meter readable; the peak marker falls slower still so it can be seen.
constexpr float VU_DECAY = 0.82f;
constexpr float VU_PEAK_DECAY = 0.94f;
// Frames of silence before the strip shows that it is armed rather than simply dead.
constexpr uint32_t VU_IDLE_FRAMES = 20;
constexpr uint8_t VU_IDLE_BRIGHTNESS = 15;
// Levels arrive in dB, so a beat is a difference in counts above the average, not a ratio: at
// 255 counts across 48 dB, a 6 dB kick is only 32 counts.
constexpr float VU_BEAT_MAX_DELTA = 60.0f;
// How fast the average follows the level. Slow enough that a loud passage stops retriggering,
// fast enough to follow a change of section.
constexpr float VU_BEAT_AVERAGE_RATE = 0.06f;
// Nothing below this counts, so silence between tracks cannot trigger on its own noise.
constexpr float VU_BEAT_MIN_LEVEL = 24.0f;
// Frames to wait before another beat can fire: 400 BPM, above anything musical.
constexpr uint32_t VU_BEAT_REFRACTORY_FRAMES = 3;

// How many deviations above the running average a hit has to reach in automatic mode. Around one
// and a half is where a kick separates from the body of a mix on most material.
constexpr float VU_BEAT_AUTO_FACTOR = 1.5f;
constexpr float VU_BEAT_DEVIATION_RATE = 0.02f;
// The threshold can never collapse to nothing on a track that barely moves, nor climb past what
// a real hit could reach.
constexpr float VU_BEAT_AUTO_MIN = 6.0f;
constexpr float VU_BEAT_AUTO_MAX = 48.0f;
// The governor is judged over four seconds at 20 frames a second.
constexpr uint32_t VU_BEAT_RATE_WINDOW_FRAMES = 80;
// Roughly 60 to 210 bpm counting one hit a beat, which covers everything from a ballad to drum
// and bass without letting a rate outside it stand.
constexpr uint32_t VU_BEAT_RATE_MIN = 4;
constexpr uint32_t VU_BEAT_RATE_MAX = 14;
constexpr float VU_BEAT_GOVERNOR_STEP = 1.08f;
constexpr float VU_BEAT_GOVERNOR_MIN = 0.4f;
constexpr float VU_BEAT_GOVERNOR_MAX = 3.0f;
constexpr uint8_t VU_BEAT_BOOST_PERCENT = 60;
// Slow enough to sit at a track's loudest, so auto gain does not pump within a phrase.
constexpr float VU_AGC_DECAY = 0.995f;
constexpr float VU_AGC_MIN_PEAK = 64.0f;

/** Every pixel on the strip, however the animations choose to lay them out. */
constexpr int LED_COUNT = VU_LEDS;

// Per 50 ms frame, for the animations that leave a trail behind a moving head.
constexpr float ANIM_TAIL_DECAY = 0.72f;

// A pattern that lights no more than this many pixels at once costs little enough that a power
// limit set for a full strip does not have to apply to it at full force.
constexpr int SPARSE_LED_LIMIT = 10;
constexpr uint8_t SPARSE_HEADROOM = 50;

// A powered strip costs the boost's quiescent draw plus ~1 mA per WS2812 whether lit or not.
// Held long enough that a passing dark frame never switches the rail.
constexpr uint32_t DARK_FRAMES_BEFORE_POWER_OFF = 1000 / RENDER_INTERVAL_MS;

// Long enough to read as a gesture, short enough that it never delays what the badge is for.
constexpr uint32_t TRANSITION_FRAMES = 1200 / RENDER_INTERVAL_MS;

// Sleep shows its pattern in a short burst and leaves the supply off in between, which is where
// nearly all of its saving comes from. How long it stays off is the user's to set.
constexpr uint32_t SLEEP_BURST_FRAMES = 1200 / RENDER_INTERVAL_MS;

struct State {
    led_strip_handle_t led_strip = nullptr;
    std::unique_ptr<Thread> thread;
    std::atomic<bool> running { false };
    bool powered = false;

    Animation activeAnimation = Animation::Off;
    uint8_t activeR = 255, activeG = 255, activeB = 255;
    ColorMode activeColorMode = ColorMode::Static;
    uint8_t activeBrightness = 40;
    uint8_t activeSpeed = 5;

    Animation standbyAnimation = Animation::Off;
    uint8_t standbyR = 255, standbyG = 255, standbyB = 255;
    ColorMode standbyColorMode = ColorMode::Static;
    uint8_t standbyBrightness = 40;
    uint8_t standbySpeed = 3;

    SleepAnimation sleepAnimation = SleepAnimation::Beacon;
    uint8_t sleepR = 255, sleepG = 255, sleepB = 255;
    ColorMode sleepColorMode = ColorMode::Static;
    uint8_t sleepBrightness = 40;
    uint8_t sleepSpeed = SPEED_DEFAULT;
    uint8_t sleepIntervalSeconds = 3;

    uint8_t sleepMinutes = 5;
    bool idle = false;
    /** Frames since setIdle(true), which is what the sleep cut-off counts. */
    uint32_t idleFrames = 0;

    Transition wakeTransition = Transition::Bloom;
    Transition sleepTransition = Transition::Bloom;
    /** Remaining frames of the transition being played. Zero means none. */
    uint32_t transitionFrames = 0;
    Transition transitionPlaying = Transition::Off;
    /** Set for the sleep transition, which runs the same shape the other way round. */
    bool transitionReversed = false;

    /** Remaining preview frames, and which stage they show. Zero means no preview. */
    uint32_t previewFrames = 0;
    Stage previewStage = Stage::Active;

    /** 100 each until whoever manages power says otherwise. Never saved. */
    uint8_t brightnessLimit[(int) Stage::Count] = { 100, 100, 100, 100 };

    /** Advanced by the drawing stage's own speed, so a slow sleep pattern crawls. */
    float animPhase = 0.0f;
    /** Counts render ticks, which is what holds the meter to its own slower cadence. */
    uint32_t tickCount = 0;
    /** Per-pixel envelope for the patterns that decay rather than being a function of phase. */
    float animLevel[LED_COUNT] = {};
    /** Hue that goes with that envelope, for the patterns that colour pixels individually. */
    uint16_t animHue[LED_COUNT] = {};
    /** The frame being composed, and the last one actually sent, as RGB per pixel. */
    uint8_t frame[LED_COUNT][3] = {};
    uint8_t lastFrame[LED_COUNT][3] = {};
    bool lastFrameValid = false;
    /** Consecutive rendered frames that came out black, which is what drops the rail. */
    uint32_t darkFrames = 0;

    /** Buffers and phase are reset when the source or the pattern changes, so a pattern never
     *  starts from the trail another one left in them. */
    Stage animStage = Stage::Active;
    /** The pattern the buffers currently belong to, as a plain number so both enums fit. */
    uint8_t animShowing = 0;

    /** Written from an audio task, so the only cross-task state that is not dispatcher-owned. */
    std::atomic<uint8_t> vuAccum[VU_ROWS] = {};
    std::atomic<uint8_t> vuAccumBass = 0;
    std::atomic<uint8_t> vuAccumMid = 0;
    std::atomic<uint8_t> vuAccumTreble = 0;
    bool vuActive = false;
    VuOrigin vuOrigin = VuOrigin::Center;
    uint8_t vuBrightness = 40;
    uint8_t vuSpeed = 5;
    uint16_t vuInactiveSeconds = 1800;
    uint8_t vuR = 0, vuG = 255, vuB = 128;
    bool vuBassOnly = false;
    VuPalette vuPalette = VuPalette::Classic;
    bool vuDecay = true;
    bool vuPeakHold = true;
    bool vuBeatFlash = true;
    VuBeatSource vuBeatSource = VuBeatSource::Bass;
    uint8_t vuBeatSensitivity = 70;
    uint8_t vuFlashAttack = 0;
    uint8_t vuFlashDecay = 50;
    uint8_t vuPeakBrightness = 50;
    uint8_t vuSensitivity = 70;
    bool vuAutoGain = false;
    float vuBeatAverage = 0.0f;
    bool vuBeatAuto = true;
    /** Mean absolute distance of the beat band from its own average, which is how dynamic the
     *  current track is. */
    float vuBeatDeviation = 0.0f;
    /** Trims the deviation-derived threshold towards a musical number of hits per second. */
    float vuBeatGovernor = 1.0f;
    uint32_t vuBeatsInWindow = 0;
    uint32_t vuBeatWindowFrames = 0;
    uint32_t vuBeatRefractory = 0;
    /** Flash envelope. The target is set at the hit and falls; the level chases it at the
     *  attack rate, so a flash can swell rather than only snap on. */
    float vuFlash = 0.0f;
    float vuFlashTarget = 0.0f;
    float vuAgcPeak = 0.0f;
    /** Latest band mix, held so the colour does not flicker between frames. */
    uint8_t vuBass = 0, vuMid = 0, vuTreble = 0;
    float vuDisplay[VU_ROWS] = {};
    float vuPeak[VU_ROWS] = {};
    uint32_t vuIdleFrames = 0;
    uint32_t vuAutoPhase = 0;
    uint32_t vuAutoFrames = 0;
    uint32_t vuPalettePhase = 0;
    uint32_t vuPaletteFrames = 0;
    uint32_t vuRefreshErrors = 0;
};

State state;
bool started = false;

void renderLoop();

// Simple HSV to RGB conversion
void hsv2rgb(uint32_t h, uint32_t s, uint32_t v, uint32_t *r, uint32_t *g, uint32_t *b) {
    h %= 360; // h -> [0,360]
    uint32_t rgb_max = v * 255 / 100;
    uint32_t rgb_min = rgb_max * (100 - s) / 100;

    uint32_t i = h / 60;
    uint32_t diff = h % 60;

    uint32_t rgb_adj = (rgb_max - rgb_min) * diff / 60;

    switch (i) {
    case 0:
        *r = rgb_max;
        *g = rgb_min + rgb_adj;
        *b = rgb_min;
        break;
    case 1:
        *r = rgb_max - rgb_adj;
        *g = rgb_max;
        *b = rgb_min;
        break;
    case 2:
        *r = rgb_min;
        *g = rgb_max;
        *b = rgb_min + rgb_adj;
        break;
    case 3:
        *r = rgb_min;
        *g = rgb_max - rgb_adj;
        *b = rgb_max;
        break;
    case 4:
        *r = rgb_min + rgb_adj;
        *g = rgb_min;
        *b = rgb_max;
        break;
    default:
        *r = rgb_max;
        *g = rgb_min;
        *b = rgb_max - rgb_adj;
        break;
    }
}

/**
 * The strip sits behind a switched supply: powered before the RMT writes, released after the
 * last. Boards without such a supply have no rail device to look up.
 */
bool setPowered(bool powered) {
    if (powered == state.powered) return true;

    Device* device;
    if (device_get_by_name("neopixel_power", &device) != ERROR_NONE) {
        state.powered = powered;
        return true;
    }

    const bool switched = (powered ? power_rail_enable(device) : power_rail_disable(device)) == ERROR_NONE;
    if (switched) {
        state.powered = powered;
    } else {
        LOG_E(TAG, "Failed to switch NeoPixel supply %s", powered ? "on" : "off");
    }
    device_put(device);
    return switched;
}

/**
 * A pattern gives up rather than retrying every frame when the supply refuses, dropping its own
 * stage to Off so Lighting reports the dark strip. Only its own: standby must not clear another.
 */
bool ensurePowered(Stage stage) {
    if (setPowered(true)) {
        return true;
    }
    switch (stage) {
        case Stage::Active: state.activeAnimation = Animation::Off; break;
        case Stage::Standby: state.standbyAnimation = Animation::Off; break;
        case Stage::Sleep: state.sleepAnimation = SleepAnimation::Off; break;
        default: state.vuActive = false; break;
    }
    return false;
}

/** @return whether @a animation lights few enough pixels to earn headroom over a power limit */
bool animation_is_sparse(Animation animation) {
    switch (animation) {
        // Every third pixel, a couple of decaying pixels, and a couple of random ones: all well
        // inside SPARSE_LED_LIMIT. Comet and Scanner are not, their tails run to seven a row.
        case Animation::TheaterChase:
        case Animation::Twinkle:
        case Animation::Confetti:
            return true;
        default:
            return false;
    }
}

/** @return the pattern @a stage would draw, and whether that pattern is a sparse one */
bool stage_is_sparse(Stage stage) {
    switch (stage) {
        case Stage::Active: return animation_is_sparse(state.activeAnimation);
        case Stage::Standby: return animation_is_sparse(state.standbyAnimation);
        // Every sleep pattern is built to light a handful of pixels; that is what it is for.
        case Stage::Sleep: return true;
        // The bar fills, so the meter never qualifies however quiet the music is.
        default: return false;
    }
}

uint8_t allowed_brightness(Stage stage) {
    const uint8_t limit = state.brightnessLimit[(int) stage];
    if (limit >= 100 || !stage_is_sparse(stage)) {
        return limit;
    }
    return (uint8_t) std::min(100, (int) limit + SPARSE_HEADROOM);
}

/** @return @a brightness held to what @a stage is currently allowed to draw */
uint8_t limited_brightness(Stage stage, uint8_t brightness) {
    return std::min(brightness, allowed_brightness(stage));
}

int vu_led_index(int row, int position) {
    const int offset = (row == 1 && VU_SECOND_ROW_REVERSED) ? (VU_ROW_LEDS - 1 - position) : position;
    return row * VU_ROW_LEDS + offset;
}

/** Where one row grows from, once Auto and the opposed layouts have been resolved. */
enum class RowOrigin : uint8_t { Left, Right, Center };

/** @return how far up the scale a position sits, and how many ranks the scale has */
int vu_rank(RowOrigin origin, int position, int* out_max_rank) {
    switch (origin) {
        case RowOrigin::Left:
            *out_max_rank = VU_ROW_LEDS - 1;
            return position;
        case RowOrigin::Right:
            *out_max_rank = VU_ROW_LEDS - 1;
            return VU_ROW_LEDS - 1 - position;
        case RowOrigin::Center:
        default:
            // Grows outwards from the middle pixel, so a rank covers a pair.
            *out_max_rank = VU_ROW_LEDS / 2;
            return std::abs(position - VU_ROW_LEDS / 2);
    }
}

// The fixed palettes, which are also what the cycle steps through.
constexpr uint32_t VU_PALETTE_PHASES = 4;

VuPalette vu_effective_palette() {
    if (state.vuPalette != VuPalette::Cycle) {
        return state.vuPalette;
    }
    const auto phase = (VuPalette) state.vuPalettePhase;
    // Solid paints the Colours page's colour, which starts black, so cycling into it unchosen
    // would leave the strip dark for a quarter of the rotation.
    if (phase == VuPalette::Solid && (state.vuR | state.vuG | state.vuB) == 0) {
        return VuPalette::Classic;
    }
    return phase;
}

void vu_segment_color(float fraction, uint32_t* r, uint32_t* g, uint32_t* b) {
    switch (vu_effective_palette()) {
        case VuPalette::Solid:
            *r = state.vuR;
            *g = state.vuG;
            *b = state.vuB;
            break;
        case VuPalette::Rainbow:
            // Only the first 300 degrees, so the top of the bar does not wrap back to red-ish.
            hsv2rgb((uint32_t) (fraction * 300.0f), 100, 100, r, g, b);
            break;
        case VuPalette::Spectrum: {
            const uint32_t total = (uint32_t) state.vuBass + state.vuMid + state.vuTreble;
            if (total == 0) {
                *r = *g = *b = 0;
                break;
            }
            // Where the energy sits: 0 all bass, 1 all treble. Drives a hue rather than mixing the bands as
            // primaries, which would mix to white since each band is already on a dB scale.
            const float centroid = (0.5f * (float) state.vuMid + (float) state.vuTreble) / (float) total;
            // Real material keeps that centroid in a narrow band around the middle, so it is
            // stretched before becoming a hue; without this every track is the same green.
            const float tilt = std::clamp((centroid - 0.25f) / 0.5f, 0.0f, 1.0f);
            hsv2rgb((uint32_t) (tilt * 240.0f), 100, 100, r, g, b);
            break;
        }
        case VuPalette::Classic:
        default:
            // A continuous 120-to-0 degree sweep rather than three bands: on eleven pixels,
            // stepped colours read as three blocks instead of a gradient.
            hsv2rgb((uint32_t) ((1.0f - fraction) * 120.0f), 100, 100, r, g, b);
            break;
    }
}

// The two opposed layouts, where the rows run from facing ends, appear only while cycling.
constexpr uint32_t VU_AUTO_PHASES = 5;

void vu_advance_auto() {
    // Speed 1 is a change roughly every 10 s, 20 is every half second.
    const uint32_t period = std::max<uint32_t>(10, 200 / std::max<uint8_t>(state.vuSpeed, 1));

    if (state.vuOrigin == VuOrigin::Cycle && ++state.vuAutoFrames >= period) {
        state.vuAutoFrames = 0;
        state.vuAutoPhase = (state.vuAutoPhase + 1) % VU_AUTO_PHASES;
    }

    // Held three times as long: a bar that changes shape and colour on the same frame reads as a
    // glitch rather than as two effects.
    if (state.vuPalette == VuPalette::Cycle && ++state.vuPaletteFrames >= period * 3) {
        state.vuPaletteFrames = 0;
        state.vuPalettePhase = (state.vuPalettePhase + 1) % VU_PALETTE_PHASES;
    }
}

RowOrigin vu_row_origin(int row) {
    switch (state.vuOrigin) {
        case VuOrigin::BothLeft: return RowOrigin::Left;
        case VuOrigin::BothRight: return RowOrigin::Right;
        case VuOrigin::Center: return RowOrigin::Center;
        case VuOrigin::Cycle:
        default:
            switch (state.vuAutoPhase) {
                case 0: return RowOrigin::Left;
                case 1: return RowOrigin::Center;
                case 2: return RowOrigin::Right;
                case 3: return row == 0 ? RowOrigin::Left : RowOrigin::Right;
                default: return row == 0 ? RowOrigin::Right : RowOrigin::Left;
            }
    }
}

void setPixelScaled(int index, uint32_t r, uint32_t g, uint32_t b, uint8_t scale) {
    state.frame[index][0] = (uint8_t) (r * scale / 100);
    state.frame[index][1] = (uint8_t) (g * scale / 100);
    state.frame[index][2] = (uint8_t) (b * scale / 100);
}

/**
 * Never led_strip_clear() before a real frame: it transmits a black frame first, visible as a
 * flicker. commitFrame() writes every pixel, so one transmission carries the whole picture.
 */
void stripRefresh() {
    const esp_err_t error = led_strip_refresh(state.led_strip);
    if (error != ESP_OK) {
        state.vuRefreshErrors++;
        // Rate-limited: a failing RMT channel would otherwise fill the log 20 times a second.
        if (state.vuRefreshErrors % 100 == 1) {
            LOG_W(TAG, "led_strip_refresh failed (%d so far): %s",
                (int) state.vuRefreshErrors, esp_err_to_name(error));
        }
    }
}

/** Starts a frame, so a renderer that leaves a pixel alone leaves it dark rather than stale. */
void beginFrame() {
    memset(state.frame, 0, sizeof(state.frame));
}

/**
 * Takes the strip off its supply. The clear goes out first: afterwards there is nothing to
 * transmit into, and the strip would come back up holding the last frame.
 */
void powerDown() {
    if (state.powered) {
        led_strip_clear(state.led_strip);
        setPowered(false);
    }
    state.lastFrameValid = false;
}

/**
 * Sends the composed frame, and the only place the supply is switched for a drawn frame. A frame
 * that stays black is worth dropping the rail for rather than transmitting for ever.
 */
void commitFrame(Stage stage) {
    bool lit = false;
    for (int i = 0; i < LED_COUNT && !lit; i++) {
        lit = (state.frame[i][0] | state.frame[i][1] | state.frame[i][2]) != 0;
    }

    if (lit) {
        state.darkFrames = 0;
    } else if (state.darkFrames < DARK_FRAMES_BEFORE_POWER_OFF) {
        state.darkFrames++;
    } else {
        // Left latched rather than reset, so this settles instead of switching the rail once a
        // second for as long as the frame stays black.
        powerDown();
        return;
    }

    if (!ensurePowered(stage)) {
        // What the strip holds is unknown after a supply that refused, so no later frame may be
        // skipped against it.
        state.lastFrameValid = false;
        return;
    }

    // A WS2812 holds what it was last given, so an unchanged frame would cost a blocking
    // transmission and its refill interrupts for no visible difference.
    if (state.lastFrameValid && memcmp(state.frame, state.lastFrame, sizeof(state.frame)) == 0) {
        return;
    }

    for (int i = 0; i < LED_COUNT; i++) {
        led_strip_set_pixel(state.led_strip, i, state.frame[i][0], state.frame[i][1], state.frame[i][2]);
    }
    stripRefresh();
    memcpy(state.lastFrame, state.frame, sizeof(state.frame));
    state.lastFrameValid = true;
}

/** Expands the part of the range the user cares about across the whole bar. */
float vu_shape(float level) {
    const float floor_counts = (float) (100 - state.vuSensitivity) * 1.5f;
    if (level <= floor_counts) {
        return 0.0f;
    }
    return (level - floor_counts) * 255.0f / (255.0f - floor_counts);
}

float vu_beat_source_level(float mix, uint8_t bass, uint8_t treble) {
    switch (state.vuBeatSource) {
        case VuBeatSource::Bass: return (float) bass;
        case VuBeatSource::Treble: return (float) treble;
        case VuBeatSource::Mix:
        default: return mix;
    }
}

/**
 * The threshold a hit has to clear, taken from how far the band normally strays from its own
 * average on this track and trimmed by how often that has been firing.
 */
float vu_auto_beat_delta() {
    return std::clamp(state.vuBeatDeviation * VU_BEAT_AUTO_FACTOR * state.vuBeatGovernor,
        VU_BEAT_AUTO_MIN, VU_BEAT_AUTO_MAX);
}

// Nudged once a window rather than continuously: reacting to a single frame's hit count would
// chase the bar in and out of the beat instead of settling on the track's own rate.
void vu_advance_beat_governor() {
    if (++state.vuBeatWindowFrames < VU_BEAT_RATE_WINDOW_FRAMES) {
        return;
    }

    if (state.vuBeatsInWindow > VU_BEAT_RATE_MAX) {
        state.vuBeatGovernor *= VU_BEAT_GOVERNOR_STEP;
    } else if (state.vuBeatsInWindow < VU_BEAT_RATE_MIN) {
        state.vuBeatGovernor /= VU_BEAT_GOVERNOR_STEP;
    }
    state.vuBeatGovernor = std::clamp(state.vuBeatGovernor, VU_BEAT_GOVERNOR_MIN, VU_BEAT_GOVERNOR_MAX);

    state.vuBeatsInWindow = 0;
    state.vuBeatWindowFrames = 0;
}

void renderVu() {
    beginFrame();

    // Drained every frame whether or not they drive the bar, so switching the bass-only toggle
    // never starts from a value left over from before.
    const float channel[VU_ROWS] = {
        (float) state.vuAccum[0].exchange(0, std::memory_order_relaxed),
        (float) state.vuAccum[1].exchange(0, std::memory_order_relaxed),
    };
    state.vuBass = state.vuAccumBass.exchange(0, std::memory_order_relaxed);
    state.vuMid = state.vuAccumMid.exchange(0, std::memory_order_relaxed);
    state.vuTreble = state.vuAccumTreble.exchange(0, std::memory_order_relaxed);

    bool signal = false;
    float shaped[VU_ROWS] = {};
    float loudest = 0.0f;
    for (int row = 0; row < VU_ROWS; row++) {
        // Bass-only puts the same band on both rows: what is left is a beat, not a stereo image.
        const float raw = state.vuBassOnly ? (float) state.vuBass : channel[row];
        if (channel[row] > 0.0f) {
            signal = true;
        }
        shaped[row] = vu_shape(raw);
        loudest = std::max(loudest, shaped[row]);
    }

    // One gain for both rows, so rescaling never invents a stereo image that is not there.
    state.vuAgcPeak = std::max(loudest, state.vuAgcPeak * VU_AGC_DECAY);
    const float gain = state.vuAutoGain ? 255.0f / std::max(state.vuAgcPeak, VU_AGC_MIN_PEAK) : 1.0f;

    for (int row = 0; row < VU_ROWS; row++) {
        const float level = std::clamp(shaped[row] * gain, 0.0f, 255.0f);
        state.vuDisplay[row] = state.vuDecay ? std::max(level, state.vuDisplay[row] * VU_DECAY) : level;
        state.vuPeak[row] = std::max(state.vuDisplay[row], state.vuPeak[row] * VU_PEAK_DECAY);
    }

    const float beat_level = vu_beat_source_level(loudest, state.vuBass, state.vuTreble);
    const float beat_delta = state.vuBeatAuto
        ? vu_auto_beat_delta()
        : (float) (100 - state.vuBeatSensitivity) / 100.0f * VU_BEAT_MAX_DELTA;

    if (state.vuBeatRefractory > 0) {
        state.vuBeatRefractory--;
    }
    if (state.vuBeatFlash && state.vuBeatRefractory == 0 &&
        beat_level > state.vuBeatAverage + beat_delta && beat_level > VU_BEAT_MIN_LEVEL) {
        state.vuFlashTarget = 1.0f;
        state.vuBeatRefractory = VU_BEAT_REFRACTORY_FRAMES;
        state.vuBeatsInWindow++;
    }

    // Measured against the average from before this frame, so a hit widens the deviation that
    // will judge the next one rather than the one it just passed.
    state.vuBeatDeviation += (std::abs(beat_level - state.vuBeatAverage) - state.vuBeatDeviation) *
        VU_BEAT_DEVIATION_RATE;
    // Chases the level in both directions rather than following peaks down slowly: a peak
    // follower parks at the last hit and swallows the ones after it.
    state.vuBeatAverage += (beat_level - state.vuBeatAverage) * VU_BEAT_AVERAGE_RATE;
    vu_advance_beat_governor();

    // 0 falls away in a frame or two, 100 over about a second.
    const float flash_decay = 0.35f + (float) state.vuFlashDecay / 100.0f * 0.60f;
    state.vuFlashTarget *= flash_decay;
    // 1.0 reaches the target in one frame, 0.15 swells into it over roughly a third of a second.
    const float flash_attack = 1.0f - (float) state.vuFlashAttack / 100.0f * 0.85f;
    state.vuFlash += (state.vuFlashTarget - state.vuFlash) * flash_attack;

    // Limited after the boost, not before: a beat flash is the loudest thing the meter does and
    // is exactly what a power limit is there to hold down.
    const uint8_t brightness = limited_brightness(Stage::Vu, (uint8_t) std::min(100.0f,
        (float) state.vuBrightness + state.vuFlash * (float) VU_BEAT_BOOST_PERCENT));

    state.vuIdleFrames = signal ? 0 : state.vuIdleFrames + 1;
    vu_advance_auto();

    // Nothing arriving: show the origin dimly, so a strip in VU mode with no audio reads as
    // waiting rather than broken.
    if (state.vuIdleFrames > VU_IDLE_FRAMES) {
        for (int row = 0; row < VU_ROWS; row++) {
            const RowOrigin row_origin = vu_row_origin(row);
            for (int position = 0; position < VU_ROW_LEDS; position++) {
                int max_rank = 0;
                const bool at_origin = vu_rank(row_origin, position, &max_rank) == 0;
                setPixelScaled(vu_led_index(row, position), 40, 40, 40,
                    at_origin ? (uint8_t) (VU_IDLE_BRIGHTNESS * brightness / 100) : 0);
            }
        }
        commitFrame(Stage::Vu);
        return;
    }

    for (int row = 0; row < VU_ROWS; row++) {
        const RowOrigin row_origin = vu_row_origin(row);
        int max_rank = 0;
        vu_rank(row_origin, 0, &max_rank);
        const int ranks = max_rank + 1;
        // Kept fractional: eleven pixels per row is a coarse scale, and dimming the leading one
        // by the remainder is what makes it read as a smooth bar rather than a staircase.
        const float lit = state.vuDisplay[row] * (float) ranks / 256.0f;
        const int full = (int) lit;
        const uint8_t leading = (uint8_t) ((lit - (float) full) * 100.0f);
        const int peak_rank = (int) (state.vuPeak[row] * (float) ranks / 256.0f) - 1;

        for (int position = 0; position < VU_ROW_LEDS; position++) {
            const int rank = vu_rank(row_origin, position, &max_rank);
            const float fraction = max_rank > 0 ? (float) rank / (float) max_rank : 0.0f;
            uint32_t r = 0, g = 0, b = 0;
            uint8_t scale = brightness;

            if (rank < full) {
                vu_segment_color(fraction, &r, &g, &b);
            } else if (rank == full && leading > 0) {
                vu_segment_color(fraction, &r, &g, &b);
                scale = (uint8_t) (brightness * leading / 100);
            } else if (state.vuPeakHold && state.vuPeakBrightness > 0 && rank == peak_rank) {
                // White, not the bar's colour: a peak marker has to be picked out at a glance.
                // It outlives the bar by design, so on quiet passages it is the only pixel lit.
                r = 255; g = 255; b = 255;
                scale = (uint8_t) (brightness * state.vuPeakBrightness / 100);
            } else {
                // Written dark rather than skipped: see commitFrame() on why nothing is cleared.
                scale = 0;
            }
            setPixelScaled(vu_led_index(row, position), r, g, b, scale);
        }
    }

    commitFrame(Stage::Vu);
}

constexpr float TWO_PI = 6.28318531f;

/**
 * Where a breath bottoms out instead of reaching black. A cosine dwells longest at its trough,
 * where 1, 2, 3 are each near doublings of light; at 8/255 one step is a tenth of the light.
 */
constexpr float BREATH_FLOOR = 8.0f / 255.0f;

/** A smooth swell and fall, one full cycle per @a period phase units. */
float anim_breath(float phase, float period) {
    const float swell = 0.5f - 0.5f * std::cos(phase * TWO_PI / period);
    return BREATH_FLOOR + (1.0f - BREATH_FLOOR) * swell;
}

/** Advances the phase for one frame and @return it truncated, for the patterns that step pixels. */
uint32_t anim_advance(uint8_t speed) {
    state.animPhase += (float) speed * PHASE_PER_FRAME;
    if (state.animPhase >= PHASE_WRAP) {
        state.animPhase -= PHASE_WRAP;
    }
    return (uint32_t) state.animPhase;
}

/**
 * Scaled straight to the channel value: a whole-percent scale first would leave a stage set to
 * 25% only 26 levels to fade across.
 *
 * @param level 0 to 1, on top of the stage's own brightness
 */
void setPixelLevel(int index, uint32_t r, uint32_t g, uint32_t b, float level, uint8_t brightness) {
    const float scale = std::clamp(level, 0.0f, 1.0f) * (float) brightness / 100.0f;
    state.frame[index][0] = (uint8_t) ((float) r * scale + 0.5f);
    state.frame[index][1] = (uint8_t) ((float) g * scale + 0.5f);
    state.frame[index][2] = (uint8_t) ((float) b * scale + 0.5f);
}

/** How far behind the head of a trail the last still-lit pixel sits. */
constexpr float ANIM_TAIL_LENGTH = 6.0f;

/** How bright a pixel @a distance behind the head of a trail sits. */
float anim_tail(int distance) {
    return (distance < 0 || (float) distance > ANIM_TAIL_LENGTH) ? 0.0f : std::pow(0.55f, (float) distance);
}

/**
 * The same trail, for a head that can sit between pixels. Fading the pixel the head leaves while
 * raising the one it arrives at turns anim_tail()'s whole-pixel steps into movement.
 *
 * @param distance positions behind the head, negative for the pixel it is arriving at
 */
float anim_tail_smooth(float distance) {
    if (distance < -1.0f || distance > ANIM_TAIL_LENGTH) {
        return 0.0f;
    }
    return distance < 0.0f ? 1.0f + distance : std::pow(0.55f, distance);
}

float anim_random() { return (float) (rand() % 1000) / 1000.0f; }

void anim_reset() {
    state.animPhase = 0.0f;
    for (int i = 0; i < LED_COUNT; i++) {
        state.animLevel[i] = 0.0f;
        state.animHue[i] = 0;
    }
}

/** Heat 0 to 1 as black through red and yellow to white. */
void anim_fire_color(float heat, uint32_t* r, uint32_t* g, uint32_t* b) {
    *r = (uint32_t) (std::clamp(heat * 3.0f, 0.0f, 1.0f) * 255.0f);
    *g = (uint32_t) (std::clamp(heat * 3.0f - 1.0f, 0.0f, 1.0f) * 255.0f);
    *b = (uint32_t) (std::clamp(heat * 3.0f - 2.0f, 0.0f, 1.0f) * 255.0f);
}

/** Everything a stage hands the renderer, so the four of them differ only in their values. */
struct RenderParams {
    uint8_t r, g, b;
    ColorMode colorMode;
    uint8_t brightness;
    uint8_t speed;
};

/**
 * The colour a pattern paints with this frame. Cycle rotates a hue, which is what gives patterns
 * with no colours of their own something to change; those that paint their own ignore it.
 */
void resolve_color(const RenderParams& params, uint32_t phase, uint32_t* r, uint32_t* g, uint32_t* b) {
    if (params.colorMode == ColorMode::Cycle) {
        hsv2rgb((phase / 4) % 360, 100, 100, r, g, b);
    } else {
        *r = params.r;
        *g = params.g;
        *b = params.b;
    }
}

/**
 * Draws one frame of @a animation in the showing stage's colour and brightness. Every pixel is
 * written each frame, dark ones included; see commitFrame().
 */
void renderAnimation(Stage stage, Animation animation, const RenderParams& params) {
    beginFrame();
    const uint32_t phase = anim_advance(params.speed);
    const uint8_t brightness = limited_brightness(stage, params.brightness);
    const uint8_t speed = params.speed;
    uint32_t color_r = 0, color_g = 0, color_b = 0;
    resolve_color(params, phase, &color_r, &color_g, &color_b);
    const uint32_t cr = color_r, cg = color_g, cb = color_b;

    switch (animation) {
        case Animation::Breathing:
        case Animation::Pulse:
        case Animation::Solid: {
            float level = 1.0f;
            if (animation == Animation::Breathing) {
                // A cosine rather than a triangle: a ramp reversing in one frame reads as mechanical. Unrounded
                // phase, so the swell gains resolution as the frame rate rises.
                level = anim_breath(state.animPhase, 400.0f);
            } else if (animation == Animation::Pulse) {
                // Two beats close together and then a rest, the way a heart monitor reads.
                const float beat = std::fmod(state.animPhase, 200.0f) / 2.0f;
                level = 0.05f;
                if (beat < 12.0f) {
                    level = 1.0f - beat / 12.0f;
                } else if (beat >= 20.0f && beat < 32.0f) {
                    level = 0.7f * (1.0f - (beat - 20.0f) / 12.0f);
                }
            }
            for (int i = 0; i < LED_COUNT; i++) {
                setPixelLevel(i, cr, cg, cb, level, brightness);
            }
            break;
        }

        case Animation::RainbowCycle: {
            uint32_t r = 0, g = 0, b = 0;
            hsv2rgb(phase % 360, 100, 100, &r, &g, &b);
            for (int i = 0; i < LED_COUNT; i++) {
                setPixelLevel(i, r, g, b, 1.0f, brightness);
            }
            break;
        }

        case Animation::Rainbow: {
            if (params.colorMode == ColorMode::Cycle) {
                // One wheel over the raw strip order, not per row. The second row is wired back to front, so the
                // hue runs out along one row and back along the other.
                for (int i = 0; i < LED_COUNT; i++) {
                    uint32_t r = 0, g = 0, b = 0;
                    hsv2rgb((phase + i * 360 / LED_COUNT) % 360, 100, 100, &r, &g, &b);
                    setPixelLevel(i, r, g, b, 1.0f, brightness);
                }
                break;
            }
            for (int position = 0; position < VU_ROW_LEDS; position++) {
                uint32_t r = 0, g = 0, b = 0;
                hsv2rgb((phase + position * 360 / VU_ROW_LEDS) % 360, 100, 100, &r, &g, &b);
                for (int row = 0; row < VU_ROWS; row++) {
                    setPixelLevel(vu_led_index(row, position), r, g, b, 1.0f, brightness);
                }
            }
            break;
        }

        case Animation::Comet:
        case Animation::Scanner:
        case Animation::TheaterChase:
        case Animation::Wave:
        case Animation::Alternate: {
            // The head of a trail, and how far behind it a position sits, both counted in
            // positions from the left of the badge so the two rows move together.
            constexpr uint32_t SCAN_SPAN = VU_ROW_LEDS * 2 - 2;
            const uint32_t scan_step = (phase / 4) % SCAN_SPAN;
            const int head = (int) (scan_step < VU_ROW_LEDS ? scan_step : SCAN_SPAN - scan_step);
            // Taken from the unrounded phase, because a comet is the one trail here whose head
            // moves at a constant rate and so shows every step it takes.
            const float comet_head = std::fmod(state.animPhase / 4.0f, (float) VU_ROW_LEDS);
            const int chase_offset = (int) ((phase / 6) % 3);
            const int lit_row = (int) ((phase / 12) % VU_ROWS);

            for (int position = 0; position < VU_ROW_LEDS; position++) {
                float level = 0.0f;
                switch (animation) {
                    case Animation::Comet: {
                        float distance = (float) position - comet_head;
                        // The tail follows the head round the end of the row.
                        if (distance < -1.0f) {
                            distance += (float) VU_ROW_LEDS;
                        }
                        level = anim_tail_smooth(distance);
                        break;
                    }
                    case Animation::Scanner:
                        level = anim_tail(std::abs(position - head));
                        break;
                    case Animation::TheaterChase:
                        level = (position + chase_offset) % 3 == 0 ? 1.0f : 0.0f;
                        break;
                    case Animation::Wave:
                        // Its rate as a phase period rather than a bare multiplier, so it lands
                        // on a whole number of cycles at PHASE_WRAP like everything else.
                        level = (std::sin((float) position * 0.6f -
                            TWO_PI * state.animPhase / 200.0f) + 1.0f) / 2.0f;
                        break;
                    default:
                        break;
                }
                for (int row = 0; row < VU_ROWS; row++) {
                    if (animation == Animation::Alternate) {
                        level = row == lit_row ? 1.0f : 0.0f;
                    }
                    setPixelLevel(vu_led_index(row, position), cr, cg, cb, level, brightness);
                }
            }
            break;
        }

        case Animation::ColorWipe: {
            // One sweep fills the rows, the next empties them, and the hue moves on between.
            constexpr uint32_t SPAN = VU_ROW_LEDS * 2;
            const uint32_t step = (phase / 5) % SPAN;
            uint32_t r = 0, g = 0, b = 0;
            hsv2rgb((phase / 5 / SPAN * 60) % 360, 100, 100, &r, &g, &b);
            for (int position = 0; position < VU_ROW_LEDS; position++) {
                const bool lit = step < VU_ROW_LEDS
                    ? position <= (int) step
                    : position > (int) (step - VU_ROW_LEDS);
                for (int row = 0; row < VU_ROWS; row++) {
                    setPixelLevel(vu_led_index(row, position), r, g, b, lit ? 1.0f : 0.0f, brightness);
                }
            }
            break;
        }

        case Animation::Sparkle:
        case Animation::Twinkle:
        case Animation::Confetti: {
            // Decay first, then seed: a pixel lit this frame reaches the strip at full.
            const float decay = animation == Animation::Sparkle ? 0.80f : 0.92f;
            for (int i = 0; i < LED_COUNT; i++) {
                state.animLevel[i] *= decay;
            }
            if (anim_random() * 100.0f < (float) speed * 4.0f) {
                const int i = rand() % LED_COUNT;
                state.animLevel[i] = 1.0f;
                state.animHue[i] = (uint16_t) (rand() % 360);
            }
            // Sparkle keeps a bed lit so the strip reads as on between flashes; the other two
            // are meant to sit on black.
            const float bed = animation == Animation::Sparkle ? 0.06f : 0.0f;
            for (int i = 0; i < LED_COUNT; i++) {
                uint32_t r = cr, g = cg, b = cb;
                if (animation == Animation::Confetti) {
                    hsv2rgb(state.animHue[i], 100, 100, &r, &g, &b);
                }
                setPixelLevel(i, r, g, b, std::max(bed, state.animLevel[i]), brightness);
            }
            break;
        }

        case Animation::Fire: {
            for (int row = 0; row < VU_ROWS; row++) {
                float* heat = &state.animLevel[row * VU_ROW_LEDS];
                for (int position = 0; position < VU_ROW_LEDS; position++) {
                    heat[position] = std::max(0.0f, heat[position] - anim_random() * 0.06f - 0.02f);
                }
                // Drifts away from the left end, which is where the sparks land.
                for (int position = VU_ROW_LEDS - 1; position >= 2; position--) {
                    heat[position] = (heat[position - 1] * 2.0f + heat[position - 2]) / 3.0f;
                }
                if (anim_random() < 0.35f) {
                    const int position = rand() % 2;
                    heat[position] = std::min(1.0f, heat[position] + 0.6f + anim_random() * 0.4f);
                }
                for (int position = 0; position < VU_ROW_LEDS; position++) {
                    uint32_t r = 0, g = 0, b = 0;
                    anim_fire_color(heat[position], &r, &g, &b);
                    setPixelLevel(vu_led_index(row, position), r, g, b, 1.0f, brightness);
                }
            }
            break;
        }

        case Animation::Off:
        default:
            break;
    }

    commitFrame(stage);
}

/**
 * Draws one frame of @a animation. Standby lights a handful of pixels at most.
 *
 * @param burstFrame how far into this wake the strip is, in frames
 * @param burstLength how long a wake lasts, in frames
 * @param wake how many times the strip has woken
 */
void renderSleep(SleepAnimation animation, const RenderParams& params,
                 uint32_t burstFrame, uint32_t burstLength, uint32_t wake) {
    beginFrame();
    const uint32_t phase = anim_advance(params.speed);
    const uint8_t brightness = limited_brightness(Stage::Sleep, params.brightness);
    uint32_t cr = 0, cg = 0, cb = 0;
    resolve_color(params, phase, &cr, &cg, &cb);

    float level[LED_COUNT] = {};

    switch (animation) {
        case SleepAnimation::Beacon: {
            // One swell and fall per wake, taking the burst as the period. A fixed period only gets as far
            // through the cycle as the burst is long.
            const float breath = anim_breath((float) burstFrame, (float) std::max<uint32_t>(burstLength, 1));
            for (int row = 0; row < VU_ROWS; row++) {
                level[vu_led_index(row, VU_ROW_LEDS / 2)] = breath;
            }
            break;
        }
        case SleepAnimation::Dot: {
            const int position = (int) ((phase / 6) % VU_ROW_LEDS);
            for (int row = 0; row < VU_ROWS; row++) {
                level[vu_led_index(row, position)] = 1.0f;
            }
            break;
        }
        case SleepAnimation::Sweep: {
            constexpr uint32_t SPAN = VU_ROW_LEDS * 2 - 2;
            const uint32_t step = (phase / 6) % SPAN;
            const int position = (int) (step < VU_ROW_LEDS ? step : SPAN - step);
            for (int row = 0; row < VU_ROWS; row++) {
                level[vu_led_index(row, position)] = 1.0f;
            }
            break;
        }
        case SleepAnimation::Twinkle: {
            for (int i = 0; i < LED_COUNT; i++) {
                state.animLevel[i] *= 0.92f;
            }
            if (anim_random() * 100.0f < (float) params.speed * 2.0f) {
                state.animLevel[rand() % LED_COUNT] = 1.0f;
            }
            for (int i = 0; i < LED_COUNT; i++) {
                level[i] = state.animLevel[i];
            }
            break;
        }
        case SleepAnimation::Ends: {
            const int position = ((phase / 10) % 2) ? VU_ROW_LEDS - 1 : 0;
            for (int row = 0; row < VU_ROWS; row++) {
                level[vu_led_index(row, position)] = 1.0f;
            }
            break;
        }
        case SleepAnimation::Pulse: {
            // Two short flashes near the start of the wake and nothing after, so most of even the
            // lit part of the cycle costs only the supply being up.
            constexpr uint32_t FLASH = 100 / RENDER_INTERVAL_MS;
            const bool lit = burstFrame < FLASH || (burstFrame >= FLASH * 3 && burstFrame < FLASH * 4);
            for (int row = 0; row < VU_ROWS; row++) {
                level[vu_led_index(row, VU_ROW_LEDS / 2)] = lit ? 1.0f : 0.0f;
            }
            break;
        }
        case SleepAnimation::Comet: {
            // One crossing per wake rather than a loop. The head runs past the last pixel by its tail
            // length, so the comet leaves the row instead of stopping on it.
            constexpr float SPAN = (float) VU_ROW_LEDS + ANIM_TAIL_LENGTH;
            const float progress = (float) burstFrame / (float) std::max<uint32_t>(burstLength, 1);
            // Speed is a multiple of that one crossing: below the default the wake ends with the
            // comet still on the strip, above it the crossing repeats.
            const float head = progress * SPAN * ((float) params.speed / (float) SPEED_DEFAULT);
            for (int row = 0; row < VU_ROWS; row++) {
                for (int position = 0; position < VU_ROW_LEDS; position++) {
                    level[vu_led_index(row, position)] = anim_tail_smooth(head - (float) position);
                }
            }
            break;
        }
        case SleepAnimation::Drift: {
            const int position = (int) (wake % VU_ROW_LEDS);
            for (int row = 0; row < VU_ROWS; row++) {
                level[vu_led_index(row, position)] = 1.0f;
            }
            break;
        }
        case SleepAnimation::Spark: {
            // Hashed from the wake count rather than drawn from rand(), so the pixel holds still
            // for the whole wake instead of jumping about within it.
            const uint32_t hash = wake * 2654435761u;
            const float fade = 1.0f - (float) burstFrame / (float) std::max<uint32_t>(burstLength, 1);
            level[hash % LED_COUNT] = fade;
            break;
        }
        case SleepAnimation::Off:
        default:
            break;
    }

    for (int i = 0; i < LED_COUNT; i++) {
        setPixelLevel(i, cr, cg, cb, level[i], brightness);
    }
    commitFrame(Stage::Sleep);
}

/** A soft boundary: fully lit inside, dark outside, ramping across one pixel between. */
float anim_edge(float distance) {
    return std::clamp(1.0f - distance, 0.0f, 1.0f);
}

/**
 * Draws one frame of a transition. Shape and level are separate and both shape edges ramp over a
 * pixel: across eleven positions a hard edge steps, a soft one sweeps.
 *
 * @param progress 0 at the start of the gesture, 1 at its end, already reversed for sleep
 */
void renderTransition(Transition transition, float progress, const RenderParams& params) {
    beginFrame();
    // The Active stage's ceiling for both directions: a transition is the moment the badge is
    // being looked at, so it is not held down to whatever the stage it hands over to sits at.
    const uint8_t brightness = limited_brightness(Stage::Active, state.activeBrightness);
    uint32_t cr = 0, cg = 0, cb = 0;
    // Phase rather than progress, so a stage set to cycling colours keeps its own hue here.
    resolve_color(params, (uint32_t) state.animPhase, &cr, &cg, &cb);

    // The same number drives the shape and the fade, so a wipe fills as it brightens and empties
    // as it dims. That is what makes it read as one gesture rather than two effects at once.
    const float fade = progress;
    const float reach = progress * (float) (VU_ROW_LEDS - 1);
    constexpr float CENTRE = (float) (VU_ROW_LEDS / 2);

    for (int position = 0; position < VU_ROW_LEDS; position++) {
        float shape = 0.0f;
        switch (transition) {
            case Transition::Fade:
                shape = 1.0f;
                break;
            case Transition::Wipe:
                shape = anim_edge((float) position - reach);
                break;
            case Transition::Bloom:
                shape = anim_edge(std::abs((float) position - CENTRE) - progress * CENTRE);
                break;
            case Transition::Comet: {
                const float behind = reach - (float) position;
                shape = behind < 0.0f ? 0.0f : std::pow(0.55f, behind);
                break;
            }
            default:
                break;
        }
        for (int row = 0; row < VU_ROWS; row++) {
            setPixelLevel(vu_led_index(row, position), cr, cg, cb, shape * fade, brightness);
        }
    }
    commitFrame(Stage::Active);
}

/** @return whether the badge has been left alone for longer than the meter may hold the strip */
bool vu_timed_out() {
    if (state.vuInactiveSeconds == 0 || !state.idle) {
        return false;
    }
    return state.idleFrames >= (uint32_t) state.vuInactiveSeconds * 1000 / RENDER_INTERVAL_MS;
}

/** @return which stage is entitled to draw this frame */
Stage anim_stage() {
    // Above even the meter: a preview is a direct answer to something the user just tapped, and
    // it lasts seconds.
    if (state.previewFrames > 0) return state.previewStage;
    if (state.vuActive && !vu_timed_out()) return Stage::Vu;
    if (!state.idle) return Stage::Active;
    const uint32_t sleep_frames = (uint32_t) state.sleepMinutes * 60 * 1000 / RENDER_INTERVAL_MS;
    return state.idleFrames >= sleep_frames ? Stage::Sleep : Stage::Standby;
}

void onTimer() {
    if (!started || !state.led_strip) return;

    state.tickCount++;

    if (state.previewFrames > 0) {
        state.previewFrames--;
    }

    // Runs whether or not the meter is drawing: it is how long ago the badge was last touched,
    // which is what both the stage progression and the meter's own timeout are measured against.
    if (state.idle) {
        state.idleFrames++;
    }

    const Stage stage = anim_stage();

    // Dropped rather than queued while the meter is drawing: the strip is already saying something
    // about the music, and a state cue over the top of it would only interrupt that.
    if (stage == Stage::Vu) {
        state.transitionFrames = 0;
    }

    // Above the stages but below a preview, which the user is actively driving.
    if (state.previewFrames == 0 && state.transitionFrames > 0) {
        state.transitionFrames--;
        const float raw = 1.0f - (float) state.transitionFrames / (float) TRANSITION_FRAMES;
        const bool waking = !state.transitionReversed;
        // Waking runs the gesture forwards and brightens into the Active animation; going to
        // sleep runs the same shape backwards and dims out of it.
        renderTransition(state.transitionPlaying, waking ? raw : 1.0f - raw,
            waking
                ? RenderParams { state.activeR, state.activeG, state.activeB,
                      state.activeColorMode, state.activeBrightness, state.activeSpeed }
                : RenderParams { state.standbyR, state.standbyG, state.standbyB,
                      state.standbyColorMode, state.activeBrightness, state.standbySpeed });
        return;
    }

    const uint8_t showing = stage == Stage::Active ? (uint8_t) state.activeAnimation
        : stage == Stage::Standby ? (uint8_t) state.standbyAnimation
        : stage == Stage::Sleep ? (uint8_t) state.sleepAnimation
        : 0;

    if (stage != state.animStage || showing != state.animShowing) {
        state.animStage = stage;
        state.animShowing = showing;
        anim_reset();
    }

    switch (stage) {
        case Stage::Vu:
            // Only on its own slower cadence. The accumulators hold a maximum until drained, so
            // a skipped tick costs no transient and the VU_* constants stay tuned to 50 ms.
            if (state.tickCount % VU_TICK_DIVIDER == 0) {
                renderVu();
            }
            break;

        case Stage::Active:
            if (state.activeAnimation == Animation::Off) {
                powerDown();
            } else {
                renderAnimation(stage, state.activeAnimation, RenderParams {
                    state.activeR, state.activeG, state.activeB, state.activeColorMode,
                    state.activeBrightness, state.activeSpeed });
            }
            break;

        case Stage::Standby:
            if (state.standbyAnimation == Animation::Off) {
                powerDown();
            } else {
                renderAnimation(stage, state.standbyAnimation, RenderParams {
                    state.standbyR, state.standbyG, state.standbyB, state.standbyColorMode,
                    state.standbyBrightness, state.standbySpeed });
            }
            break;

        default: {
            // The phase only advances inside the burst, so a pattern resumes rather than jumping forward by
            // the dark time. A preview runs the wakes back to back off the render tick, with no gap.
            const bool previewing = state.previewFrames > 0;
            const uint32_t gap = (uint32_t) state.sleepIntervalSeconds * 1000 / RENDER_INTERVAL_MS;
            const uint32_t period = previewing ? SLEEP_BURST_FRAMES : SLEEP_BURST_FRAMES + gap;
            const uint32_t clock = previewing ? state.tickCount : state.idleFrames;
            const uint32_t burst_frame = clock % period;

            if (state.sleepAnimation == SleepAnimation::Off || burst_frame >= SLEEP_BURST_FRAMES) {
                powerDown();
            } else {
                renderSleep(state.sleepAnimation, RenderParams {
                    state.sleepR, state.sleepG, state.sleepB, state.sleepColorMode,
                    state.sleepBrightness, state.sleepSpeed },
                    burst_frame, SLEEP_BURST_FRAMES, clock / period);
            }
            break;
        }
    }
}

/** Paced against a fixed wake time, so a frame that takes a while does not push the next one out. */
void renderLoop() {
    TickType_t wake = kernel::getTicks();
    const TickType_t period = millis_to_ticks(RENDER_INTERVAL_MS);
    while (state.running.load(std::memory_order_relaxed)) {
        onTimer();
        vTaskDelayUntil(&wake, period);
    }
}

class NeoPixelService final : public Service {
public:
    bool onStart(ServiceContext& /*service*/) override {
        if (started) return true;

        LOG_I(TAG, "Starting NeoPixel service");
        
        led_strip_config_t strip_config = {
            .strip_gpio_num = 3,
            .max_leds = 22,
            .led_model = LED_MODEL_WS2812,
            .color_component_format = LED_STRIP_COLOR_COMPONENT_FMT_GRB,
            .flags = {
                .invert_out = false,
            }
        };

        // Four RMT blocks, the most one channel can annex without DMA. 22 WS2812s need 528 symbols
        // against a 48-word block, so four blocks give the refill ISR 120 us to make its deadline.
        led_strip_rmt_config_t rmt_config = {
            .clk_src = RMT_CLK_SRC_DEFAULT,
            .resolution_hz = 10 * 1000 * 1000, // 10MHz
            .mem_block_symbols = 192,
            .flags = {
                .with_dma = false,
            }
        };

        if (led_strip_new_rmt_device(&strip_config, &rmt_config, &state.led_strip) != ESP_OK) {
            LOG_E(TAG, "Failed to initialize NeoPixel RMT device");
            return false;
        }

        // A task of its own, not a tt::Timer: led_strip_refresh() blocks in rmt_tx_wait_all_done(), and
        // parking the shared timer daemon 40 times a second starves every other timer on it.
        state.running = true;
        state.thread = std::make_unique<Thread>("neopixel", 4096, [] {
            renderLoop();
            return 0;
        }, getCpuAffinityConfiguration().apps);
        state.thread->setPriority(Thread::Priority::Normal);
        state.thread->start();

        started = true;
        // After the timer is running, since the setters it uses go through the dispatcher and the
        // strip should already be able to draw whatever they restore.
        loadSettings();
        return true;
    }

    void onStop(ServiceContext& /*service*/) override {
        if (!started) return;
        LOG_I(TAG, "Stopping NeoPixel service");
        
        started = false;

        if (state.thread) {
            state.running = false;
            state.thread->join();
            state.thread.reset();
        }
        
        if (state.led_strip) {
            led_strip_clear(state.led_strip);
            led_strip_del(state.led_strip);
            state.led_strip = nullptr;
        }

        setPowered(false);
    }
};

} // namespace

extern const ServiceManifest manifest = {
    .id = "neopixel",
    .createService = create<NeoPixelService>
};

void setActiveAnimation(Animation animation) {
    getMainDispatcher().dispatch([animation] { state.activeAnimation = animation; });
}

Animation getActiveAnimation() { return state.activeAnimation; }

void setActiveColor(uint8_t r, uint8_t g, uint8_t b) {
    getMainDispatcher().dispatch([r, g, b] {
        state.activeR = r;
        state.activeG = g;
        state.activeB = b;
    });
}

void getActiveColor(uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = state.activeR;
    *g = state.activeG;
    *b = state.activeB;
}

void setActiveBrightness(uint8_t brightness) {
    getMainDispatcher().dispatch([brightness] { state.activeBrightness = brightness; });
}

uint8_t getActiveBrightness() { return state.activeBrightness; }

void setActiveSpeed(uint8_t speed) {
    getMainDispatcher().dispatch([speed] { state.activeSpeed = speed; });
}

uint8_t getActiveSpeed() { return state.activeSpeed; }

void setActiveColorMode(ColorMode mode) {
    getMainDispatcher().dispatch([mode] { state.activeColorMode = mode; });
}

ColorMode getActiveColorMode() { return state.activeColorMode; }

void setIdle(bool idle) {
    getMainDispatcher().dispatch([idle] {
        if (idle == state.idle) {
            return;
        }
        state.idle = idle;
        state.idleFrames = 0;

        const Transition transition = idle ? state.sleepTransition : state.wakeTransition;
        if (transition != Transition::Off) {
            state.transitionPlaying = transition;
            state.transitionReversed = idle;
            state.transitionFrames = TRANSITION_FRAMES;
        }
    });
}

bool isIdle() { return state.idle; }

void setStandbyAnimation(Animation animation) {
    getMainDispatcher().dispatch([animation] { state.standbyAnimation = animation; });
}

Animation getStandbyAnimation() { return state.standbyAnimation; }

void setStandbyColor(uint8_t r, uint8_t g, uint8_t b) {
    getMainDispatcher().dispatch([r, g, b] {
        state.standbyR = r;
        state.standbyG = g;
        state.standbyB = b;
    });
}

void getStandbyColor(uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = state.standbyR;
    *g = state.standbyG;
    *b = state.standbyB;
}

void setStandbyBrightness(uint8_t brightness) {
    getMainDispatcher().dispatch([brightness] { state.standbyBrightness = brightness; });
}

uint8_t getStandbyBrightness() { return state.standbyBrightness; }

void setStandbySpeed(uint8_t speed) {
    getMainDispatcher().dispatch([speed] { state.standbySpeed = speed; });
}

uint8_t getStandbySpeed() { return state.standbySpeed; }

void setStandbyColorMode(ColorMode mode) {
    getMainDispatcher().dispatch([mode] { state.standbyColorMode = mode; });
}

ColorMode getStandbyColorMode() { return state.standbyColorMode; }

void setSleepMinutes(uint8_t minutes) {
    getMainDispatcher().dispatch([minutes] { state.sleepMinutes = minutes; });
}

uint8_t getSleepMinutes() { return state.sleepMinutes; }

void setSleepAnimation(SleepAnimation animation) {
    getMainDispatcher().dispatch([animation] { state.sleepAnimation = animation; });
}

SleepAnimation getSleepAnimation() { return state.sleepAnimation; }

void setSleepColor(uint8_t r, uint8_t g, uint8_t b) {
    getMainDispatcher().dispatch([r, g, b] {
        state.sleepR = r;
        state.sleepG = g;
        state.sleepB = b;
    });
}

void getSleepColor(uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = state.sleepR;
    *g = state.sleepG;
    *b = state.sleepB;
}

void setSleepColorMode(ColorMode mode) {
    getMainDispatcher().dispatch([mode] { state.sleepColorMode = mode; });
}

ColorMode getSleepColorMode() { return state.sleepColorMode; }

void setSleepBrightness(uint8_t brightness) {
    getMainDispatcher().dispatch([brightness] { state.sleepBrightness = brightness; });
}

uint8_t getSleepBrightness() { return state.sleepBrightness; }

void setSleepSpeed(uint8_t speed) {
    getMainDispatcher().dispatch([speed] { state.sleepSpeed = speed; });
}

uint8_t getSleepSpeed() { return state.sleepSpeed; }

void setSleepIntervalSeconds(uint8_t seconds) {
    getMainDispatcher().dispatch([seconds] { state.sleepIntervalSeconds = seconds; });
}

uint8_t getSleepIntervalSeconds() { return state.sleepIntervalSeconds; }

void setWakeTransition(Transition transition) {
    getMainDispatcher().dispatch([transition] { state.wakeTransition = transition; });
}

Transition getWakeTransition() { return state.wakeTransition; }

void setSleepTransition(Transition transition) {
    getMainDispatcher().dispatch([transition] { state.sleepTransition = transition; });
}

Transition getSleepTransition() { return state.sleepTransition; }

void startPreview(Stage stage, uint16_t seconds) {
    if (stage >= Stage::Count) return;
    getMainDispatcher().dispatch([stage, seconds] {
        state.previewStage = stage;
        state.previewFrames = (uint32_t) seconds * 1000 / RENDER_INTERVAL_MS;
    });
}

void stopPreview() {
    getMainDispatcher().dispatch([] { state.previewFrames = 0; });
}

bool isPreviewActive() { return state.previewFrames > 0; }

Stage getPreviewStage() { return state.previewStage; }

void setBrightnessLimit(Stage stage, uint8_t limit) {
    if (stage >= Stage::Count) return;
    getMainDispatcher().dispatch([stage, limit] {
        state.brightnessLimit[(int) stage] = std::min<uint8_t>(limit, 100);
    });
}

uint8_t getBrightnessLimit(Stage stage) {
    return stage < Stage::Count ? state.brightnessLimit[(int) stage] : 100;
}

uint8_t getAllowedBrightness(Stage stage) {
    return stage < Stage::Count ? allowed_brightness(stage) : 100;
}

void setVuActive(bool active) {
    getMainDispatcher().dispatch([active] {
        if (active == state.vuActive) {
            return;
        }
        state.vuActive = active;
        if (active) {
            state.vuIdleFrames = 0;
            state.vuBeatAverage = 0.0f;
            state.vuBeatDeviation = 0.0f;
            state.vuBeatGovernor = 1.0f;
            state.vuBeatsInWindow = 0;
            state.vuBeatWindowFrames = 0;
            state.vuFlash = 0.0f;
            state.vuFlashTarget = 0.0f;
            state.vuAgcPeak = 0.0f;
            for (int row = 0; row < VU_ROWS; row++) {
                state.vuDisplay[row] = 0.0f;
                state.vuPeak[row] = 0.0f;
            }
        }
    });
}

bool isVuActive() { return state.vuActive; }

namespace {
/** Keeps the loudest value seen since the last frame, so a faster producer loses no transient. */
void accumulate_max(std::atomic<uint8_t>& target, uint8_t value) {
    uint8_t current = target.load(std::memory_order_relaxed);
    while (value > current &&
           !target.compare_exchange_weak(current, value, std::memory_order_relaxed)) {
    }
}
}

void setVuLevels(const VuLevels& levels) {
    // Deliberately not dispatched: this arrives from the audio output task tens of times a
    // second, and queueing that onto the main dispatcher would swamp it.
    accumulate_max(state.vuAccum[0], levels.left);
    accumulate_max(state.vuAccum[1], levels.right);
    accumulate_max(state.vuAccumBass, levels.bass);
    accumulate_max(state.vuAccumMid, levels.mid);
    accumulate_max(state.vuAccumTreble, levels.treble);
}

void setVuBrightness(uint8_t brightness) {
    getMainDispatcher().dispatch([brightness] { state.vuBrightness = brightness; });
}

uint8_t getVuBrightness() { return state.vuBrightness; }

void setVuBassOnlyEnabled(bool enabled) {
    getMainDispatcher().dispatch([enabled] { state.vuBassOnly = enabled; });
}

bool isVuBassOnlyEnabled() { return state.vuBassOnly; }

void setVuBeatSource(VuBeatSource source) {
    getMainDispatcher().dispatch([source] { state.vuBeatSource = source; });
}

VuBeatSource getVuBeatSource() { return state.vuBeatSource; }

void setVuBeatAutoEnabled(bool enabled) {
    getMainDispatcher().dispatch([enabled] {
        state.vuBeatAuto = enabled;
        state.vuBeatGovernor = 1.0f;
    });
}

bool isVuBeatAutoEnabled() { return state.vuBeatAuto; }

void setVuBeatSensitivity(uint8_t sensitivity) {
    getMainDispatcher().dispatch([sensitivity] { state.vuBeatSensitivity = sensitivity; });
}

uint8_t getVuBeatSensitivity() { return state.vuBeatSensitivity; }

void setVuFlashAttack(uint8_t attack) {
    getMainDispatcher().dispatch([attack] { state.vuFlashAttack = attack; });
}

uint8_t getVuFlashAttack() { return state.vuFlashAttack; }

void setVuFlashDecay(uint8_t decay) {
    getMainDispatcher().dispatch([decay] { state.vuFlashDecay = decay; });
}

uint8_t getVuFlashDecay() { return state.vuFlashDecay; }

void setVuPeakBrightness(uint8_t brightness) {
    getMainDispatcher().dispatch([brightness] { state.vuPeakBrightness = brightness; });
}

uint8_t getVuPeakBrightness() { return state.vuPeakBrightness; }

void setVuSensitivity(uint8_t sensitivity) {
    getMainDispatcher().dispatch([sensitivity] { state.vuSensitivity = sensitivity; });
}

uint8_t getVuSensitivity() { return state.vuSensitivity; }

void setVuAutoGainEnabled(bool enabled) {
    getMainDispatcher().dispatch([enabled] { state.vuAutoGain = enabled; });
}

bool isVuAutoGainEnabled() { return state.vuAutoGain; }

void setVuOrigin(VuOrigin origin) {
    getMainDispatcher().dispatch([origin] { state.vuOrigin = origin; });
}

VuOrigin getVuOrigin() { return state.vuOrigin; }

void setVuPalette(VuPalette palette) {
    getMainDispatcher().dispatch([palette] { state.vuPalette = palette; });
}

VuPalette getVuPalette() { return state.vuPalette; }

void setVuDecayEnabled(bool enabled) {
    getMainDispatcher().dispatch([enabled] { state.vuDecay = enabled; });
}

bool isVuDecayEnabled() { return state.vuDecay; }

void setVuPeakHoldEnabled(bool enabled) {
    getMainDispatcher().dispatch([enabled] { state.vuPeakHold = enabled; });
}

bool isVuPeakHoldEnabled() { return state.vuPeakHold; }

void setVuBeatFlashEnabled(bool enabled) {
    getMainDispatcher().dispatch([enabled] { state.vuBeatFlash = enabled; });
}

bool isVuBeatFlashEnabled() { return state.vuBeatFlash; }

void setVuSpeed(uint8_t speed) {
    getMainDispatcher().dispatch([speed] { state.vuSpeed = speed; });
}

uint8_t getVuSpeed() { return state.vuSpeed; }

void setVuInactiveSeconds(uint16_t seconds) {
    getMainDispatcher().dispatch([seconds] { state.vuInactiveSeconds = seconds; });
}

uint16_t getVuInactiveSeconds() { return state.vuInactiveSeconds; }

void setVuColor(uint8_t r, uint8_t g, uint8_t b) {
    getMainDispatcher().dispatch([r, g, b] {
        state.vuR = r;
        state.vuG = g;
        state.vuB = b;
    });
}

void getVuColor(uint8_t* r, uint8_t* g, uint8_t* b) {
    *r = state.vuR;
    *g = state.vuG;
    *b = state.vuB;
}

} // namespace tt::service::neopixel
