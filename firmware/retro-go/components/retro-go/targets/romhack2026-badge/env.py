# This file is injected late into rg_tool.py, you can run arbitrary python code here
# For example override python variables or set environment variables with os.putenv

# Espressif chip in the device
IDF_TARGET = "esp32s3"
# The badge is flashed per-partition into Tactility's partition table, never as a whole-flash
# image, so no .fw packaging is needed.
FW_FORMAT = "none"
# Default apps to build when none is specified. The badge only carries these three: retro-core is
# where NES and Game Boy live, prboom-go is DOOM.
DEFAULT_APPS = "launcher retro-core prboom-go"
