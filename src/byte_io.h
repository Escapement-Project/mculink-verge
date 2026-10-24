#ifndef BYTE_IO_H
#define BYTE_IO_H

#include <stdint.h>

static inline uint32_t read_u32_le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

#endif
