#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "dap_block.h"

int main(void) {
    uint8_t* request = malloc(4U);
    assert(request != nullptr);
    request[0] = 0U;
    for (uint32_t count = 0U; count <= 126U; count++) {
        request[1] = (uint8_t)count;
        request[2] = 0U;
        request[3] = 0x0DU;
        // A partial write still consumes its complete request before the next batch member.
        uint32_t const packed = dap_block_result(request, 3U);
        assert((packed >> 16U) == 4U + count * 4U && (packed & 0xFFFFU) == 3U);
        for (uint32_t completed = 0U; completed <= count; completed++) {
            request[3] = 0x0FU;
            uint32_t const read = dap_block_result(request, 3U + completed * 4U);
            assert((read >> 16U) == 4U && (read & 0xFFFFU) == 3U + completed * 4U);
        }
    }
    request[1] = request[2] = 0xFFU;
    request[3] = 0x06U;
    assert(dap_block_result(request, 7U) == ((4U << 16U) | 7U));
    free(request);
}
