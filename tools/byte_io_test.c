#include <assert.h>
#include <stdint.h>
#include <stdlib.h>

#include "byte_io.h"

int main(void) {
    const uint32_t values[] = {0U, 0x7fffffffU, 0x80000000U, 0xffffffffU, 0xa58019feU};
    for (uint32_t offset = 0U; offset < 4U; offset++) {
        uint8_t* allocation = malloc(offset + 4U);
        assert(allocation != NULL);
        uint8_t* bytes = allocation + offset;
        for (uint32_t i = 0U; i < sizeof(values) / sizeof(values[0]); i++) {
            for (uint32_t b = 0U; b < 4U; b++) { bytes[b] = (uint8_t)(values[i] >> (8U * b)); }
            assert(read_u32_le(bytes) == values[i]);
        }
        free(allocation);
    }
}
