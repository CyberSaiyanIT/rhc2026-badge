#pragma once
#ifdef ESP_PLATFORM

#include "Screensaver.h"

#include <array>
#include <cstdint>

namespace tt::service::displayidle {

class CassetteTapeScreensaver final : public Screensaver {
public:
    CassetteTapeScreensaver() = default;
    ~CassetteTapeScreensaver() override = default;
    CassetteTapeScreensaver(const CassetteTapeScreensaver&) = delete;
    CassetteTapeScreensaver& operator=(const CassetteTapeScreensaver&) = delete;
    CassetteTapeScreensaver(CassetteTapeScreensaver&&) = delete;
    CassetteTapeScreensaver& operator=(CassetteTapeScreensaver&&) = delete;

    void start(lv_obj_t* overlay, lv_coord_t screenW, lv_coord_t screenH) override;
    void stop() override;
    void update(lv_coord_t screenW, lv_coord_t screenH) override;

private:
    static constexpr size_t REEL_COUNT = 2;
    static constexpr size_t SPOKE_COUNT = 3;

    void positionSpokes();

    lv_obj_t* cassette_ = nullptr;
    std::array<std::array<lv_obj_t*, SPOKE_COUNT>, REEL_COUNT> spokes_{};
    std::array<std::array<std::array<lv_point_precise_t, 2>, SPOKE_COUNT>, REEL_COUNT> spokePoints_{};
    std::array<lv_obj_t*, REEL_COUNT> hubs_{};
    uint8_t phase_ = 0;
    uint32_t lastUpdate_ = 0;
    uint32_t phaseElapsed_ = 0;
};

} // namespace tt::service::displayidle

#endif // ESP_PLATFORM