
// SPDX-License-Identifier: Apache-2.0
#include <drivers/mfrc522.h>
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

static error_t pcd_communicate(Mfrc522Internal* internal, uint8_t command, uint8_t* sendData, uint8_t sendLen, uint8_t* backData, uint8_t* backLen, uint8_t* validBits) {
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
        if (_n > 16) _n = 16;
        
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
    error_t status = pcd_communicate(internal, PCD_Transceive, &reqMode, 1, bufferATQA, &len, nullptr);
    if (status != ERROR_NONE || len != 16) {
        return ERROR_RESOURCE;
    }
    return ERROR_NONE;
}

[[maybe_unused]] static error_t picc_anticoll(Mfrc522Internal* internal, uint8_t* uid) {
    pcd_clear_register_bitmask(internal, Status2Reg, 0x08);
    pcd_write_register(internal, BitFramingReg, 0x00);
    pcd_clear_register_bitmask(internal, CollReg, 0x80);
    
    uint8_t buffer[9];
    buffer[0] = PICC_CMD_SEL_CL1;
    buffer[1] = 0x20;
    
    uint8_t len = 0;
    error_t status = pcd_communicate(internal, PCD_Transceive, buffer, 2, buffer, &len, nullptr);
    if (status == ERROR_NONE) {
        uint8_t uidCheck = 0;
        for (uint8_t i = 0; i < 4; i++) {
            uid[i] = buffer[i];
            uidCheck ^= buffer[i];
        }
        if (uidCheck != buffer[4]) {
            return ERROR_RESOURCE;
        }
    }
    return status;
}

extern "C" bool mfrc522_read_uid(struct Device* dev, uint8_t* uid_out, size_t* len) {
    if (!dev || !device_get_driver_data(dev)) return false;
    auto* internal = static_cast<Mfrc522Internal*>(device_get_driver_data(dev));

    // The reader shares its controller with the display. Taken once around the whole exchange:
    // the reader's FIFO does not survive another device's transfer landing mid-conversation.
    if (spi_controller_lock_bus_of(dev) != ERROR_NONE) {
        return false;
    }

    bool found = false;
    uint8_t bufferATQA[2];
    if (picc_request(internal, PICC_CMD_REQA, bufferATQA) == ERROR_NONE) {
        if (picc_anticoll(internal, uid_out) == ERROR_NONE) {
            if (len) *len = 4;
            found = true;
        }
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
