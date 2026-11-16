#ifndef DAP_PACKET_H
#define DAP_PACKET_H

#include <stdbool.h>
#include <stdint.h>

#include "pro_bus.h"

typedef struct {
    uint32_t consumed;
    uint32_t produced;
    uint32_t limit;
} dap_packet_plan_t;

static inline uint32_t dap_packet_u16(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8U); }

/** Preflight one command, excluding batches. Info is read-only and sized by its caller. */
static inline bool dap_packet_plan(const uint8_t* p, uint32_t length, uint32_t capacity, uint32_t info_size,
                                   dap_packet_plan_t* plan) {
    if (length == 0U) {
        return false;
    }
    uint32_t in = 1U;
    uint32_t out = 2U;
    uint32_t limit = 0U;
    switch (p[0]) {
    case 0x00:
        in = 2U;
        out = info_size;
        break;
    case 0x01:
        in = 3U;
        break;
    case 0x02:
        in = 2U;
        break;
    case 0x03:
        break;
    case 0x04:
        in = 6U;
        break;
    case 0x05: {
        if (length < 3U) {
            return false;
        }
        in = 3U;
        out = 3U;
        for (uint32_t i = 0U; i < p[2]; i++) {
            if (in == length) {
                return false;
            }
            uint32_t const request = p[in++];
            bool const read = (request & 2U) != 0U;
            bool const match = (request & (read ? 0x10U : 0x20U)) != 0U;
            if (!read || match) {
                if (length - in < 4U) {
                    return false;
                }
                in += 4U;
            }
            if (!match) {
                out += read ? 4U : 0U;
                out += (request & 0x80U) != 0U ? 4U : 0U;
            }
        }
        break;
    }
    case 0x06: {
        if (length < 5U || capacity < 4U) {
            return false;
        }
        uint32_t const count = dap_packet_u16(p + 2U);
        in = 5U;
        out = 4U;
        if ((p[4] & 2U) != 0U) {
            limit = (capacity - out) / 4U;
            if (limit > count) {
                limit = count;
            }
            out += 4U * limit;
        } else {
            if (count > (length - in) / 4U) {
                return false;
            }
            in += 4U * count;
        }
        break;
    }
    case 0x08:
        in = 6U;
        break;
    case 0x09:
        in = 3U;
        break;
    case 0x0a:
        out = 3U;
        break;
    case 0x10:
        in = 7U;
        break;
    case 0x11:
        in = 5U;
        break;
    case 0x12:
        if (length < 2U) {
            return false;
        }
        in = 2U + ((p[1] != 0U ? p[1] : 256U) + 7U) / 8U;
        break;
    case 0x13:
        in = 2U;
        break;
    case 0x14:
    case 0x1d:
        if (length < 2U) {
            return false;
        }
        in = 2U;
        for (uint32_t i = 0U; i < p[1]; i++) {
            if (in == length) {
                return false;
            }
            uint32_t const info = p[in++];
            uint32_t const clocks = (info & 0x3fU) != 0U ? info & 0x3fU : 64U;
            uint32_t const bytes = (clocks + 7U) / 8U;
            bool const input = (info & 0x80U) != 0U;
            if (p[0] == 0x14U || !input) {
                if (bytes > length - in) {
                    return false;
                }
                in += bytes;
            }
            if (input) {
                out += bytes;
            }
        }
        break;
    case 0x15:
        if (length < 2U) {
            return false;
        }
        in = 2U + p[1];
        break;
    case 0x16:
        in = 2U;
        out = 6U;
        break;
    case 0x17:
    case 0x18:
    case 0x1a:
        in = 2U;
        break;
    case 0x19:
        in = 5U;
        out = 5U;
        break;
    case 0x1b:
        out = 6U;
        break;
    case 0x1c:
        if (length < 3U || capacity < 4U) {
            return false;
        }
        in = 3U;
        limit = dap_packet_u16(p + 1U);
        if (limit > capacity - 4U) {
            limit = capacity - 4U;
        }
        out = 4U + limit;
        break;
    case 0x1e:
        if (length < 2U) {
            return false;
        }
        in = 2U;
        out = 1U + ((p[1] & 1U) ? 1U : 0U) + ((p[1] & 2U) ? 4U : 0U) + ((p[1] & 4U) ? 8U : 0U);
        break;
    case 0x82:
        out = 18U + sizeof(FIRMWARE_VERSION) - 1U;
        break;
#ifdef MARKER_PINS
    case 0x84:
        in = 2U;
        out = 3U;
        break;
#endif
    case 0x86:
        in = 3U;
        out = 7U;
        break;
    case 0x87:
        in = 10U;
        out = 17U;
        break;
#ifdef CODE_LAB
    case 0x88:
        if (length < 2U) {
            return false;
        }
        in = 2U;
        if (p[1] == 0U) {
            out = 19U;
        }
        if (p[1] == 1U || p[1] == 2U) {
            if (length < 7U) {
                return false;
            }
            in = 7U;
            uint32_t const bytes = dap_packet_u16(p + 5U);
            if (p[1] == 1U) {
                in += bytes;
            } else {
                out += bytes;
            }
        }
        if (p[1] == 3U) {
            in = 21U;
            out = 10U;
        }
        if (p[1] == 4U) {
            in = 6U;
        }
        break;
#endif
#ifdef SWD_CODEGEN
    case 0x89:
        in = 3U;
        out = 58U;
        break;
#endif
#ifdef JTAG_CODEGEN
    case 0x8a:
        in = 4U;
        out = 64U;
        break;
    case 0x8b:
        in = 2U;
        out = 4U;
        break;
    case 0x8d:
        in = 2U;
        out = 25U;
        break;
#endif
    case 0x8c:
        in = 2U;
        out = 3U;
        break;
    case 0x8e:
        // Seven bytes and a group mask each: four on the standalone board (dio.h DIO_GROUPS_MAX).
        in = 4U;
        out = 11U;
        break;
    case 0x8f:
        in = 2U;
        out = 3U;
        break;
    case 0x90:
        if (length < 2U) {
            return false;
        }
        switch (p[1]) {
        case 0U: case 8U: in = 6U; break;
        case 4U: case 6U: case 10U: case 14U: in = 3U; break;
        case 5U: case 9U: in = 7U; break;
        case 11U: case 13U: in = 26U; break;
        default: in = 2U; break;
        }
        switch (p[1]) {
        case 0U: case 5U: out = 7U; break;
        case 1U: out = 21U; break;
        case 2U: case 10U: case 11U: case 13U: case 14U: out = 3U; break;
        case 3U: out = 19U; break;
        case 4U: out = 28U; break;
        case 8U: case 9U: out = 31U; break;
        case 12U: out = 32U; break;
        default: out = 2U; break;
        }
        break;
    case 0x91:
        if (length < PRO_SPI_REQUEST_FIXED) {
            return false;
        }
        in = PRO_SPI_REQUEST_FIXED + dap_packet_u16(p + 6U);
        out = PRO_BUS_RESPONSE_FIXED + dap_packet_u16(p + 6U);
        break;
    case 0x92:
        if (length < PRO_I2C_REQUEST_FIXED) {
            return false;
        }
        in = PRO_I2C_REQUEST_FIXED + dap_packet_u16(p + 6U);
        out = PRO_BUS_RESPONSE_FIXED + dap_packet_u16(p + 8U);
        break;
    case 0x95:
        in = 2U;
        break;
    case 0x96:
        in = 3U;
        break;
    case 0x94:
        if (length < 6U) {
            return false;
        }
        in = 6U + p[5];
        out = 3U;
        break;
#ifdef DAP_TIMING_DIAGNOSTICS
    case 0x93:
        in = 2U;
        out = 67U;
        break;
#endif
    default:
        out = 1U;
        break;
    }
    if (in > length || out > capacity) {
        return false;
    }
    *plan = (dap_packet_plan_t){in, out, limit};
    return true;
}

#endif
