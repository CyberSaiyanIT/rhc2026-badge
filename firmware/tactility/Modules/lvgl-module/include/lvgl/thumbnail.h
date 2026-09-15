#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * A decoded, downscaled copy of an image file, held in external RAM. LVGL re-runs the whole decode
 * at full resolution on every invalidation, so decoding once turns each later redraw into a blit.
 */
typedef struct LvglThumbnail LvglThumbnail;

/**
 * Decodes @a path to fit within @a maxWidth by @a maxHeight, preserving aspect ratio and never
 * enlarging. Decodes at full resolution on the calling task, so never call it from a timer.
 *
 * @param path an LVGL path including the drive letter, e.g. "A:/sdcard/Music/cover.jpg"
 * @return null when the file is missing, its format cannot be decoded, or memory ran out; never a
 *     reason to fail the caller, which should fall back to a placeholder
 */
LvglThumbnail* lvgl_thumbnail_create(const char* path, int32_t maxWidth, int32_t maxHeight);

/**
 * The same, for an image already in memory rather than on disk, such as one lifted out of a
 * track's tags. @a data is only read for the duration of the call.
 *
 * @param data the encoded image, still a JPEG or PNG rather than pixels
 */
LvglThumbnail* lvgl_thumbnail_create_from_memory(const void* data, size_t size,
                                                 int32_t maxWidth, int32_t maxHeight);

void lvgl_thumbnail_destroy(LvglThumbnail* thumbnail);

/**
 * @return what to hand lv_image_set_src(), valid until lvgl_thumbnail_destroy()
 *
 * \live The widget keeps this pointer rather than copying it, so the thumbnail has to outlive the
 *     widget, or the widget's source has to be replaced before the thumbnail is destroyed.
 */
const void* lvgl_thumbnail_image_source(const LvglThumbnail* thumbnail);

#ifdef __cplusplus
}
#endif
