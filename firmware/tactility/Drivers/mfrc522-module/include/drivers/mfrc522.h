
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <tactility/device.h>

#ifdef __cplusplus
extern "C" {
#endif

// Reads the UID of a Mifare Classic card, returning true on success.
// uid_out must be at least 4 bytes for 1K cards; len receives the UID length.
bool mfrc522_read_uid(struct Device* dev, uint8_t* uid_out, size_t* len);

#ifdef __cplusplus
}
#endif
