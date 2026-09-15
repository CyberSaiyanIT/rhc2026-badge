#ifdef ESP_PLATFORM

#include "NowPlayingScreensaver.h"

#include <Tactility/service/music/Music.h>

#include <cstdio>

namespace tt::service::displayidle {

namespace music = service::music;

namespace {

constexpr lv_coord_t ART_SIZE = 96;
constexpr lv_coord_t BAR_HEIGHT = 6;
constexpr lv_coord_t SIDE_PAD = 16;

/** The service exposes the performer but no title, so the file's own name stands in for one. */
std::string trackName(const std::string& path) {
    const auto slash = path.find_last_of('/');
    const auto name = slash == std::string::npos ? path : path.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    return dot == std::string::npos ? name : name.substr(0, dot);
}

void formatClock(char* out, size_t size, uint32_t seconds) {
    std::snprintf(out, size, "%lu:%02lu", (unsigned long) (seconds / 60), (unsigned long) (seconds % 60));
}

} // namespace

void NowPlayingScreensaver::start(lv_obj_t* overlay, lv_coord_t screenW, lv_coord_t screenH) {
    art_ = lv_image_create(overlay);
    lv_obj_set_size(art_, ART_SIZE, ART_SIZE);
    lv_image_set_inner_align(art_, LV_IMAGE_ALIGN_CONTAIN);
    lv_image_set_src(art_, PLACEHOLDER_ART);
    lv_obj_align(art_, LV_ALIGN_TOP_MID, 0, 20);

    titleLabel_ = lv_label_create(overlay);
    lv_label_set_long_mode(titleLabel_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(titleLabel_, screenW - 2 * SIDE_PAD);
    lv_obj_set_style_text_align(titleLabel_, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(titleLabel_, lv_color_white(), LV_STATE_DEFAULT);
    lv_obj_align(titleLabel_, LV_ALIGN_TOP_MID, 0, 20 + ART_SIZE + 10);
    const lv_font_t* title_font = lv_obj_get_style_text_font(titleLabel_, LV_PART_MAIN);
    lv_obj_set_style_max_height(titleLabel_, 2 * lv_font_get_line_height(title_font), LV_PART_MAIN);

    artistLabel_ = lv_label_create(overlay);
    lv_label_set_long_mode(artistLabel_, LV_LABEL_LONG_DOT);
    lv_obj_set_width(artistLabel_, screenW - 2 * SIDE_PAD);
    lv_obj_set_style_text_align(artistLabel_, LV_TEXT_ALIGN_CENTER, LV_STATE_DEFAULT);
    lv_obj_set_style_text_color(artistLabel_, lv_palette_main(LV_PALETTE_GREY), LV_STATE_DEFAULT);
    // Placed under whatever height the title ends up being rather than at a fixed offset, which a
    // title wrapping onto a second line overlaps.
    lv_obj_align_to(artistLabel_, titleLabel_, LV_ALIGN_OUT_BOTTOM_MID, 0, ARTIST_GAP);

    progressBar_ = lv_bar_create(overlay);
    lv_obj_set_size(progressBar_, screenW - 2 * SIDE_PAD, BAR_HEIGHT);
    lv_obj_align(progressBar_, LV_ALIGN_BOTTOM_MID, 0, -28);
    lv_bar_set_range(progressBar_, 0, 1000);
    lv_bar_set_value(progressBar_, 0, LV_ANIM_OFF);

    elapsedLabel_ = lv_label_create(overlay);
    lv_obj_set_style_text_color(elapsedLabel_, lv_palette_main(LV_PALETTE_GREY), LV_STATE_DEFAULT);
    lv_obj_align(elapsedLabel_, LV_ALIGN_BOTTOM_MID, 0, -8);
    lv_label_set_text(elapsedLabel_, "");

    refreshProgress();
    progressTimer_ = lv_timer_create(onProgressTimer, PROGRESS_INTERVAL_MS, this);
}

void NowPlayingScreensaver::onArtTimer(lv_timer_t* timer) {
    auto* self = static_cast<NowPlayingScreensaver*>(lv_timer_get_user_data(timer));
    // A one-shot timer deletes itself once it has run.
    self->artTimer_ = nullptr;
    self->loadArt();
}

void NowPlayingScreensaver::loadArt() {
    if (art_ == nullptr || artTrackPath_.empty()) {
        return;
    }

    const auto telemetry = music::getTelemetry();
    const bool thin = telemetry.state == music::State::Buffering ||
        (telemetry.sourceSampleRate != 0 && telemetry.bufferPercent < ART_SAFE_BUFFER_PERCENT);
    if (thin && artWaits_ < ART_MAX_WAITS) {
        artWaits_++;
        artTimer_ = lv_timer_create(onArtTimer, ART_LOAD_DELAY_MS, this);
        lv_timer_set_repeat_count(artTimer_, 1);
        return;
    }

    /*
     * The track's own tags first, a file beside it second, handed over as decoded pixels: a path
     * makes every redraw decode again and reads the header through a 4 kB stack buffer.
     */
    LvglThumbnail* thumbnail = nullptr;
    auto cover = music::readTrackCover(artTrackPath_);
    if (cover.data != nullptr) {
        thumbnail = lvgl_thumbnail_create_from_memory(cover.data, cover.size, ART_SIZE, ART_SIZE);
        music::releaseTrackCover(&cover);
    }
    if (thumbnail == nullptr && !shownArtPath_.empty()) {
        thumbnail = lvgl_thumbnail_create(shownArtPath_.c_str(), ART_SIZE, ART_SIZE);
    }
    if (thumbnail == nullptr) {
        return;
    }

    if (thumbnail_ != nullptr) {
        lvgl_thumbnail_destroy(thumbnail_);
    }
    thumbnail_ = thumbnail;
    lv_image_set_src(art_, lvgl_thumbnail_image_source(thumbnail));
}

void NowPlayingScreensaver::releaseArt() {
    if (artTimer_ != nullptr) {
        lv_timer_delete(artTimer_);
        artTimer_ = nullptr;
    }
    artWaits_ = 0;
    if (thumbnail_ != nullptr) {
        if (art_ != nullptr) {
            lv_image_set_src(art_, PLACEHOLDER_ART);
        }
        lvgl_thumbnail_destroy(thumbnail_);
        thumbnail_ = nullptr;
    }
}

void NowPlayingScreensaver::stop() {
    // Delete before the overlay takes its children down: the auto-off path stops the screensaver
    // without deleting the overlay, so the timer would otherwise keep touching freed widgets.
    if (progressTimer_ != nullptr) {
        lv_timer_delete(progressTimer_);
        progressTimer_ = nullptr;
    }
    // Before the widget pointers are dropped, while the image can still be taken off the pixels.
    releaseArt();

    progressBar_ = nullptr;
    elapsedLabel_ = nullptr;
    titleLabel_ = nullptr;
    artistLabel_ = nullptr;
    art_ = nullptr;
    shownTrackIndex_ = -1;
    shownArtPath_.clear();
    artTrackPath_.clear();
}

void NowPlayingScreensaver::update(lv_coord_t, lv_coord_t) {
    // Driven by its own one second timer, so the service tick has nothing to do here.
}

void NowPlayingScreensaver::onProgressTimer(lv_timer_t* timer) {
    static_cast<NowPlayingScreensaver*>(lv_timer_get_user_data(timer))->refreshProgress();
}

void NowPlayingScreensaver::refreshProgress() {
    if (progressBar_ == nullptr) {
        return;
    }

    const auto telemetry = music::getTelemetry();

    // Only the bar and the clock change while a track plays; the rest is rebuilt on track change.
    const int track_index = music::getTrackIndex();
    if (track_index != shownTrackIndex_) {
        shownTrackIndex_ = track_index;
        const auto path = music::getTrackPath();
        lv_label_set_text(titleLabel_, trackName(path).c_str());
        lv_label_set_text(artistLabel_, music::getTrackArtist().c_str());
        // The title's height changes with the track, so the artist is placed again each time
        // rather than once at start-up.
        lv_obj_update_layout(titleLabel_);
        lv_obj_align_to(artistLabel_, titleLabel_, LV_ALIGN_OUT_BOTTOM_MID, 0, ARTIST_GAP);

        const auto art_path = music::findAlbumArt(path);
        if (art_path != shownArtPath_ || path != artTrackPath_) {
            releaseArt();
            artTrackPath_ = path;
            shownArtPath_ = art_path;
            // The placeholder stands in until the decode finishes, and stays if it fails.
            lv_image_set_src(art_, PLACEHOLDER_ART);
            if (music::ALBUM_ART_ENABLED && !artTrackPath_.empty()) {
                artTimer_ = lv_timer_create(onArtTimer, ART_LOAD_DELAY_MS, this);
                lv_timer_set_repeat_count(artTimer_, 1);
            }
        }
    }

    const uint32_t duration = telemetry.durationSeconds;
    const uint32_t position = telemetry.positionSeconds;
    lv_bar_set_value(progressBar_, duration > 0 ? (int32_t)((position * 1000ULL) / duration) : 0, LV_ANIM_OFF);

    char elapsed[16];
    char total[16];
    formatClock(elapsed, sizeof(elapsed), position);
    formatClock(total, sizeof(total), duration);
    char text[40];
    std::snprintf(text, sizeof(text), "%s / %s", elapsed, total);
    lv_label_set_text(elapsedLabel_, text);
}

} // namespace tt::service::displayidle

#endif // ESP_PLATFORM
