#include <sys/stat.h>
#include <Tactility/service/music/Music.h>
#include <Tactility/service/music/MusicService.h>
#include <Tactility/service/music/TrackTags.h>
#include <Tactility/service/ServiceManifest.h>
#include <Tactility/service/ServiceRegistration.h>

#include <tactility/log.h>

namespace tt::service::music {

constexpr auto* TAG = "Music";

extern const ServiceManifest manifest;

// The service is only registered when audio hardware is present (see Tactility.cpp);
// treat "not registered" the same as "no hardware" rather than asserting.
static std::shared_ptr<MusicService> tryFindMusicService() {
    return findServiceById<MusicService>(manifest.id);
}

Stats getStats() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getStats() : Stats {};
}

void resetStats() {
    if (auto service = tryFindMusicService()) {
        service->resetStats();
    }
}

bool isAvailable() {
    auto service = tryFindMusicService();
    return service != nullptr && service->isAvailable();
}

std::vector<LibraryEntry> listLibrary(const std::string& directory) {
    auto service = tryFindMusicService();
    return service != nullptr ? service->listLibrary(directory) : std::vector<LibraryEntry> {};
}

size_t getQueueCount() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getQueueCount() : 0;
}

int getTrackIndex() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getTrackIndex() : -1;
}

std::string getTrackPath() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getTrackPath() : std::string {};
}

std::string getTrackArtist() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getTrackArtist() : std::string {};
}

std::string getQueuePathAt(int index) {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getQueuePathAt(index) : std::string {};
}

void enqueue(const std::string& path) {
    if (auto service = tryFindMusicService()) {
        service->enqueue(path);
    }
}

void enqueueAndPlay(const std::string& path) {
    if (auto service = tryFindMusicService()) {
        service->enqueueAndPlay(path);
    }
}

void removeFromQueue(int index) {
    if (auto service = tryFindMusicService()) {
        service->removeFromQueue(index);
    }
}

void removeFromQueueByPath(const std::string& path) {
    if (auto service = tryFindMusicService()) {
        service->removeFromQueueByPath(path);
    }
}

bool isQueued(const std::string& path) {
    auto service = tryFindMusicService();
    return service != nullptr && service->isQueued(path);
}

std::string getLibraryPath() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getLibraryPath() : std::string {};
}

void playQueueIndex(int index) {
    if (auto service = tryFindMusicService()) {
        service->playQueueIndex(index);
    }
}

void playPause() {
    if (auto service = tryFindMusicService()) {
        service->playPause();
    }
}

void next() {
    if (auto service = tryFindMusicService()) {
        service->next();
    }
}

void previous() {
    if (auto service = tryFindMusicService()) {
        service->previous();
    }
}

void seek(uint32_t seconds) {
    if (auto service = tryFindMusicService()) {
        service->seek(seconds);
    }
}

Telemetry getTelemetry() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getTelemetry() : Telemetry {};
}

bool isShuffleEnabled() {
    auto service = tryFindMusicService();
    return service != nullptr && service->isShuffleEnabled();
}

void setShuffleEnabled(bool enabled) {
    if (auto service = tryFindMusicService()) {
        service->setShuffleEnabled(enabled);
    }
}

Repeat getRepeat() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getRepeat() : Repeat::All;
}

void setRepeat(Repeat repeat) {
    if (auto service = tryFindMusicService()) {
        service->setRepeat(repeat);
    }
}

int getPresetIndex() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getPresetIndex() : 0;
}

void setPresetIndex(int index) {
    if (auto service = tryFindMusicService()) {
        service->setPresetIndex(index);
    }
}

int getBandGainDb(int band) {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getBandGainDb(band) : 0;
}

void setBandGainDb(int band, int gainDb) {
    if (auto service = tryFindMusicService()) {
        service->setBandGainDb(band, gainDb);
    }
}

int getGainDb() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getGainDb() : 0;
}

void setGainDb(int gainDb) {
    if (auto service = tryFindMusicService()) {
        service->setGainDb(gainDb);
    }
}

int getSpeedPercent() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getSpeedPercent() : 100;
}

void setSpeedPercent(int percent) {
    if (auto service = tryFindMusicService()) {
        service->setSpeedPercent(percent);
    }
}

int getPitchPercent() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getPitchPercent() : 100;
}

void setPitchPercent(int percent) {
    if (auto service = tryFindMusicService()) {
        service->setPitchPercent(percent);
    }
}

Dynamics getDynamics() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getDynamics() : Dynamics::Off;
}

void setDynamics(Dynamics value) {
    if (auto service = tryFindMusicService()) {
        service->setDynamics(value);
    }
}

Fade getFade() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getFade() : Fade::Off;
}

void setFade(Fade value) {
    if (auto service = tryFindMusicService()) {
        service->setFade(value);
    }
}

int getBalancePercent() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getBalancePercent() : 0;
}

void setBalancePercent(int percent) {
    if (auto service = tryFindMusicService()) {
        service->setBalancePercent(percent);
    }
}

bool isMonoDownmixEnabled() {
    auto service = tryFindMusicService();
    return service != nullptr && service->isMonoDownmixEnabled();
}

void setMonoDownmixEnabled(bool enabled) {
    if (auto service = tryFindMusicService()) {
        service->setMonoDownmixEnabled(enabled);
    }
}

int getAutoPreampTrimDb() {
    auto service = tryFindMusicService();
    return service != nullptr ? service->getAutoPreampTrimDb() : 0;
}

bool isAutoPreampEnabled() {
    auto service = tryFindMusicService();
    return service == nullptr || service->isAutoPreampEnabled();
}

void setAutoPreampEnabled(bool enabled) {
    if (auto service = tryFindMusicService()) {
        service->setAutoPreampEnabled(enabled);
    }
}

bool isVuSeedingEnabled() {
    auto service = tryFindMusicService();
    return service != nullptr && service->isVuSeedingEnabled();
}

void setVuSeedingEnabled(bool enabled) {
    if (auto service = tryFindMusicService()) {
        service->setVuSeedingEnabled(enabled);
    }
}

bool isSpeakerEnabled() {
    auto service = tryFindMusicService();
    return service != nullptr && service->isSpeakerEnabled();
}

bool setSpeakerEnabled(bool enabled) {
    if (auto service = tryFindMusicService()) {
        return service->setSpeakerEnabled(enabled);
    }
    return false;
}


TrackCover readTrackCover(const std::string& trackPath) {
    const auto cover = readCoverArt(trackPath);
    return TrackCover { cover.data, cover.size };
}

void releaseTrackCover(TrackCover* cover) {
    if (cover == nullptr) {
        return;
    }
    CoverArt owned { cover->data, cover->size };
    releaseCoverArt(&owned);
    cover->data = nullptr;
    cover->size = 0;
}

std::string findAlbumArt(const std::string& trackPath) {
    const auto slash = trackPath.find_last_of('/');
    const auto directory = slash == std::string::npos ? std::string(".") : trackPath.substr(0, slash);
    const auto dot = trackPath.find_last_of('.');
    const auto stem = trackPath.substr(0, dot == std::string::npos ? trackPath.size() : dot);

    const std::string candidates[] = {
        stem + ".jpg", stem + ".png",
        directory + "/cover.jpg", directory + "/cover.png",
        directory + "/folder.jpg", directory + "/folder.png",
    };

    for (const auto& candidate : candidates) {
        struct stat file_stat {};
        if (stat(candidate.c_str(), &file_stat) == 0) {
            LOG_I(TAG, "Cover art: %s (%ld bytes)", candidate.c_str(), (long) file_stat.st_size);
            // LVGL's stdio filesystem driver is registered on drive A.
            return "A:" + candidate;
        }
    }
    // Only sibling files are looked for; art embedded in the track's own tags is not extracted.
    LOG_I(TAG, "No cover art beside %s", trackPath.c_str());
    return {};
}

} // namespace tt::service::music
