#pragma once

#include <Tactility/service/music/Music.h>

#include <map>
#include <string>

namespace tt::service::music {

/**
 * Lifetime listening counts, plus the ones for this power-on.
 *
 * Not thread-safe: MusicService owns one and touches it under its own mutex.
 */
class MusicStats {
public:

    void load();
    /** Writes only when something has changed since the last write. */
    void saveIfDirty();
    void reset();

    /** Counts a track as played and attributes it to @a artist. */
    void addPlay(const std::string& trackName, const std::string& artist);
    void addSeconds(uint32_t seconds);
    /** Underrun counts are per-pipeline, so they are folded in as each player is released. */
    void addUnderruns(uint32_t ring, uint32_t dma, uint32_t droppedBytes);

    Stats snapshot() const;

private:

    /**
     * Counts kept per name, capped so a large library cannot grow the file without bound. At the
     * cap the least played entry goes, losing a track heard once rather than ignoring a new one.
     */
    static constexpr size_t MAX_ENTRIES = 64;

    static void record(std::map<std::string, uint32_t>& counts, const std::string& name);

    uint32_t totalSeconds = 0;
    uint32_t totalTracks = 0;
    std::map<std::string, uint32_t> trackPlays;
    std::map<std::string, uint32_t> artistPlays;

    uint32_t sessionSeconds = 0;
    uint32_t sessionTracks = 0;
    uint32_t sessionUnderruns = 0;
    uint32_t sessionDmaUnderruns = 0;
    uint32_t sessionDroppedBytes = 0;

    bool dirty = false;
};

} // namespace tt::service::music
