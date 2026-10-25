#include <assert.h>
#include <stdint.h>
#include "vcom_line.h"

int main(void) {
    uint32_t osr = 123U, brg = 456U;
    assert(vcom_line_divisors(96000000U, 115200U, 0U, 0U, 8U, &osr, &brg));
    assert(osr == 15U && brg == 51U);
    assert(vcom_line_divisors(96000000U, 19200000U, 2U, 2U, 7U, &osr, &brg));
    assert(osr == 4U && brg == 0U);
    uint32_t const rates[] = {0U, 1U, 30000000U, UINT32_MAX};
    for (unsigned i = 0U; i < sizeof(rates) / sizeof(rates[0]); i++) {
        osr = 123U; brg = 456U;
        assert(!vcom_line_divisors(96000000U, rates[i], 0U, 0U, 8U, &osr, &brg));
        assert(osr == 123U && brg == 456U);
    }
    for (unsigned stop = 0U; stop <= UINT8_MAX; stop++) {
        for (unsigned parity = 0U; parity <= UINT8_MAX; parity++) {
            for (unsigned bits = 0U; bits <= 9U; bits++) {
                bool const valid = (stop == 0U || stop == 2U) && parity <= 2U && (bits == 7U || bits == 8U);
                assert(vcom_line_divisors(96000000U, 115200U, stop, parity, bits, &osr, &brg) == valid);
            }
        }
    }
    assert(!vcom_line_divisors(0U, 115200U, 0U, 0U, 8U, &osr, &brg));
    uint32_t seed = 13U;
    for (unsigned i = 0U; i < 100000U; i++) {
        seed = seed * 1664525U + 1013904223U;
        uint32_t const clock = seed;
        seed = seed * 1664525U + 1013904223U;
        uint32_t const baud = seed;
        if (vcom_line_divisors(clock, baud, 0U, 0U, 8U, &osr, &brg)) {
            assert(osr >= 4U && osr <= 15U && brg <= UINT16_MAX);
            uint32_t const actual = clock / ((osr + 1U) * (brg + 1U));
            uint32_t const diff = actual > baud ? actual - baud : baud - actual;
            assert(diff <= (baud / 100U) * 3U);
        }
    }
}
