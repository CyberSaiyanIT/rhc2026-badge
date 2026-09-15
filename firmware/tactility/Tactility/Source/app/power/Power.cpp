#include <Tactility/lvgl/Style.h>
#include <Tactility/PowerManager.h>
#include <Tactility/Timer.h>

#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>

#include <lvgl_window_manager/window_manager.h>

#include <tactility/check.h>
#include <tactility/device.h>
#include <tactility/drivers/power_rail.h>
#include <tactility/drivers/power_supply.h>
#include <tactility/time.h>

#ifdef ESP_PLATFORM
#include <esp_system.h>
#endif

#include <lvgl/fonts.h>
#include <lvgl/lvgl.h>
#include <lvgl/widgets/toolbar.h>

#include <vector>

namespace tt::app::power {

#define TAG "power"

extern const ::AppManifest manifest;

namespace {

constexpr PowerSupplyProperty DISPLAYED_PROPERTIES[] = {
    POWER_SUPPLY_PROP_VOLTAGE,
    POWER_SUPPLY_PROP_CAPACITY,
    POWER_SUPPLY_PROP_CURRENT,
    POWER_SUPPLY_PROP_IS_CHARGING,
};

/** The switched rails, with the labels used while bringing the badge up. */
struct RailDescriptor {
    const char* deviceName;
    const char* label;
};

constexpr RailDescriptor RAILS[] = {
    { "speaker_power", "amp" },
    { "neopixel_power", "neopixel mos" },
    { "boost5v", "boost_en" },
    { "rfid_power", "rfid" },
};

struct PropertyWidget {
    PowerSupplyProperty property;
    lv_obj_t* value;
};

struct RailWidget {
    ::Device* device = nullptr;
    lv_obj_t* value = nullptr;
};

struct DeviceEntry {
    ::Device* device = nullptr;
    lv_obj_t* enableSwitch = nullptr;
    lv_obj_t* quickChargeSwitch = nullptr;
    std::vector<PropertyWidget> propertyWidgets;
};

struct Context {
    uint32_t appInstanceId;
    std::unique_ptr<Timer> timer;
    std::vector<DeviceEntry> entries;
    std::vector<RailWidget> rails;
};

const char* getResetReasonText() {
#ifdef ESP_PLATFORM
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON: return "power on";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_DEEPSLEEP: return "deep sleep wake";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_SDIO: return "SDIO";
        default: return "unknown";
    }
#else
    return "unknown";
#endif
}

const char* propertyName(PowerSupplyProperty property) {
    switch (property) {
        case POWER_SUPPLY_PROP_IS_CHARGING: return "Charging";
        case POWER_SUPPLY_PROP_VOLTAGE: return "Voltage";
        case POWER_SUPPLY_PROP_CAPACITY: return "Level";
        case POWER_SUPPLY_PROP_CURRENT: return "Current";
    }
    return "";
}

void setPropertyValueText(lv_obj_t* value, PowerSupplyProperty property, const PowerSupplyPropertyValue& raw) {
    switch (property) {
        case POWER_SUPPLY_PROP_IS_CHARGING:
            lv_label_set_text(value, raw.int_value ? "yes" : "no");
            break;
        case POWER_SUPPLY_PROP_VOLTAGE:
            lv_label_set_text_fmt(value, "%d mV", raw.int_value);
            break;
        case POWER_SUPPLY_PROP_CAPACITY:
            lv_label_set_text_fmt(value, "%d%%", raw.int_value);
            break;
        case POWER_SUPPLY_PROP_CURRENT:
            lv_label_set_text_fmt(value, "%d mA", raw.int_value);
            break;
    }
}

// Same palette as the Lighting app. A stock lv_button carries the theme's primary colour, far
// too loud for a page of mostly read-only rows.
constexpr lv_color_t surface() { return lv_color_hex(0x1B1D23); }
constexpr lv_color_t surfaceRaised() { return lv_color_hex(0x272A33); }

/**
 * Focus is drawn as a ring inside the widget: the theme's default outline is painted outside it
 * and gets clipped by the scrolling parent on the first and last rows.
 */
void styleFocusRing(lv_obj_t* object) {
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUSED);
    lv_obj_set_style_outline_width(object, 0, LV_STATE_FOCUS_KEY);
    lv_obj_set_style_border_width(object, 2, LV_STATE_FOCUSED);
    lv_obj_set_style_border_color(object, lv_color_white(), LV_STATE_FOCUSED);
    lv_obj_set_style_border_opa(object, LV_OPA_90, LV_STATE_FOCUSED);
}

void createHeading(lv_obj_t* parent, const char* text) {
    auto* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, lvgl_get_text_font(FONT_SIZE_SMALL), LV_PART_MAIN);
    lv_obj_set_style_text_opa(label, LV_OPA_60, LV_PART_MAIN);
    lv_obj_set_style_pad_top(label, 6, LV_PART_MAIN);
}

/**
 * A read-only row built on a button so it can take focus. The badge has no touch, so a page of
 * plain labels has nothing to focus and would strand everything past the first screen.
 *
 * @return the value label on the right, which the periodic refresh writes to
 */
lv_obj_t* createInfoRow(lv_obj_t* parent, const char* name) {
    auto* row = lv_button_create(parent);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_radius(row, 8, LV_PART_MAIN);
    lv_obj_set_style_bg_color(row, surfaceRaised(), LV_PART_MAIN);
    lv_obj_set_style_pad_ver(row, 6, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    styleFocusRing(row);

    auto* label = lv_label_create(row);
    lv_label_set_text(label, name);

    auto* value = lv_label_create(row);
    lv_label_set_text(value, "--");
    return value;
}

lv_obj_t* createSwitchRow(lv_obj_t* parent, const char* name, bool on, lv_event_cb_t callback, void* userData) {
    auto* row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_ver(row, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_hor(row, 10, LV_PART_MAIN);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);

    auto* label = lv_label_create(row);
    lv_label_set_text(label, name);

    auto* toggle = lv_switch_create(row);
    lv_obj_set_state(toggle, LV_STATE_CHECKED, on);
    styleFocusRing(toggle);
    lv_obj_add_event_cb(toggle, callback, LV_EVENT_VALUE_CHANGED, userData);
    return toggle;
}

void setRailValueText(const RailWidget& rail) {
    bool enabled = false;
    if (power_rail_is_enabled(rail.device, &enabled) == ERROR_NONE) {
        lv_label_set_text(rail.value, enabled ? "1" : "0");
    } else {
        lv_label_set_text(rail.value, "?");
    }
}

/**
 * A supply reporting a voltage is believed only while that voltage is plausible, since the charge
 * level comes from the same reading. A supply with no voltage, like a fuel gauge, is trusted.
 */
bool isReadingUsable(::Device* device) {
    if (!power_supply_supports_property(device, POWER_SUPPLY_PROP_VOLTAGE)) {
        return true;
    }
    PowerSupplyPropertyValue voltage;
    if (power_supply_get_property(device, POWER_SUPPLY_PROP_VOLTAGE, &voltage) != ERROR_NONE) {
        return false;
    }
    return tt::power::isPlausible(voltage.int_value);
}

void updateUi(Context* ctx) {
    lvgl_lock();

    for (const auto& rail : ctx->rails) {
        setRailValueText(rail);
    }

    for (auto& entry : ctx->entries) {
        if (entry.enableSwitch != nullptr) {
            lv_obj_set_state(entry.enableSwitch, LV_STATE_CHECKED, power_supply_is_allowed_to_charge(entry.device));
        }

        if (entry.quickChargeSwitch != nullptr) {
            lv_obj_set_state(entry.quickChargeSwitch, LV_STATE_CHECKED, power_supply_is_quick_charge_enabled(entry.device));
        }

        if (!isReadingUsable(entry.device)) {
            continue;
        }

        PowerSupplyPropertyValue value;
        for (auto& widget : entry.propertyWidgets) {
            if (power_supply_get_property(entry.device, widget.property, &value) == ERROR_NONE) {
                setPropertyValueText(widget.value, widget.property, value);
            }
        }
    }

    lvgl_unlock();
}

void onBackPressed(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    app_event_emit_close(ctx->appInstanceId);
}

void onPowerEnabledChanged(lv_event_t* event) {
    auto* enable_switch = lv_event_get_target_obj(event);
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* device = static_cast<::Device*>(lv_obj_get_user_data(enable_switch));
    bool is_on = lv_obj_has_state(enable_switch, LV_STATE_CHECKED);

    if (power_supply_is_allowed_to_charge(device) != is_on) {
        power_supply_set_allowed_to_charge(device, is_on);
        updateUi(ctx);
    }
}

void onQuickChargeChanged(lv_event_t* event) {
    auto* qc_switch = lv_event_get_target_obj(event);
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    auto* device = static_cast<::Device*>(lv_obj_get_user_data(qc_switch));
    bool is_on = lv_obj_has_state(qc_switch, LV_STATE_CHECKED);

    if (power_supply_is_quick_charge_enabled(device) != is_on) {
        power_supply_set_quick_charge_enabled(device, is_on);
        updateUi(ctx);
    }
}

void onOverrideLimitsChanged(lv_event_t* event) {
    auto* toggle = lv_event_get_target_obj(event);
    tt::power::setLimitsOverridden(lv_obj_has_state(toggle, LV_STATE_CHECKED));
}

bool collectDevice(::Device* device, void* context) {
    auto* devices = static_cast<std::vector<::Device*>*>(context);
    devices->push_back(device);
    return true;
}

void createSupplySections(lv_obj_t* parent, Context* ctx) {
    std::vector<::Device*> devices;
    device_for_each_of_type(&POWER_SUPPLY_TYPE, &devices, collectDevice);

    ctx->entries.clear();
    ctx->entries.reserve(devices.size());

    for (::Device* device : devices) {
        DeviceEntry entry;
        entry.device = device;

        createHeading(parent, device->name);

        const bool usable = isReadingUsable(device);
        PowerSupplyPropertyValue value;
        for (auto property : DISPLAYED_PROPERTIES) {
            if (power_supply_get_property(device, property, &value) == ERROR_NONE) {
                auto* value_label = createInfoRow(parent, propertyName(property));
                if (usable) {
                    setPropertyValueText(value_label, property, value);
                }
                entry.propertyWidgets.push_back({ property, value_label });
            }
        }

        if (power_supply_supports_charge_control(device)) {
            entry.enableSwitch = createSwitchRow(parent, "Charging enabled",
                power_supply_is_allowed_to_charge(device), onPowerEnabledChanged, ctx);
            lv_obj_set_user_data(entry.enableSwitch, device);
        }

        if (power_supply_supports_quick_charge(device)) {
            entry.quickChargeSwitch = createSwitchRow(parent, "Quick charge",
                power_supply_is_quick_charge_enabled(device), onQuickChargeChanged, ctx);
            lv_obj_set_user_data(entry.quickChargeSwitch, device);
        }

        ctx->entries.push_back(entry);
    }
}

void createRailSection(lv_obj_t* parent, Context* ctx) {
    // The window manager rebuilds a window's widgets when it resurfaces, so the previous round's
    // references are handed back rather than leaked.
    for (auto& rail : ctx->rails) {
        device_put(rail.device);
    }
    ctx->rails.clear();

    bool heading_created = false;
    for (const auto& descriptor : RAILS) {
        ::Device* device = nullptr;
        if (device_get_by_name(descriptor.deviceName, &device) != ERROR_NONE) {
            continue;
        }
        if (!heading_created) {
            createHeading(parent, "Rails");
            heading_created = true;
        }
        const RailWidget rail { device, createInfoRow(parent, descriptor.label) };
        setRailValueText(rail);
        ctx->rails.push_back(rail);
    }
}

void createSystemSection(lv_obj_t* parent, Context* ctx) {
    createHeading(parent, "System");
    lv_label_set_text(createInfoRow(parent, "Last reset"), getResetReasonText());
    createSwitchRow(parent, "Override limits", tt::power::isLimitsOverridden(), onOverrideLimitsChanged, ctx);
}

/**
 * The window manager deletes a buried window's widgets while the refresh timer keeps running, so
 * the pointers are dropped here along with the rail references the widget tree owns.
 */
void onWidgetsDeleted(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->entries.clear();
    for (auto& rail : ctx->rails) {
        device_put(rail.device);
    }
    ctx->rails.clear();
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(parent, 0, LV_STATE_DEFAULT);

    auto* toolbar = lvgl_toolbar_create(parent, "Power");
    // The global toolbar nav callback only knows how to stop old-model apps.
    lvgl_toolbar_set_nav_action(toolbar, LV_SYMBOL_CLOSE, onBackPressed, ctx);

    auto* list = lv_obj_create(parent);
    lv_obj_set_width(list, LV_PCT(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_style_border_width(list, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_color(list, surface(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(list, 8, LV_PART_MAIN);
    lv_obj_set_style_pad_row(list, 4, LV_PART_MAIN);
    lv_obj_add_flag(list, LV_OBJ_FLAG_SCROLLABLE);
    // Arrow keys move focus between the rows instead, which scrolls the focused one into view.
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_WITH_ARROW);
    lv_obj_add_event_cb(list, onWidgetsDeleted, LV_EVENT_DELETE, ctx);

    createSupplySections(list, ctx);
    createRailSection(list, ctx);
    createSystemSection(list, ctx);
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    Context ctx {};
    ctx.appInstanceId = appInstanceId;

    // Runs for this app instance's whole lifetime, mirroring GpsSettings/SystemInfo - there's no
    // push notification for power-supply property changes, so this is the only way this screen
    // finds out about them.
    ctx.timer = std::make_unique<Timer>(Timer::Type::Periodic, millis_to_ticks(1000), [&ctx] {
        updateUi(&ctx);
    });

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    check(app_event_subscribe(&sub, &event_group) == ERROR_NONE);

    WindowId window = window_manager_create(appInstanceId, createWidgets, &ctx);
    ctx.timer->start();

    bool shouldClose = false;
    while (!shouldClose) {
        task_event_group_wait_any(&event_group, nullptr, portMAX_DELAY);

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            switch (event.type) {
                case APP_EVENT_CLOSE:
                    shouldClose = true;
                    break;
                default:
                    break;
            }
            if (shouldClose) break;
        }
    }

    ctx.timer->stop();
    window_manager_remove(window);
    check(app_event_unsubscribe(&sub) == ERROR_NONE);
    task_event_group_destruct(&event_group);

    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "tactility.power",
    .name = "Power",
    .category = APP_CATEGORY_SETTINGS,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) }
};

} // namespace
