#pragma once

#include <Tactility/service/music/Music.h>

#include <string>

namespace tt::service::music {

/**
 * Reports the level of the audio about to reach the codec, 0-255 per channel on a dB scale.
 *
 * \task Called from the output task, which the codec is waiting on: it must not block, take a
 *     lock, or do anything beyond storing the values.
 */
using AudioLevelCallback = void (*)(const AudioLevels& levels, void* context);

/**
 * Decodes one file at a time through ESP-ADF into the kernel audio stream. The ring buffer in
 * external RAM must reach PREBUFFER_BYTES first, so a slow SD read cannot stall the output.
 */
class AudioPlayer {
public:
    AudioPlayer();
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    /** @return true if the audio hardware is present and the pipeline was built */
    bool isAvailable() const;

    /** Stops whatever is playing and starts @a path. Supports .mp3, .m4a and .aac. */
    bool play(const std::string& path);
    void stop();
    void pause();
    void resume();

    /**
     * Approximate: the byte offset is derived from the stream's average bitrate, so a
     * variable-bitrate file lands near the requested position rather than on it.
     */
    bool seek(uint32_t seconds);

    /** @param gainDb output gain, -64 to 63 */
    void setGainDb(int gainDb);
    /** @param speed playback rate, 0.5 to 2.0, pitch preserved */
    void setSpeed(float speed);
    /** @param pitch pitch scale, 0.5 to 2.0, playback rate preserved */
    void setPitch(float pitch);
    /** Sums both channels into each, for a single speaker. */
    void setMonoDownmix(bool enabled);
    void setDynamics(Dynamics value);
    /** @param percent -100 hard left to 100 hard right */
    void setBalancePercent(int percent);
    void setFade(Fade value);
    /** @param gainDb per-band gain, -13 to 13 */
    void setEqualizerBand(int band, int gainDb);

    Telemetry getTelemetry() const;

    /** Must be set before the first play(): it is read from the output task without a lock. */
    void setLevelCallback(AudioLevelCallback callback, void* context);

private:
    struct Impl;
    Impl* impl;
};

} // namespace tt::service::music
