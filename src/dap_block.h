#ifndef DAP_BLOCK_H
#define DAP_BLOCK_H

#include <stdint.h>

/** Consumed length includes every requested write, even if the transfer stopped early.
 *  The caller passes the block header after the command ID. */
static inline uint32_t dap_block_result(const uint8_t* request, uint32_t response_bytes) {
    uint32_t consumed = 4U;
    if ((request[3] & 2U) == 0U) {
        uint32_t const count = (uint32_t)request[1] | ((uint32_t)request[2] << 8U);
        consumed += 4U * count;
    }
    return (consumed << 16U) | response_bytes;
}

#endif
