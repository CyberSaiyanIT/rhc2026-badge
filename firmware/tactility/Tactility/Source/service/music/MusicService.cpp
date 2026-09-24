#include <Tactility/service/music/MusicService.h>
#include <Tactility/service/music/MediaKeys.h>
#include <Tactility/service/music/TrackTags.h>

#include <Tactility/MountPoints.h>
#include <Tactility/service/ServiceManifest.h>
#include <Tactility/service/ServiceRegistration.h>
#include <Tactility/service/music/AudioPlayer.h>
#include <Tactility/service/neopixel/NeoPixel.h>

#include <tactility/device.h>
#include <tactility/drivers/audio_stream.h>
#include <tactility/drivers/power_rail.h>
#include <tactility/drivers/sdcard.h>
#include <tactility/filesystem/file_system.h>
#include <tactility/log.h>

#include <algorithm>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <format>

namespace tt::service::music {

constexpr auto* TAG = "MusicService";
extern const ServiceManifest manifest;

namespace {

// How often the current track is checked for having run out.
constexpr TickType_t TICK_INTERVAL_TICKS = pdMS_TO_TICKS(250);
constexpr uint32_t TICKS_PER_SECOND = 4;
// Half a minute of listening is the most a badge pulled off its lanyard can lose.
constexpr uint32_t STATS_FLUSH_TICKS = 30 * TICKS_PER_SECOND;
// How long the meter keeps the strip after the music stops. Long enough that changing track,
// pausing to look something up, or a gap between albums does not throw the lighting back to its
// other animation and immediately into the meter again. Distinct from the lighting service's own
// inactive timeout, which is counted from user input and measured in tens of minutes.
constexpr uint32_t VU_LINGER_TICKS = 37 * TICKS_PER_SECOND;
// Sized for AudioPlayer::play(), which builds the whole ESP-ADF pipeline on this thread.
constexpr configSTACK_DEPTH_TYPE TICK_STACK_SIZE = 8192;

struct Preset {
    const char* name;
    int8_t gains[EQUALIZER_BANDS];
};

// Bands run 31 Hz to 16 kHz; ADF clamps each to +/-13 dB.
constexpr Preset PRESETS[] = {
    { "Flat",    {  0,  0,  0,  0,  0,  0,  0,  0,  0,  0 } },
    { "Bass",    {  9,  8,  6,  3,  0,  0,  0,  0,  0,  0 } },
    { "Treble",  {  0,  0,  0,  0,  0,  2,  4,  6,  8,  9 } },
    { "Vocal",   { -4, -3,  0,  3,  5,  5,  3,  1,  0, -1 } },
    { "Rock",    {  6,  4,  2, -1, -2,  0,  3,  5,  6,  6 } },
    { "Podcast", { -6, -4,  0,  4,  6,  5,  3,  0, -2, -4 } },
    { "Loudness", {  8,  6,  3,  0, -1, -1,  0,  3,  6,  8 } },
    // The badge's speaker cannot move air below roughly 200 Hz, so feeding it those bands only
    // spends headroom on distortion. Cutting them and lifting the upper mids is what makes it
    // sound louder and clearer at the same amplifier setting.
    { "Badge",   { -9, -7, -2,  3,  5,  6,  4,  2,  0, -2 } },
};
constexpr int PRESET_COUNT = (int) (sizeof(PRESETS) / sizeof(PRESETS[0]));

bool isPlayable(const char* name) {
    static const char* extensions[] = { ".mp3", ".m4a", ".aac" };
    const auto name_length = strlen(name);
    for (const auto* extension : extensions) {
        const auto length = strlen(extension);
        if (name_length > length && strcasecmp(name + name_length - length, extension) == 0) {
            return true;
        }
    }
    return false;
}

/**
 * How deep a folder is followed when it is queued whole, and how many files one press may add.
 *
 * Both matter because this runs with the service locked, and the UI polls telemetry through the
 * same lock four times a second. Whatever is left out is logged rather than dropped quietly.
 */
constexpr int ENQUEUE_MAX_DEPTH = 4;
constexpr int ENQUEUE_MAX_FILES = 256;

bool isDirectory(const std::string& path) {
    struct stat entry_stat {};
    return stat(path.c_str(), &entry_stat) == 0 && S_ISDIR(entry_stat.st_mode);
}

/** Folders first, then files, each group by name, which is the order a browser is expected in. */
std::vector<LibraryEntry> readDirectory(const std::string& directory) {
    std::vector<LibraryEntry> entries;

    DIR* dir = opendir(directory.c_str());
    if (dir == nullptr) {
        LOG_W(TAG, "Cannot open %s", directory.c_str());
        return entries;
    }

    while (auto* entry = readdir(dir)) {
        if (entry->d_name[0] == '.') {
            continue;
        }
        auto path = std::format("{}/{}", directory, entry->d_name);
        // d_type is not filled in by every filesystem here, so the kind comes from stat().
        const bool folder = isDirectory(path);
        if (!folder && !isPlayable(entry->d_name)) {
            continue;
        }
        entries.push_back(LibraryEntry { std::move(path), entry->d_name, folder });
    }
    closedir(dir);

    std::sort(entries.begin(), entries.end(), [](const LibraryEntry& left, const LibraryEntry& right) {
        if (left.isFolder != right.isFolder) {
            return left.isFolder;
        }
        return left.name < right.name;
    });
    return entries;
}

/**
 * @return the mount path of a mounted SD card, empty when none is. Matched on the owning device's
 *     type rather than on the path, so it holds for a device that mounts its card elsewhere.
 */
std::string mountedSdCardPath() {
    struct Found {
        char path[64];
        bool ok;
    } found = {};

    file_system_for_each(&found, [](FileSystem* fs, void* context) {
        auto* result = static_cast<Found*>(context);
        auto* owner = file_system_get_owner(fs);
        // A registered file system whose card is out reports not-mounted, so both are checked.
        if (owner == nullptr || device_get_type(owner) != &SDCARD_TYPE || !file_system_is_mounted(fs)) {
            return true;
        }
        if (file_system_get_path(fs, result->path, sizeof(result->path)) == ERROR_NONE) {
            result->ok = true;
            return false;
        }
        return true;
    });

    if (!found.ok) {
        LOG_I(TAG, "No mounted SD card; using internal storage");
    }
    return found.ok ? std::string(found.path) : std::string {};
}

} // namespace

// region Private

void MusicService::resolveLibraryRootLocked() const {
    if (!libraryDirectory.empty()) {
        return;
    }

    // A mounted SD card is the library. The internal partition is the fallback rather than a
    // second source, so what someone brings on a card is never mixed with what shipped on the
    // badge.
    const auto sdcard_path = mountedSdCardPath();
    libraryDirectory = std::format("{}/Music", sdcard_path.empty() ? file::MOUNT_POINT_DATA : sdcard_path.c_str());
    LOG_I(TAG, "Library root: %s", libraryDirectory.c_str());
}

int MusicService::enqueueLocked(const std::string& path, int depth) {
    if ((int) queue.size() >= ENQUEUE_MAX_FILES) {
        return 0;
    }

    if (!isDirectory(path)) {
        queue.push_back(path);
        return 1;
    }

    if (depth >= ENQUEUE_MAX_DEPTH) {
        LOG_W(TAG, "Not descending past %s: %d folders deep", path.c_str(), depth);
        return 0;
    }

    int added = 0;
    for (const auto& entry : readDirectory(path)) {
        added += enqueueLocked(entry.path, depth + 1);
    }
    return added;
}

void MusicService::removeQueueIndexLocked(int index) {
    if (index < 0 || index >= (int) queue.size()) {
        return;
    }

    const bool wasPlaying = index == trackIndex;
    queue.erase(queue.begin() + index);

    if (!wasPlaying) {
        // Only the entries after it shift down, so anything before keeps its position.
        if (index < trackIndex) {
            trackIndex--;
        }
        return;
    }

    if (queue.empty()) {
        trackIndex = -1;
        releasePlayer();
        return;
    }

    // Whatever moved into this slot is the next track. Off the end, the same rule that ends a
    // track applies: wrap when repeating, otherwise stop.
    if (index >= (int) queue.size()) {
        if (repeat != Repeat::All) {
            trackIndex = -1;
            releasePlayer();
            return;
        }
        startTrackLocked(0);
        return;
    }
    startTrackLocked(index);
}

// Runs on the audio output task: an atomic read and five atomic stores, nothing that can block.
void MusicService::onAudioLevels(const AudioLevels& levels, void* context) {
    auto* self = static_cast<MusicService*>(context);
    if (!self->vuSeeding.load(std::memory_order_relaxed)) {
        return;
    }
    neopixel::setVuLevels(neopixel::VuLevels {
        .left = levels.left,
        .right = levels.right,
        .bass = levels.bass,
        .mid = levels.mid,
        .treble = levels.treble,
    });
}

void MusicService::updateVuLocked(bool fromTick) {
    const auto state = player != nullptr ? player->getTelemetry().state : State::Stopped;
    // Buffering counts as playing so the strip lights the moment a track is asked for, rather
    // than a second later once the ring has filled.
    const bool playing = state == State::Playing || state == State::Buffering;

    if (vuSeeding.load() && playing) {
        vuIdleTicks = 0;
        if (!vuActive) {
            vuActive = true;
            neopixel::setVuActive(true);
        }
        return;
    }

    if (!vuActive || !fromTick) {
        return;
    }
    if (++vuIdleTicks >= VU_LINGER_TICKS) {
        vuActive = false;
        neopixel::setVuActive(false);
    }
}

void MusicService::ensurePlayer() {
    if (player != nullptr) {
        return;
    }
    player = std::make_unique<AudioPlayer>();
    // Installed before the first play(), which is what the callback contract requires.
    player->setLevelCallback(onAudioLevels, this);
    setSpeakerLocked(true);
    updateVuLocked(false);
}

void MusicService::releasePlayer() {
    if (player == nullptr) {
        return;
    }
    // These belong to the pipeline being torn down, so they are banked before it goes.
    const auto telemetry = player->getTelemetry();
    stats.addUnderruns(telemetry.underruns, telemetry.dmaUnderruns, telemetry.droppedBytes);
    player.reset();
    setSpeakerLocked(false);
    // Deliberately not handed back here: the strip stays on the meter until the tick thread has
    // aged out the dead time, so the end of one track does not flick the lighting twice.
}

void MusicService::startTrackLocked(int index) {
    if (queue.empty()) {
        return;
    }
    ensurePlayer();
    trackIndex = std::clamp(index, 0, (int) queue.size() - 1);

    // Set before playing, not after: these live past the ring buffer and the output task asks for
    // them as soon as it starts, which is early enough to race a push that came after play().
    player->setMonoDownmix(monoDownmix);
    player->setDynamics(dynamics);
    player->setFade(fade);
    player->setBalancePercent(balancePercent);
    for (int band = 0; band < EQUALIZER_BANDS; band++) {
        player->setEqualizerBand(band, bandGains[band]);
    }

    const auto& path = queue[trackIndex];
    player->play(path);
    // Reported here rather than where the player reports its own tasks: this thread outlives every
    // pipeline, and a track change is the one point it passes through per track. The value is the
    // least it ever had free, so it covers every tick before this one.
    LOG_I(TAG, "Stack headroom of music_tick: %u bytes", (unsigned) tickThread->getStackSpace());
    setMediaKeysEnabled(true);

    const auto slash = path.find_last_of('/');
    trackArtist = readArtist(path);
    stats.addPlay(slash == std::string::npos ? path : path.substr(slash + 1), trackArtist);

    // These reach pipeline elements, which only exist once play() has built them.
    player->setSpeed((float) speedPercent / 100.0f);
    player->setPitch((float) pitchPercent / 100.0f);
    applyGainLocked();
}

int MusicService::nextIndexLocked() {
    if (queue.empty()) {
        return -1;
    }
    if (shuffle) {
        std::uniform_int_distribution<int> distribution(0, (int) queue.size() - 1);
        return distribution(rng);
    }
    const int next = trackIndex + 1;
    if (next >= (int) queue.size()) {
        return repeat == Repeat::All ? 0 : -1;
    }
    return next;
}

// A boosted band adds level on top of what the file already had, and 16-bit output has no room
// above full scale. Backing the output off by the largest boost keeps the shape of the curve
// instead of trading it for clipping.
int MusicService::effectiveGainLocked() const {
    int trim = 0;
    if (autoPreamp) {
        for (const int gain : bandGains) {
            trim = std::max(trim, gain);
        }
    }
    return gainDb - trim;
}

void MusicService::applyGainLocked() {
    if (player != nullptr) {
        player->setGainDb(effectiveGainLocked());
    }
}

bool MusicService::setSpeakerLocked(bool enabled) {
    if (speakerRail == nullptr) {
        return false;
    }
    const error_t result = enabled ? power_rail_enable(speakerRail) : power_rail_disable(speakerRail);
    if (result != ERROR_NONE) {
        LOG_E(TAG, "Failed to switch speaker %s", enabled ? "on" : "off");
        return false;
    }
    speakerOn = enabled;
    return true;
}

void MusicService::tickMain() {
    // Applied from here rather than from onStart(): loadSettings() reaches the service through
    // the registry, which does not resolve it yet while it is still starting.
    loadSettings();
    {
        auto lock = mutex.asScopedLock();
        lock.lock();
        stats.load();
    }

    while (tickRunning) {
        vTaskDelay(TICK_INTERVAL_TICKS);
        onTick();
    }
}

void MusicService::onTick() {
    auto lock = mutex.asScopedLock();
    lock.lock();

    switch (takeMediaCommand()) {
        case MediaCommand::PlayPause: playPauseLocked(); break;
        case MediaCommand::Next: nextLocked(); break;
        case MediaCommand::Previous: previousLocked(); break;
        default: break;
    }

    // Ahead of the early exits below: the hand-back has to keep ageing once the player is gone.
    updateVuLocked(true);

    if (player != nullptr && player->getTelemetry().state == State::Playing) {
        statsPartialTicks++;
        if (statsPartialTicks >= TICKS_PER_SECOND) {
            statsPartialTicks = 0;
            stats.addSeconds(1);
        }
    }
    if (++statsFlushTicks >= STATS_FLUSH_TICKS) {
        statsFlushTicks = 0;
        stats.saveIfDirty();
    }

    if (player == nullptr) {
        return;
    }

    if (!player->getTelemetry().finished) {
        return;
    }

    // Repeat::One restarts the same track; anything else advances the queue.
    if (repeat == Repeat::One) {
        startTrackLocked(trackIndex);
        return;
    }
    const int next = nextIndexLocked();
    if (next < 0) {
        releasePlayer();
    } else {
        startTrackLocked(next);
    }
}

// endregion

// region Lifecycle

bool MusicService::onStart(ServiceContext& serviceContext) {
    if (device_get_by_name("speaker_power", &speakerRail) != ERROR_NONE) {
        LOG_W(TAG, "No speaker_power rail");
        speakerRail = nullptr;
    }

    LOG_I(TAG, "Started (speaker rail %s)", speakerRail != nullptr ? "found" : "missing");

    installMediaKeys();

    tickRunning = true;
    tickThread = std::make_unique<Thread>("music_tick", TICK_STACK_SIZE, [this] {
        tickMain();
        return 0;
    });
    tickThread->start();

    return true;
}

void MusicService::onStop(ServiceContext& serviceContext) {
    removeMediaKeys();
    tickRunning = false;
    if (tickThread != nullptr) {
        tickThread->join();
        tickThread.reset();
    }

    auto lock = mutex.asScopedLock();
    lock.lock();
    releasePlayer();
    if (speakerRail != nullptr) {
        device_put(speakerRail);
        speakerRail = nullptr;
    }
}

// endregion

// region Playlist

bool MusicService::isAvailable() const {
    return device_exists_of_type(&AUDIO_STREAM_TYPE);
}

std::vector<LibraryEntry> MusicService::listLibrary(const std::string& directory) const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    resolveLibraryRootLocked();
    return readDirectory(directory.empty() ? libraryDirectory : directory);
}

size_t MusicService::getQueueCount() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return queue.size();
}

int MusicService::getTrackIndex() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return trackIndex;
}

std::string MusicService::getTrackPath() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    if (trackIndex < 0 || trackIndex >= (int) queue.size()) {
        return {};
    }
    return queue[trackIndex];
}

std::string MusicService::getTrackArtist() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return trackArtist;
}

std::string MusicService::getQueuePathAt(int index) const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    if (index < 0 || index >= (int) queue.size()) {
        return {};
    }
    return queue[index];
}

std::string MusicService::getLibraryPath() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    resolveLibraryRootLocked();
    return libraryDirectory;
}

void MusicService::playQueueIndex(int index) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    if (index < 0 || index >= (int) queue.size()) {
        return;
    }
    startTrackLocked(index);
}

void MusicService::enqueue(const std::string& path) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    const int added = enqueueLocked(path, 0);
    if (added == 0) {
        LOG_W(TAG, "Nothing queued from %s (queue holds %d)", path.c_str(), (int) queue.size());
    }
}

void MusicService::enqueueAndPlay(const std::string& path) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    const int start = (int) queue.size();
    if (enqueueLocked(path, 0) == 0) {
        return;
    }
    startTrackLocked(start);
}

void MusicService::removeFromQueue(int index) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    removeQueueIndexLocked(index);
}

void MusicService::removeFromQueueByPath(const std::string& path) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    // Back to front, so each removal leaves the indexes still to check where they were.
    for (int index = (int) queue.size() - 1; index >= 0; index--) {
        if (queue[index] == path) {
            removeQueueIndexLocked(index);
        }
    }
}

bool MusicService::isQueued(const std::string& path) const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return std::find(queue.begin(), queue.end(), path) != queue.end();
}

// endregion

// region Transport

void MusicService::playPause() {
    auto lock = mutex.asScopedLock();
    lock.lock();
    playPauseLocked();
}

void MusicService::playPauseLocked() {
    const State state = player != nullptr ? player->getTelemetry().state : State::Stopped;
    switch (state) {
        case State::Playing:
            player->pause();
            break;
        case State::Paused:
            player->resume();
            break;
        default:
            // Nothing to fall back on when the queue is empty: the library is what fills it.
            if (!queue.empty()) {
                startTrackLocked(trackIndex < 0 ? 0 : trackIndex);
            }
            break;
    }
}

void MusicService::next() {
    auto lock = mutex.asScopedLock();
    lock.lock();
    nextLocked();
}

void MusicService::nextLocked() {
    const int index = nextIndexLocked();
    if (index < 0) {
        releasePlayer();
    } else {
        startTrackLocked(index);
    }
}

void MusicService::previous() {
    auto lock = mutex.asScopedLock();
    lock.lock();
    previousLocked();
}

void MusicService::previousLocked() {
    if (queue.empty()) {
        return;
    }
    if (player != nullptr && player->getTelemetry().positionSeconds > 3) {
        startTrackLocked(trackIndex);
        return;
    }
    const int previous = trackIndex - 1;
    startTrackLocked(previous < 0 ? (int) queue.size() - 1 : previous);
}

void MusicService::seek(uint32_t seconds) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    if (player != nullptr) {
        player->seek(seconds);
    }
}

Telemetry MusicService::getTelemetry() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return player != nullptr ? player->getTelemetry() : Telemetry {};
}

// endregion

// region Modes

bool MusicService::isShuffleEnabled() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return shuffle;
}

void MusicService::setShuffleEnabled(bool enabled) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    shuffle = enabled;
}

Repeat MusicService::getRepeat() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return repeat;
}

void MusicService::setRepeat(Repeat value) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    repeat = value;
}

// endregion

// region DSP

int MusicService::getPresetIndex() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return presetIndex;
}

void MusicService::setPresetIndex(int index) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    if (index < 0 || index >= PRESET_COUNT) {
        return;
    }
    presetIndex = index;
    for (int band = 0; band < EQUALIZER_BANDS; band++) {
        bandGains[band] = PRESETS[index].gains[band];
        if (player != nullptr) {
            player->setEqualizerBand(band, bandGains[band]);
        }
    }
    applyGainLocked();
}

int MusicService::getBandGainDb(int band) const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return (band >= 0 && band < EQUALIZER_BANDS) ? bandGains[band] : 0;
}

void MusicService::setBandGainDb(int band, int value) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    if (band < 0 || band >= EQUALIZER_BANDS) {
        return;
    }
    bandGains[band] = value;
    if (player != nullptr) {
        player->setEqualizerBand(band, value);
    }
    applyGainLocked();
}

int MusicService::getGainDb() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return gainDb;
}

void MusicService::setGainDb(int value) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    gainDb = value;
    applyGainLocked();
}

int MusicService::getSpeedPercent() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return speedPercent;
}

void MusicService::setSpeedPercent(int percent) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    speedPercent = percent;
    if (player != nullptr) {
        player->setSpeed((float) percent / 100.0f);
    }
}

int MusicService::getPitchPercent() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return pitchPercent;
}

void MusicService::setPitchPercent(int percent) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    pitchPercent = percent;
    if (player != nullptr) {
        player->setPitch((float) percent / 100.0f);
    }
}

Dynamics MusicService::getDynamics() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return dynamics;
}

void MusicService::setDynamics(Dynamics value) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    dynamics = value;
    if (player != nullptr) {
        player->setDynamics(value);
    }
}

Fade MusicService::getFade() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return fade;
}

void MusicService::setFade(Fade value) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    fade = value;
    if (player != nullptr) {
        player->setFade(value);
    }
}

int MusicService::getBalancePercent() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return balancePercent;
}

void MusicService::setBalancePercent(int percent) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    balancePercent = percent;
    if (player != nullptr) {
        player->setBalancePercent(percent);
    }
}

bool MusicService::isMonoDownmixEnabled() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return monoDownmix;
}

void MusicService::setMonoDownmixEnabled(bool enabled) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    monoDownmix = enabled;
    if (player != nullptr) {
        player->setMonoDownmix(enabled);
    }
}

int MusicService::getAutoPreampTrimDb() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return gainDb - effectiveGainLocked();
}

bool MusicService::isAutoPreampEnabled() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return autoPreamp;
}

void MusicService::setAutoPreampEnabled(bool enabled) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    autoPreamp = enabled;
    applyGainLocked();
}

// endregion

// region Speaker

bool MusicService::isVuSeedingEnabled() const {
    return vuSeeding.load();
}

void MusicService::setVuSeedingEnabled(bool enabled) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    vuSeeding = enabled;
    if (!enabled && vuActive) {
        // Switching it off by hand is an instruction, not a gap in the music: no dead time.
        vuActive = false;
        neopixel::setVuActive(false);
        return;
    }
    updateVuLocked(false);
}

Stats MusicService::getStats() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    auto snapshot = stats.snapshot();
    if (player != nullptr) {
        const auto telemetry = player->getTelemetry();
        snapshot.sessionUnderruns += telemetry.underruns;
        snapshot.sessionDmaUnderruns += telemetry.dmaUnderruns;
        snapshot.sessionDroppedBytes += telemetry.droppedBytes;
    }
    return snapshot;
}

void MusicService::resetStats() {
    auto lock = mutex.asScopedLock();
    lock.lock();
    stats.reset();
}

bool MusicService::isSpeakerEnabled() const {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return speakerOn;
}

bool MusicService::setSpeakerEnabled(bool enabled) {
    auto lock = mutex.asScopedLock();
    lock.lock();
    return setSpeakerLocked(enabled);
}

// endregion

int getPresetCount() { return PRESET_COUNT; }

const char* getPresetName(int index) {
    return (index >= 0 && index < PRESET_COUNT) ? PRESETS[index].name : "";
}

extern const ServiceManifest manifest = {
    .id = "tactility.music",
    .createService = create<MusicService>
};

} // namespace tt::service::music
