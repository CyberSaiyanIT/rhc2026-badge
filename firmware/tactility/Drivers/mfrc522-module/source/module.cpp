// SPDX-License-Identifier: Apache-2.0
#include <tactility/driver.h>
#include <tactility/module.h>

extern "C" {

extern Driver mfrc522_driver;

static Driver* const mfrc522_drivers[] = {
    &mfrc522_driver,
    nullptr
};

Module mfrc522_module = {
    .name = "mfrc522",
    .drivers = mfrc522_drivers
};

} // extern "C"
