// SPDX-License-Identifier: Apache-2.0
#include <soc/soc_caps.h>
#if SOC_LCD_RGB_SUPPORTED

#include <drivers/software_pixel_mapper.h>

#include <esp_heap_caps.h>

// RGB332 packs each pixel into one byte: 3 bits red, 3 bits green, 2 bits blue. The byte layout
// maps straight onto an 8-data-line panel's significant color inputs (R7..R5, G7..G5, B7..B6),
// so each RGB565 channel's top bits land on the corresponding MSB lines
static void rgb332_map(SoftwarePixelMapperData data, const uint16_t* src, uint8_t* dst, uint32_t pixel_count) {
    (void)data;
    for (uint32_t i = 0; i < pixel_count; i++) {
        uint16_t px = src[i];
        dst[i] = (uint8_t)(((px >> 13) & 0x07) << 5) | (((px >> 8) & 0x07) << 2) | ((px >> 3) & 0x03);
    }
}

// The destination buffer must hold a whole frame (1 byte/pixel for RGB332). A 1024x600 panel
// needs ~600KB, which only fits in PSRAM on most boards, prefer SPIRAM and fall back to whatever
// internal RAM is available. The returned handle is this buffer, passed back as map()'s dst.
static SoftwarePixelMapperData rgb332_create(uint16_t width, uint16_t height) {
    size_t buffer_size = (size_t)width * height;
    void* buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == nullptr) {
        buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DEFAULT);
    }
    return buffer;
}

static void rgb332_destroy(SoftwarePixelMapperData data) {
    heap_caps_free(data);
}

const struct SoftwarePixelMapper software_pixel_mapper_rgb332 = {
    .create = rgb332_create,
    .map = rgb332_map,
    .destroy = rgb332_destroy,
};

// ILI9341 6-bit RGB, 3 transfers/pixel (RIM=1, COLMOD DPI=110): R[5:0], G[5:0], B[5:0] sent one
// per DOTCLK over D[5:0], datasheet 7.6.7. The top 2 bits of each byte are not wired.
static void rgb666_6bit_triple_map(SoftwarePixelMapperData data, const uint16_t* src, uint8_t* dst, uint32_t pixel_count) {
    (void)data;
    for (uint32_t i = 0; i < pixel_count; i++) {
        uint16_t px = src[i];
        uint8_t r6 = (uint8_t)(((px >> 11) & 0x1F) << 1);
        uint8_t g6 = (uint8_t)((px >> 5) & 0x3F);
        uint8_t b6 = (uint8_t)((px & 0x1F) << 1);
        dst[i * 3 + 0] = r6;
        dst[i * 3 + 1] = g6;
        dst[i * 3 + 2] = b6;
    }
}

// 3 bytes/pixel destination buffer. Same PSRAM-preferred allocation strategy as rgb332_create.
static SoftwarePixelMapperData rgb666_6bit_triple_create(uint16_t width, uint16_t height) {
    size_t buffer_size = (size_t)width * height * 3;
    void* buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (buffer == nullptr) {
        buffer = heap_caps_malloc(buffer_size, MALLOC_CAP_DEFAULT);
    }
    return buffer;
}

static void rgb666_6bit_triple_destroy(SoftwarePixelMapperData data) {
    heap_caps_free(data);
}

const struct SoftwarePixelMapper software_pixel_mapper_rgb666_6bit_triple = {
    .create = rgb666_6bit_triple_create,
    .map = rgb666_6bit_triple_map,
    .destroy = rgb666_6bit_triple_destroy,
};

#endif // SOC_LCD_RGB_SUPPORTED
