// SPDX-License-Identifier: Apache-2.0
#include <drivers/ndef.h>

#include <string.h>

namespace {

// TLV tags in a Type 2 tag's data area.
constexpr uint8_t TLV_NULL = 0x00;
constexpr uint8_t TLV_LOCK_CONTROL = 0x01;
constexpr uint8_t TLV_MEMORY_CONTROL = 0x02;
constexpr uint8_t TLV_NDEF_MESSAGE = 0x03;
constexpr uint8_t TLV_PROPRIETARY = 0xFD;
constexpr uint8_t TLV_TERMINATOR = 0xFE;

// NDEF record header bits.
constexpr uint8_t RECORD_FLAG_SHORT = 0x10;
constexpr uint8_t RECORD_FLAG_ID_LENGTH = 0x08;
constexpr uint8_t RECORD_TNF_MASK = 0x07;
constexpr uint8_t TNF_WELL_KNOWN = 0x01;

// The Text record's payload begins with a status byte whose low bits carry the language length.
constexpr uint8_t TEXT_STATUS_LANGUAGE_LENGTH_MASK = 0x3F;

/** Locates the NDEF message inside the TLV area. Returns false when there is none. */
bool find_ndef_message(const uint8_t* data, size_t size, size_t& out_offset, size_t& out_length) {
    size_t pos = 0;
    while (pos < size) {
        const uint8_t tag = data[pos++];

        if (tag == TLV_TERMINATOR) {
            return false;
        }
        // The only tag that carries no length byte at all.
        if (tag == TLV_NULL) {
            continue;
        }
        if (pos >= size) {
            return false;
        }

        size_t length = data[pos++];
        // 0xFF escapes to a big-endian 16-bit length.
        if (length == 0xFF) {
            if (pos + 2 > size) {
                return false;
            }
            length = (static_cast<size_t>(data[pos]) << 8) | data[pos + 1];
            pos += 2;
        }

        if (pos + length > size) {
            return false;
        }

        if (tag == TLV_NDEF_MESSAGE) {
            out_offset = pos;
            out_length = length;
            return true;
        }

        // Lock/memory/proprietary TLVs are skipped by their own length; anything unrecognised is
        // treated the same way rather than abandoning the walk, since its length is still valid.
        (void) TLV_LOCK_CONTROL;
        (void) TLV_MEMORY_CONTROL;
        (void) TLV_PROPRIETARY;
        pos += length;
    }
    return false;
}

} // namespace

extern "C" bool ndef_parse_text_record(const uint8_t* data, size_t size, char* out, size_t out_size) {
    if (data == nullptr || out == nullptr || out_size == 0) {
        return false;
    }

    size_t message_offset = 0;
    size_t message_length = 0;
    if (!find_ndef_message(data, size, message_offset, message_length)) {
        return false;
    }

    const uint8_t* message = data + message_offset;
    size_t pos = 0;

    while (pos < message_length) {
        const uint8_t flags = message[pos++];
        if (pos >= message_length) {
            return false;
        }

        const uint8_t type_length = message[pos++];

        size_t payload_length = 0;
        if (flags & RECORD_FLAG_SHORT) {
            if (pos >= message_length) {
                return false;
            }
            payload_length = message[pos++];
        } else {
            if (pos + 4 > message_length) {
                return false;
            }
            payload_length = (static_cast<size_t>(message[pos]) << 24) |
                             (static_cast<size_t>(message[pos + 1]) << 16) |
                             (static_cast<size_t>(message[pos + 2]) << 8) |
                             static_cast<size_t>(message[pos + 3]);
            pos += 4;
        }

        size_t id_length = 0;
        if (flags & RECORD_FLAG_ID_LENGTH) {
            if (pos >= message_length) {
                return false;
            }
            id_length = message[pos++];
        }

        if (pos + type_length + id_length + payload_length > message_length) {
            return false;
        }

        const uint8_t* type = message + pos;
        pos += type_length;
        pos += id_length;
        const uint8_t* payload = message + pos;
        pos += payload_length;

        const bool is_text = (flags & RECORD_TNF_MASK) == TNF_WELL_KNOWN &&
                             type_length == 1 && type[0] == 'T';
        if (!is_text) {
            continue;
        }

        if (payload_length < 1) {
            return false;
        }
        const size_t language_length = payload[0] & TEXT_STATUS_LANGUAGE_LENGTH_MASK;
        if (1 + language_length > payload_length) {
            return false;
        }

        const size_t text_length = payload_length - 1 - language_length;
        if (text_length + 1 > out_size) {
            return false;
        }

        memcpy(out, payload + 1 + language_length, text_length);
        out[text_length] = '\0';
        return true;
    }

    return false;
}
