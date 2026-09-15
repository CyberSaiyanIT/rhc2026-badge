#include <Tactility/service/music/MusicStats.h>

#include <Tactility/file/PropertiesFile.h>

#include <app/paths.h>
#include <tactility/log.h>

#include <algorithm>
#include <cstdlib>
#include <format>

namespace tt::service::music {

namespace {

constexpr auto* TAG = "MusicStats";

std::string statsFilePath() {
    char path[256];
    if (app_paths_get_user_data_path("MusicPlayer", "stats.properties", path, sizeof(path)) != ERROR_NONE) {
        return {};
    }
    return path;
}

uint32_t readUnsigned(const std::map<std::string, std::string>& properties, const std::string& key) {
    const auto entry = properties.find(key);
    return entry == properties.end() ? 0u : (uint32_t) strtoul(entry->second.c_str(), nullptr, 10);
}

/** @return the name with the highest count, and its count in @a outPlays */
std::string topOf(const std::map<std::string, uint32_t>& counts, uint32_t& outPlays) {
    outPlays = 0;
    std::string name;
    for (const auto& [candidate, plays] : counts) {
        if (plays > outPlays) {
            outPlays = plays;
            name = candidate;
        }
    }
    return name;
}

void writeCounts(std::map<std::string, std::string>& properties, const char* prefix,
                 const std::map<std::string, uint32_t>& counts) {
    // The name goes in the value, never the key: the properties parser splits a line on its first
    // '=', so a name containing one would be read back cut in half.
    int index = 0;
    for (const auto& [name, plays] : counts) {
        properties[std::format("{}{}.name", prefix, index)] = name;
        properties[std::format("{}{}.plays", prefix, index)] = std::format("{}", plays);
        index++;
    }
}

void readCounts(const std::map<std::string, std::string>& properties, const char* prefix,
                std::map<std::string, uint32_t>& counts) {
    for (int index = 0;; index++) {
        const auto name = properties.find(std::format("{}{}.name", prefix, index));
        if (name == properties.end()) {
            return;
        }
        const auto plays = readUnsigned(properties, std::format("{}{}.plays", prefix, index));
        if (!name->second.empty() && plays > 0) {
            counts[name->second] = plays;
        }
    }
}

} // namespace

void MusicStats::record(std::map<std::string, uint32_t>& counts, const std::string& name) {
    const auto existing = counts.find(name);
    if (existing != counts.end()) {
        existing->second++;
        return;
    }

    if (counts.size() >= MAX_ENTRIES) {
        auto lowest = std::min_element(counts.begin(), counts.end(),
            [](const auto& left, const auto& right) { return left.second < right.second; });
        counts.erase(lowest);
    }
    counts[name] = 1;
}

void MusicStats::addPlay(const std::string& trackName, const std::string& artist) {
    totalTracks++;
    sessionTracks++;
    record(trackPlays, trackName);
    record(artistPlays, artist);
    dirty = true;
}

void MusicStats::addSeconds(uint32_t seconds) {
    if (seconds == 0) {
        return;
    }
    totalSeconds += seconds;
    sessionSeconds += seconds;
    dirty = true;
}

void MusicStats::addUnderruns(uint32_t ring, uint32_t dma, uint32_t droppedBytes) {
    sessionUnderruns += ring;
    sessionDmaUnderruns += dma;
    sessionDroppedBytes += droppedBytes;
}

Stats MusicStats::snapshot() const {
    Stats stats {};
    stats.totalSeconds = totalSeconds;
    stats.totalTracks = totalTracks;
    stats.sessionSeconds = sessionSeconds;
    stats.sessionTracks = sessionTracks;
    stats.sessionUnderruns = sessionUnderruns;
    stats.sessionDmaUnderruns = sessionDmaUnderruns;
    stats.sessionDroppedBytes = sessionDroppedBytes;
    stats.topTrack = topOf(trackPlays, stats.topTrackPlays);
    stats.topArtist = topOf(artistPlays, stats.topArtistPlays);
    return stats;
}

void MusicStats::load() {
    const auto path = statsFilePath();
    std::map<std::string, std::string> properties;
    if (path.empty() || !file::loadPropertiesFile(path, properties)) {
        return;
    }

    totalSeconds = readUnsigned(properties, "totalSeconds");
    totalTracks = readUnsigned(properties, "totalTracks");
    readCounts(properties, "track", trackPlays);
    readCounts(properties, "artist", artistPlays);
    dirty = false;
}

void MusicStats::saveIfDirty() {
    if (!dirty) {
        return;
    }

    const auto path = statsFilePath();
    if (path.empty()) {
        return;
    }

    std::map<std::string, std::string> properties {
        { "totalSeconds", std::format("{}", totalSeconds) },
        { "totalTracks", std::format("{}", totalTracks) },
    };
    writeCounts(properties, "track", trackPlays);
    writeCounts(properties, "artist", artistPlays);

    // Only cleared once the write actually landed, so a save that failed for want of memory or a
    // missing directory is retried on the next flush instead of discarding the counts.
    if (file::savePropertiesFile(path, properties)) {
        dirty = false;
    } else {
        LOG_E(TAG, "Failed to save %s", path.c_str());
    }
}

void MusicStats::reset() {
    totalSeconds = 0;
    totalTracks = 0;
    trackPlays.clear();
    artistPlays.clear();
    sessionSeconds = 0;
    sessionTracks = 0;
    sessionUnderruns = 0;
    sessionDmaUnderruns = 0;
    sessionDroppedBytes = 0;
    dirty = true;
    saveIfDirty();
}

} // namespace tt::service::music
