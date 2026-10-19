/** Recognising a host connecting to an SW-DP from the DAP_SWJ_Sequence bits it clocks out, free of
 *  hardware so tools/swd_latency_test.c can run it on the host.
 *
 * A connection is a line reset, at least 50 clocks with SWDIO high and then a low one, then on a
 * multidrop wire a TARGETSEL write sent as raw bits, then a DPIDR read (ADIv5 B4). probe-rs sends
 * the TARGETSEL write as its request, five clocks high for the turnarounds and the acknowledge
 * nobody drives, the value and its parity (debug_port_connect in its arm/sequences.rs). The JTAG
 * to SWD switch is a line reset either side of its select word, and so ends in one too.
 */
#ifndef SWD_CONNECT_H
#define SWD_CONNECT_H

#include <stdbool.h>
#include <stdint.h>

#define SWD_CONNECT_RESET_ONES 50U

/** TARGETSEL's request as clocked, start bit first: DP write of 0xC. */
#define SWD_CONNECT_TARGETSEL_REQUEST 0x99U
/** Its request, the five clocks after it, the value and the parity. */
#define SWD_CONNECT_TARGETSEL_BITS 46U

#define SWD_CONNECT_IDLE 0U        // nothing to follow
#define SWD_CONNECT_AFTER_RESET 1U // idle clocks after a line reset
#define SWD_CONNECT_PACKET 2U      // a request after a line reset, which may be TARGETSEL

struct swd_connect {
    uint32_t ones;  // consecutive high clocks
    uint32_t state; // SWD_CONNECT_*
    uint64_t packet;
    uint32_t packet_bits;
    bool reset;     // a line reset since the host's last DAP_Transfer
    bool multidrop; // a TARGETSEL write followed it
    uint32_t targetsel;
};

static inline void swd_connect_bit(struct swd_connect* c, uint32_t bit) {
    if (c->state == SWD_CONNECT_PACKET) {
        c->packet |= (uint64_t)bit << c->packet_bits;
        if (++c->packet_bits == SWD_CONNECT_TARGETSEL_BITS) {
            const uint32_t value = (uint32_t)(c->packet >> 13);
            const uint32_t parity = (uint32_t)(c->packet >> 45) & 1U;
            // The five clocks between the request and the value are not checked: the target
            // ignores them.
            if (((c->packet & 0xFFU) == SWD_CONNECT_TARGETSEL_REQUEST) &&
                (parity == ((uint32_t)__builtin_parity(value)))) {
                c->multidrop = true;
                c->targetsel = value;
            }
            c->state = SWD_CONNECT_IDLE;
        }
    } else if ((c->state == SWD_CONNECT_AFTER_RESET) && (bit != 0U) && (c->ones == 0U)) {
        // A start bit: the request that follows a line reset.
        c->state = SWD_CONNECT_PACKET;
        c->packet = 1U;
        c->packet_bits = 1U;
    }

    if (bit != 0U) {
        c->ones++;
        return;
    }
    if (c->ones >= SWD_CONNECT_RESET_ONES) {
        c->reset = true;
        c->multidrop = false;
        c->targetsel = 0U;
        c->state = SWD_CONNECT_AFTER_RESET;
    }
    c->ones = 0U;
}

/** DAP_SWJ_Sequence's `count` clocks of `data`, least significant bit first. */
static inline void swd_connect_sequence(struct swd_connect* c, const uint8_t* data, uint32_t count) {
    for (uint32_t i = 0U; i < count; i++) {
        swd_connect_bit(c, (data[i / 8U] >> (i % 8U)) & 1U);
    }
}

#endif
