#pragma once

#include <Tactility/service/music/Music.h>

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace tt::service::music {

/**
 * The processing decoded PCM passes through after the ring buffer. Every handle belongs to the
 * output task, and setters only leave values it picks up next chunk; blocking it is an underrun.
 */
class AudioEffects {
public:

    AudioEffects();
    ~AudioEffects();

    AudioEffects(const AudioEffects&) = delete;
    AudioEffects& operator=(const AudioEffects&) = delete;

    /** Reopens every stage for a stream format, replacing whatever was open. */
    void configure(uint32_t sampleRate, uint8_t channels);
    void release();
    /** Processes @a frames interleaved 16-bit frames in place. */
    void process(int16_t* samples, size_t frames);

    /** @param gains one per band, -13 to 13; applied as a whole curve so no chunk sees half of it */
    void setBandGains(const int* gains);
    void setDynamics(Dynamics value);
    /** @param percent -100 hard left to 100 hard right */
    void setBalancePercent(int percent);
    void setMonoDownmix(bool enabled);
    void setFade(Fade value);
    /** Runs a fade over the next transition, or does nothing when fading is off. */
    void startFade(bool fadeIn);
    /** @return how many frames a fade covers, so a fade out can be started early enough */
    size_t fadeFrameCount() const;
    /**
     * @return whether the output has reached full scale recently
     *
     * Held for a fixed time by the output task rather than cleared by the reader: the service's
     * own tick polls telemetry as well, and a latch would leave the two readers racing for it.
     */
    bool isClipping() const;

private:

    /** Picks up whatever the setters left, from the output task. */
    void applyPending();
    void openEqualizer();
    void openCompressor();
    void openMultiband();
    void openFade();
    void closeStages();
    /** Balance, mono fold and clip detection, which are one pass over the samples. */
    void mixAndMeasure(int16_t* samples, size_t frames);

    void* equalizer = nullptr;
    void* compressor = nullptr;
    void* multiband = nullptr;
    void* fader = nullptr;

    uint32_t sampleRate = 0;
    uint8_t channels = 0;
    /** Bands whose centre frequency fits below Nyquist for the current rate. */
    int filterCount = 0;

    int bandGains[EQUALIZER_BANDS] = {};

    /**
     * Bumped after a whole curve has been written, so the output task can tell a complete set of
     * gains from one it has caught halfway through a preset change.
     */
    std::atomic<uint32_t> gainGeneration { 0 };
    uint32_t appliedGainGeneration = 0;
    int pendingGains[EQUALIZER_BANDS] = {};

    std::atomic<uint8_t> dynamics { (uint8_t) Dynamics::Off };
    uint8_t appliedDynamics = (uint8_t) Dynamics::Off;
    std::atomic<uint8_t> fade { (uint8_t) Fade::Off };
    uint8_t appliedFade = (uint8_t) Fade::Off;

    std::atomic<int8_t> balancePercent { 0 };
    std::atomic<bool> monoDownmix { false };
    /** Frames for which clipping is still reported, counted down as they are processed. */
    std::atomic<uint32_t> clipHoldFrames { 0 };
    /** 0 for nothing pending, 1 for a fade in, 2 for a fade out. */
    std::atomic<uint8_t> fadeRequest { 0 };
    /** Frames of the current fade still to run, so a finished one stops costing anything. */
    size_t fadeFramesLeft = 0;
};

} // namespace tt::service::music
