// Target definition
#define RG_TARGET_NAME             "ROMHACK2026-BADGE"

#define RG_APP_FACTORY              "factory"
// components/retro-go/config.h defines RG_APP_FACTORY to NULL for every target that has no
// factory app, so #ifdef cannot tell the two apart. This is the flag code actually gates on.
#define RG_HAVE_FACTORY_APP         1

#define RG_STORAGE_ROOT             "/sd"
#define RG_STORAGE_SDMMC_HOST       SDMMC_HOST_SLOT_1
#define RG_STORAGE_SDMMC_SPEED      SDMMC_FREQ_DEFAULT
#define RG_STORAGE_FLASH_PARTITION  "data"

// GPIO Extender
#define RG_I2C_GPIO_DRIVER          1   // 1 = AW9523, 2 = PCF9539, 3 = MCP23017
#define RG_I2C_GPIO_ADDR            0x58

// AW9523B pins, in the flat 0-15 numbering rg_i2c.c uses (P0.x = x, P1.x = 8 + x).
#define BADGE_EXP_SPEAKER_SD        7   // P0.7, amplifier shutdown: driven LOW to play
#define BADGE_EXP_BOOST_5V          10  // P1.2, 5V boost feeding the speaker amp and the LEDs
#define BADGE_EXP_NEOPIXEL_EN       15  // P1.7, LED strip power switch
#define BADGE_EXP_LCD_RST           14  // P1.6, ILI9341 reset

#define RG_POWER_RAIL_SUPPLY { \
    .pin = BADGE_EXP_BOOST_5V, .startup_delay_us = 2000 \
}
#define RG_POWER_RAIL_SPEAKER { \
    .pin = BADGE_EXP_SPEAKER_SD, .active_low = true, .supply = &rg_power_rail_supply \
}

#define RG_AUDIO_USE_INT_DAC        0   // 0 = Disable, 1 = GPIO25, 2 = GPIO26, 3 = Both
#define RG_AUDIO_USE_SPEAKER        1   // 0 = Disable, 1 = Enable
#define RG_AUDIO_USE_HEADPHONES     1   // 0 = Disable, 1 = Enable

// rg_i2c_gpio_init() software-resets the expander and leaves every pin an input, so the LED strip's
// switch is driven off here rather than left to whatever its gate settles at.
#define RG_CUSTOM_PLATFORM_INIT()                                        \
    rg_i2c_gpio_init();                                                  \
    rg_i2c_gpio_set_level(BADGE_EXP_NEOPIXEL_EN, 0);                     \
    rg_i2c_gpio_set_direction(BADGE_EXP_NEOPIXEL_EN, RG_GPIO_OUTPUT);    \
    rg_i2c_gpio_set_level(BADGE_EXP_LCD_RST, 0);                         \
    rg_i2c_gpio_set_direction(BADGE_EXP_LCD_RST, RG_GPIO_OUTPUT);        \
    rg_usleep(100 * 1000);                                               \
    rg_i2c_gpio_set_level(BADGE_EXP_LCD_RST, 1);                         \
    rg_usleep(10 * 1000);                                                \
    badge_es8156_write(0x02, 0x04);                                      \
    badge_es8156_write(0x20, 0x2A);                                      \
    badge_es8156_write(0x21, 0x3C);                                      \
    badge_es8156_write(0x22, 0x00);                                      \
    badge_es8156_write(0x24, 0x07);                                      \
    badge_es8156_write(0x23, 0x00);                                      \
    badge_es8156_write(0x0A, 0x01);                                      \
    badge_es8156_write(0x0B, 0x01);                                      \
    badge_es8156_write(0x11, 0x00);                                      \
    badge_es8156_write(0x14, 179); /* volume 70% */                      \
    badge_es8156_write(0x0D, 0x14);                                      \
    badge_es8156_write(0x18, 0x00);                                      \
    badge_es8156_write(0x08, 0x3F);                                      \
    badge_es8156_write(0x00, 0x02);                                      \
    badge_es8156_write(0x00, 0x03);                                      \
    badge_es8156_write(0x25, 0x20);

// Video
#define RG_SCREEN_DRIVER            0   // 0 = ILI9341/ST7789
#define RG_SCREEN_HOST              SPI2_HOST
#define RG_SCREEN_SPEED             SPI_MASTER_FREQ_40M
#define RG_SCREEN_BACKLIGHT         1
#define RG_SCREEN_WIDTH             320
#define RG_SCREEN_HEIGHT            240
// 5 = MV|MY, settled on hardware; the devicetree's mirror flags do not carry over to a raw MADCTL
// write. MV swaps rows and columns, so MY is what fixes a lateral inversion, not MX.
#define RG_SCREEN_ROTATION          5
#define RG_SCREEN_RGB_BGR           1
#define RG_SCREEN_VISIBLE_AREA      {0, 0, 0, 0}
#define RG_SCREEN_SAFE_AREA         {0, 0, 0, 0}
#define RG_SCREEN_INIT()                                                                                         \
    ILI9341_CMD(0xCF, 0x00, 0xc3, 0x30);                                                                         \
    ILI9341_CMD(0xED, 0x64, 0x03, 0x12, 0x81);                                                                   \
    ILI9341_CMD(0xE8, 0x85, 0x00, 0x78);                                                                         \
    ILI9341_CMD(0xCB, 0x39, 0x2c, 0x00, 0x34, 0x02);                                                             \
    ILI9341_CMD(0xF7, 0x20);                                                                                     \
    ILI9341_CMD(0xEA, 0x00, 0x00);                                                                               \
    ILI9341_CMD(0xC0, 0x1B);                 /* Power control   //VRH[5:0] */                                    \
    ILI9341_CMD(0xC1, 0x12);                 /* Power control   //SAP[2:0];BT[3:0] */                            \
    ILI9341_CMD(0xC5, 0x32, 0x3C);           /* VCM control */                                                   \
    ILI9341_CMD(0xC7, 0x91);                 /* VCM control2 */                                                  \
    ILI9341_CMD(0xB1, 0x00, 0x10);           /* Frame Rate Control (1B=70, 1F=61, 10=119) */                     \
    ILI9341_CMD(0xB6, 0x0A, 0xA2);           /* Display Function Control */                                      \
    ILI9341_CMD(0xF6, 0x01, 0x30);                                                                               \
    ILI9341_CMD(0xF2, 0x00);                 /* 3Gamma Function Disable */                                       \
    ILI9341_CMD(0x26, 0x01);                 /* Gamma curve selected */                                          \
    ILI9341_CMD(0xE0, 0xD0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x32, 0x44, 0x42, 0x06, 0x0e, 0x12, 0x14, 0x17);       \
    ILI9341_CMD(0xE1, 0xD0, 0x00, 0x02, 0x07, 0x0a, 0x28, 0x31, 0x54, 0x47, 0x0E, 0x1C, 0x17, 0x1b, 0x1e);       \

#define RG_GAMEPAD_I2C_MAP { \
    {RG_KEY_UP,     .num = 6,  .level = 0},\
    {RG_KEY_RIGHT,  .num = 2,  .level = 0},\
    {RG_KEY_DOWN,   .num = 3,  .level = 0},\
    {RG_KEY_LEFT,   .num = 5,  .level = 0},\
    {RG_KEY_A,      .num = 0,  .level = 0},\
    {RG_KEY_B,      .num = 9,  .level = 0},\
    {RG_KEY_START,  .num = 1,  .level = 0},\
    {RG_KEY_MENU,   .num = 8,  .level = 0},\
}
#define RG_GAMEPAD_GPIO_MAP { \
    {RG_KEY_SELECT, .num = GPIO_NUM_0, .pullup = 1, .level = 0},\
}

#define RG_GAMEPAD_DEFERRED_CHORD { \
    .hold = RG_KEY_MENU, .with = RG_KEY_A, .alone = RG_KEY_MENU, .combo = RG_KEY_OPTION \
}

#define RG_RECOVERY_BTN             RG_KEY_B

// Battery
#define RG_BATTERY_DRIVER           1
#define RG_BATTERY_ADC_UNIT         ADC_UNIT_1
#define RG_BATTERY_ADC_CHANNEL      ADC_CHANNEL_5
#define RG_BATTERY_CALC_VOLTAGE(raw) ((raw) * 4.191489f * 0.001f)
#define RG_BATTERY_CALC_PERCENT(raw) (((raw) * 4.191489f - 3500.f) / (4200.f - 3500.f) * 100.f)

// I2C BUS
#define RG_GPIO_I2C_SDA             GPIO_NUM_17
#define RG_GPIO_I2C_SCL             GPIO_NUM_16

// SPI Display
#define RG_GPIO_LCD_MISO            GPIO_NUM_13
#define RG_GPIO_LCD_MOSI            GPIO_NUM_11
#define RG_GPIO_LCD_CLK             GPIO_NUM_12
#define RG_GPIO_LCD_CS              GPIO_NUM_15
#define RG_GPIO_LCD_DC              GPIO_NUM_9
#define RG_GPIO_LCD_BCKL            GPIO_NUM_21
#define RG_GPIO_SDSPI_CLK           GPIO_NUM_4
#define RG_GPIO_SDSPI_CMD           GPIO_NUM_5
#define RG_GPIO_SDSPI_D0            GPIO_NUM_7

// External I2S DAC (ES8156)
#define RG_GPIO_SND_I2S_MCK         GPIO_NUM_10
#define RG_GPIO_SND_I2S_BCK         GPIO_NUM_43
#define RG_GPIO_SND_I2S_WS          GPIO_NUM_46
#define RG_GPIO_SND_I2S_DATA        GPIO_NUM_45
#define BADGE_ES8156_ADDR           0x08

#define badge_es8156_write(reg, value) rg_i2c_write_byte(BADGE_ES8156_ADDR, (reg), (value))
