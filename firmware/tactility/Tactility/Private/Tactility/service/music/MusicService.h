#pragma once

#include <Tactility/Mutex.h>
#include <Tactility/Thread.h>
#include <Tactility/service/Service.h>
#include <Tactility/service/ServiceContext.h>
#include <Tactility/service/music/Music.h>
#include <Tactility/service/music/MusicStats.h>

#include <atomic>
#include <memory>
#include <random>
#include <string>
#include <vector>

struct Device;

namespace tt::service::music {

class AudioPlayer;

/**
 * Owns playback so it outlives the music player app. The AudioPlayer is built on first play and
 * released when the playlist ends, its ring buffer costing half a megabyte of external RAM.
 */
class MusicService final : public Service {

    Mutex mutex;
    std::unique_ptr<AudioPlayer> player;
    /**
     * Advances the playlist at the end of a track. A thread, not a Timer, since rebuilding the
     * pipeline would block the shared timer daemon. Joining it on the player's lifetime deadlocks.
     */
    std::unique_ptr<Thread> tickThread;
    std::atomic<bool> tickRunning { false };

    /** What plays, in order. Filled only from the library; nothing scans into it on its own. */
    std::vector<std::string> queue;
    /** Resolved on first use rather than at boot, so the service costs no SD card I/O until asked. */
    mutable std::string libraryDirectory;
    int trackIndex = -1;
    /** Read from the track's tags as it starts, so the UI never pays for the lookup. */
    std::string trackArtist;
    bool shuffle = false;
    Repeat repeat = Repeat::All;
    std::mt19937 rng { 12345 };

    int gainDb = 0;
    int speedPercent = 100;
    int pitchPercent = 100;
    int presetIndex = 0;
    int bandGains[EQUALIZER_BANDS] = {};
    bool monoDownmix = false;
    Dynamics dynamics = Dynamics::Off;
    Fade fade = Fade::Short;
    int balancePercent = 0;
    bool autoPreamp = true;

    /** The amplifier's rail, so the speaker can be switched off without stopping playback. */
    MusicStats stats;
    /** Ticks since the stats were last written, so playtime does not touch flash every second. */
    uint32_t statsFlushTicks = 0;
    /** Ticks of playback not yet rounded up into a whole second. */
    uint32_t statsPartialTicks = 0;

    Device* speakerRail = nullptr;
    bool speakerOn = false;

    /** Read from the audio output task on every chunk, so never guarded by `mutex`. */
    std::atomic<bool> vuSeeding { false };
    /** Whether the lighting service has been handed over to the meter. */
    bool vuActive = false;
    /** Ticks since playback stopped, counted only while the meter still holds the strip. */
    uint32_t vuIdleTicks = 0;

    /** @param fromTick true when called on the tick thread, which is what ages the hand-back */
    void updateVuLocked(bool fromTick);
    static void onAudioLevels(const AudioLevels& levels, void* context);

    /** All of these expect `mutex` to be held. */
    void resolveLibraryRootLocked() const;
    /** Appends @a path, or the playable files under it. @return how many were added */
    int enqueueLocked(const std::string& path, int depth);
    void removeQueueIndexLocked(int index);
    void ensurePlayer();
    void releasePlayer();
    void startTrackLocked(int index);
    int nextIndexLocked();
    /** The transport, split out so the tick thread can run it under the lock it already holds. */
    void playPauseLocked();
    void nextLocked();
    void previousLocked();
    /** @return true when the amplifier rail actually switched; its supply can refuse to come up */
    bool setSpeakerLocked(bool enabled);
    /** Output gain with the auto trim folded in, which is what actually reaches the player. */
    int effectiveGainLocked() const;
    void applyGainLocked();

    void tickMain();
    void onTick();

public:

    bool onStart(ServiceContext& serviceContext) override;
    void onStop(ServiceContext& serviceContext) override;

    bool isAvailable() const;

    std::vector<LibraryEntry> listLibrary(const std::string& directory) const;
    std::string getLibraryPath() const;

    size_t getQueueCount() const;
    int getTrackIndex() const;
    std::string getTrackPath() const;
    std::string getTrackArtist() const;
    std::string getQueuePathAt(int index) const;
    void playQueueIndex(int index);
    void enqueue(const std::string& path);
    void enqueueAndPlay(const std::string& path);
    void removeFromQueue(int index);
    void removeFromQueueByPath(const std::string& path);
    bool isQueued(const std::string& path) const;

    void playPause();
    void next();
    void previous();
    void seek(uint32_t seconds);
    Telemetry getTelemetry() const;

    bool isShuffleEnabled() const;
    void setShuffleEnabled(bool enabled);
    Repeat getRepeat() const;
    void setRepeat(Repeat value);

    int getPresetIndex() const;
    void setPresetIndex(int index);
    int getBandGainDb(int band) const;
    void setBandGainDb(int band, int value);
    int getGainDb() const;
    void setGainDb(int value);
    int getSpeedPercent() const;
    void setSpeedPercent(int percent);
    int getPitchPercent() const;
    void setPitchPercent(int percent);
    Dynamics getDynamics() const;
    void setDynamics(Dynamics value);
    Fade getFade() const;
    void setFade(Fade value);
    int getBalancePercent() const;
    void setBalancePercent(int percent);
    bool isMonoDownmixEnabled() const;
    void setMonoDownmixEnabled(bool enabled);
    int getAutoPreampTrimDb() const;
    bool isAutoPreampEnabled() const;
    void setAutoPreampEnabled(bool enabled);

    Stats getStats() const;
    void resetStats();

    bool isSpeakerEnabled() const;
    /** @return true when the amplifier rail actually switched */
    bool setSpeakerEnabled(bool enabled);

    bool isVuSeedingEnabled() const;
    void setVuSeedingEnabled(bool enabled);
};

} // namespace tt::service::music
