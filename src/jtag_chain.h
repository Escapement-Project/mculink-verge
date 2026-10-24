#ifndef JTAG_CHAIN_H
#define JTAG_CHAIN_H

#include <stdbool.h>
#include <stdint.h>

static inline bool jtag_chain_valid(const uint8_t* lengths, uint32_t count, uint32_t capacity) {
    if (count > capacity) {
        return false;
    }
    for (uint32_t i = 0U; i < count; i++) {
        if (lengths[i] == 0U) {
            return false;
        }
    }
    return true;
}

#endif
