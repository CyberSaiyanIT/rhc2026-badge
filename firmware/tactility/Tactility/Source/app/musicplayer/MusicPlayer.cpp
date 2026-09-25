#include <Tactility/service/audio/Audio.h>
#include <Tactility/service/music/Music.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <lvgl/fonts.h>
#include <lvgl/thumbnail.h>
#include <lvgl/lvgl.h>
#include <lvgl_window_manager/window_manager.h>
#include <tactility/check.h>
#include <tactility/log.h>

#include <lvgl.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <sys/stat.h>
#include <format>
#include <string>
#include <vector>

namespace tt::app::musicplayer {

extern const ::AppManifest manifest;

namespace {

namespace music = service::music;

constexpr auto* TAG = "MusicPlayer";

// The badge's dedicated media keys, as emitted by romhack_keypad.cpp.
constexpr uint32_t KEY_MEDIA_PLAY_PAUSE = 0x20000;
constexpr uint32_t KEY_MEDIA_PREV = 0x20001;
constexpr uint32_t KEY_MEDIA_NEXT = 0x20002;

constexpr float VOLUME_STEP_PERCENT = 5.0f;
// What full scale on screen asks of the codec. Above this the amplifier distorts.
constexpr float VOLUME_SOFT_MAX = 95.0f;
// Volume is counted in whole steps rather than percent: the codec quantises what it is given, so
// reading a percentage back and stepping that drifts off the grid within a press or two.
constexpr float VOLUME_CODEC_STEP = VOLUME_SOFT_MAX * VOLUME_STEP_PERCENT / 100.0f;
constexpr int VOLUME_STEPS = (int) (100.0f / VOLUME_STEP_PERCENT);
// How long a list row holds still before its text starts scrolling.
constexpr uint32_t LIST_SCROLL_DELAY_MS = 1500;
// How long a change waits before it reaches flash. Long enough to coalesce a slider drag, short
// enough that a badge unplugged mid-session keeps what was set.
constexpr uint32_t SETTINGS_FLUSH_MS = 2000;

constexpr int32_t ART_SIZE = 88;
// Long enough for the page to have been laid out and drawn once. Decoding a cover takes long
// enough to be seen as the whole screen arriving late if it happens on the first pass.
constexpr uint32_t ART_LOAD_DELAY_MS = 120;
/**
 * How full the ring buffer must be before the cover is decoded. Decoding reads the same SD card
 * the track does, so it waits for slack rather than for a fixed delay.
 */
constexpr uint8_t ART_SAFE_BUFFER_PERCENT = 40;
/** After this many tries the art is decoded regardless, so a stalled buffer cannot hide it. */
constexpr int ART_MAX_WAITS = 20;

// How many values each cycle steps through. Both enums are contiguous from 0, so deriving the
// count here means adding an entry cannot leave a cycle stepping over it.
constexpr uint8_t DYNAMICS_CHOICES = (uint8_t) music::Dynamics::Multiband + 1;
constexpr uint8_t FADE_CHOICES = (uint8_t) music::Fade::Long + 1;

/**
 * Set when a control has moved, cleared once the service has written the file. At namespace scope
 * because it tracks the service's state, which is one thing however many apps are open.
 */
bool settingsDirty = false;

enum class Page : uint8_t { Player, Library, Queue, Dsp, Equalizer, Stats };

/** The rows of the stats page, in the order they are shown. */
enum class Stat : uint8_t {
    SessionTime, SessionTracks, Buffer, RingDry, DmaDry, Dropped,
    TotalTime, TotalTracks, TopTrack, TopArtist,
    Count
};

enum class Unit : uint8_t { Plain, Decibels, Percent, Balance };

struct Context {
    uint32_t appInstanceId = 0;
    Page page = Page::Player;

    /** Last values rendered into the header, so it is only rebuilt when they actually change. */
    int shownTrackIndex = -1;
    bool shownSpeakerOn = false;

    lv_obj_t* root = nullptr;
    lv_obj_t* titleLabel = nullptr;
    lv_obj_t* artistLabel = nullptr;
    lv_obj_t* statusLabel = nullptr;
    lv_obj_t* progressBar = nullptr;
    lv_obj_t* timeLabel = nullptr;
    lv_obj_t* bufferBar = nullptr;
    lv_obj_t* shuffleButton = nullptr;
    lv_obj_t* repeatButton = nullptr;
    lv_obj_t* speakerButton = nullptr;
    lv_obj_t* volumeButton = nullptr;
    lv_obj_t* trackList = nullptr;

    /** Where the library browser currently is. Empty means the library root. */
    std::string libraryDir;
    /** What that directory holds, kept so a row's key handler can look itself up by index. */
    std::vector<music::LibraryEntry> libraryEntries;
    /** Which queue row to put the focus back on once a removal has rebuilt the page. */
    int queueFocusIndex = 0;
    lv_timer_t* rebuildTimer = nullptr;
    Page rebuildPage = Page::Player;
    /** The library/queue chooser while it is up, so back can dismiss it. */
    lv_obj_t* chooser = nullptr;
    lv_obj_t* bandSliders[music::EQUALIZER_BANDS] = {};
    lv_obj_t* bandValueLabels[music::EQUALIZER_BANDS] = {};
    lv_obj_t* presetValueLabel = nullptr;
    lv_obj_t* statLabels[(int) Stat::Count] = {};
    lv_obj_t* preampTrimLabel = nullptr;
    lv_obj_t* trackLabel = nullptr;
    lv_obj_t* durationLabel = nullptr;
    lv_obj_t* albumArt = nullptr;
    std::string albumArtPath;
    /** The track the art belongs to, since the tags are where it is looked for first. */
    std::string artTrackPath;
    /** Owned here, since the widget only holds a pointer to its pixels. */
    LvglThumbnail* thumbnail = nullptr;
    /** Decoding is deferred off the first paint, so the page appears before the art does. */
    lv_timer_t* artTimer = nullptr;
    int artWaits = 0;

    lv_timer_t* refreshTimer = nullptr;
    /** The media key that is down, so holding it acts only once. */
    uint32_t heldKey = 0;
};

std::string fileName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The service answers the same way when stopped as when it found no files, so the two are told
// apart here rather than both reading as an empty library.
std::string emptyReason() {
    const auto library = music::getLibraryPath();
    if (library.empty()) {
        return "Music service unavailable";
    }
    return std::format("Add files to {}", library);
}

/**
 * Builds a thumbnail from the track's own tags, falling back to a file beside it. Tags first: a
 * folder image is the exception, and a flat library has no folder to hold one.
 */
LvglThumbnail* loadTrackThumbnail(const std::string& trackPath, const std::string& artPath, int32_t size) {
    if (!trackPath.empty()) {
        auto cover = music::readTrackCover(trackPath);
        if (cover.data != nullptr) {
            auto* thumbnail = lvgl_thumbnail_create_from_memory(cover.data, cover.size, size, size);
            music::releaseTrackCover(&cover);
            if (thumbnail != nullptr) {
                return thumbnail;
            }
        }
    }
    return artPath.empty() ? nullptr : lvgl_thumbnail_create(artPath.c_str(), size, size);
}

// Frees what the widget is pointing at, after taking the widget off it: LVGL keeps the pointer
// rather than a copy, so the order matters.
void releaseArt(Context* ctx) {
    if (ctx->artTimer != nullptr) {
        lv_timer_delete(ctx->artTimer);
        ctx->artTimer = nullptr;
    }
    ctx->artWaits = 0;
    if (ctx->thumbnail != nullptr) {
        if (ctx->albumArt != nullptr) {
            lv_image_set_src(ctx->albumArt, LV_SYMBOL_AUDIO);
        }
        lvgl_thumbnail_destroy(ctx->thumbnail);
        ctx->thumbnail = nullptr;
    }
}

/**
 * Decodes the cover into the box, or leaves the placeholder. An unreadable file is not worth
 * reporting: many covers are encodings the built-in decoders do not handle.
 */
void loadArt(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    // A one-shot timer deletes itself once it has run.
    ctx->artTimer = nullptr;

    if (ctx->albumArt == nullptr) {
        return;
    }

    const auto telemetry = music::getTelemetry();
    const bool thin = telemetry.state == music::State::Buffering ||
        (telemetry.sourceSampleRate != 0 && telemetry.bufferPercent < ART_SAFE_BUFFER_PERCENT);
    if (thin && ctx->artWaits < ART_MAX_WAITS) {
        ctx->artWaits++;
        ctx->artTimer = lv_timer_create(loadArt, ART_LOAD_DELAY_MS, ctx);
        lv_timer_set_repeat_count(ctx->artTimer, 1);
        return;
    }

    auto* thumbnail = loadTrackThumbnail(ctx->artTrackPath, ctx->albumArtPath, ART_SIZE);
    if (thumbnail == nullptr) {
        return;
    }

    if (ctx->thumbnail != nullptr) {
        lvgl_thumbnail_destroy(ctx->thumbnail);
    }
    ctx->thumbnail = thumbnail;
    lv_image_set_src(ctx->albumArt, lvgl_thumbnail_image_source(thumbnail));
}

// The header shows whichever track the service has selected, including one it advanced to on
// its own while this app was closed.
void showTrack(Context* ctx) {
    auto path = music::getTrackPath();
    // Nothing is selected until something has played, so a filled but unstarted queue has no
    // current track. Shows the head of the queue, which is what play would pick.
    if (path.empty() && music::getQueueCount() > 0) {
        path = music::getQueuePathAt(0);
    }
    ctx->shownTrackIndex = music::getTrackIndex();
    releaseArt(ctx);
    ctx->artTrackPath = path;
    ctx->albumArtPath = music::findAlbumArt(path);

    if (ctx->albumArt != nullptr) {
        // The placeholder goes up now and the cover replaces it once decoded, so nothing waits
        // on the tags or the file for the page to appear.
        lv_image_set_src(ctx->albumArt, LV_SYMBOL_AUDIO);
        if (music::ALBUM_ART_ENABLED && !path.empty()) {
            ctx->artTimer = lv_timer_create(loadArt, ART_LOAD_DELAY_MS, ctx);
            lv_timer_set_repeat_count(ctx->artTimer, 1);
        }
    }
    if (ctx->titleLabel != nullptr) {
        // Nothing selected means nothing queued: the library is what fills the queue, so there
        // is no library to have failed to find.
        lv_label_set_text(ctx->titleLabel, path.empty() ? "Queue empty" : fileName(path).c_str());
    }
    if (ctx->artistLabel != nullptr) {
        // Empty until a track has started: the tag is only read then, and an "Unknown Artist"
        // line under a track nothing has played yet would be a claim about a file never opened.
        const auto artist = music::getTrackArtist();
        lv_label_set_text(ctx->artistLabel, artist.c_str());
        lv_obj_set_style_opa(ctx->artistLabel, artist.empty() ? LV_OPA_TRANSP : LV_OPA_COVER, LV_PART_MAIN);
    }
    if (ctx->trackLabel != nullptr) {
        const auto count = music::getQueueCount();
        lv_label_set_text(ctx->trackLabel, count == 0
            ? "Add tracks from the library"
            : std::format("{} of {}", std::max(ctx->shownTrackIndex, 0) + 1, count).c_str());
    }
}

void showPage(Context* ctx, Page page);
void releaseArt(Context* ctx);

int volumeStep() {
    return (int) std::lround(service::audio::getOutputVolume() / VOLUME_CODEC_STEP);
}

void adjustVolume(int steps) {
    if (!service::audio::isOutputAvailable()) {
        return;
    }
    const int target = std::clamp(volumeStep() + steps, 0, VOLUME_STEPS);
    service::audio::setOutputVolume((float) target * VOLUME_CODEC_STEP);
}

std::string volumeText() {
    if (!service::audio::isOutputAvailable()) {
        return std::string(LV_SYMBOL_MUTE);
    }
    const int shown = std::min(volumeStep(), VOLUME_STEPS) * (int) VOLUME_STEP_PERCENT;
    return std::format("{} {}", LV_SYMBOL_VOLUME_MAX, shown);
}

constexpr lv_color_t accent() { return lv_color_hex(0x4F8CFF); }
// Deliberately not accent(): the picker's focus ring and its playing row must not read as one.
constexpr lv_color_t playing() { return lv_color_hex(0x22C55E); }
constexpr lv_color_t surface() { return lv_color_hex(0x1B1D23); }
constexpr lv_color_t surfaceRaised() { return lv_color_hex(0x272A33); }

// Bubbling has to be set on every level: LVGL passes an event to a parent only if the child that
// received it carries the flag, so a container without it swallows the media keys on their way up.
void addBubbling(lv_obj_t* object) {
    lv_obj_add_flag(object, LV_OBJ_FLAG_EVENT_BUBBLE);
}

lv_obj_t* createRow(lv_obj_t* parent, lv_flex_align_t justify, int32_t padTop) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, justify, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_top(row, padTop, LV_PART_MAIN);
    lv_obj_set_style_pad_column(row, 6, LV_PART_MAIN);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    addBubbling(row);
    return row;
}

lv_obj_t* createCaption(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, LV_OPA_60, LV_PART_MAIN);
    return label;
}

std::string formatTime(uint32_t seconds) {
    return std::format("{}:{:02}", seconds / 60, seconds % 60);
}

// Listening time runs to hours, where a track's position never does.
std::string formatDuration(uint32_t seconds) {
    const uint32_t hours = seconds / 3600;
    return hours == 0
        ? std::format("{}:{:02}", seconds / 60, seconds % 60)
        : std::format("{}:{:02}:{:02}", hours, (seconds / 60) % 60, seconds % 60);
}

const char* statName(Stat stat) {
    switch (stat) {
        case Stat::SessionTime: return "Playing";
        case Stat::SessionTracks: return "Tracks";
        case Stat::Buffer: return "Buffer";
        case Stat::RingDry: return "Ring dry";
        case Stat::DmaDry: return "DMA dry";
        case Stat::Dropped: return "Dropped";
        case Stat::TotalTime: return "Playing";
        case Stat::TotalTracks: return "Tracks";
        case Stat::TopTrack: return "Most played";
        default: return "Top artist";
    }
}

std::string statValue(Stat stat, const music::Stats& stats, const music::Telemetry& telemetry) {
    switch (stat) {
        case Stat::SessionTime: return formatDuration(stats.sessionSeconds);
        case Stat::SessionTracks: return std::format("{}", stats.sessionTracks);
        case Stat::Buffer:
            // The low mark alongside the current one: against a ring this size the instantaneous
            // figure reads full almost always, and says nothing about the margin left.
            return telemetry.sourceSampleRate == 0
                ? "idle"
                : std::format("{}%  low {}%", telemetry.bufferPercent, telemetry.bufferLowPercent);
        case Stat::RingDry: return std::format("{}", stats.sessionUnderruns);
        case Stat::DmaDry: return std::format("{}", stats.sessionDmaUnderruns);
        case Stat::Dropped: return std::format("{} B", stats.sessionDroppedBytes);
        case Stat::TotalTime: return formatDuration(stats.totalSeconds);
        case Stat::TotalTracks: return std::format("{}", stats.totalTracks);
        case Stat::TopTrack:
            return stats.topTrack.empty() ? "-"
                : std::format("{}  x{}", stats.topTrack, stats.topTrackPlays);
        default:
            return stats.topArtist.empty() ? "-"
                : std::format("{}  x{}", stats.topArtist, stats.topArtistPlays);
    }
}

void setChipActive(lv_obj_t* chip, bool active);
void movePickerHighlight(Context* ctx, int current);
void scheduleRebuild(Context* ctx, Page page);
bool hideChooser(Context* ctx);

void speakerGlyph(Context* ctx, bool on) {
    if (ctx->speakerButton != nullptr) {
        lv_label_set_text(lv_obj_get_child(ctx->speakerButton, 0), on ? LV_SYMBOL_AUDIO : LV_SYMBOL_MUTE);
        setChipActive(ctx->speakerButton, on);
    }
    ctx->shownSpeakerOn = on;
}

void refresh(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));

    if (ctx->page == Page::Stats) {
        const auto stats = music::getStats();
        const auto telemetry = music::getTelemetry();
        for (int index = 0; index < (int) Stat::Count; index++) {
            if (ctx->statLabels[index] != nullptr) {
                lv_label_set_text(ctx->statLabels[index],
                    statValue((Stat) index, stats, telemetry).c_str());
            }
        }
        return;
    }

    if (ctx->page == Page::Queue) {
        const int current = music::getTrackIndex();
        if (current != ctx->shownTrackIndex) {
            movePickerHighlight(ctx, current);
        }
        return;
    }

    if (ctx->page != Page::Player || ctx->statusLabel == nullptr) {
        return;
    }

    // Both can change without this app doing anything: the service advances the playlist on its
    // own, and it switches the amplifier off once the playlist runs out.
    if (music::getTrackIndex() != ctx->shownTrackIndex) {
        showTrack(ctx);
    }
    const bool speaker_on = music::isSpeakerEnabled();
    if (speaker_on != ctx->shownSpeakerOn) {
        speakerGlyph(ctx, speaker_on);
    }

    const auto telemetry = music::getTelemetry();
    const char* glyph = telemetry.state == music::State::Playing ? LV_SYMBOL_PLAY
        : (telemetry.state == music::State::Paused ? LV_SYMBOL_PAUSE : LV_SYMBOL_STOP);

    const char* clip = telemetry.clipping ? "  CLIP" : "";

    if (telemetry.state == music::State::Buffering) {
        lv_label_set_text(ctx->statusLabel,
            std::format("{}  {}%", LV_SYMBOL_REFRESH, telemetry.bufferPercent).c_str());
    } else if (telemetry.sourceSampleRate == 0) {
        // No pipeline: the service releases it once the playlist runs out.
        lv_label_set_text(ctx->statusLabel, glyph);
    } else {
        lv_label_set_text(ctx->statusLabel, std::format("{}  {} kHz  {}{}",
            glyph, telemetry.sourceSampleRate / 1000,
            telemetry.underruns == 0 ? "" : std::format("{} drops", telemetry.underruns),
            clip).c_str());
    }
    lv_obj_set_style_text_color(ctx->statusLabel,
        telemetry.clipping ? lv_color_hex(0xEF4444) : lv_color_white(), LV_PART_MAIN);

    lv_bar_set_value(ctx->bufferBar, telemetry.bufferPercent, LV_ANIM_OFF);

    const uint32_t duration = telemetry.durationSeconds;
    lv_bar_set_value(ctx->progressBar,
        duration > 0 ? (int32_t) (telemetry.positionSeconds * 100 / duration) : 0, LV_ANIM_ON);
    lv_label_set_text(ctx->timeLabel, formatTime(telemetry.positionSeconds).c_str());
    lv_label_set_text(ctx->durationLabel, duration > 0 ? formatTime(duration).c_str() : "--:--");
}

void showVolume(Context* ctx) {
    if (ctx->volumeButton != nullptr) {
        lv_label_set_text(lv_obj_get_child(ctx->volumeButton, 0), volumeText().c_str());
    }
}

/**
 * Every media key as it happens, taken ahead of LVGL so no widget has to be focused to reach
 * playback. The keypad driver repeats a key for as long as it is down, so only the transition to
 * a new key acts. Runs on the LVGL task with the lock held.
 */
bool onMediaKey(uint32_t key, bool pressed, void* context) {
    auto* ctx = static_cast<Context*>(context);
    const bool media = key == KEY_MEDIA_PLAY_PAUSE || key == KEY_MEDIA_NEXT || key == KEY_MEDIA_PREV;

    if (!pressed) {
        ctx->heldKey = 0;
        return media;
    }
    if (!media || ctx->heldKey == key) {
        return media;
    }

    ctx->heldKey = key;
    if (key == KEY_MEDIA_PLAY_PAUSE) {
        music::playPause();
    } else if (key == KEY_MEDIA_NEXT) {
        music::next();
    } else {
        music::previous();
    }
    return true;
}

// Up and down are volume wherever the player page has the focus; the chips claim them from the
// keypad driver with LV_OBJ_FLAG_USER_2 so they never walk the focus ring here.
void onDirectionKey(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const uint32_t key = lv_event_get_key(event);
    if (key != LV_KEY_UP && key != LV_KEY_DOWN) {
        return;
    }
    adjustVolume(key == LV_KEY_UP ? 1 : -1);
    showVolume(ctx);
}

// Focus reads as a ring drawn inside the widget. The theme's default is an outline, which is
// painted outside the object and gets clipped by the parent on the edge rows.
void styleFocusRing(lv_obj_t* object) {
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(object, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(object, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(object, LV_OPA_90, LV_STATE_FOCUSED);
}

lv_obj_t* createChip(lv_obj_t* parent, const char* symbol, lv_event_cb_t callback, Context* ctx, bool active) {
    auto* chip = lv_button_create(parent);
    lv_obj_set_size(chip, 40, 34);
    lv_obj_set_style_radius(chip, 17, LV_PART_MAIN);
    lv_obj_set_style_bg_color(chip, active ? accent() : surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(chip, 0, LV_PART_MAIN);
    lv_obj_set_style_border_width(chip, 0, LV_PART_MAIN);

    styleFocusRing(chip);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_USER_2);

    auto* label = lv_label_create(chip);
    lv_label_set_text(label, symbol);
    lv_obj_set_style_text_opa(label, active ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
    lv_obj_center(label);

    lv_obj_add_event_cb(chip, callback, LV_EVENT_CLICKED, ctx);
    addBubbling(chip);
    return chip;
}

void setChipActive(lv_obj_t* chip, bool active) {
    lv_obj_set_style_bg_color(chip, active ? accent() : surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(lv_obj_get_child(chip, 0), active ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);
}

void onShuffleClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    const bool enabled = !music::isShuffleEnabled();
    music::setShuffleEnabled(enabled);
    setChipActive(ctx->shuffleButton, enabled);
}

void onRepeatClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    const auto current = music::getRepeat();
    const auto next = current == music::Repeat::Off ? music::Repeat::All
        : (current == music::Repeat::All ? music::Repeat::One : music::Repeat::Off);
    music::setRepeat(next);
    lv_label_set_text(lv_obj_get_child(ctx->repeatButton, 0),
        next == music::Repeat::One ? "1" : LV_SYMBOL_LOOP);
    setChipActive(ctx->repeatButton, next != music::Repeat::Off);
}

void onSpeakerClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    const bool enabled = !music::isSpeakerEnabled();
    if (!music::setSpeakerEnabled(enabled)) {
        // The power manager refuses the amplifier's supply on a flat battery, so the chip is
        // relabelled from what the service actually did rather than from what was asked for.
        LOG_W(TAG, "Speaker stayed %s", enabled ? "off" : "on");
    }
    const bool now_on = music::isSpeakerEnabled();
    speakerGlyph(ctx, now_on);
    ctx->shownSpeakerOn = now_on;
}

// Clicking cycles the volume so it is reachable without the media keys too.
void onVolumeClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    adjustVolume(1);
    showVolume(ctx);
}

void onDspClicked(lv_event_t* e) { showPage(static_cast<Context*>(lv_event_get_user_data(e)), Page::Dsp); }
void onLibraryOpen(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    ctx->libraryDir.clear();
    showPage(ctx, Page::Library);
}

/** Both chooser rows rebuild rather than switch page: the row is a child of what it replaces. */
void onChooseLibrary(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    ctx->libraryDir.clear();
    scheduleRebuild(ctx, Page::Library);
}

void onChooseQueue(lv_event_t* e) {
    scheduleRebuild(static_cast<Context*>(lv_event_get_user_data(e)), Page::Queue);
}

void onQueueOpen(lv_event_t* e) { showPage(static_cast<Context*>(lv_event_get_user_data(e)), Page::Queue); }

void showChooser(Context* ctx);

// With nothing queued there is only one list worth opening, so the question is not worth asking.
void onListsClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    if (music::getQueueCount() == 0) {
        ctx->libraryDir.clear();
        showPage(ctx, Page::Library);
        return;
    }
    showChooser(ctx);
}

/**
 * Steps the browser up one folder.
 *
 * @return false when it was already at the root, so the caller can treat that as leaving the
 *     library rather than climbing out of it into the rest of the filesystem
 */
bool libraryUp(Context* ctx) {
    const auto root = music::getLibraryPath();
    if (ctx->libraryDir.empty() || ctx->libraryDir == root) {
        return false;
    }
    const auto slash = ctx->libraryDir.find_last_of('/');
    auto parent = slash == std::string::npos ? std::string {} : ctx->libraryDir.substr(0, slash);
    // Never shorter than the root, whatever the path turned out to look like.
    ctx->libraryDir = parent.size() < root.size() ? root : parent;
    return true;
}

void onLibraryUpClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    libraryUp(ctx);
    showPage(ctx, Page::Library);
}

/**
 * Rebuilds @a page once this event has finished being delivered: a row cannot tear down the list
 * it is a child of from inside its own handler.
 */
void onRebuildTimer(lv_timer_t* timer) {
    auto* ctx = static_cast<Context*>(lv_timer_get_user_data(timer));
    ctx->rebuildTimer = nullptr;
    showPage(ctx, ctx->rebuildPage);
}

void scheduleRebuild(Context* ctx, Page page) {
    ctx->rebuildPage = page;
    if (ctx->rebuildTimer == nullptr) {
        ctx->rebuildTimer = lv_timer_create(onRebuildTimer, 1, ctx);
        lv_timer_set_repeat_count(ctx->rebuildTimer, 1);
    }
}
void onBackToPlayerClicked(lv_event_t* e) { showPage(static_cast<Context*>(lv_event_get_user_data(e)), Page::Player); }

std::string formatValue(int value, Unit unit) {
    switch (unit) {
        case Unit::Percent: return std::format("{}%", value);
        case Unit::Decibels: return std::format("{:+d} dB", value);
        case Unit::Balance:
            return value == 0 ? std::string("C")
                : std::format("{} {}", value < 0 ? "L" : "R", std::abs(value));
        default: return std::format("{:+d}", value);
    }
}

// Pushed on every step rather than debounced: the service only copies the curve and the output
// task reads the latest once per chunk, so the rate is already bounded.
void onBandChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* slider = lv_event_get_target_obj(event);
    for (int band = 0; band < music::EQUALIZER_BANDS; band++) {
        if (ctx->bandSliders[band] != slider) {
            continue;
        }
        const int gain = (int) lv_slider_get_value(slider);
        music::setBandGainDb(band, gain);
        settingsDirty = true;
        lv_label_set_text(ctx->bandValueLabels[band], formatValue(gain, Unit::Plain).c_str());
        return;
    }
}

void showBandValues(Context* ctx) {
    for (int band = 0; band < music::EQUALIZER_BANDS; band++) {
        if (ctx->bandSliders[band] == nullptr) {
            continue;
        }
        const int gain = music::getBandGainDb(band);
        lv_slider_set_value(ctx->bandSliders[band], gain, LV_ANIM_ON);
        lv_label_set_text(ctx->bandValueLabels[band], formatValue(gain, Unit::Plain).c_str());
    }
}

// A lifted curve is paid for by backing the output off, which is otherwise invisible: the preamp
// slider still reads what the user set while something quieter is being played.
void showPreampTrim(Context* ctx) {
    if (ctx->preampTrimLabel == nullptr) {
        return;
    }
    const int trim = music::getAutoPreampTrimDb();
    lv_label_set_text(ctx->preampTrimLabel, trim == 0 ? "" : std::format("-{} dB", trim).c_str());
}

// Cycled rather than picked from a dropdown: the badge's four direction keys reach a row and
// step it in place, where an open dropdown needs its own focus handling to escape again.
void onPresetClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const int next = (music::getPresetIndex() + 1) % std::max(music::getPresetCount(), 1);
    music::setPresetIndex(next);
    settingsDirty = true;
    if (ctx->presetValueLabel != nullptr) {
        lv_label_set_text(ctx->presetValueLabel, music::getPresetName(next));
    }
    showBandValues(ctx);
    showPreampTrim(ctx);
}

void onGainChanged(lv_event_t* event) {
    auto* label = static_cast<lv_obj_t*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    const int value = (int) lv_slider_get_value(lv_event_get_target_obj(event));
    music::setGainDb(value);
    settingsDirty = true;
    lv_label_set_text(label, formatValue(value, Unit::Decibels).c_str());
}

void onSpeedChanged(lv_event_t* event) {
    auto* label = static_cast<lv_obj_t*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    const int value = (int) lv_slider_get_value(lv_event_get_target_obj(event));
    music::setSpeedPercent(value);
    settingsDirty = true;
    lv_label_set_text(label, formatValue(value, Unit::Percent).c_str());
}

void onPitchChanged(lv_event_t* event) {
    auto* label = static_cast<lv_obj_t*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    const int value = (int) lv_slider_get_value(lv_event_get_target_obj(event));
    music::setPitchPercent(value);
    settingsDirty = true;
    lv_label_set_text(label, formatValue(value, Unit::Percent).c_str());
}

void onMonoChanged(lv_event_t* event) {
    music::setMonoDownmixEnabled(lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
    settingsDirty = true;
}

void onAutoPreampChanged(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    music::setAutoPreampEnabled(lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
    showPreampTrim(ctx);
    settingsDirty = true;
}

const char* dynamicsName(music::Dynamics value) {
    switch (value) {
        case music::Dynamics::Compressor: return "Compressor";
        case music::Dynamics::Multiband: return "Multiband";
        default: return "Off";
    }
}

void onDynamicsClicked(lv_event_t* event) {
    auto* label = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    const auto next = (music::Dynamics) (((uint8_t) music::getDynamics() + 1) % DYNAMICS_CHOICES);
    music::setDynamics(next);
    settingsDirty = true;
    lv_label_set_text(label, dynamicsName(next));
}

const char* fadeName(music::Fade value) {
    switch (value) {
        case music::Fade::Short: return "Short";
        case music::Fade::Long: return "Long";
        default: return "Off";
    }
}

void onFadeClicked(lv_event_t* event) {
    auto* label = static_cast<lv_obj_t*>(lv_event_get_user_data(event));
    const auto next = (music::Fade) (((uint8_t) music::getFade() + 1) % FADE_CHOICES);
    music::setFade(next);
    settingsDirty = true;
    lv_label_set_text(label, fadeName(next));
}

void onBalanceChanged(lv_event_t* event) {
    auto* label = static_cast<lv_obj_t*>(lv_obj_get_user_data(lv_event_get_target_obj(event)));
    const int value = (int) lv_slider_get_value(lv_event_get_target_obj(event));
    music::setBalancePercent(value);
    settingsDirty = true;
    lv_label_set_text(label, formatValue(value, Unit::Balance).c_str());
}

void onVuSeedingChanged(lv_event_t* event) {
    music::setVuSeedingEnabled(lv_obj_has_state(lv_event_get_target_obj(event), LV_STATE_CHECKED));
    settingsDirty = true;
}

void onStatsClicked(lv_event_t* e) { showPage(static_cast<Context*>(lv_event_get_user_data(e)), Page::Stats); }

void onResetStatsClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    music::resetStats();
    showPage(ctx, Page::Stats);
}

void onResetSoundClicked(lv_event_t* e) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(e));
    music::resetSettings();
    settingsDirty = false;
    showPage(ctx, Page::Dsp);
}

void onEqualizerClicked(lv_event_t* e) { showPage(static_cast<Context*>(lv_event_get_user_data(e)), Page::Equalizer); }
void onBackToDspClicked(lv_event_t* e) { showPage(static_cast<Context*>(lv_event_get_user_data(e)), Page::Dsp); }

// A full-width button closing off a page: going back, or the action the page ends with.
lv_obj_t* createWideButton(lv_obj_t* parent, Context* ctx, const char* text, lv_event_cb_t callback) {
    auto* back = lv_button_create(parent);
    lv_obj_set_width(back, LV_PCT(100));
    lv_obj_set_style_radius(back, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(back, surfaceRaised(), LV_PART_MAIN);
    styleFocusRing(back);
    auto* back_label = lv_label_create(back);
    lv_label_set_text(back_label, text);
    lv_obj_center(back_label);
    lv_obj_add_event_cb(back, callback, LV_EVENT_CLICKED, ctx);
    addBubbling(back);
    return back;
}

void createBackToPlayer(lv_obj_t* parent, Context* ctx) {
    createWideButton(parent, ctx, LV_SYMBOL_LEFT "  Player", onBackToPlayerClicked);
}

void styleProgressBar(lv_obj_t* bar, int32_t height, lv_color_t indicator, lv_opa_t trackOpa) {
    lv_obj_set_size(bar, LV_PCT(100), height);
    lv_obj_set_style_radius(bar, height / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(bar, height / 2, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(bar, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(bar, trackOpa, LV_PART_MAIN);
    lv_obj_set_style_bg_color(bar, indicator, LV_PART_INDICATOR);
    lv_bar_set_range(bar, 0, 100);
    addBubbling(bar);
}

void buildPlayerPage(Context* ctx, lv_obj_t* parent) {
    lv_obj_set_style_pad_all(parent, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 4, LV_PART_MAIN);
    // Everything fits, so the page fills the window and spreads its rows instead of leaving an
    // empty scrollable strip below the last one.
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(parent, LV_PCT(100));
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);

    // Art and text side by side: the display is far wider than it is tall.
    auto* header = createRow(parent, LV_FLEX_ALIGN_START, 0);

    ctx->albumArt = lv_image_create(header);
    lv_obj_set_size(ctx->albumArt, ART_SIZE, ART_SIZE);
    lv_obj_set_style_radius(ctx->albumArt, 10, LV_PART_MAIN);
    lv_obj_set_style_clip_corner(ctx->albumArt, true, LV_PART_MAIN);
    lv_obj_set_style_bg_color(ctx->albumArt, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->albumArt, LV_OPA_COVER, LV_PART_MAIN);
    lv_image_set_inner_align(ctx->albumArt, LV_IMAGE_ALIGN_CONTAIN);

    auto* details = lv_obj_create(header);
    lv_obj_remove_style_all(details);
    lv_obj_set_flex_grow(details, 1);
    lv_obj_set_height(details, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(details, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(details, 3, LV_PART_MAIN);
    lv_obj_remove_flag(details, LV_OBJ_FLAG_SCROLLABLE);
    addBubbling(details);

    ctx->titleLabel = lv_label_create(details);
    lv_label_set_long_mode(ctx->titleLabel, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_width(ctx->titleLabel, LV_PCT(100));
    lv_obj_set_style_text_font(ctx->titleLabel, lvgl_get_text_font(FONT_SIZE_LARGE), LV_PART_MAIN);

    ctx->artistLabel = createCaption(details, "");
    lv_label_set_long_mode(ctx->artistLabel, LV_LABEL_LONG_DOT);
    lv_obj_set_width(ctx->artistLabel, LV_PCT(100));
    lv_obj_set_style_text_opa(ctx->artistLabel, LV_OPA_80, LV_PART_MAIN);

    ctx->trackLabel = createCaption(details, "");
    ctx->statusLabel = createCaption(details, "Stopped");

    ctx->progressBar = lv_bar_create(parent);
    styleProgressBar(ctx->progressBar, 6, accent(), LV_OPA_COVER);

    auto* times = createRow(parent, LV_FLEX_ALIGN_SPACE_BETWEEN, 0);
    ctx->timeLabel = createCaption(times, "0:00");
    ctx->durationLabel = createCaption(times, "--:--");

    // Buffer fill is shown continuously: it is the thing that explains a stutter.
    ctx->bufferBar = lv_bar_create(parent);
    styleProgressBar(ctx->bufferBar, 3, lv_color_hex(0x5A6070), LV_OPA_30);

    // Transport has no on-screen buttons: play/pause, previous and next are physical keys. Only
    // what has no key of its own stays on screen.
    auto* chips = createRow(parent, LV_FLEX_ALIGN_CENTER, 4);
    lv_obj_set_style_pad_bottom(chips, 2, LV_PART_MAIN);
    const auto repeat = music::getRepeat();
    const bool speaker_on = music::isSpeakerEnabled();
    ctx->shuffleButton = createChip(chips, LV_SYMBOL_SHUFFLE, onShuffleClicked, ctx, music::isShuffleEnabled());
    ctx->repeatButton = createChip(chips, repeat == music::Repeat::One ? "1" : LV_SYMBOL_LOOP,
        onRepeatClicked, ctx, repeat != music::Repeat::Off);
    ctx->speakerButton = createChip(chips, speaker_on ? LV_SYMBOL_AUDIO : LV_SYMBOL_MUTE,
        onSpeakerClicked, ctx, speaker_on);
    ctx->shownSpeakerOn = speaker_on;
    ctx->volumeButton = createChip(chips, volumeText().c_str(), onVolumeClicked, ctx, false);
    lv_obj_set_width(ctx->volumeButton, 60);
    createChip(chips, LV_SYMBOL_LIST, onListsClicked, ctx, false);
    createChip(chips, LV_SYMBOL_SETTINGS, onDspClicked, ctx, false);

    showTrack(ctx);
}

void styleTrackRow(lv_obj_t* row, bool isPlaying) {
    lv_obj_set_style_bg_color(row, isPlaying ? playing() : surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, isPlaying ? LV_OPA_COVER : LV_OPA_40, LV_PART_MAIN);
    // lv_list_add_button() puts the icon in child 0 as an image whose source is the symbol text.
    lv_image_set_src(lv_obj_get_child(row, 0), isPlaying ? LV_SYMBOL_PLAY : LV_SYMBOL_AUDIO);
}

// Restyles in place rather than rebuilding the page, so the service advancing a track does not
// throw away where the user had scrolled to or which row they had focused.
void movePickerHighlight(Context* ctx, int current) {
    ctx->shownTrackIndex = current;
    if (ctx->trackList == nullptr) {
        return;
    }
    const uint32_t rows = lv_obj_get_child_count(ctx->trackList);
    for (uint32_t row = 0; row < rows; row++) {
        styleTrackRow(lv_obj_get_child(ctx->trackList, row), (int) row == current);
    }
}

/** Held by the label's style, so it outlives the rows it is applied to. */
const lv_anim_t* listScrollAnim() {
    static lv_anim_t anim;
    static bool ready = false;
    if (!ready) {
        lv_anim_init(&anim);
        lv_anim_set_delay(&anim, LIST_SCROLL_DELAY_MS);
        lv_anim_set_repeat_delay(&anim, LIST_SCROLL_DELAY_MS);
        // Copied over the label's own animation, which would otherwise stop after one pass.
        lv_anim_set_repeat_count(&anim, LV_ANIM_REPEAT_INFINITE);
        ready = true;
    }
    return &anim;
}

void delayRowScroll(lv_obj_t* row) {
    for (uint32_t child = 0; child < lv_obj_get_child_count(row); child++) {
        auto* object = lv_obj_get_child(row, child);
        if (lv_obj_check_type(object, &lv_label_class)) {
            lv_obj_set_style_anim(object, listScrollAnim(), LV_PART_MAIN);
        }
    }
}

lv_obj_t* createRowList(lv_obj_t* parent, Context* ctx) {
    auto* list = lv_list_create(parent);
    ctx->trackList = list;
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_bg_color(list, surface(), LV_PART_MAIN);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(list, 0, LV_PART_MAIN);
    addBubbling(list);
    return list;
}

void styleListPage(lv_obj_t* parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(parent, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 6, LV_PART_MAIN);
    lv_obj_set_height(parent, LV_PCT(100));
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
}

/**
 * Marks a library row as queued. Only regular Montserrat is built in, so emphasis is a brighter
 * row against dimmed ones, with the icon repeating it.
 */
void styleLibraryRow(lv_obj_t* row, const music::LibraryEntry& entry, bool queued) {
    const bool emphasised = queued && !entry.isFolder;
    lv_obj_set_style_bg_color(row, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(row, emphasised ? LV_OPA_COVER : LV_OPA_30, LV_PART_MAIN);
    lv_obj_set_style_text_color(row, emphasised ? lv_color_white() : lv_color_hex(0xB8BDC9), LV_PART_MAIN);
    lv_obj_set_style_text_opa(row, emphasised ? LV_OPA_COVER : LV_OPA_70, LV_PART_MAIN);

    const char* icon = entry.isFolder ? LV_SYMBOL_DIRECTORY : (queued ? LV_SYMBOL_OK : LV_SYMBOL_AUDIO);
    lv_image_set_src(lv_obj_get_child(row, 0), icon);
}

const music::LibraryEntry* entryFor(Context* ctx, lv_obj_t* row) {
    const auto index = (size_t) (uintptr_t) lv_obj_get_user_data(row);
    return index < ctx->libraryEntries.size() ? &ctx->libraryEntries[index] : nullptr;
}

// A is "play this now": the track goes on the end of the queue and playback jumps straight to it.
void onLibraryClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* row = lv_event_get_target_obj(event);
    const auto* entry = entryFor(ctx, row);
    if (entry == nullptr) {
        return;
    }

    if (entry->isFolder) {
        ctx->libraryDir = entry->path;
        scheduleRebuild(ctx, Page::Library);
        return;
    }

    music::enqueueAndPlay(entry->path);
    showPage(ctx, Page::Player);
}

// Right queues, left unqueues. Neither leaves the library, so a run of tracks can be picked out
// without stepping back and forth to another page.
void onLibraryKey(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* row = lv_event_get_target_obj(event);
    const uint32_t key = lv_event_get_key(event);
    const auto* entry = entryFor(ctx, row);
    if (entry == nullptr || (key != LV_KEY_RIGHT && key != LV_KEY_LEFT)) {
        return;
    }

    if (key == LV_KEY_RIGHT) {
        music::enqueue(entry->path);
    } else {
        music::removeFromQueueByPath(entry->path);
    }
    styleLibraryRow(row, *entry, music::isQueued(entry->path));
}

/**
 * Asks which of the two lists to open, over the player. Up and down pick, A opens, back dismisses:
 * B is not a key an app receives, only a close, so it cannot be spent on a choice.
 */
void showChooser(Context* ctx) {
    if (ctx->chooser != nullptr || ctx->root == nullptr) {
        return;
    }

    auto* overlay = lv_obj_create(ctx->root);
    ctx->chooser = overlay;
    lv_obj_remove_style_all(overlay);
    // Outside the page's own column, or it would be laid out as one more row of it.
    lv_obj_add_flag(overlay, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(overlay, LV_PCT(100), LV_PCT(100));
    lv_obj_align(overlay, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(overlay, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(overlay, LV_OPA_70, LV_PART_MAIN);
    lv_obj_set_flex_flow(overlay, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(overlay, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(overlay, 24, LV_PART_MAIN);
    lv_obj_set_style_pad_row(overlay, 8, LV_PART_MAIN);
    lv_obj_remove_flag(overlay, LV_OBJ_FLAG_SCROLLABLE);
    addBubbling(overlay);

    auto* library = createWideButton(overlay, ctx, LV_SYMBOL_DIRECTORY "  Music library", onChooseLibrary);
    createWideButton(overlay, ctx,
        std::format("{}  Queue ({})", LV_SYMBOL_LIST, music::getQueueCount()).c_str(), onChooseQueue);
    lv_group_focus_obj(library);
}

bool hideChooser(Context* ctx) {
    if (ctx->chooser == nullptr) {
        return false;
    }
    lv_obj_delete(ctx->chooser);
    ctx->chooser = nullptr;
    return true;
}

void buildLibraryPage(Context* ctx, lv_obj_t* parent) {
    styleListPage(parent);

    const auto root = music::getLibraryPath();
    if (ctx->libraryDir.empty()) {
        ctx->libraryDir = root;
    }
    ctx->libraryEntries = music::listLibrary(ctx->libraryDir);

    const auto queued = music::getQueueCount();
    const auto title = ctx->libraryDir == root ? std::string("Library") : fileName(ctx->libraryDir);
    createCaption(parent, queued == 0
        ? title.c_str() : std::format("{}   {} queued", title, queued).c_str());

    if (ctx->libraryEntries.empty()) {
        createCaption(parent, emptyReason().c_str());
    } else {
        auto* list = createRowList(parent, ctx);
        lv_obj_t* first_row = nullptr;
        for (size_t index = 0; index < ctx->libraryEntries.size(); index++) {
            const auto& entry = ctx->libraryEntries[index];
            auto* button = lv_list_add_button(list, LV_SYMBOL_AUDIO, entry.name.c_str());
            styleLibraryRow(button, entry, music::isQueued(entry.path));
            styleFocusRing(button);
            delayRowScroll(button);
            lv_obj_set_user_data(button, (void*) (uintptr_t) index);
            // Claims left and right for itself, which the keypad driver reads to stop them
            // walking the focus ring while this row is focused.
            lv_obj_add_flag(button, LV_OBJ_FLAG_USER_1);
            lv_obj_add_event_cb(button, onLibraryClicked, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(button, onLibraryKey, LV_EVENT_KEY, ctx);
            addBubbling(button);
            if (first_row == nullptr) {
                first_row = button;
            }
        }
        lv_group_focus_obj(first_row);
    }

    // No Queue or Player rows: back reaches both, and on a 320x240 screen two more full-width
    // buttons cost more listing than they are worth.
}

void onQueueClicked(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    const auto index = (int) (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(event));
    music::playQueueIndex(index);
    showPage(ctx, Page::Player);
}

void onQueueKey(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    if (lv_event_get_key(event) != LV_KEY_LEFT) {
        return;
    }
    const auto index = (int) (uintptr_t) lv_obj_get_user_data(lv_event_get_target_obj(event));
    music::removeFromQueue(index);
    // The rows below shift up, so the focus stays on the position rather than on the entry.
    ctx->queueFocusIndex = index;
    scheduleRebuild(ctx, Page::Queue);
}

void buildQueuePage(Context* ctx, lv_obj_t* parent) {
    styleListPage(parent);

    const int count = (int) music::getQueueCount();
    createCaption(parent, count == 0
        ? "Queue is empty" : std::format("Queue: {} tracks", count).c_str());

    if (count == 0) {
        createCaption(parent, "Add tracks from the library with right or A");
    } else {
        auto* list = createRowList(parent, ctx);
        const int current = music::getTrackIndex();
        lv_obj_t* focus_row = nullptr;
        for (int index = 0; index < count; index++) {
            const auto name = fileName(music::getQueuePathAt(index));
            auto* button = lv_list_add_button(list, LV_SYMBOL_AUDIO, name.c_str());
            styleTrackRow(button, index == current);
            styleFocusRing(button);
            delayRowScroll(button);
            lv_obj_set_user_data(button, (void*) (uintptr_t) index);
            lv_obj_add_flag(button, LV_OBJ_FLAG_USER_1);
            lv_obj_add_event_cb(button, onQueueClicked, LV_EVENT_CLICKED, ctx);
            lv_obj_add_event_cb(button, onQueueKey, LV_EVENT_KEY, ctx);
            addBubbling(button);
            if (index == std::clamp(ctx->queueFocusIndex, 0, count - 1)) {
                focus_row = button;
            }
        }
        ctx->shownTrackIndex = current;
        if (focus_row != nullptr) {
            lv_group_focus_obj(focus_row);
            lv_obj_scroll_to_view(focus_row, LV_ANIM_OFF);
        }
    }

    createWideButton(parent, ctx, LV_SYMBOL_DIRECTORY "  Library", onLibraryOpen);
    createBackToPlayer(parent, ctx);
}

// A section title, so the page reads as groups rather than one run of sliders.
lv_obj_t* createHeading(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
    lv_obj_set_style_text_color(label, accent(), LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 6, LV_PART_MAIN);
    return label;
}

// One row: name on the left, slider between, value on the right. Up/down walks the rows and
// left/right adjusts, which is what the shared keypad policy delivers.
lv_obj_t* createSliderRow(lv_obj_t* parent, const char* name, int32_t nameWidth, int min, int max,
                          int value, Unit unit, lv_event_cb_t callback, Context* ctx,
                          lv_obj_t** outValueLabel) {
    auto* row = createRow(parent, LV_FLEX_ALIGN_START, 0);

    auto* label = createCaption(row, name);
    lv_obj_set_width(label, nameWidth);

    auto* slider = lv_slider_create(row);
    lv_obj_set_flex_grow(slider, 1);
    lv_obj_set_height(slider, 6);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(slider, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, accent(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, accent(), LV_PART_KNOB);
    lv_obj_set_style_outline_width(slider, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(slider, 0, LV_PART_KNOB | LV_STATE_FOCUSED);
    // The knob grows when focused, which reads clearly without painting outside the row.
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB | LV_STATE_FOCUSED);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    addBubbling(slider);

    const auto text = formatValue(value, unit);
    auto* value_label = createCaption(row, text.c_str());
    lv_obj_set_width(value_label, unit == Unit::Plain ? 30 : 54);
    lv_obj_set_style_text_align(value_label, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    // Handlers reach their label through the slider, not Context, so a row needs no field of its
    // own. Both must exist before the callback is registered.
    lv_obj_set_user_data(slider, value_label);
    lv_obj_add_event_cb(slider, callback, LV_EVENT_VALUE_CHANGED, ctx);
    if (outValueLabel != nullptr) {
        *outValueLabel = value_label;
    }
    return slider;
}

/**
 * A full-width button showing a name and its value, clicked to step to the next.
 *
 * @param cyclesValue true for a row whose handler only rewrites its own value, and is handed that
 *     instead of the Context
 */
lv_obj_t* createActionRow(lv_obj_t* parent, const char* name, const char* value,
                          lv_event_cb_t callback, Context* ctx, bool cyclesValue = false) {
    auto* button = lv_button_create(parent);
    lv_obj_set_width(button, LV_PCT(100));
    lv_obj_set_height(button, LV_SIZE_CONTENT);
    lv_obj_set_style_radius(button, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(button, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_shadow_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(button, 8, LV_PART_MAIN);
    lv_obj_set_flex_flow(button, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(button, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    styleFocusRing(button);

    auto* name_label = lv_label_create(button);
    lv_label_set_text(name_label, name);
    lv_obj_set_flex_grow(name_label, 1);

    auto* value_label = createCaption(button, value);
    lv_obj_set_style_text_color(value_label, accent(), LV_PART_MAIN);
    lv_obj_set_style_text_opa(value_label, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_add_event_cb(button, callback, LV_EVENT_CLICKED,
        cyclesValue ? (void*) value_label : (void*) ctx);
    addBubbling(button);
    return value_label;
}

void createSwitchRow(lv_obj_t* parent, const char* name, bool checked, lv_event_cb_t callback,
                     Context* ctx, lv_obj_t** outValueLabel = nullptr) {
    auto* row = createRow(parent, LV_FLEX_ALIGN_START, 0);
    auto* label = createCaption(row, name);
    lv_obj_set_flex_grow(label, 1);
    if (outValueLabel != nullptr) {
        auto* value_label = createCaption(row, "");
        lv_obj_set_style_text_color(value_label, accent(), LV_PART_MAIN);
        lv_obj_set_style_text_opa(value_label, LV_OPA_COVER, LV_PART_MAIN);
        *outValueLabel = value_label;
    }
    auto* toggle = lv_switch_create(row);
    if (checked) {
        lv_obj_add_state(toggle, LV_STATE_CHECKED);
    }
    styleFocusRing(toggle);
    lv_obj_add_event_cb(toggle, callback, LV_EVENT_VALUE_CHANGED, ctx);
    addBubbling(toggle);
}

void styleSettingsPage(lv_obj_t* parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_add_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_all(parent, 10, LV_PART_MAIN);
    lv_obj_set_style_pad_row(parent, 6, LV_PART_MAIN);
}

// The short page: what most listeners will touch. The ten bands live one level down, so this
// one fits the screen without scrolling past the thing being looked for.
void buildDspPage(Context* ctx, lv_obj_t* parent) {
    styleSettingsPage(parent);

    createHeading(parent, "Tone");
    ctx->presetValueLabel = createActionRow(parent, "Preset",
        music::getPresetName(music::getPresetIndex()), onPresetClicked, ctx);
    createActionRow(parent, "Equalizer", LV_SYMBOL_RIGHT, onEqualizerClicked, ctx);
    createSliderRow(parent, "Preamp", 58, -20, 20, music::getGainDb(), Unit::Decibels,
        onGainChanged, ctx, nullptr);
    createSwitchRow(parent, "Auto preamp", music::isAutoPreampEnabled(), onAutoPreampChanged, ctx,
        &ctx->preampTrimLabel);
    showPreampTrim(ctx);

    createHeading(parent, "Dynamics");
    createActionRow(parent, "Compression", dynamicsName(music::getDynamics()), onDynamicsClicked, ctx, true);

    createHeading(parent, "Playback");
    createSliderRow(parent, "Speed", 58, 50, 200, music::getSpeedPercent(), Unit::Percent,
        onSpeedChanged, ctx, nullptr);
    createSliderRow(parent, "Pitch", 58, 50, 200, music::getPitchPercent(), Unit::Percent,
        onPitchChanged, ctx, nullptr);
    createSliderRow(parent, "Balance", 58, -100, 100, music::getBalancePercent(), Unit::Balance,
        onBalanceChanged, ctx, nullptr);
    createSwitchRow(parent, "Mono", music::isMonoDownmixEnabled(), onMonoChanged, ctx);

    createActionRow(parent, "Fade", fadeName(music::getFade()), onFadeClicked, ctx, true);

    createHeading(parent, "Lighting");
    // Only whether playback feeds the meter. What the meter looks like lives in the Lighting app.
    createSwitchRow(parent, "VU lights", music::isVuSeedingEnabled(), onVuSeedingChanged, ctx);

    createHeading(parent, "Listening");
    createActionRow(parent, "Stats", LV_SYMBOL_RIGHT, onStatsClicked, ctx);

    createWideButton(parent, ctx, "Reset to defaults", onResetSoundClicked);
    createBackToPlayer(parent, ctx);
}

// A read-only page, so the rows are plain captions and only the value labels are kept: the
// refresh rewrites those in place rather than rebuilding and stealing the focus.
void buildStatsPage(Context* ctx, lv_obj_t* parent) {
    styleSettingsPage(parent);

    const auto stats = music::getStats();
    const auto telemetry = music::getTelemetry();

    for (int index = 0; index < (int) Stat::Count; index++) {
        const auto stat = (Stat) index;
        if (stat == Stat::SessionTime) {
            createHeading(parent, "This session");
        } else if (stat == Stat::TotalTime) {
            createHeading(parent, "All time");
        }

        auto* row = createRow(parent, LV_FLEX_ALIGN_START, 0);
        auto* name = createCaption(row, statName(stat));
        lv_obj_set_width(name, 88);
        auto* value = createCaption(row, statValue(stat, stats, telemetry).c_str());
        lv_obj_set_flex_grow(value, 1);
        lv_label_set_long_mode(value, LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_opa(value, LV_OPA_COVER, LV_PART_MAIN);
        ctx->statLabels[index] = value;
    }

    createWideButton(parent, ctx, "Reset stats", onResetStatsClicked);
    createWideButton(parent, ctx, LV_SYMBOL_LEFT "  Sound", onBackToDspClicked);
}

void buildEqualizerPage(Context* ctx, lv_obj_t* parent) {
    styleSettingsPage(parent);

    ctx->presetValueLabel = createActionRow(parent, "Preset",
        music::getPresetName(music::getPresetIndex()), onPresetClicked, ctx);
    createHeading(parent, "Band gain, dB");

    for (int band = 0; band < music::EQUALIZER_BANDS; band++) {
        const auto frequency = music::EQUALIZER_BAND_FREQUENCIES[band];
        const auto name = frequency >= 1000
            ? std::format("{}k", frequency / 1000)
            : std::format("{}", frequency);
        ctx->bandSliders[band] = createSliderRow(parent, name.c_str(), 32, -13, 13,
            music::getBandGainDb(band), Unit::Plain, onBandChanged, ctx, &ctx->bandValueLabels[band]);
    }

    createWideButton(parent, ctx, LV_SYMBOL_LEFT "  Sound", onBackToDspClicked);
}

// The pages share one window; switching rebuilds the children rather than hiding them, so no
// hidden widget is ever left in the focus group.
void populate(lv_obj_t* root, void* userData) {
    auto* ctx = static_cast<Context*>(userData);
    ctx->root = root;

    // Ahead of lv_obj_clean(), which destroys the image widget: the thumbnail can only be taken
    // off it while it is still there to take off.
    releaseArt(ctx);

    lv_obj_clean(root);
    std::fill(std::begin(ctx->bandSliders), std::end(ctx->bandSliders), nullptr);
    std::fill(std::begin(ctx->bandValueLabels), std::end(ctx->bandValueLabels), nullptr);
    ctx->statusLabel = nullptr;
    ctx->albumArt = nullptr;
    ctx->trackList = nullptr;
    ctx->chooser = nullptr;
    ctx->titleLabel = nullptr;
    ctx->artistLabel = nullptr;
    ctx->trackLabel = nullptr;
    ctx->speakerButton = nullptr;
    ctx->volumeButton = nullptr;
    ctx->presetValueLabel = nullptr;
    ctx->preampTrimLabel = nullptr;
    std::fill(std::begin(ctx->statLabels), std::end(ctx->statLabels), nullptr);

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_set_style_bg_color(root, surface(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, LV_PART_MAIN);

    // Re-registering on every rebuild would stack duplicate handlers on the same root.
    lv_obj_remove_event_cb_with_user_data(root, onDirectionKey, ctx);
    lv_obj_add_event_cb(root, onDirectionKey, LV_EVENT_KEY, ctx);

    if (ctx->page == Page::Player) {
        buildPlayerPage(ctx, root);
    } else if (ctx->page == Page::Library) {
        buildLibraryPage(ctx, root);
    } else if (ctx->page == Page::Queue) {
        buildQueuePage(ctx, root);
    } else if (ctx->page == Page::Equalizer) {
        buildEqualizerPage(ctx, root);
    } else if (ctx->page == Page::Stats) {
        buildStatsPage(ctx, root);
    } else {
        buildDspPage(ctx, root);
    }
}

void showPage(Context* ctx, Page page) {
    ctx->page = page;
    populate(ctx->root, ctx);
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();

    Context ctx {};
    ctx.appInstanceId = appInstanceId;

    if (!music::isAvailable()) {
        LOG_E(TAG, "No audio output available");
    }

    music::claimMediaKeys(appInstanceId);
    music::setMediaKeyHandler(appInstanceId, onMediaKey, &ctx);

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(appInstanceId, populate, &ctx);
    ctx.refreshTimer = lv_timer_create(refresh, 250, &ctx);

    bool shouldClose = false;
    while (!shouldClose) {
        // A bounded wait so the settings get written while the app is still open: the badge is
        // unplugged rather than shut down, so saving only on close would rarely happen at all.
        task_event_group_wait_any(&event_group, nullptr, pdMS_TO_TICKS(SETTINGS_FLUSH_MS));

        if (settingsDirty) {
            settingsDirty = false;
            music::saveSettings();
        }

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                // B arrives as a close, not a key, so on a subpage it must mean "back to the
                // player" and only the player page closes the app. Held across the ctx.page read.
                lvgl_lock();
                if (hideChooser(&ctx)) {
                    lvgl_unlock();
                    continue;
                }
                const bool on_subpage = ctx.page != Page::Player;
                if (on_subpage) {
                    const bool nested = ctx.page == Page::Equalizer || ctx.page == Page::Stats;
                    // Inside a folder, back climbs out of it before it leaves the library.
                    if (ctx.page == Page::Library && libraryUp(&ctx)) {
                        showPage(&ctx, Page::Library);
                    } else {
                        showPage(&ctx, nested ? Page::Dsp : Page::Player);
                    }
                }
                lvgl_unlock();

                if (!on_subpage) {
                    shouldClose = true;
                    break;
                }
            }
        }
    }

    lv_timer_delete(ctx.refreshTimer);
    releaseArt(&ctx);
    if (ctx.rebuildTimer != nullptr) {
        lv_timer_delete(ctx.rebuildTimer);
        ctx.rebuildTimer = nullptr;
    }

    if (settingsDirty) {
        settingsDirty = false;
        music::saveSettings();
    }

    // Playback deliberately survives this app: the service keeps the pipeline alive so music
    // continues while the user is elsewhere.
    music::releaseMediaKeys(appInstanceId);
    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);
    return 0;
}

}

extern const ::AppManifest manifest = {
    .id = "MusicPlayer",
    .name = "Music Player",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
};

}
