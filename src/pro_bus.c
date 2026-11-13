#include "pro_bus.h"

#include <stdbool.h>
#include <stdint.h>

#include "fsl_clock.h"
#include "fsl_device_registers.h"
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "fsl_i2c.h"
#pragma GCC diagnostic pop
#include "fsl_iocon.h"
#include "fsl_spi.h"

#include "DAP_config.h"
#include "board_pins.h"
#include "board_straps.h"
#include "dap_shared.h"
#include "dio.h"
#include "swo.h"

#define SPI_MODE_CPOL (1U << 0)
#define SPI_MODE_CPHA (1U << 1)
#define SPI_MODE_LSB_FIRST (1U << 2)
#define SPI_MODE_MASK (SPI_MODE_CPOL | SPI_MODE_CPHA | SPI_MODE_LSB_FIRST)

#define I2C_MIN_CLOCK_HZ 10000U
#define I2C_MAX_CLOCK_HZ 1000000U

// HS SPI's timing is specified at fast slew (LPC55S6x data sheet, Table 37).
#define SPI_PIN_MODE (IOCON_MODE_INACT | IOCON_DIGITAL_EN | IOCON_SLEW_FAST)
// These are standard pins, not the true open-drain I2C pair, so I2C needs their high driver off
// (UM11126, 15.4.2.9).
#define I2C_PIN_MODE (IOCON_MODE_INACT | IOCON_DIGITAL_EN | IOCON_OPENDRAIN_EN)

#define I2C_SDA_PIN 4U
#define I2C_SCL_PIN 21U
/** Half an SCL period of the bus clear, at 100 kHz. */
#define I2C_CLEAR_HALF_US 5U

static uint16_t get_u16(const uint8_t* bytes) {
    return (uint16_t)((uint16_t)bytes[0] | ((uint16_t)bytes[1] << 8U));
}

static uint32_t get_u32(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

static void put_u16(uint8_t* bytes, uint16_t value) {
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8U);
}

bool pro_bus_available(void) {
    return (board_kind == BOARD_KIND_PRO) && ((dap_shared.jumpers & BOARD_JUMPER_USBSIO_OFF) == 0U);
}

/** HS_SPI_SSEL0, MOSI, SCK and MISO on DIO0 to DIO3 (LPC55S6x data sheet, Table 3). */
static void spi_pinmux(void) {
    CLOCK_EnableClock(kCLOCK_Iocon);
    IOCON_PinMuxSet(IOCON, 0U, 20U, IOCON_FUNC8 | SPI_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 0U, 26U, IOCON_FUNC9 | SPI_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 1U, 2U, IOCON_FUNC6 | SPI_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 1U, 3U, IOCON_FUNC6 | SPI_PIN_MODE);
}

uint32_t pro_spi_command(const uint8_t* request, uint8_t* response) {
    uint16_t const length = get_u16(request + 6U);
    uint32_t const consumed = PRO_SPI_REQUEST_FIXED + length;
    response[1] = PRO_BUS_INVALID;
    put_u16(response + 2U, 0U);

    if ((length == 0U) || (length > DAP_PACKET_SIZE - PRO_SPI_REQUEST_FIXED) ||
        ((request[1] & (uint8_t)~SPI_MODE_MASK) != 0U)) {
        return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
    }
    if (!pro_bus_available()) {
        response[1] = PRO_BUS_UNAVAILABLE;
        return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
    }

    // A refused clock leaves the pins where they are: checked before SPI takes them.
    uint32_t const requested_clock = get_u32(request + 2U);
    CLOCK_AttachClk(kFRO_HF_DIV_to_HSLSPI);
    uint32_t const source_clock = CLOCK_GetHsLspiClkFreq();
    if ((source_clock == 0U) || (requested_clock < (source_clock / 4096U)) ||
        (requested_clock > (source_clock / 2U))) {
        CLOCK_AttachClk(kNONE_to_HSLSPI);
        response[1] = PRO_BUS_INVALID;
        return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
    }
    if (!dio_spi_begin()) {
        CLOCK_AttachClk(kNONE_to_HSLSPI);
        response[1] = PRO_BUS_BUSY;
        return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
    }

    spi_pinmux();
    spi_master_config_t config;
    SPI_MasterGetDefaultConfig(&config);
    config.polarity = ((request[1] & SPI_MODE_CPOL) != 0U) ? kSPI_ClockPolarityActiveLow
                                                            : kSPI_ClockPolarityActiveHigh;
    config.phase = ((request[1] & SPI_MODE_CPHA) != 0U) ? kSPI_ClockPhaseSecondEdge : kSPI_ClockPhaseFirstEdge;
    config.direction = ((request[1] & SPI_MODE_LSB_FIRST) != 0U) ? kSPI_LsbFirst : kSPI_MsbFirst;
    // The fastest clock the divider makes that doesn't exceed the request: the SDK rounds its
    // divider to the nearest, which can come out faster.
    uint32_t const divider = (source_clock + requested_clock - 1U) / requested_clock;
    config.baudRate_Bps = source_clock / divider;
    status_t status = SPI_MasterInit(SPI8, &config, source_clock);
    if (status == kStatus_Success) {
        spi_transfer_t transfer = {
            .txData = request + PRO_SPI_REQUEST_FIXED,
            .rxData = response + PRO_BUS_RESPONSE_FIXED,
            .dataSize = length,
            .configFlags = kSPI_FrameAssert,
        };
        status = SPI_MasterTransferBlocking(SPI8, &transfer);
        SPI_Deinit(SPI8);
    }
    CLOCK_AttachClk(kNONE_to_HSLSPI);
    dio_spi_end();

    if (status == kStatus_Success) {
        response[1] = PRO_BUS_OK;
        put_u16(response + 2U, length);
        return (consumed << 16U) | (PRO_BUS_RESPONSE_FIXED + length);
    }

    response[1] = PRO_BUS_TRANSFER_ERROR;
    return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
}

static void i2c_wait_half(void) {
    uint32_t const start = TIMESTAMP_GET();
    while ((TIMESTAMP_GET() - start) < (I2C_CLEAR_HALF_US * (TIMESTAMP_CLOCK / 1000000U))) {
    }
}

/** Lets go of `pin` on port 0, or pulls it low: open drain on its GPIO. */
static void i2c_line(uint32_t pin, bool low) {
    if (low) {
        GPIO->CLR[0] = 1UL << pin;
        GPIO->DIRSET[0] = 1UL << pin;
    } else {
        GPIO->DIRCLR[0] = 1UL << pin;
    }
}

/** After a transfer ended without its STOP, a target may still hold SDA low mid-byte. Clock SCL
 *  until it lets go, nine times at most, then send a STOP, with the pins as GPIO. */
static void i2c_clear_bus(void) {
    IOCON_PinMuxSet(IOCON, 0U, I2C_SDA_PIN, IOCON_FUNC0 | I2C_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 0U, I2C_SCL_PIN, IOCON_FUNC0 | I2C_PIN_MODE);
    i2c_line(I2C_SDA_PIN, false);
    i2c_line(I2C_SCL_PIN, false);
    i2c_wait_half();
    for (uint32_t n = 0U; (n < 9U) && ((GPIO->B[0][I2C_SDA_PIN]) == 0U); n++) {
        i2c_line(I2C_SCL_PIN, true);
        i2c_wait_half();
        i2c_line(I2C_SCL_PIN, false);
        i2c_wait_half();
    }
    i2c_line(I2C_SCL_PIN, true);
    i2c_line(I2C_SDA_PIN, true);
    i2c_wait_half();
    i2c_line(I2C_SCL_PIN, false);
    i2c_wait_half();
    i2c_line(I2C_SDA_PIN, false);
    i2c_wait_half();
}

static status_t i2c_transfer(uint8_t address, i2c_direction_t direction, uint8_t* data,
                            uint16_t length, uint32_t flags) {
    i2c_master_transfer_t transfer = {
        .flags = flags,
        .slaveAddress = address,
        .direction = direction,
        .data = data,
        .dataSize = length,
    };
    return I2C_MasterTransferBlocking(I2C3, &transfer);
}

uint32_t pro_i2c_command(const uint8_t* request, uint8_t* response) {
    uint8_t const address = request[1];
    uint32_t const requested_clock = get_u32(request + 2U);
    uint16_t const tx_length = get_u16(request + 6U);
    uint16_t const rx_length = get_u16(request + 8U);
    uint32_t const consumed = PRO_I2C_REQUEST_FIXED + tx_length;
    response[1] = PRO_BUS_INVALID;
    put_u16(response + 2U, 0U);

    if (((address & 0x80U) != 0U) || ((tx_length == 0U) && (rx_length == 0U)) ||
        (tx_length > DAP_PACKET_SIZE - PRO_I2C_REQUEST_FIXED) ||
        (rx_length > DAP_PACKET_SIZE - PRO_BUS_RESPONSE_FIXED) ||
        (requested_clock < I2C_MIN_CLOCK_HZ) || (requested_clock > I2C_MAX_CLOCK_HZ)) {
        return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
    }
    if (!pro_bus_available()) {
        response[1] = PRO_BUS_UNAVAILABLE;
        return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
    }
    if (SWO_IsConfigured()) {
        response[1] = PRO_BUS_BUSY;
        return (consumed << 16U) | PRO_BUS_RESPONSE_FIXED;
    }

    CLOCK_EnableClock(kCLOCK_Iocon);
    CLOCK_AttachClk(kFRO_HF_DIV_to_FLEXCOMM3);
    IOCON_PinMuxSet(IOCON, 0U, I2C_SDA_PIN, IOCON_FUNC8 | I2C_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 0U, I2C_SCL_PIN, IOCON_FUNC1 | I2C_PIN_MODE);

    uint32_t const source_clock = CLOCK_GetFlexCommClkFreq(3U);
    uint16_t returned = 0U;
    if (source_clock == 0U) {
        response[1] = PRO_BUS_TRANSFER_ERROR;
        goto i2c_release_pins;
    }

    i2c_master_config_t config;
    I2C_MasterGetDefaultConfig(&config);
    config.baudRate_Bps = requested_clock;
    I2C_MasterInit(I2C3, &config, source_clock);

    status_t status = kStatus_Success;
    if (tx_length != 0U) {
        uint32_t const flags = (rx_length != 0U) ? kI2C_TransferNoStopFlag : kI2C_TransferDefaultFlag;
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcast-qual"
        // The SDK declares its write buffer mutable, though this transfer only reads it.
        status = i2c_transfer(address, kI2C_Write, (uint8_t*)(request + PRO_I2C_REQUEST_FIXED), tx_length, flags);
#pragma GCC diagnostic pop
    }
    if ((status == kStatus_Success) && (rx_length != 0U)) {
        uint32_t const flags = (tx_length != 0U) ? kI2C_TransferRepeatedStartFlag : kI2C_TransferDefaultFlag;
        status = i2c_transfer(address, kI2C_Read, response + PRO_BUS_RESPONSE_FIXED, rx_length, flags);
    }
    I2C_MasterDeinit(I2C3);

    if (status == kStatus_Success) {
        response[1] = PRO_BUS_OK;
        put_u16(response + 2U, rx_length);
        returned = rx_length;
        goto i2c_release_pins;
    }
    bool const nack = (status == kStatus_I2C_Nak) || (status == kStatus_I2C_Addr_Nak);
    response[1] = nack ? PRO_BUS_NACK : PRO_BUS_TRANSFER_ERROR;
    // A NACK ends with the SDK's STOP; a timeout or a lost arbitration doesn't.
    if (!nack) {
        i2c_clear_bus();
    }

i2c_release_pins:
    IOCON_PinMuxSet(IOCON, 0U, I2C_SDA_PIN, IOCON_FUNC0 | I2C_PIN_MODE);
    IOCON_PinMuxSet(IOCON, 0U, I2C_SCL_PIN, IOCON_FUNC0 | I2C_PIN_MODE);
    CLOCK_AttachClk(kNONE_to_FLEXCOMM3);
    return (consumed << 16U) | (PRO_BUS_RESPONSE_FIXED + returned);
}
