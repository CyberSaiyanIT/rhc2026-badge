// SPDX-License-Identifier: Apache-2.0
#include <lvgl/thumbnail.h>

#include <lvgl/lvgl.h>
// The decoder session and its arguments are only declared in full here, and opening a
// decode by hand is the point of this file.
#include <src/draw/lv_image_decoder_private.h>
#include <tactility/log.h>

#include <algorithm>
#include <cstdlib>

#define TAG "lvgl_thumbnail"

namespace {

/** One destination pixel averages the source pixels it covers, so a large reduction does not alias. */
struct Accumulator {
    uint32_t r = 0;
    uint32_t g = 0;
    uint32_t b = 0;
    uint32_t count = 0;
};

bool read_pixel(const lv_draw_buf_t* source, int32_t x, int32_t y, Accumulator* out) {
    const uint8_t* pixel = (const uint8_t*) lv_draw_buf_goto_xy(source, (uint32_t) x, (uint32_t) y);
    if (pixel == nullptr) {
        return false;
    }

    switch (source->header.cf) {
        case LV_COLOR_FORMAT_RGB565: {
            const uint16_t value = (uint16_t) (pixel[0] | (pixel[1] << 8));
            // Expanded to eight bits by repeating the high bits, so full scale stays full scale.
            out->r += (uint32_t) (((value >> 11) & 0x1f) * 255 / 31);
            out->g += (uint32_t) (((value >> 5) & 0x3f) * 255 / 63);
            out->b += (uint32_t) ((value & 0x1f) * 255 / 31);
            break;
        }
        case LV_COLOR_FORMAT_RGB888:
        case LV_COLOR_FORMAT_XRGB8888:
        case LV_COLOR_FORMAT_ARGB8888:
            // All three are stored blue first.
            out->b += pixel[0];
            out->g += pixel[1];
            out->r += pixel[2];
            break;
        default:
            return false;
    }

    out->count++;
    return true;
}

/** Fits @a sourceWidth by @a sourceHeight inside the box without enlarging it or losing its shape. */
void fit(int32_t sourceWidth, int32_t sourceHeight, int32_t maxWidth, int32_t maxHeight,
         int32_t* outWidth, int32_t* outHeight) {
    if (sourceWidth <= maxWidth && sourceHeight <= maxHeight) {
        *outWidth = sourceWidth;
        *outHeight = sourceHeight;
        return;
    }

    int32_t width = maxWidth;
    int32_t height = (int32_t) ((int64_t) sourceHeight * maxWidth / sourceWidth);
    if (height > maxHeight) {
        height = maxHeight;
        width = (int32_t) ((int64_t) sourceWidth * maxHeight / sourceHeight);
    }

    *outWidth = std::max<int32_t>(width, 1);
    *outHeight = std::max<int32_t>(height, 1);
}

struct Reduction {
    int32_t sourceWidth;
    int32_t sourceHeight;
    int32_t width;
    int32_t height;
};

/**
 * Adds every pixel of one decoded piece into the destination cell it falls in. Mapped forwards: a
 * block-at-a-time decoder cannot say which source pixels a destination pixel covers.
 *
 * @param originX where this piece sits in the full image
 */
void accumulate(const lv_draw_buf_t* piece, int32_t originX, int32_t originY,
                const Reduction& reduction, Accumulator* cells, bool* readable) {
    const int32_t piece_width = (int32_t) piece->header.w;
    const int32_t piece_height = (int32_t) piece->header.h;

    for (int32_t y = 0; y < piece_height; y++) {
        const int32_t source_y = originY + y;
        if (source_y < 0 || source_y >= reduction.sourceHeight) {
            continue;
        }
        const int32_t cell_y = (int32_t) ((int64_t) source_y * reduction.height / reduction.sourceHeight);

        for (int32_t x = 0; x < piece_width; x++) {
            const int32_t source_x = originX + x;
            if (source_x < 0 || source_x >= reduction.sourceWidth) {
                continue;
            }
            const int32_t cell_x = (int32_t) ((int64_t) source_x * reduction.width / reduction.sourceWidth);

            if (!read_pixel(piece, x, y, &cells[cell_y * reduction.width + cell_x])) {
                *readable = false;
                return;
            }
        }
    }
}

} // namespace

struct LvglThumbnail {
    lv_draw_buf_t* buffer;
};

namespace {

// Takes whatever lv_image_decoder_open() accepts: a path, or an lv_image_dsc_t holding encoded
// bytes. @a label only names the source in a log line.
LvglThumbnail* create_from_source(const void* source, const char* label,
                                 int32_t maxWidth, int32_t maxHeight) {
    const char* path = label;

    lv_image_decoder_dsc_t decoder {};
    lv_image_decoder_args_t args {};
    // The original is wanted once and then thrown away, so there is no point offering it to a
    // cache that is sized to nothing on this build anyway.
    args.no_cache = true;

    if (lv_image_decoder_open(&decoder, source, &args) != LV_RESULT_OK) {
        LOG_W(TAG, "Cannot decode %s", path);
        return nullptr;
    }

    const int32_t source_width = (int32_t) decoder.header.w;
    const int32_t source_height = (int32_t) decoder.header.h;
    if (source_width <= 0 || source_height <= 0) {
        lv_image_decoder_close(&decoder);
        LOG_W(TAG, "Nothing decoded from %s", path);
        return nullptr;
    }

    int32_t width = 0;
    int32_t height = 0;
    fit(source_width, source_height, maxWidth, maxHeight, &width, &height);

    auto* cells = (Accumulator*) lv_malloc_zeroed((size_t) width * (size_t) height * sizeof(Accumulator));
    if (cells == nullptr) {
        lv_image_decoder_close(&decoder);
        LOG_W(TAG, "No memory to scale %s", path);
        return nullptr;
    }

    const Reduction reduction { source_width, source_height, width, height };
    bool readable = true;

    if (decoder.decoded != nullptr) {
        // A decoder that hands over the whole image at once, which is what the PNG one does.
        accumulate(decoder.decoded, 0, 0, reduction, cells, &readable);
    } else {
        /*
         * The JPEG decoder returns one MCU block per call, each with its own block-local stride.
         * Following that is why a large cover never exists in memory at full size.
         */
        lv_area_t wanted = { 0, 0, source_width - 1, source_height - 1 };
        lv_area_t piece = { LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN, LV_COORD_MIN };
        while (readable && lv_image_decoder_get_area(&decoder, &wanted, &piece) == LV_RESULT_OK) {
            if (decoder.decoded == nullptr) {
                break;
            }
            accumulate(decoder.decoded, piece.x1, piece.y1, reduction, cells, &readable);
        }
    }

    lv_image_decoder_close(&decoder);

    if (!readable) {
        LOG_W(TAG, "Unsupported colour format in %s", path);
        lv_free(cells);
        return nullptr;
    }

    // RGB565 to match the display, which is what the blit wants and half the size of RGB888.
    lv_draw_buf_t* scaled = lv_draw_buf_create((uint32_t) width, (uint32_t) height,
        LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (scaled == nullptr) {
        LOG_W(TAG, "No memory for a %ldx%ld thumbnail", (long) width, (long) height);
        lv_free(cells);
        return nullptr;
    }

    for (int32_t y = 0; y < height; y++) {
        auto* row = (uint16_t*) lv_draw_buf_goto_xy(scaled, 0, (uint32_t) y);
        for (int32_t x = 0; x < width; x++) {
            const Accumulator& cell = cells[y * width + x];
            // A cell with no samples only happens if the decoder skipped a piece; black is a
            // better answer there than reading an uninitialised average.
            const uint32_t count = cell.count > 0 ? cell.count : 1;
            const uint32_t r = cell.r / count;
            const uint32_t g = cell.g / count;
            const uint32_t b = cell.b / count;
            row[x] = (uint16_t) (((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3));
        }
    }

    lv_free(cells);

    auto* thumbnail = (LvglThumbnail*) malloc(sizeof(LvglThumbnail));
    if (thumbnail == nullptr) {
        lv_draw_buf_destroy(scaled);
        return nullptr;
    }
    thumbnail->buffer = scaled;
    return thumbnail;
}

} // namespace

extern "C" {

LvglThumbnail* lvgl_thumbnail_create(const char* path, int32_t maxWidth, int32_t maxHeight) {
    if (path == nullptr || maxWidth <= 0 || maxHeight <= 0) {
        return nullptr;
    }
    return create_from_source(path, path, maxWidth, maxHeight);
}

LvglThumbnail* lvgl_thumbnail_create_from_memory(const void* data, size_t size,
                                                 int32_t maxWidth, int32_t maxHeight) {
    if (data == nullptr || size == 0 || maxWidth <= 0 || maxHeight <= 0) {
        return nullptr;
    }

    // RAW is what the decoders look for to mean "still encoded"; each sniffs the format itself.
    lv_image_dsc_t described {};
    described.header.magic = LV_IMAGE_HEADER_MAGIC;
    described.header.cf = LV_COLOR_FORMAT_RAW;
    described.data = (const uint8_t*) data;
    described.data_size = (uint32_t) size;

    // Safe to pass a local: the decode finishes before this returns.
    return create_from_source(&described, "embedded art", maxWidth, maxHeight);
}

void lvgl_thumbnail_destroy(LvglThumbnail* thumbnail) {
    if (thumbnail == nullptr) {
        return;
    }
    lv_draw_buf_destroy(thumbnail->buffer);
    free(thumbnail);
}

const void* lvgl_thumbnail_image_source(const LvglThumbnail* thumbnail) {
    // lv_image_dsc_t is laid out to match lv_draw_buf_t, which is what lets a decoded buffer be
    // handed to a widget as an image source directly.
    return thumbnail != nullptr ? (const void*) thumbnail->buffer : nullptr;
}

}
