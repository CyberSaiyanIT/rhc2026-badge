# Romhack Badge


## Hardware

| Function        | Detail |
|-----------------|--------|
| MCU             | ESP32-S3, 16 MB flash, Octal PSRAM @ 80 MHz |
| Display         | ILI9341, 320×240, SPI2 @ 80 MHz. MOSI=11, MISO=13, SCLK=12, CS=15, DC=9 |
| Backlight       | GPIO21 (PWM) |
| Display reset   | AW9523 P1.6 |
| GPIO expander   | AW9523B @ 0x58 on I2C0 (SDA=17, SCL=16) |
| RFID power      | AW9523 P1.4 |
| SD card detect  | AW9523 P1.3, low while sdcard is preset |
| SD card         | SDMMC 1-bit: CLK=4, CMD=5, D0=7 |
| Audio           | I2S0 output: BCLK=43, WS=44, DATA_OUT=45 |
| Battery         | ADC1 channel 5 |
| 5V boost enable | AW9523 P1.2  |
| NeoPixel enable | AW9523 P1.7 (MOSFET) |
| Amp enable      | AW9523 P0.7  |


