/* Sweeps the generators over their configurations and prints a hash of what each builds. */
#include <stdint.h>
#include "DAP_config.h"
#include "DAP.h"
#include "jtag_gen.h"
#include "swd_gen.h"

void out(const char* s);
DAP_Data_t DAP_Data;

static uint32_t fnv(const uint8_t* code, uint32_t bytes) {
    uint32_t h = 2166136261U;
    for (uint32_t k = 0; k < bytes; k++) {
        h = (h ^ code[k]) * 16777619U;
    }
    return h;
}

static char* hex(char* p, uint32_t v) {
    for (int i = 7; i >= 0; i--) {
        *p++ = "0123456789abcdef"[(v >> (4 * i)) & 0xF];
    }
    return p;
}

void sweep(void) {
    static const uint32_t hz[] = {100000, 1000000, 3750000, 10000000, 11538462, 12500000, 13636364, 15000000,
                                  16666667, 18750000, 21428572, 25000000, 30000000, 37500000, 50000000};
    char line[64];
    for (unsigned t = 1; t <= 4; t++)
    for (unsigned d = 0; d <= 1; d++)
    for (unsigned idle = 0; idle <= 8; idle += 4)
    for (unsigned lat = 1; lat <= 24; lat += 3)
    for (unsigned i = 0; i < sizeof hz / sizeof hz[0]; i++) {
        DAP_Data.swd_conf.turnaround = (uint8_t)t;
        DAP_Data.swd_conf.data_phase = (uint8_t)d;
        DAP_Data.transfer.idle_cycles = (uint8_t)idle;
        swd_gen_latency = lat;
        swd_gen_clock_hz = hz[i];
        const bool ok = swd_gen_ready();
        uint32_t h = 2166136261U;
        const uint8_t* code = (const uint8_t*)SWD_GEN_BASE;
        for (uint32_t k = 0; ok && k < swd_gen_info.code_bytes; k++) {
            h = (h ^ code[k]) * 16777619U;
        }
        char* p = line;
        p = hex(p, (t << 24) | (d << 20) | (idle << 12) | (lat << 4));
        *p++ = ' ';
        p = hex(p, hz[i]);
        *p++ = ' ';
        *p++ = ok ? '1' : '0';
        *p++ = ' ';
        p = hex(p, swd_gen_info.code_bytes);
        *p++ = ' ';
        p = hex(p, h);
        *p++ = '\n';
        *p = 0;
        out(line);
    }
    for (unsigned lat = 1; lat <= 24; lat++)
    for (unsigned p = 3; p <= 44; p++) {
        static const uint32_t far[] = {75, 150, 1500, 150000};
        const uint32_t period = p <= 40 ? p : far[p - 41];
        jtag_gen_latency = lat;
        jtag_gen_clock_hz = (CPU_CLOCK + period - 1U) / period;
        const bool ok = jtag_gen_ready();
        const uint32_t h = ok ? fnv((const uint8_t*)JTAG_GEN_BASE, jtag_gen_info.code_bytes) : 2166136261U;
        char* q = line;
        q = hex(q, 0x4A000000U | (lat << 4));
        *q++ = ' ';
        q = hex(q, jtag_gen_clock_hz);
        *q++ = ' ';
        *q++ = ok ? '1' : '0';
        *q++ = ' ';
        q = hex(q, jtag_gen_info.code_bytes);
        *q++ = ' ';
        q = hex(q, h);
        *q++ = ' ';
        q = hex(q, jtag_gen_info.frame);
        *q++ = '\n';
        *q = 0;
        out(line);
    }
}
