# Romhack Camp Badge 2026

## Hardware

|  |  |
| --- | --- |
| SoC | ESP32-S3, 16 MB flash, 8 MB octal PSRAM @ 80 MHz |
| Display | 2.4" 320x240 ILI9341, SPI |
| IO expander | AW9523B|
| Audio | ES8156 codec, FM8002A Amp|
| LEDs | 22 RGB LEDs|
| RFID | Ci522 13.56 MHz |
| Storage | microSD, 1-bit SDMMC |
| Radio | Wi-Fi and BLE |

## Build(Docker)

```bash
firmware/tactility/Buildscripts/init-submodules.sh
```

```bash
cd firmware/tactility
docker run --rm -v "$PWD":/project -w /project espressif/idf:v5.5.2 \
    bash -c "python device.py romhack2026-badge && idf.py build"
```

```bash
cd firmware/retro-go
docker run --rm -v "$PWD":/retro-go -w /retro-go espressif/idf:v5.5.2 \
    bash -c "python rg_tool.py --target romhack2026-badge build launcher retro-core prboom-go"
```

## Flash(Docker)

Tactility first, it writes the partition table the retro-go slots live in.

```bash
cd firmware/tactility
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    --device /dev/ttyACM0 --group-add dialout \
    -v "$PWD":/project -w /project/build espressif/idf:v5.5.2 \
    bash -c "python -m esptool --chip esp32s3 -p /dev/ttyACM0 -b 460800 \
             --before default_reset --after hard_reset write_flash @flash_project_args"
```

```bash
cd firmware/retro-go
rm -f partitions.bin
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp \
    --device /dev/ttyACM0 --group-add dialout \
    -v "$PWD":/retro-go -w /retro-go espressif/idf:v5.5.2 \
    bash -c "python rg_tool.py --target romhack2026-badge --port /dev/ttyACM0 \
             flash launcher retro-core prboom-go"
```

## MicroSD card setup

### Format

FAT32 with an MBR partition table

```bash
lsblk
sudo mkfs.fat -F 32 /dev/sdX1
```

### Layout


```
/
├── roms/
│   ├── nes/      .nes .fc .fds .nsf .zip
│   ├── snes/     .smc .sfc .zip
│   ├── gb/       .gb .gbc .zip
│   ├── gbc/      .gbc .gb .zip
│   ├── gw/       .gw
│   ├── sms/      .sms .sg .zip
│   ├── gg/       .gg .zip
│   ├── col/      .col .rom .zip
│   ├── pce/      .pce .zip
│   ├── lnx/      .lnx .zip
│   └── doom/     .wad .zip
├── Music/        .mp3 .m4a .aac, upto 4 folders deep
├── romart/       game covers/artwork
└── retro-go/     saves, config, themes
```

## Acknowledgements

The badge firmware is based on:

- [Tactility](https://github.com/TactilityProject/Tactility) by ByteWelder 
- [Retro-Go](https://github.com/ducalex/retro-go) by ducalex

