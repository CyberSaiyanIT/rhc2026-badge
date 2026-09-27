#ifdef ESP_PLATFORM

#include "CassetteTapeScreensaver.h"

namespace tt::service::displayidle {

namespace {

constexpr std::array<int16_t, 12> COSINE = {1000, 866, 500, 0, -500, -866, -1000, -866, -500, 0, 500, 866};
constexpr std::array<int16_t, 12> SINE = {0, 500, 866, 1000, 866, 500, 0, -500, -866, -1000, -866, -500};
constexpr uint32_t PHASE_DURATION_MS = 60;
constexpr lv_coord_t CASSETTE_WIDTH = 320;
constexpr lv_coord_t CASSETTE_HEIGHT = 240;
constexpr lv_coord_t REEL_SIZE = 32;
constexpr lv_coord_t HUB_SIZE = 20;
constexpr lv_coord_t SPOKE_RADIUS = 12;
constexpr std::array<lv_point_t, 2> REEL_CENTERS = {{{95, 107}, {224, 107}}};

void setPlainBox(lv_obj_t* object) {
    lv_obj_remove_style_all(object);
    lv_obj_remove_flag(object, LV_OBJ_FLAG_SCROLLABLE);
}

} // namespace

void CassetteTapeScreensaver::start(lv_obj_t* overlay, lv_coord_t, lv_coord_t) {
    cassette_ = lv_image_create(overlay);
    lv_image_set_src(cassette_, CASSETTE_ASSET);
    lv_obj_set_size(cassette_, CASSETTE_WIDTH, CASSETTE_HEIGHT);
    lv_image_set_inner_align(cassette_, LV_IMAGE_ALIGN_STRETCH);
    lv_obj_center(cassette_);

    for (size_t reelIndex = 0; reelIndex < REEL_COUNT; reelIndex++) {
        auto* reel = lv_obj_create(cassette_);
        setPlainBox(reel);
        lv_obj_set_size(reel, REEL_SIZE, REEL_SIZE);
        lv_obj_set_pos(reel, REEL_CENTERS[reelIndex].x - REEL_SIZE / 2, REEL_CENTERS[reelIndex].y - REEL_SIZE / 2);
        lv_obj_set_style_bg_opa(reel, LV_OPA_TRANSP, 0);

        for (size_t spokeIndex = 0; spokeIndex < SPOKE_COUNT; spokeIndex++) {
            auto* spoke = lv_line_create(reel);
            lv_obj_set_size(spoke, REEL_SIZE, REEL_SIZE);
            lv_obj_set_pos(spoke, 0, 0);
            lv_obj_set_style_line_color(spoke, lv_color_hex(0xEEE8D8), 0);
            lv_obj_set_style_line_width(spoke, 4, 0);
            lv_obj_set_style_line_rounded(spoke, true, 0);
            spokes_[reelIndex][spokeIndex] = spoke;
        }

        auto* hub = lv_obj_create(reel);
        setPlainBox(hub);
        lv_obj_set_size(hub, HUB_SIZE, HUB_SIZE);
        lv_obj_center(hub);
        lv_obj_set_style_bg_color(hub, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(hub, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(hub, LV_RADIUS_CIRCLE, 0);
        hubs_[reelIndex] = hub;
    }

    phase_ = 0;
    phaseElapsed_ = 0;
    lastUpdate_ = lv_tick_get();
    positionSpokes();
}

void CassetteTapeScreensaver::stop() {
    cassette_ = nullptr;
    hubs_.fill(nullptr);
    for (auto& spokes : spokes_) {
        spokes.fill(nullptr);
    }
}

void CassetteTapeScreensaver::update(lv_coord_t, lv_coord_t) {
    if (cassette_ == nullptr) {
        return;
    }

    phaseElapsed_ += lv_tick_elaps(lastUpdate_);
    lastUpdate_ = lv_tick_get();
    if (phaseElapsed_ < PHASE_DURATION_MS) {
        return;
    }

    phase_ = (phase_ + phaseElapsed_ / PHASE_DURATION_MS) % COSINE.size();
    phaseElapsed_ %= PHASE_DURATION_MS;
    positionSpokes();
}

void CassetteTapeScreensaver::positionSpokes() {
    const lv_coord_t center = REEL_SIZE / 2;
    for (size_t reelIndex = 0; reelIndex < REEL_COUNT; reelIndex++) {
        for (size_t spokeIndex = 0; spokeIndex < SPOKE_COUNT; spokeIndex++) {
            const size_t angle = (phase_ + spokeIndex * 2) % COSINE.size();
            auto& points = spokePoints_[reelIndex][spokeIndex];
            const lv_coord_t offsetX = SPOKE_RADIUS * COSINE[angle] / 1000;
            const lv_coord_t offsetY = SPOKE_RADIUS * SINE[angle] / 1000;
            points[0] = {static_cast<lv_value_precise_t>(center - offsetX), static_cast<lv_value_precise_t>(center - offsetY)};
            points[1] = {static_cast<lv_value_precise_t>(center + offsetX), static_cast<lv_value_precise_t>(center + offsetY)};
            lv_line_set_points(spokes_[reelIndex][spokeIndex], points.data(), points.size());
        }
    }
}

} // namespace tt::service::displayidle

#endif // ESP_PLATFORM