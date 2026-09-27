#include <Tactility/app/map/MapAppPrivate.h>
#include <Tactility/lvgl/Theme.h>
#include <Tactility/network/Http.h>
#include <Tactility/service/music/Music.h>
#include <Tactility/service/wifi/Wifi.h>
#include <Tactility/file/File.h>
#include <app/event.h>
#include <app/manager.h>
#include <app/manifest.h>
#include <app/scheduler.h>
#include <lvgl_window_manager/window_manager.h>
#include <lvgl/lvgl.h>
#include <lvgl.h>
#include <tactility/log.h>

#include <miniz.h>

#include <esp_heap_caps.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

namespace tt::app::map {

constexpr auto* TAG = "MapApp";

constexpr auto* MAP_URL = "https://romhack.io/RomHack%20Camp%20Map.png";
constexpr auto* MAP_PATH = "/data/map.png";
constexpr auto* MAP_DOWNLOAD_PATH = "/data/map.png.tmp";
/** The Last-Modified of the installed map, so a check that finds nothing new transfers no body. */
constexpr auto* MAP_STAMP_PATH = "/data/map.png.stamp";

/** The auto-connect timer retries about every two seconds, so this allows several attempts. */
constexpr int WIFI_CONNECT_TIMEOUT_MS = 20000;

namespace {

/** Download runs on the main dispatcher; the decode and all LVGL work happen on the app's own task. */
enum class DownloadState {
    Idle,
    WaitingForWifi,
    Busy,
    Succeeded,
    NotModified,
    Failed
};

/**
 * Outlives the app's task: download callbacks run on the main dispatcher and can fire after the
 * app closed, so both they and the app task hold a share.
 */
struct Context {
    uint32_t appInstanceId = 0;
    uint8_t* imageData = nullptr;
    lv_image_dsc_t imageDsc {};
    lv_obj_t* imageObj = nullptr;
    lv_obj_t* imageWrapper = nullptr;
    lv_obj_t* wrapper = nullptr;
    lv_obj_t* statusLabel = nullptr;
    std::string statusText;
    uint32_t width = 0;
    uint32_t height = 0;
    int zoomLevel = 0;
    /** Decoded on the app task after the window exists, so the app is never a blank screen. */
    bool decodePending = true;
    int wifiWaitedMs = 0;
    std::string lastModified;
    std::atomic<bool> refreshRequested { false };
    std::atomic<DownloadState> downloadState { DownloadState::Idle };

    ~Context() { heap_caps_free(imageData); }
};

constexpr int ZOOM_SCALES[] = { 85, 128, 256 };

/** Falls back to internal RAM for devices without PSRAM. */
void* allocPreferExternal(size_t size) {
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

void freeImage(Context* ctx) {
    heap_caps_free(ctx->imageData);
    ctx->imageData = nullptr;
    ctx->width = 0;
    ctx->height = 0;
    ctx->imageDsc = lv_image_dsc_t {};
}

/**
 * Streams a PNG into RGB565 a scanline at a time: ~5.6 MB of free PSRAM cannot hold the ~6.3 MB
 * RGBA8888 intermediate a whole-image decoder needs. 8-bit non-interlaced RGB/RGBA only.
 */
struct PngRowWriter {
    uint8_t* dest;
    uint8_t* current;
    uint8_t* previous;
    uint32_t width;
    uint32_t height;
    uint32_t bytesPerPixel;
    size_t rowStride; // unfiltered scanline bytes, excluding the leading filter byte
    size_t rowFilled; // bytes of the current scanline (filter byte included) received so far
    uint32_t row;
    /** RGB triples from PLTE, used only when bytesPerPixel is 1. */
    uint8_t* palette;
};

uint8_t paethPredictor(uint8_t a, uint8_t b, uint8_t c) {
    const int p = static_cast<int>(a) + b - c;
    const int pa = abs(p - a);
    const int pb = abs(p - b);
    const int pc = abs(p - c);
    if (pa <= pb && pa <= pc) return a;
    return (pb <= pc) ? b : c;
}

bool writeRow(PngRowWriter& writer) {
    const uint8_t filter = writer.current[0];
    uint8_t* data = writer.current + 1;
    const uint32_t bpp = writer.bytesPerPixel;

    for (size_t x = 0; x < writer.rowStride; ++x) {
        const uint8_t a = (x >= bpp) ? data[x - bpp] : 0;
        const uint8_t b = writer.previous[x];
        const uint8_t c = (x >= bpp) ? writer.previous[x - bpp] : 0;
        switch (filter) {
            case 0: break;
            case 1: data[x] += a; break;
            case 2: data[x] += b; break;
            case 3: data[x] += static_cast<uint8_t>((static_cast<int>(a) + b) / 2); break;
            case 4: data[x] += paethPredictor(a, b, c); break;
            default:
                LOG_E(TAG, "Unsupported PNG filter %u", filter);
                return false;
        }
    }

    uint8_t* out = writer.dest + static_cast<size_t>(writer.row) * writer.width * 2;
    for (uint32_t x = 0; x < writer.width; ++x) {
        const uint8_t* pixel = (bpp == 1) ? writer.palette + data[x] * 3 : data + x * bpp;
        const uint16_t rgb565 = ((pixel[0] & 0xF8) << 8) | ((pixel[1] & 0xFC) << 3) | (pixel[2] >> 3);
        out[x * 2] = static_cast<uint8_t>(rgb565 & 0xFF);
        out[x * 2 + 1] = static_cast<uint8_t>(rgb565 >> 8);
    }

    memcpy(writer.previous, data, writer.rowStride);
    writer.row++;
    return true;
}

bool consumeInflated(PngRowWriter& writer, const uint8_t* data, size_t size) {
    while (size > 0 && writer.row < writer.height) {
        const size_t take = std::min(size, writer.rowStride + 1 - writer.rowFilled);
        memcpy(writer.current + writer.rowFilled, data, take);
        writer.rowFilled += take;
        data += take;
        size -= take;
        if (writer.rowFilled == writer.rowStride + 1) {
            writer.rowFilled = 0;
            if (!writeRow(writer)) {
                return false;
            }
        }
    }
    return true;
}

uint32_t readBigEndian32(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

bool decodePng(Context* ctx, const char* path) {
    FILE* file = fopen(path, "rb");
    if (file == nullptr) {
        LOG_E(TAG, "Failed to open %s", path);
        return false;
    }

    uint8_t header[8];
    static constexpr uint8_t PNG_SIGNATURE[8] = { 137, 'P', 'N', 'G', '\r', '\n', 26, '\n' };
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, PNG_SIGNATURE, sizeof(header)) != 0) {
        LOG_E(TAG, "%s is not a PNG", path);
        fclose(file);
        return false;
    }

    PngRowWriter writer {};
    tinfl_decompressor* decompressor = nullptr;
    uint8_t* dictionary = nullptr;
    size_t dictionaryOffset = 0; // the LZ77 window carries across IDAT chunk boundaries
    bool failed = false;
    bool done = false;
    bool inflated = false;
    bool havePalette = false;

    while (!failed && !done) {
        uint8_t chunk_header[8];
        if (fread(chunk_header, 1, sizeof(chunk_header), file) != sizeof(chunk_header)) {
            LOG_E(TAG, "Truncated PNG");
            failed = true;
            break;
        }
        uint32_t chunk_size = readBigEndian32(chunk_header);
        const uint8_t* type = chunk_header + 4;

        if (memcmp(type, "IHDR", 4) == 0) {
            uint8_t ihdr[13];
            if (chunk_size != sizeof(ihdr) || fread(ihdr, 1, sizeof(ihdr), file) != sizeof(ihdr)) {
                LOG_E(TAG, "Bad IHDR");
                failed = true;
                break;
            }
            const uint8_t bit_depth = ihdr[8];
            const uint8_t color_type = ihdr[9];
            if (bit_depth != 8 || (color_type != 2 && color_type != 3 && color_type != 6) || ihdr[12] != 0) {
                LOG_E(TAG, "Unsupported PNG (depth=%u colorType=%u interlace=%u)", bit_depth, color_type, ihdr[12]);
                failed = true;
                break;
            }
            writer.width = readBigEndian32(ihdr);
            writer.height = readBigEndian32(ihdr + 4);
            // Palette entries are one byte; the RGB they stand for comes from PLTE below.
            writer.bytesPerPixel = (color_type == 6) ? 4 : (color_type == 3 ? 1 : 3);
            writer.rowStride = static_cast<size_t>(writer.width) * writer.bytesPerPixel;

            // Plain working memory, never DMA. Left to malloc the three smaller ones land in
            // internal RAM and take ~21 kB of the pool app stacks come from.
            writer.dest = static_cast<uint8_t*>(allocPreferExternal(static_cast<size_t>(writer.width) * writer.height * 2));
            writer.current = static_cast<uint8_t*>(allocPreferExternal(writer.rowStride + 1));
            writer.previous = static_cast<uint8_t*>(allocPreferExternal(writer.rowStride));
            decompressor = static_cast<tinfl_decompressor*>(allocPreferExternal(sizeof(tinfl_decompressor)));
            dictionary = static_cast<uint8_t*>(allocPreferExternal(TINFL_LZ_DICT_SIZE));
            if (color_type == 3) {
                writer.palette = static_cast<uint8_t*>(allocPreferExternal(256 * 3));
                if (writer.palette != nullptr) {
                    memset(writer.palette, 0, 256 * 3);
                }
            }
            if (writer.dest == nullptr || writer.current == nullptr || writer.previous == nullptr || decompressor == nullptr || dictionary == nullptr || (color_type == 3 && writer.palette == nullptr)) {
                LOG_E(TAG, "Out of memory for %ux%u map", writer.width, writer.height);
                failed = true;
                break;
            }
            memset(writer.previous, 0, writer.rowStride); // allocPreferExternal does not zero
            tinfl_init(decompressor);
            chunk_size = 0;
        } else if (memcmp(type, "PLTE", 4) == 0 && writer.palette != nullptr) {
            if (chunk_size > 256 * 3 || chunk_size % 3 != 0) {
                LOG_E(TAG, "Bad PLTE (%lu bytes)", (unsigned long) chunk_size);
                failed = true;
                break;
            }
            if (fread(writer.palette, 1, chunk_size, file) != chunk_size) {
                LOG_E(TAG, "Truncated PLTE");
                failed = true;
                break;
            }
            havePalette = true;
            chunk_size = 0;
        } else if (memcmp(type, "IDAT", 4) == 0) {
            if (writer.dest == nullptr) {
                LOG_E(TAG, "IDAT before IHDR");
                failed = true;
                break;
            }
            if (writer.bytesPerPixel == 1 && !havePalette) {
                LOG_E(TAG, "Indexed PNG without a palette");
                failed = true;
                break;
            }
            uint8_t input[512];
            while (chunk_size > 0 && !failed && !inflated) {
                const size_t wanted = std::min<size_t>(chunk_size, sizeof(input));
                const size_t available = fread(input, 1, wanted, file);
                if (available != wanted) {
                    LOG_E(TAG, "Truncated IDAT");
                    failed = true;
                    break;
                }
                chunk_size -= available;

                size_t input_offset = 0;
                while (input_offset < available) {
                    size_t in_size = available - input_offset;
                    size_t out_size = TINFL_LZ_DICT_SIZE - dictionaryOffset;
                    const tinfl_status status = tinfl_decompress(
                        decompressor,
                        input + input_offset, &in_size,
                        dictionary, dictionary + dictionaryOffset, &out_size,
                        TINFL_FLAG_PARSE_ZLIB_HEADER | TINFL_FLAG_HAS_MORE_INPUT
                    );
                    input_offset += in_size;
                    if (!consumeInflated(writer, dictionary + dictionaryOffset, out_size)) {
                        failed = true;
                        break;
                    }
                    dictionaryOffset = (dictionaryOffset + out_size) & (TINFL_LZ_DICT_SIZE - 1);
                    if (status < TINFL_STATUS_DONE) {
                        LOG_E(TAG, "Inflate failed (status=%d)", static_cast<int>(status));
                        failed = true;
                        break;
                    }
                    if (status == TINFL_STATUS_DONE) {
                        inflated = true;
                        break;
                    }
                }
            }
            chunk_size = 0;
        } else if (memcmp(type, "IEND", 4) == 0) {
            done = true;
        }

        if (fseek(file, chunk_size + 4 /* CRC */, SEEK_CUR) != 0) {
            failed = true;
            break;
        }
    }

    fclose(file);
    heap_caps_free(writer.palette);
    heap_caps_free(writer.current);
    heap_caps_free(writer.previous);
    heap_caps_free(decompressor);
    heap_caps_free(dictionary);

    if (failed || writer.row != writer.height || writer.height == 0) {
        LOG_E(TAG, "Failed to decode %s (%u of %u rows)", path, writer.row, writer.height);
        heap_caps_free(writer.dest);
        return false;
    }

    ctx->width = writer.width;
    ctx->height = writer.height;
    ctx->imageDsc.header.cf = LV_COLOR_FORMAT_RGB565;
    ctx->imageDsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    ctx->imageDsc.header.flags = 0;
    ctx->imageDsc.header.w = writer.width;
    ctx->imageDsc.header.h = writer.height;
    ctx->imageDsc.header.stride = writer.width * 2;
    ctx->imageDsc.data_size = static_cast<size_t>(writer.width) * writer.height * 2;
    ctx->imageDsc.data = writer.dest;
    // Published last: createWidgets can resurface this window on another task mid-decode and keys off it.
    ctx->imageData = writer.dest;
    return true;
}

bool loadImage(Context* ctx, const char* path) {
    LOG_I(TAG, "Decoding %s (free SPIRAM: %zu)", path, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    if (!decodePng(ctx, path)) {
        return false;
    }
    LOG_I(TAG, "Loaded %s (%lux%lu, free SPIRAM: %zu)", path, ctx->width, ctx->height, heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
    return true;
}

void renderStatus(Context* ctx) {
    if (ctx->statusLabel == nullptr) {
        return;
    }
    lv_label_set_text(ctx->statusLabel, ctx->statusText.c_str());
    if (ctx->statusText.empty()) {
        lv_obj_add_flag(ctx->statusLabel, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(ctx->statusLabel, LV_OBJ_FLAG_HIDDEN);
    }
}

void setStatus(Context* ctx, const char* text) {
    ctx->statusText = (text != nullptr) ? text : "";
    renderStatus(ctx);
}

void applyZoom(Context* ctx, int oldScale, bool keepCenter) {
    if (ctx->imageObj == nullptr) {
        return;
    }
    const int scale = ZOOM_SCALES[ctx->zoomLevel];

    const lv_coord_t wrapper_w = lv_obj_get_width(ctx->wrapper);
    const lv_coord_t wrapper_h = lv_obj_get_height(ctx->wrapper);

    lv_coord_t new_scroll_x = 0;
    lv_coord_t new_scroll_y = 0;
    if (keepCenter) {
        new_scroll_x = (lv_obj_get_scroll_x(ctx->wrapper) + wrapper_w / 2) * scale / oldScale - wrapper_w / 2;
        new_scroll_y = (lv_obj_get_scroll_y(ctx->wrapper) + wrapper_h / 2) * scale / oldScale - wrapper_h / 2;
    }

    lv_image_set_scale(ctx->imageObj, scale);
    lv_obj_set_size(ctx->imageWrapper, (ctx->width * scale) / 256, (ctx->height * scale) / 256);

    // Ensure we are aligned center visually when zoomed
    lv_obj_align(ctx->imageObj, LV_ALIGN_CENTER, 0, 0);

    lv_coord_t max_x = static_cast<lv_coord_t>((ctx->width * scale) / 256) - wrapper_w;
    lv_coord_t max_y = static_cast<lv_coord_t>((ctx->height * scale) / 256) - wrapper_h;

    if (max_x < 0) max_x = 0;
    if (max_y < 0) max_y = 0;

    if (new_scroll_x < 0) new_scroll_x = 0;
    if (new_scroll_x > max_x) new_scroll_x = max_x;
    if (new_scroll_y < 0) new_scroll_y = 0;
    if (new_scroll_y > max_y) new_scroll_y = max_y;

    lv_obj_scroll_to(ctx->wrapper, new_scroll_x, new_scroll_y, LV_ANIM_OFF);
}

void onKeyEvent(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    uint32_t key = lv_event_get_key(event);

    if (key == LV_KEY_ESC) {
        app_event_emit_close(ctx->appInstanceId);
    } else if (key == 0x20000) { // Media Play/Pause: check for a newer map
        ctx->refreshRequested = true;
    } else if (key == LV_KEY_ENTER) {
        if (ctx->imageData == nullptr) {
            return;
        }
        const int old_scale = ZOOM_SCALES[ctx->zoomLevel];
        ctx->zoomLevel++;
        if (ctx->zoomLevel > 2) ctx->zoomLevel = 0;
        applyZoom(ctx, old_scale, true);
    } else if (key == LV_KEY_UP || key == LV_KEY_DOWN || key == LV_KEY_LEFT || key == LV_KEY_RIGHT) {
        lv_coord_t dx = 0;
        lv_coord_t dy = 0;
        if (key == LV_KEY_UP) dy = -50;
        else if (key == LV_KEY_DOWN) dy = 50;
        else if (key == LV_KEY_LEFT) dx = -50;
        else if (key == LV_KEY_RIGHT) dx = 50;

        lv_coord_t scroll_x = lv_obj_get_scroll_x(ctx->wrapper) + dx;
        lv_coord_t scroll_y = lv_obj_get_scroll_y(ctx->wrapper) + dy;

        lv_coord_t max_x = lv_obj_get_width(ctx->imageWrapper) - lv_obj_get_width(ctx->wrapper);
        lv_coord_t max_y = lv_obj_get_height(ctx->imageWrapper) - lv_obj_get_height(ctx->wrapper);

        if (max_x < 0) max_x = 0;
        if (max_y < 0) max_y = 0;

        if (scroll_x < 0) scroll_x = 0;
        if (scroll_x > max_x) scroll_x = max_x;
        if (scroll_y < 0) scroll_y = 0;
        if (scroll_y > max_y) scroll_y = max_y;

        lv_obj_scroll_to(ctx->wrapper, scroll_x, scroll_y, LV_ANIM_ON);
    }
}

/** The window manager deletes the widget tree whenever this window is buried, while main() keeps running. */
void onWidgetsDeleted(lv_event_t* event) {
    auto* ctx = static_cast<Context*>(lv_event_get_user_data(event));
    ctx->wrapper = nullptr;
    ctx->imageWrapper = nullptr;
    ctx->imageObj = nullptr;
    ctx->statusLabel = nullptr;
}

void createWidgets(lv_obj_t* parent, void* userData) {
    auto* ctx = static_cast<Context*>(userData);

    auto* wrapper = lv_obj_create(parent);
    lv_obj_set_size(wrapper, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_border_width(wrapper, 0, 0);
    lv_obj_set_style_pad_all(wrapper, 0, 0);
    lv_obj_add_flag(wrapper, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_clear_flag(wrapper, LV_OBJ_FLAG_SCROLL_ELASTIC); // Stricter bounds clamping
    lv_obj_set_scrollbar_mode(wrapper, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_scroll_dir(wrapper, LV_DIR_ALL);

    ctx->wrapper = wrapper; // Just use root wrapper

    auto* image_wrapper = lv_obj_create(wrapper);
    ctx->imageWrapper = image_wrapper;

    const int scale = ZOOM_SCALES[ctx->zoomLevel];
    lv_obj_set_size(image_wrapper, (ctx->width * scale) / 256, (ctx->height * scale) / 256);
    lv_obj_set_style_border_width(image_wrapper, 0, 0);
    lv_obj_set_style_pad_all(image_wrapper, 0, 0);
    lv_obj_set_style_bg_opa(image_wrapper, LV_OPA_TRANSP, 0);
    lv_obj_clear_flag(image_wrapper, LV_OBJ_FLAG_SCROLLABLE);

    auto* image = lv_image_create(image_wrapper);
    ctx->imageObj = image;
    if (ctx->imageData != nullptr) {
        lv_image_set_src(image, &ctx->imageDsc);
    } else {
        lv_obj_add_flag(image, LV_OBJ_FLAG_HIDDEN);
    }
    lv_image_set_scale(image, scale);
    // Center the pivot for scaling, and initially center the image
    lv_obj_align(image, LV_ALIGN_CENTER, 0, 0);

    ctx->statusLabel = lv_label_create(parent);
    lv_obj_align(ctx->statusLabel, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_set_style_bg_color(ctx->statusLabel, lv_color_black(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(ctx->statusLabel, LV_OPA_70, LV_PART_MAIN);
    // The pill is a dark scrim over the map, not a themed surface, so its ink follows the scrim
    // rather than the theme: a light theme's dark text would be invisible on it.
    lv_obj_set_style_text_color(ctx->statusLabel, lvgl::getContrastingText(lv_color_black()), LV_PART_MAIN);
    lv_obj_set_style_pad_all(ctx->statusLabel, 4, LV_PART_MAIN);
    renderStatus(ctx);

    // Listen to keys
    lv_obj_add_flag(wrapper, LV_OBJ_FLAG_CLICKABLE);
    lv_group_add_obj(lv_group_get_default(), wrapper);
    lv_group_focus_obj(wrapper);
    lv_group_set_editing(lv_group_get_default(), true); // Force edit mode to receive arrow keys
    lv_obj_add_event_cb(wrapper, onKeyEvent, LV_EVENT_KEY, ctx);
    lv_obj_add_event_cb(wrapper, onWidgetsDeleted, LV_EVENT_DELETE, ctx);
}

std::string readStamp() {
    auto stamp = file::readString(MAP_STAMP_PATH);
    return stamp ? std::string(reinterpret_cast<const char*>(stamp.get())) : std::string();
}

void writeStamp(const std::string& lastModified) {
    if (lastModified.empty()) {
        return;
    }
    file::writeString(MAP_STAMP_PATH, lastModified);
}

/**
 * Asks for the map only if it changed, which the server answers with a 304 and no body. A
 * re-download is ~100 kB through a wear-levelled partition.
 */
void startDownload(const std::shared_ptr<Context>& ctx) {
    ctx->downloadState = DownloadState::Busy;
    lvgl_lock();
    setStatus(ctx.get(), "Checking for updates...");
    lvgl_unlock();

    network::http::downloadIfNewer(
        MAP_URL,
        "",
        MAP_DOWNLOAD_PATH,
        ctx->lastModified,
        [ctx](const std::string& lastModified) {
            ctx->lastModified = lastModified;
            ctx->downloadState = DownloadState::Succeeded;
        },
        [ctx] { ctx->downloadState = DownloadState::NotModified; },
        [ctx](const char* errorMessage) {
            LOG_E(TAG, "Download error: %s", errorMessage);
            ctx->downloadState = DownloadState::Failed;
        }
    );
}

/**
 * The radio is usually off, so a refresh turns it on and lets auto-connect reach a saved network.
 * Progress is polled from the app loop, so the map stays pannable.
 */
void startWifi(const std::shared_ptr<Context>& ctx) {
    ctx->downloadState = DownloadState::WaitingForWifi;
    ctx->wifiWaitedMs = 0;
    const auto radio = service::wifi::getRadioState();
    if (radio == service::wifi::RadioState::Off || radio == service::wifi::RadioState::OffPending) {
        service::wifi::setEnabled(true);
    }
    lvgl_lock();
    setStatus(ctx.get(), "Connecting to Wi-Fi...");
    lvgl_unlock();
}

/** @return true once the wait is over, one way or the other. */
bool pollWifi(const std::shared_ptr<Context>& ctx, int elapsedMs) {
    using service::wifi::RadioState;
    const auto state = service::wifi::getRadioState();
    if (state == RadioState::ConnectionActive) {
        startDownload(ctx);
        return true;
    }

    ctx->wifiWaitedMs += elapsedMs;
    if (ctx->wifiWaitedMs < WIFI_CONNECT_TIMEOUT_MS) {
        return false;
    }

    ctx->downloadState = DownloadState::Idle;
    lvgl_lock();
    setStatus(ctx.get(), "Could not connect to Wi-Fi");
    lvgl_unlock();
    return true;
}

/** Swaps in the freshly downloaded map. Frees the old image first so the two never coexist in PSRAM. */
void applyDownload(Context* ctx) {
    lvgl_lock();
    setStatus(ctx, "Loading map...");
    if (ctx->imageObj != nullptr) {
        lv_obj_add_flag(ctx->imageObj, LV_OBJ_FLAG_HIDDEN);
        lv_image_cache_drop(&ctx->imageDsc);
    }
    lvgl_unlock();

    freeImage(ctx);

    // Only replace the installed map once the download is known to decode; a truncated one must not destroy it.
    bool loaded = loadImage(ctx, MAP_DOWNLOAD_PATH);
    if (loaded) {
        // FatFs rename() fails with EEXIST rather than replacing; safe to unlink first now that the decode succeeded.
        remove(MAP_PATH);
        if (rename(MAP_DOWNLOAD_PATH, MAP_PATH) != 0) {
            LOG_E(TAG, "Failed to replace %s", MAP_PATH);
        }
        writeStamp(ctx->lastModified);
    } else {
        remove(MAP_DOWNLOAD_PATH);
        loaded = loadImage(ctx, MAP_PATH);
    }

    lvgl_lock();
    if (loaded) {
        ctx->zoomLevel = 0;
        if (ctx->imageObj != nullptr) {
            lv_image_set_src(ctx->imageObj, &ctx->imageDsc);
            lv_obj_remove_flag(ctx->imageObj, LV_OBJ_FLAG_HIDDEN);
        }
        applyZoom(ctx, ZOOM_SCALES[0], false);
        setStatus(ctx, nullptr);
    } else {
        setStatus(ctx, "Map is unreadable");
    }
    lvgl_unlock();
}

int32_t appMain(int argc, char* argv[]) {
    uint32_t appInstanceId = app_scheduler_current_app_id();
    auto ctx = std::make_shared<Context>();
    ctx->appInstanceId = appInstanceId;

    // Decoding blocks for seconds; deferred to the loop so the window is up first and the app does
    // not look frozen. The status is what the user sees until it lands.
    ctx->statusText = "Loading map...";
    ctx->lastModified = readStamp();

    TaskEventGroup event_group {};
    task_event_group_construct(&event_group);

    AppEventSubscription sub {};
    app_event_subscribe(&sub, &event_group);

    // Play/pause is this app's refresh key, so playback must not take it while the map is up.
    service::music::claimMediaKeys(appInstanceId);

    WindowId window = window_manager_create(appInstanceId, createWidgets, ctx.get());

    bool shouldClose = false;
    while (!shouldClose) {
        // No dedicated wake-up bit exists for refreshRequested/downloadState, which are set from
        // LVGL's key handler and the download's dispatcher-thread callbacks, so those are polled.
        task_event_group_wait_any(&event_group, nullptr, pdMS_TO_TICKS(100));

        AppEvent event {};
        while (app_event_poll(&sub, &event) == ERROR_NONE) {
            if (event.type == APP_EVENT_CLOSE) {
                shouldClose = true;
                break;
            }
        }
        if (shouldClose) {
            continue;
        }

        if (ctx->decodePending) {
            ctx->decodePending = false;
            const bool loaded = loadImage(ctx.get(), MAP_PATH);
            lvgl_lock();
            if (loaded) {
                if (ctx->imageObj != nullptr) {
                    lv_image_set_src(ctx->imageObj, &ctx->imageDsc);
                    lv_obj_remove_flag(ctx->imageObj, LV_OBJ_FLAG_HIDDEN);
                }
                applyZoom(ctx.get(), ZOOM_SCALES[ctx->zoomLevel], false);
                setStatus(ctx.get(), "Press Play to check for updates");
            } else {
                // Without a map the stamp cannot be trusted: sending it would earn a 304 and
                // leave the app with nothing to show.
                ctx->lastModified.clear();
                setStatus(ctx.get(), "No map yet - press Play to download");
            }
            lvgl_unlock();
            continue;
        }

        if (ctx->downloadState == DownloadState::WaitingForWifi) {
            pollWifi(ctx, 100);
            continue;
        }

        if (ctx->refreshRequested.exchange(false) && ctx->downloadState == DownloadState::Idle) {
            startWifi(ctx);
        } else if (ctx->downloadState == DownloadState::NotModified) {
            ctx->downloadState = DownloadState::Idle;
            lvgl_lock();
            setStatus(ctx.get(), "Map is already up to date");
            lvgl_unlock();
        } else if (ctx->downloadState == DownloadState::Succeeded) {
            ctx->downloadState = DownloadState::Idle;
            applyDownload(ctx.get());
        } else if (ctx->downloadState == DownloadState::Failed) {
            ctx->downloadState = DownloadState::Idle;
            lvgl_lock();
            setStatus(ctx.get(), "Download failed");
            lvgl_unlock();
        }
    }

    service::music::releaseMediaKeys(appInstanceId);
    window_manager_remove(window);
    app_event_unsubscribe(&sub);
    task_event_group_destruct(&event_group);
    return 0;
}

} // namespace

extern const ::AppManifest manifest = {
    .id = "VenueMap",
    .name = "Venue Map",
    .category = APP_CATEGORY_USER,
    .location = { APP_LOCATION_MEMORY, reinterpret_cast<void*>(appMain) },
    .flags = 0,
};

} // namespace tt::app::map
