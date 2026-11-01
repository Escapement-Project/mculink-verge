#ifndef ENERGY_IDENTITY_H
#define ENERGY_IDENTITY_H

#include <stdbool.h>
#include <stdint.h>

#define ENERGY_IDENTITY_BYTES 24U

typedef struct {
    uint8_t bytes[ENERGY_IDENTITY_BYTES];
} energy_capture_id_t;

__attribute__((always_inline)) static inline bool energy_identity_matches(energy_capture_id_t const* current, uint8_t const* expected) {
    uint8_t difference = 0U;
    uint8_t counter = 0U;
    for (uint32_t i = 0U; i < ENERGY_IDENTITY_BYTES; i++) {
        difference |= current->bytes[i] ^ expected[i];
        if (i >= 16U) {
            counter |= expected[i];
        }
    }
    return (difference == 0U) && (counter != 0U);
}

/** CPU 1's cycle counter must be running before this call. */
[[nodiscard]] bool energy_identity_boot(uint8_t boot[16]);

#endif
