#ifndef PRO_BUS_H
#define PRO_BUS_H

#include <stdbool.h>
#include <stdint.h>

/** Whether Pro SPI and I2C are available with the startup jumpers. */
bool pro_bus_available(void);

/** Process vendor command 0x91: one full-duplex SPI transaction. */
uint32_t pro_spi_command(const uint8_t* request, uint8_t* response);

/** Process vendor command 0x92: one I2C write, read, or write/read transaction. */
uint32_t pro_i2c_command(const uint8_t* request, uint8_t* response);

#endif
