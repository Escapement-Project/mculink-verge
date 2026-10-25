#ifndef PRO_BUS_H
#define PRO_BUS_H

#include <stdbool.h>
#include <stdint.h>

typedef enum : uint8_t {
    PRO_BUS_OK = 0,
    PRO_BUS_UNAVAILABLE = 1,
    PRO_BUS_INVALID = 2,
    PRO_BUS_BUSY = 3,
    PRO_BUS_TRANSFER_ERROR = 4,
    PRO_BUS_NACK = 5,
} pro_bus_status_t;

static constexpr uint32_t PRO_SPI_REQUEST_FIXED = 8U;
static constexpr uint32_t PRO_I2C_REQUEST_FIXED = 10U;
static constexpr uint32_t PRO_BUS_RESPONSE_FIXED = 4U;

/** Whether Pro SPI and I2C are available with the startup jumpers. */
bool pro_bus_available(void);

/** Process vendor command 0x91: one full-duplex SPI transaction. */
[[nodiscard]] uint32_t pro_spi_command(const uint8_t* request, uint8_t* response);

/** Process vendor command 0x92: one I2C write, read, or write/read transaction. */
[[nodiscard]] uint32_t pro_i2c_command(const uint8_t* request, uint8_t* response);

#endif
