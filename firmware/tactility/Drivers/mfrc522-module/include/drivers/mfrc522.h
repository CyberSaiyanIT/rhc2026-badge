#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <tactility/device.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads the UID of a card in the reader's field.
// Returns true if a card was read successfully, false otherwise.
// uid_out must be at least 10 bytes: MIFARE Classic answers with 4, NTAG and Ultralight with 7.
// len will be set to the length of the read UID.
bool mfrc522_read_uid(struct Device* dev, uint8_t* uid_out, size_t* len);

// Reads the first NDEF Text record from an NTAG or Ultralight tag.
// Returns true and writes NUL-terminated UTF-8 to out, which must be large enough for the text.
// Returns false when no tag is present, the tag is not NDEF-formatted, or it holds no Text record.
// out_tag_present, when given, distinguishes those: it is set true whenever a tag answered and was
// selected, so a caller can tell an empty field from a tag it could not read.
bool mfrc522_read_ndef_text(struct Device* dev, char* out, size_t out_size, bool* out_tag_present);

#ifdef __cplusplus
}
#endif
