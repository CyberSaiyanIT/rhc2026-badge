#pragma once

#include <cstdint>

namespace tt::service::neopixel {

/**
 * The patterns the strip can draw, one list serving both the awake and asleep badge. Count is a
 * sentinel and never selectable.
 */
enum class Animation : uint8_t {
    Off,
    /** The chosen colour, unchanging. */
    Solid,
    /** The chosen colour fading in and out. */
    Breathing,
    /**
     * A hue spread along the strip. Static mirrors one wheel per row; Cycle sends one wheel out along
     * a row and back along the other, the second row being wired back to front.
     */
    Rainbow,
    /** The whole strip on one hue, cycling through them. */
    RainbowCycle,
    /** A head with a fading tail running along each row. */
    Comet,
    /** A head with a fading tail bouncing between the ends. */
    Scanner,
    /** Every third pixel lit, marching along. */
    TheaterChase,
    /** Single pixels flashing the chosen colour over a dim bed. */
    Sparkle,
    /** Pixels swelling and fading, slower and softer than Sparkle. */
    Twinkle,
    /** Flicker on a black-red-yellow-white heat palette. Ignores the chosen colour. */
    Fire,
    /** A brightness wave travelling along the strip. */
    Wave,
    /** Single pixels lit at random hues, fading out. */
    Confetti,
    /** Fills the strip one pixel at a time, clears it the same way, then shifts hue. */
    ColorWipe,
    /** A double beat, the way a heart monitor reads. */
    Pulse,
    /** The two rows lighting in turn. */
    Alternate,
    Count
};

/**
 * The patterns the sleep stage can draw. A separate, smaller set: every one of them lights a
 * handful of pixels at most, because sleep exists to keep the badge alive rather than seen.
 */
enum class SleepAnimation : uint8_t {
    Off,
    /** One pixel at the middle of each row, breathing. */
    Beacon,
    /** A single pixel running along each row. */
    Dot,
    /** A single pixel bouncing between the ends. */
    Sweep,
    /** One or two pixels fading in and out at random. */
    Twinkle,
    /** The end pixels of each row, blinking in turn. */
    Ends,
    /** Two short flashes and then nothing, the way a status light reports in. */
    Pulse,
    /** A single pixel with a short tail, crossing the rows once per wake. */
    Comet,
    /** One steady pixel that moves on by a place each time the strip wakes. */
    Drift,
    /** One pixel somewhere new each wake, fading out over it. */
    Spark,
    Count
};

/**
 * The short one-shot patterns played entering and leaving use. One set serves both: sleep runs
 * the same shape backwards, so a wipe that fills on waking empties on sleeping.
 */
enum class Transition : uint8_t {
    Off,
    /** A fill running the length of the rows. */
    Wipe,
    /** A bar growing outwards from the middle pixel. */
    Bloom,
    /** The whole strip fading in or out, with no shape to it. */
    Fade,
    /** A single pixel with a tail, running the length of the rows. */
    Comet,
    Count
};

/** Where an animation's colour comes from. */
enum class ColorMode : uint8_t {
    /** The colour set for the stage. */
    Static,
    /**
     * A hue that rotates on its own, for the patterns that have no colour of their own to cycle.
     * The ones that already paint their own colours ignore it.
     */
    Cycle
};

/**
 * The four owners of the strip, each with its own settings and power limit, in the order the
 * badge moves through them.
 */
enum class Stage : uint8_t { Active, Standby, Sleep, Vu, Count };

/** One frame of metering, mirroring music::AudioLevels so this service needs no audio headers. */
struct VuLevels {
    uint8_t left = 0;
    uint8_t right = 0;
    uint8_t bass = 0;
    uint8_t mid = 0;
    uint8_t treble = 0;
};

/** Which end of each row the bar grows from. */
enum class VuOrigin : uint8_t {
    /** Both rows from the left end. */
    BothLeft,
    /** Both rows from the right end. */
    BothRight,
    /** Both rows outwards from the middle pixel. */
    Center,
    /**
     * Rotates through the fixed patterns on a period set by setVuSpeed(), and through the two
     * opposed layouts (one row from each end) that only appear here.
     */
    Cycle
};

enum class VuPalette : uint8_t {
    /** Green through amber to red by height, the way a meter is expected to look. */
    Classic,
    /** The colour set by setVuColor(), at every height. */
    Solid,
    Rainbow,
    /**
     * Bass red, mid green, treble blue, mixed per frame. Height still shows loudness, so colour
     * is spent on what the music is doing rather than repeating the number the bar already gives.
     */
    Spectrum,
    /** Rotates through the four above, held longer than the pattern cycle so the two do not
     *  change on the same frame. */
    Cycle
};

/**
 * What the strip draws while the badge is in use. Falls back to Off if the supply refuses to come
 * up, and the getter reports that.
 */
void setActiveAnimation(Animation animation);
Animation getActiveAnimation();
void setActiveColor(uint8_t r, uint8_t g, uint8_t b);
void getActiveColor(uint8_t* r, uint8_t* g, uint8_t* b);
void setActiveBrightness(uint8_t brightness);
uint8_t getActiveBrightness();
void setActiveSpeed(uint8_t speed);
uint8_t getActiveSpeed();
void setActiveColorMode(ColorMode mode);
ColorMode getActiveColorMode();

/**
 * Hands the strip to the standby animation, and back on false. Driven by the display idle
 * service, so the strip steps down on the same timeout that turns the screen off.
 */
void setIdle(bool idle);
bool isIdle();

void setStandbyAnimation(Animation animation);
Animation getStandbyAnimation();
void setStandbyColor(uint8_t r, uint8_t g, uint8_t b);
void getStandbyColor(uint8_t* r, uint8_t* g, uint8_t* b);
void setStandbyColorMode(ColorMode mode);
ColorMode getStandbyColorMode();
void setStandbyBrightness(uint8_t brightness);
uint8_t getStandbyBrightness();
void setStandbySpeed(uint8_t speed);
uint8_t getStandbySpeed();

/** How long standby runs before sleep takes over, 0 going straight to sleep. */
void setSleepMinutes(uint8_t minutes);
uint8_t getSleepMinutes();

/**
 * What the strip draws once the badge has been left alone. The supply is switched off between
 * bursts rather than the pattern merely dimmed, so most of each cycle costs nothing.
 */
void setSleepAnimation(SleepAnimation animation);
SleepAnimation getSleepAnimation();
void setSleepColor(uint8_t r, uint8_t g, uint8_t b);
void getSleepColor(uint8_t* r, uint8_t* g, uint8_t* b);
void setSleepColorMode(ColorMode mode);
ColorMode getSleepColorMode();
void setSleepBrightness(uint8_t brightness);
uint8_t getSleepBrightness();

/** How fast the sleep pattern moves, 1 to 20. Its own, so it is not tied to the standby rate. */
void setSleepSpeed(uint8_t speed);
uint8_t getSleepSpeed();

/**
 * How long the strip stays dark between wakes, in seconds. The supply is released throughout, so
 * this decides what sleep costs: a powered strip draws ~1 mA per WS2812 even unlit.
 */
void setSleepIntervalSeconds(uint8_t seconds);
uint8_t getSleepIntervalSeconds();

/**
 * Played once when the badge is used again, over whatever the stages would draw. Both transitions
 * peak at the Active stage's brightness rather than the dimmer level that follows.
 */
void setWakeTransition(Transition transition);
Transition getWakeTransition();

/** Played once when the badge is left alone, in the Standby stage's colour. */
void setSleepTransition(Transition transition);
Transition getSleepTransition();

/**
 * Shows @a stage's animation for @a seconds, outranking everything including the meter. Calling
 * it again restarts the countdown. Sleep draws continuously while previewed, not in bursts.
 */
void startPreview(Stage stage, uint16_t seconds);
void stopPreview();
bool isPreviewActive();
/** Which stage is being previewed. Only meaningful while isPreviewActive(). */
Stage getPreviewStage();

/**
 * Caps what a stage may draw, 100 being no cap. Runtime state for whoever manages power, never
 * saved: a limit is a fact about the battery, not a setting.
 */
void setBrightnessLimit(Stage stage, uint8_t limit);
uint8_t getBrightnessLimit(Stage stage);

/**
 * The highest brightness @a stage may reach, which is what its slider should run to. A pattern
 * lighting few pixels gets fixed headroom over the limit rather than the worst-case number.
 */
uint8_t getAllowedBrightness(Stage stage);

/**
 * Hands the strip over to the meter, and back to whichever animation was showing on false. It
 * outranks the sleep animation, so music started on a dozing badge still meters.
 */
void setVuActive(bool active);
bool isVuActive();

/**
 * Feeds the meter, 0-255 per channel, accumulated as a maximum until the next frame, so pushing
 * faster than the strip refreshes loses no transients. Stores to atomics; safe from an audio task.
 */
void setVuLevels(const struct VuLevels& levels);

/** Brightness while the meter is showing, kept apart from the one the animations use. */
void setVuBrightness(uint8_t brightness);
uint8_t getVuBrightness();

/**
 * How long after the last touch the meter gives the strip back, in seconds; 0 keeps it while music
 * plays. Counted from user input, not from the music stopping.
 */
void setVuInactiveSeconds(uint16_t seconds);
uint16_t getVuInactiveSeconds();

/** How fast VuOrigin::Cycle and VuPalette::Cycle step. */
void setVuSpeed(uint8_t speed);
uint8_t getVuSpeed();

/** The colour VuPalette::Solid paints, kept apart from the animations' own. */
void setVuColor(uint8_t r, uint8_t g, uint8_t b);
void getVuColor(uint8_t* r, uint8_t* g, uint8_t* b);

/**
 * Drives the bar from the bass band alone, so it moves with the kick rather than with everything
 * at once. The colour still reflects the full mix.
 */
void setVuBassOnlyEnabled(bool enabled);
bool isVuBassOnlyEnabled();

/** Which band a beat is looked for in. Bass finds kicks, treble finds hats. */
enum class VuBeatSource : uint8_t { Mix, Bass, Treble };

void setVuBeatSource(VuBeatSource source);
VuBeatSource getVuBeatSource();

/**
 * How far above its running average the beat band must jump, 0 needing the largest jump and 100
 * firing on almost anything. Levels are in dB, so this is a difference, not a ratio.
 */
void setVuBeatSensitivity(uint8_t sensitivity);
uint8_t getVuBeatSensitivity();

/**
 * Sets the beat threshold from how much the band moves on the current track, trimmed towards a
 * musical hit rate. While this is on, setVuBeatSensitivity() is ignored.
 */
void setVuBeatAutoEnabled(bool enabled);
bool isVuBeatAutoEnabled();

/** How fast a flash rises, 0 snapping to full and 100 swelling into it. */
void setVuFlashAttack(uint8_t attack);
uint8_t getVuFlashAttack();

/** How long a flash takes to fall away, 0 being a blink and 100 a slow fade. */
void setVuFlashDecay(uint8_t decay);
uint8_t getVuFlashDecay();

/**
 * Which part of the level range fills the bar: 100 shows everything down to the noise floor, 0
 * only the loudest few dB. Quiet material needs a high value to move the bar at all.
 */
void setVuSensitivity(uint8_t sensitivity);
uint8_t getVuSensitivity();

/** Rescales against a slowly falling peak, so any material ends up using the whole bar. */
void setVuAutoGainEnabled(bool enabled);
bool isVuAutoGainEnabled();

void setVuOrigin(VuOrigin origin);
VuOrigin getVuOrigin();
void setVuPalette(VuPalette palette);
VuPalette getVuPalette();
/** Off makes the bar follow the level exactly, which reads as jittery on most material. */
void setVuDecayEnabled(bool enabled);
bool isVuDecayEnabled();
/** A single white pixel marking the recent peak, falling slower than the bar. */
void setVuPeakHoldEnabled(bool enabled);
bool isVuPeakHoldEnabled();

/** Brightness of that marker as a percentage of the meter's own, 0 leaving it unlit. */
void setVuPeakBrightness(uint8_t brightness);
uint8_t getVuPeakBrightness();
/** Punches the whole strip brighter when the level jumps well above its recent average. */
void setVuBeatFlashEnabled(bool enabled);
bool isVuBeatFlashEnabled();

/** Applies the saved settings, or the built-in ones when nothing has been saved yet. */
void loadSettings();
/**
 * Writes the current settings. Not automatic: the Lighting app flushes from its own event loop,
 * keeping the write off the LVGL lock and the strip's render timer.
 */
void saveSettings();
/** Puts every setting back to its built-in value and stores that. */
void resetSettings();

} // namespace tt::service::neopixel
