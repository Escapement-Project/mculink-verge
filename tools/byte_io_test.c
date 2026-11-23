#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "byte_io.h"

/** Guard bytes either side of a write; the writer must touch exactly `n` bytes between them. */
#define GUARD 4U
#define FILL 0xa5U

static void check_bytes(const uint8_t* buffer, uint32_t offset, const uint8_t* expected, uint32_t n) {
    for (uint32_t i = 0U; i < offset + GUARD; i++) {
        assert(buffer[i] == FILL);
    }
    assert(memcmp(buffer + offset + GUARD, expected, n) == 0);
    for (uint32_t i = 0U; i < GUARD; i++) {
        assert(buffer[offset + GUARD + n + i] == FILL);
    }
}

int main(void) {
    const uint32_t values[] = {0U, 0x7fffffffU, 0x80000000U, 0xffffffffU, 0xa58019feU};
    for (uint32_t offset = 0U; offset < 4U; offset++) {
        uint8_t* allocation = malloc(offset + 4U);
        assert(allocation != NULL);
        uint8_t* bytes = allocation + offset;
        for (uint32_t i = 0U; i < sizeof(values) / sizeof(values[0]); i++) {
            for (uint32_t b = 0U; b < 4U; b++) { bytes[b] = (uint8_t)(values[i] >> (8U * b)); }
            assert(read_u32_le(bytes) == values[i]);
            assert(read_u16_le(bytes) == (uint16_t)values[i]);
        }
        free(allocation);
    }

    // Each width once with every byte distinct, so a swapped or dropped byte shows.
    static const uint8_t u16[] = {0x02, 0x01};
    static const uint8_t u32[] = {0x04, 0x03, 0x02, 0x01};
    static const uint8_t u64[] = {0x08, 0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01};
    static const uint8_t top[] = {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80};
    for (uint32_t offset = 0U; offset < 8U; offset++) {
        uint8_t buffer[8U + 8U + (2U * GUARD)];
        uint8_t* const at = buffer + offset + GUARD;

        memset(buffer, FILL, sizeof buffer);
        assert(put_u16_le(at, 0x0102U) == at + 2);
        check_bytes(buffer, offset, u16, 2U);
        assert(read_u16_le(at) == 0x0102U);

        memset(buffer, FILL, sizeof buffer);
        assert(put_u32_le(at, 0x01020304U) == at + 4);
        check_bytes(buffer, offset, u32, 4U);
        assert(read_u32_le(at) == 0x01020304U);

        memset(buffer, FILL, sizeof buffer);
        assert(put_u64_le(at, 0x0102030405060708ULL) == at + 8);
        check_bytes(buffer, offset, u64, 8U);

        memset(buffer, FILL, sizeof buffer);
        assert(put_u64_le(at, 0x8000000000000000ULL) == at + 8);
        check_bytes(buffer, offset, top, 8U);
    }
}
