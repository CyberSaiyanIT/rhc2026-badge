#pragma once
#ifdef ESP_PLATFORM

#include "Screensaver.h"

#include <lvgl/thumbnail.h>

#include <string>

namespace tt::service::displayidle {

/**
 * Shown instead of an animated screensaver while something is playing. Animating costs a redraw
 * every refresh cycle, which the audio pipeline pays for in dropouts; this draws once a second.
 */
class NowPlayingScreensaver final : public Screensaver {
public:
    NowPlayingScreensaver() = default;
    ~NowPlayingScreensaver() override = default;
    NowPlayingScreensaver(const NowPlayingScreensaver&) = delete;
    NowPlayingScreensaver& operator=(const NowPlayingScreensaver&) = delete;
    NowPlayingScreensaver(NowPlayingScreensaver&&) = delete;
    NowPlayingScreensaver& operator=(NowPlayingScreensaver&&) = delete;

    void start(lv_obj_t* overlay, lv_coord_t screenW, lv_coord_t screenH) override;
    void stop() override;
    void update(lv_coord_t screenW, lv_coord_t screenH) override;

private:
    /**
     * Shown where the art goes until a cover is decoded, and left there when none can be. A note
     * glyph rather than a logo: branding in the album's own spot reads as claiming the work.
     */
    static constexpr auto* PLACEHOLDER_ART = LV_SYMBOL_AUDIO;
    /** Between the bottom of the title, however many lines it took, and the artist. */
    static constexpr lv_coord_t ARTIST_GAP = 4;
    static constexpr uint32_t PROGRESS_INTERVAL_MS = 1000;
    /**
     * How long after a track change the cover is decoded. Deferred onto an LVGL timer: the change
     * is noticed on the shared timer daemon, where a JPEG would hold up every other timer.
     */
    static constexpr uint32_t ART_LOAD_DELAY_MS = 150;
    /**
     * How full the ring buffer must be before the cover is decoded, the SD card being read for
     * both. Waits for slack rather than for a fixed delay.
     */
    static constexpr uint8_t ART_SAFE_BUFFER_PERCENT = 40;
    /** After this many tries the art is decoded regardless. */
    static constexpr int ART_MAX_WAITS = 20;

    static void onProgressTimer(lv_timer_t* timer);
    static void onArtTimer(lv_timer_t* timer);

    void refreshProgress();
    void loadArt();
    void releaseArt();

    lv_obj_t* progressBar_ = nullptr;
    lv_obj_t* elapsedLabel_ = nullptr;
    lv_timer_t* progressTimer_ = nullptr;
    /** Redrawn only when the track changes, so a steady track costs one bar update a second. */
    int shownTrackIndex_ = -1;
    lv_obj_t* titleLabel_ = nullptr;
    lv_obj_t* artistLabel_ = nullptr;
    lv_obj_t* art_ = nullptr;
    std::string shownArtPath_;
    /** The track the art belongs to, since its tags are searched before any file. */
    std::string artTrackPath_;
    /** Owned here: the widget holds a pointer to the pixels rather than a copy of them. */
    LvglThumbnail* thumbnail_ = nullptr;
    lv_timer_t* artTimer_ = nullptr;
    int artWaits_ = 0;
};

} // namespace tt::service::displayidle

#endif // ESP_PLATFORM
