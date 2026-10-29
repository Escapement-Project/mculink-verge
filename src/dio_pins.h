/** The Pro's DIO pins (src/dio.h), for both cores: CPU 1 drives them and CPU 0 times their edges
 *  for the energy stream (src/dio_edges.c). */
#ifndef DIO_PINS_H
#define DIO_PINS_H

#include <stdint.h>

/** DIO0 to DIO3 are the HS SPI's SSEL0, MOSI, SCK and MISO pins, P0_20, P0_26, P1_2 and P1_3. */
#define DIO_PINS 0x0FU

/** The pin interrupt select (INPUTMUX PINTSEL) numbers of DIO0 to DIO3: port * 32 + pin. */
#define DIO_PINTSEL {20U, 26U, 34U, 35U}

/** Bit n DIOn, from the two GPIO ports' PIN registers. */
static inline uint8_t dio_pins_levels(uint32_t port0, uint32_t port1) {
    return (uint8_t)(((port0 >> 20U) & 1U) | (((port0 >> 26U) & 1U) << 1U) | (((port1 >> 2U) & 1U) << 2U) |
                     (((port1 >> 3U) & 1U) << 3U));
}

#endif
