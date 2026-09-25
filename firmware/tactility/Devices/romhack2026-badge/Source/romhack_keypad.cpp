#include <tactility/device.h>
#include <tactility/driver.h>
#include <tactility/drivers/i2c_controller.h>
#include <tactility/drivers/keyboard.h>
#include <tactility/error.h>
#include <tactility/log.h>
#include <lvgl.h>
#include <lvgl/widgets/toolbar.h>
#include <app/manager.h>
#include <string.h>
#include <driver/gpio.h>

static constexpr const char* TAG = "RomhackKeypad";

static constexpr uint8_t AW9523_REG_INPUT_P0 = 0x00;
static constexpr uint8_t AW9523_REG_INPUT_P1 = 0x01;
static constexpr uint8_t AW9523_REG_CONFIG_P0 = 0x04;
static constexpr uint8_t AW9523_REG_CONFIG_P1 = 0x05;
static constexpr uint8_t AW9523_REG_LEDMODE_P0 = 0x12;
static constexpr uint8_t AW9523_REG_LEDMODE_P1 = 0x13;

static constexpr uint8_t P0_B = (1U << 0U);
static constexpr uint8_t P0_PLAY = (1U << 1U);
static constexpr uint8_t P0_RIGHT = (1U << 2U);
static constexpr uint8_t P0_DOWN = (1U << 3U);
static constexpr uint8_t P0_LEFT = (1U << 5U);
static constexpr uint8_t P0_UP = (1U << 6U);
static constexpr uint8_t P1_PREV = (1U << 0U);
static constexpr uint8_t P1_A1 = (1U << 1U);

#define DEBOUNCE_MS 50

// DIAGNOSTIC: carries scan_key's LVGL state out to keypad_read_key's log.
// Remove once the keypad misbehaviour is pinned down.
static bool diag_editing = false;
static bool diag_dropdown_open = false;
static bool diag_slider_focused = false;

struct RomhackKeypadInternal {
    Device* i2c_controller;
    uint32_t active_key;
    uint32_t physical_keys;
    uint32_t pending_key;
    bool pending_pressed;
    bool has_pending;
    uint32_t last_state_change_time;
};

static error_t start(Device* device) {
    auto* internal = static_cast<RomhackKeypadInternal*>(malloc(sizeof(RomhackKeypadInternal)));
    if (internal == nullptr) {
        return ERROR_OUT_OF_MEMORY;
    }
    *internal = {};
    internal->last_state_change_time = 0;
    if (device_get_by_name("i2c_internal", &internal->i2c_controller) != ERROR_NONE) {
        free(internal);
        return ERROR_NOT_FOUND;
    }

    uint8_t val = 0xFF;
    i2c_controller_write_register(internal->i2c_controller, 0x58, AW9523_REG_LEDMODE_P0, &val, 1, 1000);
    i2c_controller_write_register(internal->i2c_controller, 0x58, AW9523_REG_LEDMODE_P1, &val, 1, 1000);
    // P0.7 is the amplifier's shutdown pin and is driven by its power-rail device; the remaining
    // P0 pins are the buttons this driver reads.
    val = 0x7F;
    i2c_controller_write_register(internal->i2c_controller, 0x58, AW9523_REG_CONFIG_P0, &val, 1, 1000);

    // The expander clears its configuration only on a power cycle, so the two P1 buttons are
    // re-asserted as inputs: a stale output latch reads low, which looks like a key held forever.
    i2c_controller_register8_set_bits(internal->i2c_controller, 0x58, AW9523_REG_CONFIG_P1, P1_PREV | P1_A1, 1000);

    gpio_config_t io_conf = {};
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_INPUT;
    io_conf.pin_bit_mask = (1ULL << 0);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io_conf);

    // DIAGNOSTIC: expander config bits; 1 means input, so P1 bits 0 and 1 must be set.
    uint8_t config_p0 = 0;
    uint8_t config_p1 = 0;
    i2c_controller_register8_get(internal->i2c_controller, 0x58, AW9523_REG_CONFIG_P0, &config_p0, 1000);
    i2c_controller_register8_get(internal->i2c_controller, 0x58, AW9523_REG_CONFIG_P1, &config_p1, 1000);
    LOG_I(TAG, "config after init: P0=0x%02X P1=0x%02X", config_p0, config_p1);

    device_set_driver_data(device, internal);
    return ERROR_NONE;
}

static error_t stop(Device* device) {
    free(device_get_driver_data(device));
    return ERROR_NONE;
}

static uint32_t scan_key(RomhackKeypadInternal* internal, uint32_t* out_physical) {
    uint8_t p0 = 0xFF;
    uint8_t p1 = 0xFF;
    
    if (i2c_controller_read_register(internal->i2c_controller, 0x58, AW9523_REG_INPUT_P0, &p0, 1, 100) != ERROR_NONE ||
        i2c_controller_read_register(internal->i2c_controller, 0x58, AW9523_REG_INPUT_P1, &p1, 1, 100) != ERROR_NONE) {
        if (out_physical) *out_physical = 0;
        return 0;
    }

    uint8_t pressed0 = (uint8_t)~p0;
    uint8_t pressed1 = (uint8_t)~p1;
    bool gpio0_pressed = (gpio_get_level(GPIO_NUM_0) == 0);
    
    if (out_physical) {
        *out_physical = ((uint32_t)gpio0_pressed << 16) | ((uint32_t)pressed1 << 8) | pressed0;
    }
    if (pressed0 == 0 && pressed1 == 0 && !gpio0_pressed) return 0;

    bool editing = false;
    bool dropdown_open = false;
    // Whether the focused widget uses left and right itself, rather than leaving them to walk
    // the focus ring. A slider always does; anything else says so with LV_OBJ_FLAG_USER_1.
    bool horizontal_keys = false;
    // The same for up and down, claimed with LV_OBJ_FLAG_USER_2.
    bool vertical_keys = false;
    lv_indev_t* indev = lv_indev_get_next(nullptr);
    while (indev) {
        if (lv_indev_get_type(indev) == LV_INDEV_TYPE_KEYPAD) {
            lv_group_t* g = lv_indev_get_group(indev);
            if (g != nullptr) {
                if (lv_group_get_editing(g)) {
                    editing = true;
                }
                extern const lv_obj_class_t lv_dropdown_class;
                extern const lv_obj_class_t lv_slider_class;
                lv_obj_t* focused = lv_group_get_focused(g);
                if (focused != nullptr) {
                    if (lv_obj_check_type(focused, &lv_dropdown_class)) {
                        if (lv_dropdown_is_open(focused)) {
                            dropdown_open = true;
                        }
                    } else if (lv_obj_check_type(focused, &lv_slider_class) ||
                               lv_obj_has_flag(focused, LV_OBJ_FLAG_USER_1)) {
                        horizontal_keys = true;
                    }
                    if (lv_obj_has_flag(focused, LV_OBJ_FLAG_USER_2)) {
                        vertical_keys = true;
                    }
                }
            }
        }
        indev = lv_indev_get_next(indev);
    }

    diag_editing = editing;
    diag_dropdown_open = dropdown_open;
    diag_slider_focused = horizontal_keys;

    bool use_dir = editing || dropdown_open;
    if (pressed0 & P0_UP) return (use_dir || vertical_keys) ? LV_KEY_UP : LV_KEY_PREV;
    if (pressed0 & P0_DOWN) return (use_dir || vertical_keys) ? LV_KEY_DOWN : LV_KEY_NEXT;
    if (pressed0 & P0_LEFT) return (use_dir || horizontal_keys) ? LV_KEY_LEFT : LV_KEY_PREV;
    if (pressed0 & P0_RIGHT) return (use_dir || horizontal_keys) ? LV_KEY_RIGHT : LV_KEY_NEXT;
    if (pressed1 & P1_A1) return LV_KEY_ENTER;
    
    if (pressed0 & P0_B) {
        if (!editing && !dropdown_open) {
            return 0x10000; // Special internal code for "Toolbar Back"
        }
        return LV_KEY_ESC;
    }
    
    if (pressed0 & P0_PLAY) return 0x20000; // Custom code for Media Play/Pause
    if (pressed1 & P1_PREV) return 0x20001; // Custom code for Media Prev
    if (gpio0_pressed) return 0x20002;      // Custom code for Media Next

    return 0;
}

static error_t keypad_read_key(Device* device, KeyboardKeyData* data) {
    auto* internal = static_cast<RomhackKeypadInternal*>(device_get_driver_data(device));

    uint32_t new_physical = 0;
    uint32_t new_key = scan_key(internal, &new_physical);
    uint32_t current_time = lv_tick_get();

    // DIAGNOSTIC: logged only on a physical change. p0 bit0=B bit1=PLAY bit2=RIGHT bit3=DOWN
    // bit5=LEFT bit6=UP, p1 bit0=PREV bit1=A1.
    if (new_physical != internal->physical_keys) {
        LOG_I(TAG, "raw p0=0x%02X p1=0x%02X menu=%d | edit=%d drop=%d slider=%d | key=0x%X",
            (unsigned)(new_physical & 0xFF), (unsigned)((new_physical >> 8) & 0xFF),
            (int)((new_physical >> 16) & 1), (int)diag_editing, (int)diag_dropdown_open,
            (int)diag_slider_focused, (unsigned)new_key);
    }

    if (new_physical == internal->physical_keys && internal->active_key != 0) {
        new_key = internal->active_key;
    }

    if (new_physical != internal->physical_keys) {
        if (current_time - internal->last_state_change_time >= DEBOUNCE_MS) {
            if (internal->physical_keys != 0 && new_physical != 0) {
                // Force a release of the old key first before accepting the new press
                new_key = 0;
                new_physical = 0;
                // last_state_change_time is deliberately not updated, so the next press lands immediately.
            } else {
                internal->last_state_change_time = current_time;
            }

            if (internal->active_key != 0) {
                internal->pending_key = internal->active_key;
                internal->pending_pressed = false;
                internal->has_pending = true;
            } else if (new_key != 0) {
                internal->pending_key = new_key;
                internal->pending_pressed = true;
                internal->has_pending = true;

                if (new_key == 0x10000) {
                    lvgl_toolbar_trigger_back();
                }
            }
            internal->active_key = new_key;
            internal->physical_keys = new_physical;
        }
    }

    uint32_t current_report_key = internal->has_pending ? internal->pending_key : internal->active_key;
    if (current_report_key == 0x10000) {
        // Acted on here, so the press must be reported as activity or back cannot wake the badge.
        lv_display_trigger_activity(nullptr);
        data->key = 0;
        data->pressed = false;
        data->continue_reading = false;
        internal->has_pending = false;
        return ERROR_NONE;
    }

    if (internal->has_pending) {
        data->key = internal->pending_key;
        data->pressed = internal->pending_pressed;
        data->continue_reading = false; // We process one at a time for simplicity
        internal->has_pending = false;
    } else {
        data->key = internal->active_key;
        data->pressed = (internal->active_key != 0);
        data->continue_reading = false;
    }

    return ERROR_NONE;
}

static bool keypad_is_present(Device* device) {
    return false; // Not a full QWERTY keyboard, so don't suppress the onscreen touch keyboard
}

static const KeyboardApi keypad_api = {
    .read_key = keypad_read_key,
    .get_backlight = nullptr,
    .is_present = keypad_is_present,
};

extern Module romhack2026_badge_module;

Driver romhack_keypad_driver = {
    .name = "romhack_keypad",
    .compatible = (const char*[]) { "romhack,keypad", nullptr },
    .start_device = start,
    .stop_device = stop,
    .api = &keypad_api,
    .device_type = &KEYBOARD_TYPE,
    .owner = &romhack2026_badge_module,
    .internal = nullptr
};
