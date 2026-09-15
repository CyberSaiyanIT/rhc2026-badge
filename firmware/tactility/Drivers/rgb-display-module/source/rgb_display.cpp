// SPDX-License-Identifier: Apache-2.0
#include <soc/soc_caps.h>
#if SOC_LCD_RGB_SUPPORTED

#include <drivers/rgb_display.h>
#include <rgb_display_module.h>
#include <drivers/software_pixel_mapper.h>

#include <tactility/delay.h>
#include <tactility/device.h>
#include <tactility/driver.h>
#include <tactility/drivers/display.h>
#include <tactility/drivers/esp32_spi.h>
#include <tactility/drivers/gpio.h>
#include <tactility/drivers/gpio_controller.h>
#include <tactility/drivers/spi_controller.h>
#include <tactility/error.h>
#include <tactility/log.h>

#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_lcd_io_spi.h>
#include <esp_lcd_panel_commands.h>
#include <esp_lcd_panel_io.h>
#include <esp_lcd_panel_rgb.h>
#include <esp_lcd_panel_ops.h>

#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

#include <cstdlib>

#define TAG "RgbDisplay"
#define GET_CONFIG(device) (static_cast<const RgbDisplayConfig*>((device)->config))

// Generic lvgl-module display glue (Modules/lvgl-module/source/lvgl_display.c) only ever asks
// for frame buffer index 0 and 1, so caching more than that would be dead weight.
constexpr size_t MAX_CACHED_FRAME_BUFFERS = 2;

struct RgbDisplayInternal {
    esp_lcd_panel_handle_t panel_handle;
    void* frame_buffers[MAX_CACHED_FRAME_BUFFERS];
    uint8_t frame_buffer_count;
    // Size of each buffer in frame_buffers, in bytes - used to range-check whether a given
    // draw_bitmap() color_data pointer is actually one of them (see rgb_display_draw_bitmap()).
    size_t frame_buffer_size_bytes;
    // Signaled by on_frame_buf_complete once per real DMA scan-out of a whole frame. Only
    // waited on in draw_bitmap() when color_data is one of frame_buffers - see the comment there for why.
    SemaphoreHandle_t frame_complete_semaphore;
    // Software pixel-format conversion active when custom-pixel-format != DEFAULT (see start()).
    // LVGL always renders RGB565 for this driver, so draw_bitmap() converts each tile through the
    // mapper before handing it to esp_lcd. The mapper owns its scratch buffer, held in
    // pixel_mapper_data. Null when no conversion is active.
    const struct SoftwarePixelMapper* pixel_mapper;
    SoftwarePixelMapperData pixel_mapper_data;
    // RGB666_6BIT_TRIPLE only: the romhack ILI9341 module is mounted landscape, but its RGB/DPI
    // interface is hardwired to the panel's native PORTRAIT silicon raster (240 source drivers x
    // 320 gate lines - MADCTL cannot rotate this, only GRAM addressing). draw_bitmap() rotates
    // each tile 90 degrees CW from logical (LVGL-facing) into native coordinates using this second
    // whole-frame scratch buffer, confirmed as the correct fixed rotation for this board on
    // hardware. Null unless is_rgb666_triple.
    bool is_rgb666_triple;
    void* rotation_scratch;
    // Which of frame_buffers[] the next draw_bitmap() writes into, the other being scanned out.
    // is_rgb666_triple with two frame buffers only; toggled after each call.
    uint8_t native_fb_write_index;
};

// esp_lcd_rgb_panel's draw_bitmap() has a zero-copy path when color_data is one of the panel's
// own frame buffers (as returned by esp_lcd_rgb_panel_get_frame_buffer()): it just repoints which
// buffer is scanned out and returns almost instantly - well before the RGB peripheral's DMA has
// actually finished scanning out the *previous* buffer, let alone started on this one. Callers in
// full/direct LVGL render mode render straight into these real frame buffers, so if draw_bitmap()
// returned that quickly, LVGL would be free to start overwriting the *other* buffer - which may
// still be mid-scanout - producing visible tearing/flashing. on_frame_buf_complete fires once per
// actual whole-frame DMA completion (continuously, at the panel's refresh rate, independent of
// draw_bitmap calls), so waiting for the next occurrence after each draw_bitmap() genuinely
// blocks until it's safe to start writing into the frame buffers again.
static bool IRAM_ATTR on_frame_buf_complete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void* user_ctx) {
    auto* internal = static_cast<RgbDisplayInternal*>(user_ctx);
    BaseType_t high_task_woken = pdFALSE;
    xSemaphoreGiveFromISR(internal->frame_complete_semaphore, &high_task_woken);
    return high_task_woken == pdTRUE;
}

static int pin_or_unused(const GpioPinSpec& pin) {
    return pin.gpio_controller == nullptr ? -1 : static_cast<int>(pin.pin);
}

// Pulses the panel's own driver-IC reset line, if configured. Transient: the descriptor is
// released immediately after, since nothing else needs to touch this pin afterward.
static error_t perform_hardware_reset(const RgbDisplayConfig* config) {
    if (config->pin_reset.gpio_controller == nullptr) {
        return ERROR_NONE;
    }

    auto* descriptor = gpio_descriptor_acquire(config->pin_reset.gpio_controller, config->pin_reset.pin, GPIO_FLAG_DIRECTION_OUTPUT | GPIO_FLAG_ACTIVE_LOW, GPIO_OWNER_GPIO);
    if (descriptor == nullptr) {
        LOG_E(TAG, "Failed to acquire reset GPIO descriptor");
        return ERROR_RESOURCE;
    }

    bool ok = gpio_descriptor_set_level(descriptor, true) == ERROR_NONE;
    if (ok) {
        delay_millis(100);
        ok = gpio_descriptor_set_level(descriptor, false) == ERROR_NONE;
        delay_millis(10);
    }

    gpio_descriptor_release(descriptor);
    return ok ? ERROR_NONE : ERROR_RESOURCE;
}

// An ILI9341 needs its own registers set over SPI before it will accept the RGB parallel bus at
// all. One-shot bring-up plus the 6-bit/3-transfers-per-pixel mode switch; see the writes below.
static error_t perform_ili9341_rgb666_command_init(Device* device, const RgbDisplayConfig* config) {
    struct GpioPinSpec cs_pin;
    if (esp32_spi_get_cs_pin(device, &cs_pin) != ERROR_NONE) {
        LOG_E(TAG, "Failed to resolve CS pin for command init (device must be an SPI_CONTROLLER child)");
        return ERROR_RESOURCE;
    }

    auto* parent = device_get_parent(device);
    const auto* spi_config = static_cast<const Esp32SpiConfig*>(parent->config);

    esp_lcd_panel_io_spi_config_t io_config = {
        .cs_gpio_num = pin_or_unused(cs_pin),
        .dc_gpio_num = pin_or_unused(config->pin_dc),
        .spi_mode = 0,
        .pclk_hz = 10'000'000, // command init only, well within spec regardless of the RGB pixel clock
        .trans_queue_depth = 4,
        .on_color_trans_done = nullptr,
        .user_ctx = nullptr,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .cs_ena_pretrans = 0,
        .cs_ena_posttrans = 0,
        .flags = {
            .dc_high_on_cmd = 0,
            .dc_low_on_data = 0,
            .dc_low_on_param = 0,
            .octal_mode = 0,
            .quad_mode = 0,
            .sio_mode = 1,
            .lsb_first = 0,
            .cs_high_active = 0,
        },
    };

    esp_lcd_panel_io_handle_t io_handle;
    esp_err_t ret = esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)spi_config->host, &io_config, &io_handle);
    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to create command-init panel IO: %s", esp_err_to_name(ret));
        return ERROR_RESOURCE;
    }

    // Full power-on sequence covering power control, VCOM and gamma, proven against an SPI/GRAM
    // test card before RGB666 was attempted. A real RESX/SWRESET puts the chip back to reset defaults for ALL of
    // this, not just the RGB-mode registers below, so it has to be redone here too, not just the
    // final mode-switch commands.
    bool ok = esp_lcd_panel_io_tx_param(io_handle, LCD_CMD_SWRESET, nullptr, 0) == ESP_OK;
    delay_millis(120);

    static const uint8_t power_ctrl_b[] = { 0x00, 0x83, 0x30 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xCF, power_ctrl_b, sizeof(power_ctrl_b)) == ESP_OK;
    static const uint8_t power_on_seq[] = { 0x64, 0x03, 0x12, 0x81 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xED, power_on_seq, sizeof(power_on_seq)) == ESP_OK;
    static const uint8_t driver_timing_a[] = { 0x85, 0x01, 0x79 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xE8, driver_timing_a, sizeof(driver_timing_a)) == ESP_OK;
    static const uint8_t power_ctrl_a[] = { 0x39, 0x2C, 0x00, 0x34, 0x02 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xCB, power_ctrl_a, sizeof(power_ctrl_a)) == ESP_OK;
    static const uint8_t pump_ratio[] = { 0x20 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xF7, pump_ratio, sizeof(pump_ratio)) == ESP_OK;
    static const uint8_t driver_timing_b[] = { 0x00, 0x00 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xEA, driver_timing_b, sizeof(driver_timing_b)) == ESP_OK;
    static const uint8_t power_ctrl_1[] = { 0x26 }; // GVDD
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xC0, power_ctrl_1, sizeof(power_ctrl_1)) == ESP_OK;
    static const uint8_t power_ctrl_2[] = { 0x11 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xC1, power_ctrl_2, sizeof(power_ctrl_2)) == ESP_OK;
    static const uint8_t vcom_ctrl_1[] = { 0x35, 0x3E };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xC5, vcom_ctrl_1, sizeof(vcom_ctrl_1)) == ESP_OK;
    static const uint8_t vcom_ctrl_2[] = { 0xBE };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xC7, vcom_ctrl_2, sizeof(vcom_ctrl_2)) == ESP_OK;
    static const uint8_t frame_rate[] = { 0x00, 0x1B }; // DIV=1, RTN=27
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xB1, frame_rate, sizeof(frame_rate)) == ESP_OK;
    static const uint8_t gamma_set[] = { 0x01 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, LCD_CMD_GAMSET, gamma_set, sizeof(gamma_set)) == ESP_OK;
    static const uint8_t pos_gamma[] = { 0x0F, 0x1D, 0x1A, 0x0A, 0x0D, 0x07, 0x49, 0x66,
                                          0x3B, 0x07, 0x11, 0x01, 0x09, 0x05, 0x04 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xE0, pos_gamma, sizeof(pos_gamma)) == ESP_OK;
    static const uint8_t neg_gamma[] = { 0x00, 0x18, 0x1D, 0x02, 0x0F, 0x04, 0x36, 0x13,
                                          0x4C, 0x07, 0x13, 0x0F, 0x2E, 0x2F, 0x05 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xE1, neg_gamma, sizeof(neg_gamma)) == ESP_OK;

    // Straight to RGB666 DPI mode (no intermediate GRAM/MADCTL rotation phase -- MADCTL cannot
    // rotate the DPI stream anyway, see this function's own top comment).
    static const uint8_t madctl_param[] = { 0x08 }; // BGR order, no rotation
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, LCD_CMD_MADCTL, madctl_param, sizeof(madctl_param)) == ESP_OK;

    static const uint8_t colmod_param[] = { 0x66 }; // DPI[2:0]=110 -> RGB666
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, LCD_CMD_COLMOD, colmod_param, sizeof(colmod_param)) == ESP_OK;

    static const uint8_t ifmode_param[] = { 0x40 }; // RCM[1:0]=10 (DE mode), polarities=0
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xB0, ifmode_param, sizeof(ifmode_param)) == ESP_OK;

        // RIM must be set before the RGB peripheral streams, and left alone afterwards.
        static const uint8_t ifctl_param[] = { 0x01, 0x00, 0x07 }; // DM=01 (RGB), RM=1, RIM=1 (6-bit x3)
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xF6, ifctl_param, sizeof(ifctl_param)) == ESP_OK;

    // B6h 4th param PCDIV=0: ILI9341_DS_V1.11.pdf page 46 / 8.3.7 documents PCDIV as deriving an
    // internal clock PCLKD = DOTCLK / (2*(PCDIV+1)) that should track the panel's own 615kHz
    // oscillator, which would suggest scaling PCDIV with pixel_clock_hz. Live-tested on real
    // hardware, sweeping PCDIV both by that formula and by hand, and disproven: PCDIV=0 is the only value that keeps a
    // properly synced image at every PCLK tried up to 30MHz, formula-derived non-zero values
    // included. Left hardcoded rather than derived. Params 1-3 (PTG/PT, REV/GS/SS/SM/ISC, NL) kept
    // at their HW-reset defaults (0x0A, 0x82, 0x27).
    static const uint8_t dfc_param[] = { 0x0A, 0x82, 0x27, 0x00 };
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, 0xB6, dfc_param, sizeof(dfc_param)) == ESP_OK;

    ok = ok && esp_lcd_panel_io_tx_param(io_handle, LCD_CMD_SLPOUT, nullptr, 0) == ESP_OK;
    delay_millis(120);
    ok = ok && esp_lcd_panel_io_tx_param(io_handle, LCD_CMD_DISPON, nullptr, 0) == ESP_OK;
    delay_millis(20);

    esp_lcd_panel_io_del(io_handle);

    if (!ok) {
        LOG_E(TAG, "ILI9341 RGB666 command init failed");
        return ERROR_RESOURCE;
    }

    LOG_I(TAG, "ILI9341 RGB666 command init done (COLMOD=0x66, RIM=1, PCDIV=0, 6-bit x3 RGB interface)");
    return ERROR_NONE;
}

// region Driver lifecycle

static error_t cache_frame_buffers(RgbDisplayInternal* internal, const RgbDisplayConfig* config) {
    internal->frame_buffer_count = 0;
    internal->frame_buffer_size_bytes = (size_t)config->horizontal_resolution * config->vertical_resolution *
        ((config->bits_per_pixel + 7) / 8);
    if (config->num_fbs == 0) {
        return ERROR_NONE;
    }

    // esp_lcd_rgb_panel_get_frame_buffer() is variadic: the number of out-pointer arguments
    // passed must match fb_num exactly, so this can't be a loop.
    size_t fb_num = config->num_fbs < MAX_CACHED_FRAME_BUFFERS ? config->num_fbs : MAX_CACHED_FRAME_BUFFERS;
    esp_err_t ret;
    switch (fb_num) {
        case 1:
            ret = esp_lcd_rgb_panel_get_frame_buffer(internal->panel_handle, 1, &internal->frame_buffers[0]);
            break;
        case 2:
            ret = esp_lcd_rgb_panel_get_frame_buffer(internal->panel_handle, 2, &internal->frame_buffers[0], &internal->frame_buffers[1]);
            break;
        default:
            return ERROR_NONE;
    }

    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to get frame buffer(s): %s", esp_err_to_name(ret));
        return ERROR_RESOURCE;
    }

    internal->frame_buffer_count = (uint8_t)fb_num;
    return ERROR_NONE;
}

static error_t start(Device* device) {
    const auto* config = GET_CONFIG(device);

    auto* internal = static_cast<RgbDisplayInternal*>(malloc(sizeof(RgbDisplayInternal)));
    if (internal == nullptr) {
        return ERROR_OUT_OF_MEMORY;
    }
    internal->pixel_mapper = nullptr;
    internal->pixel_mapper_data = nullptr;
    internal->is_rgb666_triple = config->pixel_format == RGB_DISPLAY_PIXEL_FORMAT_RGB666_6BIT_TRIPLE;
    internal->rotation_scratch = nullptr;

    error_t reset_error = perform_hardware_reset(config);
    if (reset_error != ERROR_NONE) {
        LOG_E(TAG, "Failed to reset panel");
        free(internal);
        return reset_error;
    }

    if (config->pixel_format == RGB_DISPLAY_PIXEL_FORMAT_RGB666_6BIT_TRIPLE) {
        error_t command_init_error = perform_ili9341_rgb666_command_init(device, config);
        if (command_init_error != ERROR_NONE) {
            free(internal);
            return command_init_error;
        }
    }

    // 6-bit x3 sends three DOTCLK bytes per pixel: DOTCLK porches scale x3 (7.2.2 note 4), while
    // vertical timings are in lines. bits_per_pixel=24 is esp_lcd's own Serial RGB mechanism, so
    // h_res stays the real pixel count and data_width the physical bus width.
    bool is_rgb666_triple = internal->is_rgb666_triple;
    uint32_t porch_multiplier = is_rgb666_triple ? 3 : 1;
    uint8_t bits_per_pixel = is_rgb666_triple ? 24 : config->bits_per_pixel;

    // The peripheral's raster must match the panel's native geometry, the transpose of what LVGL
    // sees. Only these timings use the swapped values; the rest of the driver stays logical.
    uint16_t native_h_res = is_rgb666_triple ? config->vertical_resolution : config->horizontal_resolution;
    uint16_t native_v_res = is_rgb666_triple ? config->horizontal_resolution : config->vertical_resolution;

    esp_lcd_rgb_panel_config_t panel_config = {
        .clk_src = LCD_CLK_SRC_DEFAULT,
        .timings = {
            .pclk_hz = config->pixel_clock_hz,
            .h_res = native_h_res,
            .v_res = native_v_res,
            .hsync_pulse_width = config->hsync_pulse_width * porch_multiplier,
            .hsync_back_porch = config->hsync_back_porch * porch_multiplier,
            .hsync_front_porch = config->hsync_front_porch * porch_multiplier,
            .vsync_pulse_width = config->vsync_pulse_width,
            .vsync_back_porch = config->vsync_back_porch,
            .vsync_front_porch = config->vsync_front_porch,
            .flags = {
                .hsync_idle_low = config->hsync_idle_low,
                .vsync_idle_low = config->vsync_idle_low,
                .de_idle_high = config->de_idle_high,
                .pclk_active_neg = config->pclk_active_neg,
                .pclk_idle_high = config->pclk_idle_high,
            }
        },
        .data_width = config->data_width,
        .bits_per_pixel = bits_per_pixel,
        .num_fbs = config->num_fbs,
        .bounce_buffer_size_px = config->bounce_buffer_size_px,
        .sram_trans_align = config->sram_trans_align,
        .psram_trans_align = config->psram_trans_align,
        .hsync_gpio_num = pin_or_unused(config->pin_hsync),
        .vsync_gpio_num = pin_or_unused(config->pin_vsync),
        .de_gpio_num = pin_or_unused(config->pin_de),
        .pclk_gpio_num = pin_or_unused(config->pin_pclk),
        .disp_gpio_num = pin_or_unused(config->pin_disp),
        .data_gpio_nums = {
            pin_or_unused(config->pin_data0),
            pin_or_unused(config->pin_data1),
            pin_or_unused(config->pin_data2),
            pin_or_unused(config->pin_data3),
            pin_or_unused(config->pin_data4),
            pin_or_unused(config->pin_data5),
            pin_or_unused(config->pin_data6),
            pin_or_unused(config->pin_data7),
            pin_or_unused(config->pin_data8),
            pin_or_unused(config->pin_data9),
            pin_or_unused(config->pin_data10),
            pin_or_unused(config->pin_data11),
            pin_or_unused(config->pin_data12),
            pin_or_unused(config->pin_data13),
            pin_or_unused(config->pin_data14),
            pin_or_unused(config->pin_data15),
        },
        .flags = {
            .disp_active_low = config->disp_active_low,
            .refresh_on_demand = config->refresh_on_demand,
            .fb_in_psram = config->fb_in_psram,
            .double_fb = config->double_fb,
            .no_fb = config->no_fb,
            .bb_invalidate_cache = config->bb_invalidate_cache,
        }
    };

    // This Config struct only exposes 16 named data pins, so on chips whose RGB peripheral has
    // more data lines than that (e.g. ESP32-P4's 24), the tail of the array must be explicitly
    // marked unused rather than left as the aggregate-init default of 0 (which would look like
    // "GPIO0 is wired to this line").
    for (size_t i = 16; i < sizeof(panel_config.data_gpio_nums) / sizeof(panel_config.data_gpio_nums[0]); i++) {
        panel_config.data_gpio_nums[i] = -1;
    }

    LOG_I(TAG, "esp_lcd_rgb_panel_config_t before esp_lcd_new_rgb_panel():");
    LOG_I(TAG, "  timings.pclk_hz=%lu h_res=%u v_res=%u", (unsigned long)panel_config.timings.pclk_hz,
        (unsigned)panel_config.timings.h_res, (unsigned)panel_config.timings.v_res);
    LOG_I(TAG, "  timings.hsync pulse/back/front=%lu/%lu/%lu", (unsigned long)panel_config.timings.hsync_pulse_width,
        (unsigned long)panel_config.timings.hsync_back_porch, (unsigned long)panel_config.timings.hsync_front_porch);
    LOG_I(TAG, "  timings.vsync pulse/back/front=%lu/%lu/%lu", (unsigned long)panel_config.timings.vsync_pulse_width,
        (unsigned long)panel_config.timings.vsync_back_porch, (unsigned long)panel_config.timings.vsync_front_porch);
    LOG_I(TAG, "  timings.flags: hsync_idle_low=%d vsync_idle_low=%d de_idle_high=%d pclk_active_neg=%d pclk_idle_high=%d",
        panel_config.timings.flags.hsync_idle_low, panel_config.timings.flags.vsync_idle_low,
        panel_config.timings.flags.de_idle_high, panel_config.timings.flags.pclk_active_neg,
        panel_config.timings.flags.pclk_idle_high);
    LOG_I(TAG, "  data_width=%u bits_per_pixel=%u num_fbs=%u", (unsigned)panel_config.data_width,
        (unsigned)panel_config.bits_per_pixel, (unsigned)panel_config.num_fbs);
    LOG_I(TAG, "  bounce_buffer_size_px=%u sram_trans_align=%u psram_trans_align=%u",
        (unsigned)panel_config.bounce_buffer_size_px, (unsigned)panel_config.sram_trans_align,
        (unsigned)panel_config.psram_trans_align);
    LOG_I(TAG, "  gpio hsync=%d vsync=%d de=%d pclk=%d disp=%d", panel_config.hsync_gpio_num,
        panel_config.vsync_gpio_num, panel_config.de_gpio_num, panel_config.pclk_gpio_num, panel_config.disp_gpio_num);
    LOG_I(TAG, "  gpio data0-7=%d,%d,%d,%d,%d,%d,%d,%d", panel_config.data_gpio_nums[0], panel_config.data_gpio_nums[1],
        panel_config.data_gpio_nums[2], panel_config.data_gpio_nums[3], panel_config.data_gpio_nums[4],
        panel_config.data_gpio_nums[5], panel_config.data_gpio_nums[6], panel_config.data_gpio_nums[7]);
    LOG_I(TAG, "  flags: disp_active_low=%d refresh_on_demand=%d fb_in_psram=%d double_fb=%d no_fb=%d bb_invalidate_cache=%d",
        panel_config.flags.disp_active_low, panel_config.flags.refresh_on_demand, panel_config.flags.fb_in_psram,
        panel_config.flags.double_fb, panel_config.flags.no_fb, panel_config.flags.bb_invalidate_cache);

    esp_err_t ret = esp_lcd_new_rgb_panel(&panel_config, &internal->panel_handle);
    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to create panel: %s", esp_err_to_name(ret));
        free(internal);
        return ERROR_RESOURCE;
    }

    bool ok =
        esp_lcd_panel_reset(internal->panel_handle) == ESP_OK &&
        esp_lcd_panel_init(internal->panel_handle) == ESP_OK &&
        esp_lcd_panel_swap_xy(internal->panel_handle, config->swap_xy) == ESP_OK &&
        esp_lcd_panel_mirror(internal->panel_handle, config->mirror_x, config->mirror_y) == ESP_OK &&
        esp_lcd_panel_invert_color(internal->panel_handle, config->invert_color) == ESP_OK;

    if (!ok) {
        LOG_E(TAG, "Failed to bring up panel");
        esp_lcd_panel_del(internal->panel_handle);
        free(internal);
        return ERROR_RESOURCE;
    }

    if (is_rgb666_triple) {
        // Scratch for draw_bitmap()'s per-tile rotation from logical landscape to native
        // portrait. width*height*3, a full-screen draw being the largest tile it holds.
        size_t rotation_scratch_size = (size_t)config->horizontal_resolution * config->vertical_resolution * 3;
        internal->rotation_scratch = heap_caps_malloc(rotation_scratch_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (internal->rotation_scratch == nullptr) {
            LOG_E(TAG, "Failed to allocate rotation scratch buffer");
            esp_lcd_panel_del(internal->panel_handle);
            free(internal);
            return ERROR_OUT_OF_MEMORY;
        }
    }

    error_t error = cache_frame_buffers(internal, config);
    if (error != ERROR_NONE) {
        esp_lcd_panel_del(internal->panel_handle);
        free(internal);
        return error;
    }
    // Buffer 0 is what esp_lcd_rgb_panel scans out from the moment the panel starts, so the first
    // ping-pong write must target buffer 1.
    internal->native_fb_write_index = 1;

    if (config->pixel_format != RGB_DISPLAY_PIXEL_FORMAT_DEFAULT) {
        switch (config->pixel_format) {
            case RGB_DISPLAY_PIXEL_FORMAT_RGB332:
                internal->pixel_mapper = &software_pixel_mapper_rgb332;
                break;
            case RGB_DISPLAY_PIXEL_FORMAT_RGB666_6BIT_TRIPLE:
                internal->pixel_mapper = &software_pixel_mapper_rgb666_6bit_triple;
                break;
            default:
                LOG_E(TAG, "Unsupported pixel format %d", (int)config->pixel_format);
                free(internal);
                return ERROR_INVALID_ARGUMENT;
        }

        // The mapper sizes and allocates its own whole-frame destination buffer, which lands in
        // PSRAM on boards that have it (see software_pixel_mapper.cpp).
        internal->pixel_mapper_data = internal->pixel_mapper->create(config->horizontal_resolution, config->vertical_resolution);
        if (internal->pixel_mapper_data == nullptr) {
            LOG_E(TAG, "Failed to create pixel mapper");
            esp_lcd_panel_del(internal->panel_handle);
            free(internal);
            return ERROR_OUT_OF_MEMORY;
        }
    }

    internal->frame_complete_semaphore = xSemaphoreCreateBinary();
    if (internal->frame_complete_semaphore == nullptr) {
        heap_caps_free(internal->rotation_scratch);
        esp_lcd_panel_del(internal->panel_handle);
        free(internal);
        return ERROR_OUT_OF_MEMORY;
    }

    esp_lcd_rgb_panel_event_callbacks_t callbacks = {};
    callbacks.on_frame_buf_complete = on_frame_buf_complete;
    if (esp_lcd_rgb_panel_register_event_callbacks(internal->panel_handle, &callbacks, internal) != ESP_OK) {
        LOG_E(TAG, "Failed to register panel event callbacks");
        vSemaphoreDelete(internal->frame_complete_semaphore);
        heap_caps_free(internal->rotation_scratch);
        esp_lcd_panel_del(internal->panel_handle);
        free(internal);
        return ERROR_RESOURCE;
    }

    device_set_driver_data(device, internal);
    return ERROR_NONE;
}

static error_t stop(Device* device) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));

    if (internal->panel_handle != nullptr) {
        if (esp_lcd_panel_del(internal->panel_handle) != ESP_OK) {
            LOG_E(TAG, "Failed to delete panel");
            return ERROR_RESOURCE;
        }
        internal->panel_handle = nullptr;
    }

    vSemaphoreDelete(internal->frame_complete_semaphore);
    if (internal->pixel_mapper != nullptr) {
        internal->pixel_mapper->destroy(internal->pixel_mapper_data);
    }
    heap_caps_free(internal->rotation_scratch);
    free(internal);
    device_set_driver_data(device, nullptr);
    return ERROR_NONE;
}

// endregion

// region DisplayApi

static error_t rgb_display_reset(Device* device) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    return esp_lcd_panel_reset(internal->panel_handle) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

static error_t rgb_display_init(Device* device) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    return esp_lcd_panel_init(internal->panel_handle) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

// Only block for scan-out completion when color_data is actually one of the panel's own frame
// buffers (see on_frame_buf_complete's comment above for why that matters) - i.e. this specific
// call is a zero-copy flip, not a plain CPU copy into the panel's buffer from a caller-owned one
// (e.g. LVGL bound in owned-buffer mode), which has no reuse race to guard against and shouldn't
// pay the up-to-one-frame latency cost for every partial update.
static bool rgb_display_color_data_is_frame_buffer(const RgbDisplayInternal* internal, const void* color_data) {
    const auto* ptr = static_cast<const uint8_t*>(color_data);
    for (uint8_t i = 0; i < internal->frame_buffer_count; i++) {
        const auto* base = static_cast<const uint8_t*>(internal->frame_buffers[i]);
        if (ptr >= base && ptr < base + internal->frame_buffer_size_bytes) {
            return true;
        }
    }
    return false;
}

// Converts a contiguous RGB565 region into the configured scan-out format, if any. The
// conversion is delegated to the pixel mapper selected in start(); the result is written into the
// mapper's scratch buffer so the zero-copy frame-buffer wait logic below can treat it like any
// other caller-owned buffer.
static error_t rgb_display_draw_bitmap(Device* device, int32_t x_start, int32_t y_start, int32_t x_end, int32_t y_end, const void* color_data) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    const auto* config = GET_CONFIG(device);

    const void* source_data = color_data;
    // Native (post-rotation) draw coordinates, defaulting to the logical ones unchanged for every
    // format except RGB666_6BIT_TRIPLE (see below).
    int32_t draw_x_start = x_start, draw_y_start = y_start, draw_x_end = x_end, draw_y_end = y_end;
    bool use_native_double_buffer = false;

    if (internal->pixel_mapper != nullptr) {
        int32_t tile_w = x_end - x_start;
        int32_t tile_h = y_end - y_start;
        uint32_t pixel_count = (uint32_t)tile_w * (uint32_t)tile_h;
        // The mapper's own destination buffer is its instance data, so it is passed as both the
        // instance handle and the output buffer.
        internal->pixel_mapper->map(
            internal->pixel_mapper_data,
            static_cast<const uint16_t*>(color_data),
            static_cast<uint8_t*>(internal->pixel_mapper_data),
            pixel_count
        );
        source_data = internal->pixel_mapper_data;

        if (internal->is_rgb666_triple) {
            // Rotate this tile 90 degrees CW from logical (LVGL-facing, landscape) into native
            // (portrait) coordinates, PLUS a horizontal mirror (confirmed needed on real hardware
            // after the rotation alone left the image mirrored) - see is_rgb666_triple's struct
            // comment for why the rotation is needed at all. The mapper's output above is
            // row-major over the LOGICAL tile (tile_w x tile_h); this repacks it into row-major
            // over the NATIVE tile (tile_h x tile_w, axes swapped) using the transform confirmed
            // on hardware for the rotation=1 case, with the mirror folded in:
            //   native_x = vertical_resolution - 1 - logical_y
            //   native_y = horizontal_resolution - 1 - logical_x
            // Derivation for the per-tile loop below: for output position (nx_rel, ny_rel) within
            // the native tile, ny_rel corresponds to logical tx=tile_w-1-ny_rel (native_y
            // decreases as logical_x increases, from the mirror); nx_rel corresponds to logical
            // ty=tile_h-1-nx_rel (native_x decreases as logical_y increases, unchanged by the mirror).
            // With 2 native frame buffers (see native_fb_write_index), write the rotated frame
            // straight into the one *not* currently scanned out instead of into rotation_scratch -
            // this makes source_data one of internal->frame_buffers[] below, which puts it on the
            // zero-copy path (esp_lcd_rgb_panel just repoints scan-out to it) instead of racing a
            // CPU copy into the single live buffer against the free-running DMA. Requires the panel
            // to always be flushed as one full-screen tile (DISPLAY_CAPABILITY_REQUIRES_FULL_FRAME,
            // see rgb_display_has_capability()): a partial tile written to only one of the two
            // buffers would leave the other buffer showing a stale mix once it's flipped back to.
            use_native_double_buffer = internal->frame_buffer_count >= 2;
            const auto* mapped = static_cast<const uint8_t*>(source_data);
            auto* transposed = use_native_double_buffer
                ? static_cast<uint8_t*>(internal->frame_buffers[internal->native_fb_write_index])
                : static_cast<uint8_t*>(internal->rotation_scratch);
            int32_t native_tile_w = tile_h;
            int32_t native_tile_h = tile_w;
            for (int32_t ny_rel = 0; ny_rel < native_tile_h; ny_rel++) {
                int32_t tx = tile_w - 1 - ny_rel;
                for (int32_t nx_rel = 0; nx_rel < native_tile_w; nx_rel++) {
                    int32_t ty = tile_h - 1 - nx_rel;
                    size_t src_idx = ((size_t)ty * tile_w + tx) * 3;
                    size_t dst_idx = ((size_t)ny_rel * native_tile_w + nx_rel) * 3;
                    transposed[dst_idx + 0] = mapped[src_idx + 0];
                    transposed[dst_idx + 1] = mapped[src_idx + 1];
                    transposed[dst_idx + 2] = mapped[src_idx + 2];
                }
            }
            source_data = transposed;

            draw_x_start = (int32_t)config->vertical_resolution - y_end;
            draw_x_end = (int32_t)config->vertical_resolution - y_start;
            draw_y_start = (int32_t)config->horizontal_resolution - x_end;
            draw_y_end = (int32_t)config->horizontal_resolution - x_start;
        }
    }

    // With num_fbs=1 nothing synchronises: LVGL never binds the real buffer, so every copy races
    // the free-running DMA. Wait for on_frame_buf_complete before the copy, not after.
    if (internal->is_rgb666_triple && !use_native_double_buffer) {
        xSemaphoreTake(internal->frame_complete_semaphore, 0); // clear any stale signal
        xSemaphoreTake(internal->frame_complete_semaphore, portMAX_DELAY);
    }

    // esp_lcd repoints scan-out near-instantly, before the DMA has finished the previous buffer,
    // so that buffer must not be written again until on_frame_buf_complete confirms it is free.
    bool wait_for_scanout = rgb_display_color_data_is_frame_buffer(internal, source_data);
    if (wait_for_scanout) {
        xSemaphoreTake(internal->frame_complete_semaphore, 0); // clear any already-pending signal
    }

    // Unscaled x/y for every format but RGB666_6BIT_TRIPLE, since esp_lcd derives the byte
    // multiplication from bits_per_pixel. For that format, the rotated native coordinates above.
    if (esp_lcd_panel_draw_bitmap(internal->panel_handle, draw_x_start, draw_y_start, draw_x_end, draw_y_end, source_data) != ESP_OK) {
        return ERROR_RESOURCE;
    }

    if (use_native_double_buffer) {
        // The buffer just handed to esp_lcd is now the one on-screen; the next write must target
        // the other one.
        internal->native_fb_write_index = 1 - internal->native_fb_write_index;
    }

    if (wait_for_scanout) {
        xSemaphoreTake(internal->frame_complete_semaphore, portMAX_DELAY);
    }

    return ERROR_NONE;
}

static error_t rgb_display_mirror(Device* device, bool x_axis, bool y_axis) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    return esp_lcd_panel_mirror(internal->panel_handle, x_axis, y_axis) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

static error_t rgb_display_swap_xy(Device* device, bool swap_axes) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    return esp_lcd_panel_swap_xy(internal->panel_handle, swap_axes) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

static bool rgb_display_get_swap_xy(Device* device) {
    return GET_CONFIG(device)->swap_xy;
}

static bool rgb_display_get_mirror_x(Device* device) {
    return GET_CONFIG(device)->mirror_x;
}

static bool rgb_display_get_mirror_y(Device* device) {
    return GET_CONFIG(device)->mirror_y;
}

// set_gap is not exposed: RGB panels are raw scan-out framebuffers with no addressable-window
// concept the way MIPI/SPI panels have, so there's no gap to set.

static error_t rgb_display_invert_color(Device* device, bool invert_color_data) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    return esp_lcd_panel_invert_color(internal->panel_handle, invert_color_data) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

static error_t rgb_display_disp_on_off(Device* device, bool on_off) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    return esp_lcd_panel_disp_on_off(internal->panel_handle, on_off) == ESP_OK ? ERROR_NONE : ERROR_RESOURCE;
}

// disp_sleep is not exposed: RGB panels have no MIPI DCS command interface, so there's no sleep
// mode to enter.

static enum DisplayColorFormat rgb_display_get_color_format(Device*) {
    return DISPLAY_COLOR_FORMAT_RGB565;
}

static uint16_t rgb_display_get_resolution_x(Device* device) {
    return GET_CONFIG(device)->horizontal_resolution;
}

static uint16_t rgb_display_get_resolution_y(Device* device) {
    return GET_CONFIG(device)->vertical_resolution;
}

static void rgb_display_get_frame_buffer(Device* device, uint8_t index, void** out_buffer) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    *out_buffer = index < internal->frame_buffer_count ? internal->frame_buffers[index] : nullptr;
}

static uint8_t rgb_display_get_frame_buffer_count(Device* device) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    // A converted-format panel's frame buffer runs at the native bits_per_pixel, which LVGL can't
    // write directly (it always renders RGB565 for this driver). Exposing it would make
    // lvgl_display.c bind LVGL straight onto it and corrupt the buffer; instead report 0 so LVGL
    // renders into its own RGB565 buffers and flushes per-tile through the conversion in
    // draw_bitmap().
    return GET_CONFIG(device)->pixel_format != RGB_DISPLAY_PIXEL_FORMAT_DEFAULT ? 0 : internal->frame_buffer_count;
}

static error_t rgb_display_get_backlight(Device* device, Device** backlight) {
    auto* configured_backlight = GET_CONFIG(device)->backlight;
    if (configured_backlight == nullptr) {
        return ERROR_NOT_SUPPORTED;
    }
    *backlight = configured_backlight;
    return ERROR_NONE;
}

constexpr uint32_t RGB_DISPLAY_CAPABILITIES = DISPLAY_CAPABILITY_CAP_MIRROR | DISPLAY_CAPABILITY_CAP_SWAP_XY |
    DISPLAY_CAPABILITY_INVERT_COLOR | DISPLAY_CAPABILITY_ON_OFF | DISPLAY_CAPABILITY_BACKLIGHT;

// esp_lcd_panel_rgb only applies mirror()/swap_xy()'s rotate_mask when draw_bitmap()'s color_data
// is copied into the frame buffer by CPU. When frame_buffer_count > 0, LVGL is bound directly onto
// the panel's own frame buffers (see lvgl_display.c), so color_data always already *is* the frame
// buffer and that copy - and with it the rotation - never happens. Report those two capabilities as
// unavailable in that configuration so callers (e.g. lvgl_display.c) don't rely on a rotation that
// silently does nothing.
static bool rgb_display_has_capability(Device* device, uint32_t capability) {
    auto* internal = static_cast<RgbDisplayInternal*>(device_get_driver_data(device));
    uint32_t capabilities = RGB_DISPLAY_CAPABILITIES;
    if (internal->frame_buffer_count > 0) {
        capabilities &= ~(DISPLAY_CAPABILITY_CAP_MIRROR | DISPLAY_CAPABILITY_CAP_SWAP_XY);
    }
    // The native ping-pong writes a whole frame buffer per call; a partial tile would leave the
    // other buffer showing a stale mix. LVGL must therefore flush the full screen.
    if (internal->is_rgb666_triple && internal->frame_buffer_count >= 2) {
        capabilities |= DISPLAY_CAPABILITY_REQUIRES_FULL_FRAME;
    }
    return (capabilities & capability) == capability;
}

// endregion

static const DisplayApi rgb_display_api = {
    .capabilities = RGB_DISPLAY_CAPABILITIES,
    .reset = rgb_display_reset,
    .init = rgb_display_init,
    .draw_bitmap = rgb_display_draw_bitmap,
    .mirror = rgb_display_mirror,
    .swap_xy = rgb_display_swap_xy,
    .get_swap_xy = rgb_display_get_swap_xy,
    .get_mirror_x = rgb_display_get_mirror_x,
    .get_mirror_y = rgb_display_get_mirror_y,
    .set_gap = nullptr,
    .invert_color = rgb_display_invert_color,
    .disp_on_off = rgb_display_disp_on_off,
    .disp_sleep = nullptr,
    .get_color_format = rgb_display_get_color_format,
    .get_resolution_x = rgb_display_get_resolution_x,
    .get_resolution_y = rgb_display_get_resolution_y,
    .get_frame_buffer = rgb_display_get_frame_buffer,
    .get_frame_buffer_count = rgb_display_get_frame_buffer_count,
    .get_backlight = rgb_display_get_backlight,
    .has_capability = rgb_display_has_capability,
};

Driver rgb_display_driver = {
    .name = "rgb_display",
    .compatible = (const char*[]) { "espressif,esp32-rgb-display", nullptr },
    .start_device = start,
    .stop_device = stop,
    .api = &rgb_display_api,
    .device_type = &DISPLAY_TYPE,
    .owner = &rgb_display_module,
    .internal = nullptr
};

#endif // SOC_LCD_RGB_SUPPORTED
