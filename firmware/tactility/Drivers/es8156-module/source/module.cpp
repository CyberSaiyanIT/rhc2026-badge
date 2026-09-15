// SPDX-License-Identifier: Apache-2.0
#include <tactility/driver.h>
#include <tactility/module.h>

extern "C" {

extern Driver es8156_driver;

static Driver* const es8156_drivers[] = {
    &es8156_driver,
    nullptr
};

extern const ModuleSymbol es8156_module_symbols[];

Module es8156_module = {
    .name = "es8156",
    .drivers = es8156_drivers,
    .symbols = es8156_module_symbols
};

}
