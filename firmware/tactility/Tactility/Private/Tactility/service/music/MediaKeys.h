#pragma once

#include <cstdint>

namespace tt::service::music {

/** What a media key press asked for, queued for the service's own thread to carry out. */
enum class MediaCommand : uint8_t { None, PlayPause, Next, Previous };

/**
 * Installs the key filter that makes the media keys work from wherever the user is. It runs on the
 * LVGL task, so it only leaves a command behind rather than blocking the interface on a pipeline.
 */
void installMediaKeys();
void removeMediaKeys();

/**
 * Whether the keys do anything yet. Set once a track has been started, so a badge that has never
 * played anything does not begin playing because a key was brushed in the launcher.
 */
void setMediaKeysEnabled(bool enabled);

/** @return the queued command, and clears it */
MediaCommand takeMediaCommand();

} // namespace tt::service::music
