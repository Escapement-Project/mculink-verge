#ifndef BYTE_IO_H
#define BYTE_IO_H

#include <stdint.h>

static inline uint16_t read_u16_le(const uint8_t* bytes) {
    return (uint16_t)((uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U));
}

static inline uint32_t read_u32_le(const uint8_t* bytes) {
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8U) | ((uint32_t)bytes[2] << 16U) |
           ((uint32_t)bytes[3] << 24U);
}

/** The writers store a byte at a time, so `out` needs no alignment, and return the byte after
 *  what they wrote. */
static inline uint8_t* put_u16_le(uint8_t* out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    return out + 2;
}

static inline uint8_t* put_u32_le(uint8_t* out, uint32_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8U);
    out[2] = (uint8_t)(value >> 16U);
    out[3] = (uint8_t)(value >> 24U);
    return out + 4;
}

static inline uint8_t* put_u64_le(uint8_t* out, uint64_t value) {
    out = put_u32_le(out, (uint32_t)value);
    return put_u32_le(out, (uint32_t)(value >> 32U));
}

#endif
