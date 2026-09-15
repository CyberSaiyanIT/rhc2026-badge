#include <Tactility/service/music/TrackTags.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <vector>

#include <esp_heap_caps.h>

namespace tt::service::music {

namespace {

constexpr auto* UNKNOWN = "Unknown Artist";

/**
 * How much of an ID3v2 tag is read looking for the artist. Kept small: this runs with the service
 * mutex held, and anything larger is cover art, which sits after the text frames.
 */
constexpr size_t MAX_TAG_BYTES = 32 * 1024;
constexpr size_t MAX_VALUE_BYTES = 128;
// Deep enough for moov/udta/meta/ilst/©ART/data without letting a malformed file recurse away.
constexpr int MAX_ATOM_DEPTH = 6;

/**
 * The largest picture worth lifting out of a tag. Cover art runs to hundreds of kilobytes, so this
 * far exceeds MAX_TAG_BYTES and the buffer comes from external RAM.
 */
constexpr size_t MAX_COVER_BYTES = 2u * 1024u * 1024u;

uint32_t be32(const uint8_t* data) {
    return ((uint32_t) data[0] << 24) | ((uint32_t) data[1] << 16) | ((uint32_t) data[2] << 8) | data[3];
}

/** ID3v2 stores sizes seven bits per byte so a size can never contain a frame sync. */
uint32_t syncsafe32(const uint8_t* data) {
    return ((uint32_t) (data[0] & 0x7f) << 21) | ((uint32_t) (data[1] & 0x7f) << 14) |
        ((uint32_t) (data[2] & 0x7f) << 7) | (uint32_t) (data[3] & 0x7f);
}

std::string trimmed(std::string value) {
    // Fixed-width tags are space or NUL padded, and some writers leave a trailing newline.
    const auto first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return {};
    }
    const auto last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

/**
 * Turns one ID3 text frame's payload into a string. UTF-16 is folded by dropping each unit's high
 * byte, so names outside Latin-1 come out mangled but still group and display.
 */
std::string decodeTextFrame(const uint8_t* data, size_t length) {
    if (length == 0) {
        return {};
    }
    const uint8_t encoding = data[0];
    const uint8_t* text = data + 1;
    size_t text_length = length - 1;

    if (encoding == 0 || encoding == 3) {
        return trimmed(std::string((const char*) text, strnlen((const char*) text, text_length)));
    }

    bool little_endian = true;
    if (encoding == 1 && text_length >= 2) {
        little_endian = !(text[0] == 0xfe && text[1] == 0xff);
        if ((text[0] == 0xff && text[1] == 0xfe) || (text[0] == 0xfe && text[1] == 0xff)) {
            text += 2;
            text_length -= 2;
        }
    } else if (encoding == 2) {
        little_endian = false;
    }

    std::string out;
    for (size_t index = 0; index + 1 < text_length; index += 2) {
        const uint8_t byte = little_endian ? text[index] : text[index + 1];
        if (byte == 0) {
            break;
        }
        out.push_back((char) byte);
    }
    return trimmed(out);
}

std::string readId3v2(FILE* file, bool& outFound) {
    uint8_t header[10];
    if (fread(header, 1, sizeof(header), file) != sizeof(header) || memcmp(header, "ID3", 3) != 0) {
        return {};
    }
    outFound = true;

    const uint8_t major = header[3];
    const size_t tag_size = std::min<size_t>(syncsafe32(header + 6), MAX_TAG_BYTES);
    if (tag_size == 0) {
        return {};
    }

    // An extended header cannot be skipped without parsing it, so a tag carrying one fails to
    // match and falls through to ID3v1 rather than reading a frame id out of its middle.
    if (header[5] & 0x40) {
        return {};
    }

    std::vector<uint8_t> tag(tag_size);
    if (fread(tag.data(), 1, tag_size, file) != tag_size) {
        return {};
    }

    const size_t id_length = major <= 2 ? 3 : 4;
    const size_t header_length = major <= 2 ? 6 : 10;
    const char* wanted = major <= 2 ? "TP1" : "TPE1";

    size_t offset = 0;
    while (offset + header_length <= tag_size) {
        // Padding: the rest of the tag is zeroed out to leave room for later edits.
        if (tag[offset] == 0) {
            break;
        }

        size_t frame_size;
        if (major <= 2) {
            frame_size = ((size_t) tag[offset + 3] << 16) | ((size_t) tag[offset + 4] << 8) | tag[offset + 5];
        } else if (major == 4) {
            frame_size = syncsafe32(&tag[offset + 4]);
        } else {
            frame_size = be32(&tag[offset + 4]);
        }

        if (frame_size == 0 || offset + header_length + frame_size > tag_size) {
            break;
        }

        if (memcmp(&tag[offset], wanted, id_length) == 0) {
            return decodeTextFrame(&tag[offset + header_length], std::min(frame_size, MAX_VALUE_BYTES));
        }
        offset += header_length + frame_size;
    }
    return {};
}

uint8_t* allocate_cover(size_t size) {
    return (uint8_t*) heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/**
 * Finds where the image starts in an APIC payload: encoding byte, null-terminated MIME type,
 * picture type byte, description terminated per the encoding, then the image.
 *
 * @param major the ID3v2 major version, which decides whether the type is a 3-character code
 * @return 0 when the payload is malformed
 */
size_t cover_image_offset(const uint8_t* payload, size_t size, uint8_t major) {
    if (size < 4) {
        return 0;
    }
    const uint8_t encoding = payload[0];
    size_t offset;

    if (major <= 2) {
        // A three character format code, "JPG" or "PNG", rather than a MIME type.
        offset = 4;
    } else {
        const void* terminator = memchr(payload + 1, 0, size - 1);
        if (terminator == nullptr) {
            return 0;
        }
        offset = (size_t) ((const uint8_t*) terminator - payload) + 1;
    }

    offset += 1; // picture type
    if (offset >= size) {
        return 0;
    }

    if (encoding == 1 || encoding == 2) {
        // Two bytes per character, so the terminator is a pair of them on an even boundary.
        while (offset + 1 < size && !(payload[offset] == 0 && payload[offset + 1] == 0)) {
            offset += 2;
        }
        offset += 2;
    } else {
        const void* terminator = memchr(payload + offset, 0, size - offset);
        if (terminator == nullptr) {
            return 0;
        }
        offset = (size_t) ((const uint8_t*) terminator - payload) + 1;
    }

    return offset < size ? offset : 0;
}

CoverArt readId3v2Cover(FILE* file) {
    uint8_t header[10];
    if (fseek(file, 0, SEEK_SET) != 0 ||
        fread(header, 1, sizeof(header), file) != sizeof(header) ||
        memcmp(header, "ID3", 3) != 0) {
        return {};
    }

    const uint8_t major = header[3];
    const uint8_t flags = header[5];
    const long tag_end = 10 + (long) syncsafe32(header + 6);

    // Unsynchronisation rewrites the tag body and cannot be undone while seeking header by
    // header. Rare enough to give up on, and the sibling cover file is still tried.
    if ((flags & 0x80) != 0 || (flags & 0x40) != 0) {
        return {};
    }

    const size_t id_length = major <= 2 ? 3 : 4;
    const size_t header_length = major <= 2 ? 6 : 10;
    const char* wanted = major <= 2 ? "PIC" : "APIC";

    long offset = 10;
    while (offset + (long) header_length <= tag_end) {
        uint8_t frame[10];
        if (fseek(file, offset, SEEK_SET) != 0 ||
            fread(frame, 1, header_length, file) != header_length) {
            return {};
        }
        // Padding: the rest of the tag is zeroed to leave room for later edits.
        if (frame[0] == 0) {
            return {};
        }

        size_t frame_size;
        if (major <= 2) {
            frame_size = ((size_t) frame[3] << 16) | ((size_t) frame[4] << 8) | frame[5];
        } else if (major == 4) {
            frame_size = syncsafe32(&frame[4]);
        } else {
            frame_size = be32(&frame[4]);
        }

        if (frame_size == 0 || offset + (long) header_length + (long) frame_size > tag_end) {
            return {};
        }

        if (memcmp(frame, wanted, id_length) == 0) {
            if (frame_size > MAX_COVER_BYTES) {
                return {};
            }
            uint8_t* payload = allocate_cover(frame_size);
            if (payload == nullptr) {
                return {};
            }
            if (fread(payload, 1, frame_size, file) != frame_size) {
                heap_caps_free(payload);
                return {};
            }

            const size_t image_offset = cover_image_offset(payload, frame_size, major);
            if (image_offset == 0) {
                heap_caps_free(payload);
                return {};
            }
            // Moved to the front so the caller owns one buffer that is exactly the image.
            const size_t image_size = frame_size - image_offset;
            memmove(payload, payload + image_offset, image_size);
            return CoverArt { payload, image_size };
        }

        offset += (long) header_length + (long) frame_size;
    }
    return {};
}

/** Walks the atom tree for the \c covr atom, mirroring findArtistAtom(). */
CoverArt findCoverAtom(FILE* file, long offset, long end, int depth) {
    if (depth > MAX_ATOM_DEPTH) {
        return {};
    }

    while (offset + 8 <= end) {
        uint8_t header[8];
        if (fseek(file, offset, SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header)) {
            return {};
        }
        long size = (long) be32(header);
        if (size == 0) {
            size = end - offset;
        }
        if (size < 8 || offset + size > end) {
            return {};
        }

        const char* type = (const char*) header + 4;
        const bool container = memcmp(type, "moov", 4) == 0 || memcmp(type, "udta", 4) == 0 ||
            memcmp(type, "ilst", 4) == 0 || memcmp(type, "meta", 4) == 0;

        if (container) {
            const long child = offset + 8 + (memcmp(type, "meta", 4) == 0 ? 4 : 0);
            auto cover = findCoverAtom(file, child, offset + size, depth + 1);
            if (cover.data != nullptr) {
                return cover;
            }
        } else if (memcmp(type, "covr", 4) == 0) {
            uint8_t data_header[16];
            if (fread(data_header, 1, sizeof(data_header), file) == sizeof(data_header) &&
                memcmp(data_header + 4, "data", 4) == 0) {
                const long payload = size - 8 - 16;
                if (payload > 0 && (size_t) payload <= MAX_COVER_BYTES) {
                    uint8_t* image = allocate_cover((size_t) payload);
                    if (image != nullptr) {
                        if (fread(image, 1, (size_t) payload, file) == (size_t) payload) {
                            return CoverArt { image, (size_t) payload };
                        }
                        heap_caps_free(image);
                    }
                }
            }
            return {};
        }

        offset += size;
    }
    return {};
}

std::string readId3v1(FILE* file) {
    if (fseek(file, -128, SEEK_END) != 0) {
        return {};
    }
    uint8_t tag[128];
    if (fread(tag, 1, sizeof(tag), file) != sizeof(tag) || memcmp(tag, "TAG", 3) != 0) {
        return {};
    }
    return trimmed(std::string((const char*) tag + 33, strnlen((const char*) tag + 33, 30)));
}

/**
 * Walks one level of an MPEG-4 atom tree looking for the artist.
 *
 * @param end offset one past the last byte belonging to the parent atom
 */
std::string findArtistAtom(FILE* file, long offset, long end, int depth) {
    if (depth > MAX_ATOM_DEPTH) {
        return {};
    }

    while (offset + 8 <= end) {
        uint8_t header[8];
        if (fseek(file, offset, SEEK_SET) != 0 || fread(header, 1, sizeof(header), file) != sizeof(header)) {
            return {};
        }
        long size = (long) be32(header);
        // 0 means "to the end of the parent"; 1 means a 64-bit size follows, which no tag needs.
        if (size == 0) {
            size = end - offset;
        }
        if (size < 8 || offset + size > end) {
            return {};
        }

        const char* type = (const char*) header + 4;
        // The four containers on the way down, plus meta, which carries four bytes of version and
        // flags before its children start.
        const bool container = memcmp(type, "moov", 4) == 0 || memcmp(type, "udta", 4) == 0 ||
            memcmp(type, "ilst", 4) == 0 || memcmp(type, "meta", 4) == 0;

        if (container) {
            const long child = offset + 8 + (memcmp(type, "meta", 4) == 0 ? 4 : 0);
            auto artist = findArtistAtom(file, child, offset + size, depth + 1);
            if (!artist.empty()) {
                return artist;
            }
        } else if (memcmp(type + 1, "ART", 3) == 0 && (uint8_t) type[0] == 0xa9) {
            // The value lives in a data atom: size, "data", version and flags, locale, payload.
            uint8_t data_header[16];
            if (fread(data_header, 1, sizeof(data_header), file) == sizeof(data_header) &&
                memcmp(data_header + 4, "data", 4) == 0) {
                const long payload = size - 8 - 16;
                if (payload > 0) {
                    std::string value((size_t) std::min<long>(payload, MAX_VALUE_BYTES), '\0');
                    if (fread(value.data(), 1, value.size(), file) == value.size()) {
                        return trimmed(value);
                    }
                }
            }
        }

        offset += size;
    }
    return {};
}

std::string readMp4(FILE* file) {
    if (fseek(file, 0, SEEK_END) != 0) {
        return {};
    }
    const long end = ftell(file);
    return end <= 0 ? std::string {} : findArtistAtom(file, 0, end, 0);
}

bool hasExtension(const std::string& path, const char* extension) {
    const auto length = strlen(extension);
    return path.size() > length && strcasecmp(path.c_str() + path.size() - length, extension) == 0;
}

} // namespace

std::string readArtist(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return UNKNOWN;
    }

    std::string artist;
    if (hasExtension(path, ".mp3")) {
        bool had_id3v2 = false;
        artist = readId3v2(file, had_id3v2);
        // Only worth seeking to the far end when the front carried no tag at all, since this runs
        // with the service locked and an ID3v2 file rarely has a useful ID3v1 tag too.
        if (artist.empty() && !had_id3v2) {
            artist = readId3v1(file);
        }
    } else {
        artist = readMp4(file);
    }

    fclose(file);
    return artist.empty() ? UNKNOWN : artist;
}

CoverArt readCoverArt(const std::string& path) {
    FILE* file = fopen(path.c_str(), "rb");
    if (file == nullptr) {
        return {};
    }

    CoverArt cover {};
    if (hasExtension(path, ".mp3")) {
        cover = readId3v2Cover(file);
    } else if (fseek(file, 0, SEEK_END) == 0) {
        const long end = ftell(file);
        if (end > 0) {
            cover = findCoverAtom(file, 0, end, 0);
        }
    }

    fclose(file);
    return cover;
}

void releaseCoverArt(CoverArt* art) {
    if (art == nullptr || art->data == nullptr) {
        return;
    }
    heap_caps_free(art->data);
    art->data = nullptr;
    art->size = 0;
}

} // namespace tt::service::music
