// SPDX-License-Identifier: Apache-2.0
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Extracts the first NDEF Text record's text from a tag's data area.
 *
 * Pure: it parses a byte buffer and touches no hardware, which is what makes it testable on the
 * simulator. The hardware walk that produces @a data lives in mfrc522_read_ndef_text().
 *
 * @a data starts at the first TLV, which on an NTAG/Ultralight is page 4.
 * @param[out] out receives the NUL-terminated UTF-8 text
 * @return false when no Text record was found, or when it does not fit @a out_size
 */
bool ndef_parse_text_record(const uint8_t* data, size_t size, char* out, size_t out_size);

#ifdef __cplusplus
}
#endif
