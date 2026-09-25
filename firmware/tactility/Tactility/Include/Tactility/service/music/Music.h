#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace tt::service::music {

constexpr int EQUALIZER_BANDS = 10;

/** Centre frequencies of the equalizer bands, for labelling. */
extern const uint16_t EQUALIZER_BAND_FREQUENCIES[EQUALIZER_BANDS];

enum class State : uint8_t {
    Stopped,
    /** Filling the ring buffer; playback has not started yet. */
    Buffering,
    Playing,
    Paused,
    Error
};

enum class Repeat : uint8_t { Off, All, One };

/** What, if anything, evens out the difference between the loud and quiet parts of a track. */
enum class Dynamics : uint8_t {
    Off,
    /** One compressor over the whole signal. */
    Compressor,
    /** Four bands compressed separately, so a loud kick stops ducking the vocal with it. */
    Multiband
};

/** How long playback takes to arrive and to leave. */
enum class Fade : uint8_t { Off, Short, Long };

/** One frame of metering for whatever wants to visualise the audio. All fields are 0-255. */
struct AudioLevels {
    /** Per-channel RMS on a dB scale. */
    uint8_t left = 0;
    uint8_t right = 0;
    /** Energy below 200 Hz, 200 Hz to 4 kHz, and above 4 kHz. Gentle 6 dB/oct splits that
     *  overlap heavily: enough to colour a light by tone, not a spectrum analysis. */
    uint8_t bass = 0;
    uint8_t mid = 0;
    uint8_t treble = 0;
};

struct Telemetry {
    State state = State::Stopped;
    /** How full the ring buffer is, 0-100. */
    uint8_t bufferPercent = 0;
    /** Times the ring buffer ran dry after playback started, so the decoder fell behind. */
    uint32_t underruns = 0;
    /**
     * Times the transmit DMA had nothing left to send. Counted by the I2S hardware, so it catches
     * what the ring cannot: data ready on time but the output task scheduled too late.
     */
    uint32_t dmaUnderruns = 0;
    /** Bytes the codec would not accept within the write timeout, and which were dropped. */
    uint32_t droppedBytes = 0;
    /** How near empty the ring came since the track started, 0-100. */
    uint8_t bufferLowPercent = 100;
    uint32_t sourceSampleRate = 0;
    uint8_t sourceChannels = 0;
    uint32_t positionSeconds = 0;
    uint32_t durationSeconds = 0;
    /** Set once the decoder reaches the end of the file. */
    bool finished = false;
    /** The output reached full scale since this was last read. */
    bool clipping = false;
};

/** What has been listened to, since power-on and over the badge's life. */
struct Stats {
    uint32_t sessionSeconds = 0;
    uint32_t sessionTracks = 0;
    /** Summed over every track this session, where Telemetry's count covers only the current one. */
    uint32_t sessionUnderruns = 0;
    uint32_t sessionDmaUnderruns = 0;
    uint32_t sessionDroppedBytes = 0;
    uint32_t totalSeconds = 0;
    /** Every start counts, so a track played twice counts twice. */
    uint32_t totalTracks = 0;
    std::string topTrack;
    uint32_t topTrackPlays = 0;
    std::string topArtist;
    uint32_t topArtistPlays = 0;
};

Stats getStats();
/** Clears the listening history, leaving the sound settings alone. */
void resetStats();

/** @return true when audio output hardware is present */
bool isAvailable();

/** One row of the library browser: a folder to descend into, or a track to queue. */
struct LibraryEntry {
    std::string path;
    /** What to show: the file or folder name, without the rest of the path. */
    std::string name;
    bool isFolder = false;
};

/**
 * @return the folders and playable files directly inside @a directory, folders first and each
 *     group sorted by name
 */
std::vector<LibraryEntry> listLibrary(const std::string& directory);

/** How many entries the queue holds. Only the library puts anything in it. */
size_t getQueueCount();
/** @return index into the queue, or -1 when nothing has been selected yet */
int getTrackIndex();
/** @return path of the selected track, empty when the queue is empty */
std::string getTrackPath();
/**
 * @return the selected track's performer, or "Unknown Artist" when it carries no usable tag
 *
 * Read once when the track starts rather than on demand, since it costs a file read.
 */
std::string getTrackArtist();
/**
 * Cover art is looked up as a file beside the track. Art embedded in the tags is not extracted
 * \embedded, so a track whose art is only in its tags reports none here.
 *
 * \embedded TrackTags.cpp skips the oversized frames that carry it.
 *
 * @return LVGL path ("A:"-prefixed) of the art beside @a trackPath, empty when there is none
 */
std::string findAlbumArt(const std::string& trackPath);

/**
 * Whether a track's cover art is read and shown. Off while playback's internal RAM use is being
 * accounted for, decoding being the only part of the art path that runs while music plays.
 */
constexpr bool ALBUM_ART_ENABLED = false;

/** Encoded cover art read out of a track's own tags, held in external RAM. */
struct TrackCover {
    uint8_t* data = nullptr;
    size_t size = 0;
};

/**
 * Reads the picture a track carries in its tags, which is where cover art normally lives. A file
 * beside the track, via findAlbumArt(), is the fallback rather than the first choice.
 *
 * @return empty when the track has no picture; release what it does return with releaseTrackCover()
 */
TrackCover readTrackCover(const std::string& trackPath);

void releaseTrackCover(TrackCover* cover);

/** @return path of the queue entry at @a index, empty when it is out of range */
std::string getQueuePathAt(int index);
/** Selects and plays the queue entry at @a index, whatever is playing now. */
void playQueueIndex(int index);

/** Appends @a path, or every playable file under it when it names a folder. */
void enqueue(const std::string& path);
/** Appends @a path and starts playing it straight away. */
void enqueueAndPlay(const std::string& path);

/**
 * Drops the queue entry at @a index. If it was the one playing, playback moves on to whatever
 * took its place, or stops once the queue runs out.
 */
void removeFromQueue(int index);
/**
 * Drops every entry naming @a path, so a second press does nothing and the library can show one
 * state per file rather than a count.
 */
void removeFromQueueByPath(const std::string& path);
bool isQueued(const std::string& path);

/** @return the root the library is browsed from: a mounted SD card's, or the internal one */
std::string getLibraryPath();

/** Plays, pauses or resumes. Starts the selected track when stopped. */
void playPause();
void next();
/** Restarts the current track when more than a few seconds in, like a conventional previous button. */
void previous();
void seek(uint32_t seconds);
Telemetry getTelemetry();

bool isShuffleEnabled();
void setShuffleEnabled(bool enabled);
Repeat getRepeat();
void setRepeat(Repeat repeat);

int getPresetCount();
const char* getPresetName(int index);
int getPresetIndex();
void setPresetIndex(int index);

/** @param gainDb per-band gain, -13 to 13 */
int getBandGainDb(int band);
void setBandGainDb(int band, int gainDb);

/** @param gainDb output gain, -20 to 20 */
int getGainDb();
void setGainDb(int gainDb);

/** @param percent playback rate, 50 to 200, pitch preserved */
int getSpeedPercent();
void setSpeedPercent(int percent);

Dynamics getDynamics();
void setDynamics(Dynamics value);

/** @param percent -100 hard left to 100 hard right */
int getBalancePercent();
void setBalancePercent(int percent);

Fade getFade();
void setFade(Fade value);

/** @param percent pitch, 50 to 200, playback rate preserved */
int getPitchPercent();
void setPitchPercent(int percent);

/** Sums both channels into each, for the badge's single speaker. */
bool isMonoDownmixEnabled();
void setMonoDownmixEnabled(bool enabled);

/** Trims the output by the largest band boost, so a lifted curve does not clip. */
bool isAutoPreampEnabled();
/** @return how many dB the trim is taking off, 0 when it is off or the curve has no boost */
int getAutoPreampTrimDb();
void setAutoPreampEnabled(bool enabled);

/**
 * While seeding, playback feeds the NeoPixel service's VU meter and switches it into that mode,
 * putting back whatever it was showing once playback stops.
 */
bool isVuSeedingEnabled();
void setVuSeedingEnabled(bool enabled);

/** @return true when the amplifier's power rail is on */
bool isSpeakerEnabled();
/** @return true when the amplifier rail actually switched; the power manager can refuse its supply */
bool setSpeakerEnabled(bool enabled);

/**
 * Takes the play/pause, next and previous keys for @a appInstanceId while it is in front. Without
 * a claim those keys drive playback from wherever the user is.
 */
void claimMediaKeys(uint32_t appInstanceId);
void releaseMediaKeys(uint32_t appInstanceId);

/**
 * Hands the claiming app every media key as it happens, press and release, instead of leaving it
 * to LVGL. Runs on the LVGL task with the lock held, so it must not block.
 *
 * @return true to consume the key
 */
using MediaKeyHandlerFn = bool (*)(uint32_t key, bool pressed, void* context);
void setMediaKeyHandler(uint32_t appInstanceId, MediaKeyHandlerFn handler, void* context);

/** Applies the saved settings, or the built-in ones when nothing has been saved yet. */
void loadSettings();
/**
 * Writes the current settings. Not automatic: the player flushes from its own event loop, keeping
 * the write off the LVGL lock its widget callbacks hold.
 */
void saveSettings();
/** Puts every setting back to its built-in value and stores that. */
void resetSettings();

} // namespace tt::service::music
