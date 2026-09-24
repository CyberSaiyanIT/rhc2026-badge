
// SPDX-License-Identifier: Apache-2.0
#include <drivers/mfrc522.h>
#include <drivers/ndef.h>
#include <mfrc522_module.h>

#include <bindings/nxp_mfrc522.h>
#include <tactility/device.h>
#include <tactility/driver.h>
#include <tactility/drivers/esp32_spi.h>
#include <tactility/drivers/gpio_controller.h>
#include <tactility/drivers/power_rail.h>
#include <tactility/drivers/spi_controller.h>
#include <tactility/error.h>
#include <tactility/log.h>
#include <tactility/check.h>

#include <driver/spi_master.h>

#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <string.h>

#define TAG "MFRC522"

// One tag's data area, staged on the stack. Deliberately smaller than an NTAG216's 888-byte area: the
// quiz payload is a 36-character UUID and this runs on a timer task's stack.
#define NDEF_READ_MAX_BYTES 256

#define GET_CONFIG(device) (static_cast<const NxpMfrc522Config*>((device)->config))

struct Mfrc522Internal {
    spi_device_handle_t spi;
    GpioDescriptor* reset_descriptor;
    Device* supply;
};

// Registers
enum PCD_Register {
    CommandReg            = 0x01 << 1,
    ComIEnReg             = 0x02 << 1,
    DivIEnReg             = 0x03 << 1,
    ComIrqReg             = 0x04 << 1,
    DivIrqReg             = 0x05 << 1,
    ErrorReg              = 0x06 << 1,
    Status1Reg            = 0x07 << 1,
    Status2Reg            = 0x08 << 1,
    FIFODataReg           = 0x09 << 1,
    FIFOLevelReg          = 0x0A << 1,
    WaterLevelReg         = 0x0B << 1,
    ControlReg            = 0x0C << 1,
    BitFramingReg         = 0x0D << 1,
    CollReg               = 0x0E << 1,

    ModeReg               = 0x11 << 1,
    TxModeReg             = 0x12 << 1,
    RxModeReg             = 0x13 << 1,
    TxControlReg          = 0x14 << 1,
    TxASKReg              = 0x15 << 1,
    TxSelReg              = 0x16 << 1,
    RxSelReg              = 0x17 << 1,
    RxThresholdReg        = 0x18 << 1,
    DemodReg              = 0x19 << 1,
    MfTxReg               = 0x1C << 1,
    MfRxReg               = 0x1D << 1,
    SerialSpeedReg        = 0x1F << 1,

    CRCResultRegH         = 0x21 << 1,
    CRCResultRegL         = 0x22 << 1,
    ModWidthReg           = 0x24 << 1,
    RFCfgReg              = 0x26 << 1,
    GsNReg                = 0x27 << 1,
    CWGsPReg              = 0x28 << 1,
    ModGsPReg             = 0x29 << 1,
    TModeReg              = 0x2A << 1,
    TPrescalerReg         = 0x2B << 1,
    TReloadRegH           = 0x2C << 1,
    TReloadRegL           = 0x2D << 1,
    TCounterValueRegH     = 0x2E << 1,
    TCounterValueRegL     = 0x2F << 1,
    VersionReg            = 0x37 << 1,
};

// Commands
enum PCD_Command {
    PCD_Idle              = 0x00,
    PCD_Mem               = 0x01,
    PCD_GenerateRandomID  = 0x02,
    PCD_CalcCRC           = 0x03,
    PCD_Transmit          = 0x04,
    PCD_NoCmdChange       = 0x07,
    PCD_Receive           = 0x08,
    PCD_Transceive        = 0x0C,
    PCD_MFAuthent         = 0x0E,
    PCD_SoftReset         = 0x0F
};

// PICC commands
enum PICC_Command {
    PICC_CMD_REQA         = 0x26,
    PICC_CMD_WUPA         = 0x52,
    PICC_CMD_CT           = 0x88,
    PICC_CMD_SEL_CL1      = 0x93,
    PICC_CMD_SEL_CL2      = 0x95,
    PICC_CMD_SEL_CL3      = 0x97,
    PICC_CMD_HLTA         = 0x50,
    PICC_CMD_READ         = 0x30,
};

static void pcd_write_register(Mfrc522Internal* internal, uint8_t reg, uint8_t value) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;
    uint16_t data = ((reg & 0x7E) << 8) | value;
    data = __builtin_bswap16(data);
    t.tx_buffer = &data;
    spi_device_polling_transmit(internal->spi, &t);
}

static uint8_t pcd_read_register(Mfrc522Internal* internal, uint8_t reg) {
    spi_transaction_t t;
    memset(&t, 0, sizeof(t));
    t.length = 16;
    uint16_t tx_data = ((reg & 0x7E) | 0x80) << 8;
    tx_data = __builtin_bswap16(tx_data);
    uint16_t rx_data = 0;
    t.tx_buffer = &tx_data;
    t.rx_buffer = &rx_data;
    spi_device_polling_transmit(internal->spi, &t);
    return __builtin_bswap16(rx_data) & 0xFF;
}

[[maybe_unused]] static void pcd_write_register_buffer(Mfrc522Internal* internal, uint8_t reg, const uint8_t* data, uint8_t length) {
    for (uint8_t i = 0; i < length; i++) {
        pcd_write_register(internal, reg, data[i]);
    }
}

[[maybe_unused]] static void pcd_read_register_buffer(Mfrc522Internal* internal, uint8_t reg, uint8_t* data, uint8_t length) {
    if (length == 0) return;
    for (uint8_t i = 0; i < length; i++) {
        data[i] = pcd_read_register(internal, reg); // Actually reading multiple times from same reg usually requires FIFO access
        // For MFRC522, FIFODataReg is used.
    }
}

static void pcd_set_register_bitmask(Mfrc522Internal* internal, uint8_t reg, uint8_t mask) {
    uint8_t tmp = pcd_read_register(internal, reg);
    pcd_write_register(internal, reg, tmp | mask);
}

static void pcd_clear_register_bitmask(Mfrc522Internal* internal, uint8_t reg, uint8_t mask) {
    uint8_t tmp = pcd_read_register(internal, reg);
    pcd_write_register(internal, reg, tmp & (~mask));
}

static void pcd_antenna_on(Mfrc522Internal* internal) {
    uint8_t value = pcd_read_register(internal, TxControlReg);
    if ((value & 0x03) != 0x03) {
        pcd_write_register(internal, TxControlReg, value | 0x03);
    }
}

static void pcd_init(Mfrc522Internal* internal) {
    // Soft reset
    pcd_write_register(internal, CommandReg, PCD_SoftReset);
    vTaskDelay(pdMS_TO_TICKS(50));
    while (pcd_read_register(internal, CommandReg) & (1<<4)) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    
    pcd_write_register(internal, TModeReg, 0x80);
    pcd_write_register(internal, TPrescalerReg, 0xA9);
    pcd_write_register(internal, TReloadRegH, 0x03);
    pcd_write_register(internal, TReloadRegL, 0xE8);

    pcd_write_register(internal, TxASKReg, 0x40);
    pcd_write_register(internal, ModeReg, 0x3D);
    uint8_t version = pcd_read_register(internal, VersionReg);
    LOG_I(TAG, "MFRC522 Version: 0x%02X", version);
    pcd_antenna_on(internal);
}

static error_t pcd_communicate(Mfrc522Internal* internal, uint8_t command, uint8_t* sendData, uint8_t sendLen, uint8_t* backData, uint8_t backDataSize, uint8_t* backLen, uint8_t* validBits) {
    uint8_t irqEn = 0x00;
    uint8_t waitIRq = 0x00;
    
    if (command == PCD_Transceive) {
        irqEn = 0x77;
        waitIRq = 0x30;
    } else if (command == PCD_MFAuthent) {
        irqEn = 0x12;
        waitIRq = 0x10;
    }
    
    pcd_write_register(internal, ComIEnReg, irqEn | 0x80);
    pcd_clear_register_bitmask(internal, ComIrqReg, 0x80);
    pcd_set_register_bitmask(internal, FIFOLevelReg, 0x80);
    
    pcd_write_register(internal, CommandReg, PCD_Idle);
    
    for (uint8_t i = 0; i < sendLen; i++) {
        pcd_write_register(internal, FIFODataReg, sendData[i]);
    }
    
    pcd_write_register(internal, CommandReg, command);
    if (command == PCD_Transceive) {
        pcd_set_register_bitmask(internal, BitFramingReg, 0x80);
    }
    
    uint16_t i = 2000;
    uint8_t n = 0;
    while (1) {
        n = pcd_read_register(internal, ComIrqReg);
        i--;
        if (i == 0 || (n & 0x01) || (n & waitIRq)) {
            break;
        }
    }
    
    pcd_clear_register_bitmask(internal, BitFramingReg, 0x80);
    
    if (i == 0) return ERROR_TIMEOUT;
    if (pcd_read_register(internal, ErrorReg) & 0x1B) {
        return ERROR_RESOURCE;
    }
    
    if (n & irqEn & 0x01) return ERROR_TIMEOUT;
    
    if (command == PCD_Transceive) {
        uint8_t _n = pcd_read_register(internal, FIFOLevelReg);
        uint8_t lastBits = pcd_read_register(internal, ControlReg) & 0x07;
        if (lastBits) {
            *backLen = (_n - 1) * 8 + lastBits;
        } else {
            *backLen = _n * 8;
        }
        
        if (_n == 0) _n = 1;
        if (_n > backDataSize) _n = backDataSize;
        
        for (uint8_t j = 0; j < _n; j++) {
            if (backData) backData[j] = pcd_read_register(internal, FIFODataReg);
        }
        if (validBits) *validBits = lastBits;
    }
    
    return ERROR_NONE;
}

[[maybe_unused]] static error_t picc_request(Mfrc522Internal* internal, uint8_t reqMode, uint8_t* bufferATQA) {
    pcd_clear_register_bitmask(internal, Status2Reg, 0x08);
    pcd_write_register(internal, BitFramingReg, 0x07);
    
    uint8_t len = 0;
    error_t status = pcd_communicate(internal, PCD_Transceive, &reqMode, 1, bufferATQA, 2, &len, nullptr);
    if (status != ERROR_NONE || len != 16) {
        return ERROR_RESOURCE;
    }
    return ERROR_NONE;
}

/**
 * Computes CRC_A over @a data using the reader's own CRC coprocessor.
 * SELECT, READ and HLTA are all CRC-protected frames, so nothing past anticollision works
 * without this.
 */
static error_t pcd_calculate_crc(Mfrc522Internal* internal, const uint8_t* data, uint8_t length, uint8_t* result) {
    pcd_write_register(internal, CommandReg, PCD_Idle);
    pcd_write_register(internal, DivIrqReg, 0x04);
    pcd_set_register_bitmask(internal, FIFOLevelReg, 0x80);

    for (uint8_t i = 0; i < length; i++) {
        pcd_write_register(internal, FIFODataReg, data[i]);
    }
    pcd_write_register(internal, CommandReg, PCD_CalcCRC);

    for (uint16_t i = 5000; i > 0; i--) {
        if (pcd_read_register(internal, DivIrqReg) & 0x04) {
            pcd_write_register(internal, CommandReg, PCD_Idle);
            result[0] = pcd_read_register(internal, CRCResultRegL);
            result[1] = pcd_read_register(internal, CRCResultRegH);
            return ERROR_NONE;
        }
    }
    return ERROR_TIMEOUT;
}

/**
 * Runs anticollision and SELECT for every cascade level, leaving the PICC in ACTIVE.
 *
 * Anticollision alone leaves it in READY, where \read is not a legal command, so the NDEF
 * path needs the SELECT frames this adds. NTAG and Ultralight carry a 7-byte UID, which is
 * delivered across two cascade levels rather than one.
 *
 * \read PICC_CMD_READ, and equally HLTA. See ISO/IEC 14443-3 section 6.4.3.
 *
 * @param[out] uid_out receives 4, 7 or 10 bytes
 * @param[out] sak the last cascade level's Select Acknowledge
 */
static error_t picc_select(Mfrc522Internal* internal, uint8_t* uid_out, size_t* uid_len, uint8_t* sak) {
    static const uint8_t cascade_commands[3] = { PICC_CMD_SEL_CL1, PICC_CMD_SEL_CL2, PICC_CMD_SEL_CL3 };
    size_t uid_index = 0;

    pcd_clear_register_bitmask(internal, Status2Reg, 0x08);

    for (uint8_t level = 0; level < 3; level++) {
        uint8_t buffer[9];
        buffer[0] = cascade_commands[level];
        buffer[1] = 0x20; // NVB: nothing of the UID is known yet at this level.

        // Anticollision: the tag answers with 4 UID bytes (or a cascade tag plus 3) and a BCC.
        pcd_write_register(internal, BitFramingReg, 0x00);
        pcd_clear_register_bitmask(internal, CollReg, 0x80);

        uint8_t received[5];
        uint8_t back_len = 0;
        error_t status = pcd_communicate(internal, PCD_Transceive, buffer, 2, received, sizeof(received), &back_len, nullptr);
        if (status != ERROR_NONE || back_len != 40) {
            return ERROR_RESOURCE;
        }

        uint8_t bcc = 0;
        for (uint8_t i = 0; i < 4; i++) {
            bcc ^= received[i];
        }
        if (bcc != received[4]) {
            return ERROR_RESOURCE;
        }

        // SELECT: the same command with the full UID, NVB=0x70 and a CRC_A.
        buffer[1] = 0x70;
        memcpy(&buffer[2], received, 5);
        if (pcd_calculate_crc(internal, buffer, 7, &buffer[7]) != ERROR_NONE) {
            return ERROR_TIMEOUT;
        }

        uint8_t sak_response[3];
        back_len = 0;
        status = pcd_communicate(internal, PCD_Transceive, buffer, 9, sak_response, sizeof(sak_response), &back_len, nullptr);
        if (status != ERROR_NONE || back_len != 24) {
            return ERROR_RESOURCE;
        }

        // A set cascade bit means byte 0 of the anticollision answer was the cascade tag rather
        // than UID data, and another level follows.
        const bool more_levels = (sak_response[0] & 0x04) != 0;
        if (more_levels) {
            if (received[0] != PICC_CMD_CT) {
                return ERROR_RESOURCE;
            }
            memcpy(uid_out + uid_index, &received[1], 3);
            uid_index += 3;
        } else {
            memcpy(uid_out + uid_index, received, 4);
            uid_index += 4;
            if (sak) *sak = sak_response[0];
            if (uid_len) *uid_len = uid_index;
            return ERROR_NONE;
        }
    }

    return ERROR_RESOURCE;
}

/** Reads the 4 pages starting at @a page. Type 2 tags always answer 16 bytes. */
static error_t picc_read_page(Mfrc522Internal* internal, uint8_t page, uint8_t* out) {
    uint8_t buffer[4];
    buffer[0] = PICC_CMD_READ;
    buffer[1] = page;
    if (pcd_calculate_crc(internal, buffer, 2, &buffer[2]) != ERROR_NONE) {
        return ERROR_TIMEOUT;
    }

    uint8_t received[18]; // 16 data bytes plus CRC_A
    uint8_t back_len = 0;
    error_t status = pcd_communicate(internal, PCD_Transceive, buffer, 4, received, sizeof(received), &back_len, nullptr);
    if (status != ERROR_NONE || back_len != 144) {
        return ERROR_RESOURCE;
    }

    memcpy(out, received, 16);
    return ERROR_NONE;
}

/**
 * Puts the tag into HALT.
 * Without it a tag left on the antenna keeps answering REQA, so the next scan re-reads the tag
 * the caller has already handled.
 */
static error_t picc_halt(Mfrc522Internal* internal) {
    uint8_t buffer[4];
    buffer[0] = PICC_CMD_HLTA;
    buffer[1] = 0x00;
    if (pcd_calculate_crc(internal, buffer, 2, &buffer[2]) != ERROR_NONE) {
        return ERROR_TIMEOUT;
    }

    // A correctly halted tag stays silent, so the timeout this returns is the success case.
    uint8_t back_len = 0;
    error_t status = pcd_communicate(internal, PCD_Transceive, buffer, 4, nullptr, 0, &back_len, nullptr);
    return (status == ERROR_TIMEOUT) ? ERROR_NONE : ERROR_RESOURCE;
}

extern "C" bool mfrc522_read_uid(struct Device* dev, uint8_t* uid_out, size_t* len) {
    if (!dev || !device_get_driver_data(dev)) return false;
    auto* internal = static_cast<Mfrc522Internal*>(device_get_driver_data(dev));

    // The reader shares its controller with the display, which flushes from the LVGL task while a
    // scan runs from the app's timer. Taken once around the whole exchange rather than per
    // transfer: a REQA and the anticollision that answers it are one conversation, and the
    // reader's FIFO does not survive another device's transfer landing in the middle of it.
    if (spi_controller_lock_bus_of(dev) != ERROR_NONE) {
        return false;
    }

    bool found = false;
    uint8_t bufferATQA[2];
    if (picc_request(internal, PICC_CMD_REQA, bufferATQA) == ERROR_NONE) {
        // The full cascade, not just anticollision: a 7-byte-UID tag answers cascade level 1 with
        // the cascade tag 0x88 followed by only three UID bytes, so the anticollision response on
        // its own is not the UID.
        if (picc_select(internal, uid_out, len, nullptr) == ERROR_NONE) {
            found = true;
        }
        picc_halt(internal);
    }

    spi_controller_unlock_bus_of(dev);
    return found;
}

extern "C" bool mfrc522_read_ndef_text(struct Device* dev, char* out, size_t out_size, bool* out_tag_present) {
    if (out_tag_present) *out_tag_present = false;
    if (!dev || !device_get_driver_data(dev) || !out || out_size == 0) return false;
    auto* internal = static_cast<Mfrc522Internal*>(device_get_driver_data(dev));

    if (spi_controller_lock_bus_of(dev) != ERROR_NONE) {
        return false;
    }

    bool found = false;
    uint8_t bufferATQA[2];
    if (picc_request(internal, PICC_CMD_REQA, bufferATQA) == ERROR_NONE) {
        uint8_t uid[10];
        size_t uid_len = 0;
        uint8_t sak = 0;
        if (picc_select(internal, uid, &uid_len, &sak) == ERROR_NONE) {
            // Reported even when the NDEF walk below fails, so the caller can tell a tag it could
            // not read from no tag at all.
            if (out_tag_present) *out_tag_present = true;

            // The capability container sits in page 3. Its first byte is the NDEF magic number;
            // anything else means the tag was never formatted for NDEF and the data area holds
            // no TLVs to walk.
            uint8_t page_data[16];
            if (picc_read_page(internal, 3, page_data) == ERROR_NONE && page_data[0] == 0xE1) {
                // The usable data area is bounded by the CC's size byte, which counts 8-byte
                // blocks. Capped so a corrupt CC cannot drive an unbounded read.
                size_t available = static_cast<size_t>(page_data[2]) * 8;
                if (available > NDEF_READ_MAX_BYTES) {
                    available = NDEF_READ_MAX_BYTES;
                }

                uint8_t data[NDEF_READ_MAX_BYTES];
                size_t read_bytes = 0;
                // Each READ answers with 4 pages, so the data area is walked 16 bytes at a time
                // from page 4.
                for (uint8_t page = 4; read_bytes < available; page += 4) {
                    if (picc_read_page(internal, page, page_data) != ERROR_NONE) {
                        break;
                    }
                    size_t chunk = available - read_bytes;
                    if (chunk > sizeof(page_data)) {
                        chunk = sizeof(page_data);
                    }
                    memcpy(data + read_bytes, page_data, chunk);
                    read_bytes += chunk;
                }

                found = ndef_parse_text_record(data, read_bytes, out, out_size);
            }
        }
        picc_halt(internal);
    }

    spi_controller_unlock_bus_of(dev);
    return found;
}

static error_t pulse_reset(GpioDescriptor* descriptor) {
    if (descriptor == nullptr) {
        return ERROR_NONE;
    }
    // MFRC522 requires a low pulse on reset pin
    error_t error = gpio_descriptor_set_level(descriptor, false);
    if (error != ERROR_NONE) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(10));
    error = gpio_descriptor_set_level(descriptor, true);
    if (error != ERROR_NONE) {
        return error;
    }
    vTaskDelay(pdMS_TO_TICKS(50));
    return ERROR_NONE;
}

static error_t start(Device* device) {
    auto* parent = device_get_parent(device);
    check(device_get_type(parent) == &SPI_CONTROLLER_TYPE);

    const auto* spi_config = static_cast<const Esp32SpiConfig*>(parent->config);
    const auto* config = GET_CONFIG(device);

    struct GpioPinSpec cs_pin;
    if (esp32_spi_get_cs_pin(device, &cs_pin) != ERROR_NONE) {
        LOG_E(TAG, "Device missing CS pin on parent bus");
        return ERROR_INVALID_ARGUMENT;
    }

    auto* internal = new Mfrc522Internal();
    device_set_driver_data(device, internal);

    // Resolve reset pin. A supply rail replaces it: the rail is off (reader held in reset) until
    // it is enabled here, so switching it on is the reset release the reader needs.
    internal->reset_descriptor = nullptr;
    internal->supply = config->supply;
    if (internal->supply != nullptr) {
        if (power_rail_enable(internal->supply) != ERROR_NONE) {
            LOG_E(TAG, "Failed to power on the reader");
            delete internal;
            device_set_driver_data(device, nullptr);
            return ERROR_RESOURCE;
        }
    } else if (config->pin_reset.gpio_controller != nullptr) {
        internal->reset_descriptor = gpio_descriptor_acquire_pin_spec(&config->pin_reset, GPIO_OWNER_GPIO);
        gpio_descriptor_set_flags(internal->reset_descriptor, GPIO_FLAG_DIRECTION_OUTPUT);
        pulse_reset(internal->reset_descriptor);
    }

    // Initialize SPI device on the parent bus
    spi_device_interface_config_t devcfg = {};
    devcfg.clock_speed_hz = 1000000; // 1MHz for MFRC522
    devcfg.mode = 0; // SPI mode 0
    devcfg.spics_io_num = cs_pin.gpio_controller == nullptr ? -1 : cs_pin.pin;
    devcfg.queue_size = 7;
    
    esp_err_t ret = spi_bus_add_device(spi_config->host, &devcfg, &internal->spi);
    if (ret != ESP_OK) {
        LOG_E(TAG, "Failed to add SPI device");
        return ERROR_NOT_SUPPORTED;
    }

    // A burst of register writes on the same shared controller, so it is bracketed like a scan.
    if (spi_controller_lock_bus_of(device) != ERROR_NONE) {
        LOG_E(TAG, "Failed to lock the SPI bus");
        return ERROR_RESOURCE;
    }
    pcd_init(internal);
    spi_controller_unlock_bus_of(device);

    LOG_I(TAG, "MFRC522 started successfully");
    return ERROR_NONE;
}

static error_t stop(Device* device) {
    auto* internal = static_cast<Mfrc522Internal*>(device_get_driver_data(device));
    if (internal->spi) {
        spi_bus_remove_device(internal->spi);
    }
    if (internal->reset_descriptor) {
        gpio_descriptor_release(internal->reset_descriptor);
    }
    if (internal->supply) {
        power_rail_disable(internal->supply);
    }
    delete internal;
    device_set_driver_data(device, nullptr);
    return ERROR_NONE;
}

extern "C" {
extern Module mfrc522_module;

Driver mfrc522_driver = {
    .name = "mfrc522",
    .compatible = (const char*[]) { "nxp,mfrc522", nullptr },
    .start_device = start,
    .stop_device = stop,
    .api = nullptr,
    .device_type = nullptr,
    .owner = &mfrc522_module,
    .internal = nullptr
};
}
