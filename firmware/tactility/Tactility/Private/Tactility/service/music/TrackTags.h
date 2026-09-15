#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace tt::service::music {

/**
 * Reads the performer from a track's own metadata: an ID3v2 TPE1 frame, an ID3v1 tag, or an
 * MPEG-4 \c ©ART atom.
 *
 * @return the artist, or "Unknown Artist" when the file carries no usable tag
 */
std::string readArtist(const std::string& path);

/** Encoded image bytes lifted out of a track's tags, held in external RAM. */
struct CoverArt {
    uint8_t* data = nullptr;
    size_t size = 0;
};

/**
 * Reads the picture a track carries in its own tags: an ID3v2 \c APIC frame or an MPEG-4
 * \c covr atom.
 *
 * The bytes come back still encoded, as the JPEG or PNG the tag holds, for a decoder to deal with.
 *
 * @return empty when the track has no picture, or one too large or awkwardly stored to read
 */
CoverArt readCoverArt(const std::string& path);

void releaseCoverArt(CoverArt* art);

} // namespace tt::service::music
