/** Vendor 0x87: where a read has to sample SWDIO, measured rather than assumed.
 *
 * Every production read samples just before its rising edge, as ARM's code does. The target
 * drives on that edge, so its bit has one clock period to travel out through the probe's pin and
 * buffer, through the target, and back in through the buffer and the GPIO input. On the bench
 * that trip is most of 60 ns, and reads fail as soon as the period gets that short.
 *
 * This runs DPIDR reads with the sample placed by the host instead: before the rising edge as
 * production does, or an exact number of cycles after it. After the edge the pin still holds the
 * previous edge's bit until the new one has made the same trip, so sweeping the position finds
 * the length of the trip directly, and with it the window a production read can be moved into.
 *
 * Exact cycles come from a computed branch into a run of single-cycle instructions. The branch
 * costs the same however far in it lands, and a second run after the sample takes up whatever
 * the first did not, so moving the sample leaves the clock period where it was. A dry run moves
 * no pin and times the same clock with the cycle counter, which is how the host learns the
 * period it asked for.
 *
 * Request, after the id:
 *
 *     [0]     low      delay-loop iterations in the low phase, less one
 *     [1]     fill     filler cycles in the high phase, at most SAMPLE_FILL_MAX
 *     [2]     at       filler cycles before the sample, at most fill; SAMPLE_BEFORE_EDGE for production's place
 *     [3..4]  count    reads, little-endian
 *     [5..8]  expected the DPIDR value a correct read returns
 *
 * Reply, after the id:
 *
 *     [0]      status          DAP_ERROR if a field is out of range or the port is not plain SWD
 *     [1..2]   good            acknowledge OK, parity right, word as expected
 *     [3..4]   bad acknowledge
 *     [5..6]   bad word        acknowledge OK, but the word or its parity wrong
 *     [7]      the last bad acknowledge
 *     [8..11]  the last bad word
 *     [12..15] cycles a dry data phase takes, which is 32 read clocks
 *
 * Every read is clocked to the end whatever its acknowledge looked like. The target answered OK
 * whatever the probe made of it, and stopping early would desynchronise the two.
 */
#include <stdbool.h>
#include <string.h>

#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "sample_probe.h"

// The filler's repeat count in the asm, which has to be a bare number.
#define FILL_REPT 48
_Static_assert(FILL_REPT == SAMPLE_FILL_MAX, "the asm filler and SAMPLE_FILL_MAX disagree");
#define STR_(x) #x
#define STR(x) STR_(x)

#define SAMPLE_REQUEST_LEN 10U
#define SAMPLE_REPLY_LEN 17U

// A DP read of DPIDR, start bit first: start, APnDP 0, RnW 1, A2 0, A3 0, parity 1, stop, park.
#define DPIDR_READ_REQUEST 0xA5U

/** In SRAM like the bit paths, so no fetch pays flash wait states, but after all of them.
 *
 * The linker takes every .ramfunc section before any .data one, so this cannot move a bit path.
 * Marked .ramfunc it could land ahead of them, which src/hot.h measured costing throughput.
 */
#define PROBE_CODE __attribute__((noinline, section(".data_sample_probe")))

struct timing {
    volatile uint32_t* set;
    volatile uint32_t* clr;
    volatile uint8_t* pin;
    uint32_t clk;         // the SWCLK mask, or zero for a dry run
    uint32_t low;         // delay-loop iterations, at least one
    uint32_t skip_at;     // filler bytes skipped before the sample
    uint32_t skip_rest;   // and after it, so the two runs add up to the fill
    uint32_t skip_fill;   // for a clock that does not sample after the edge
    uint32_t skip_none;
};

#define ASM_LOW "mov %[n], %[low]\n\t1: subs %[n], #1\n\tbne 1b\n\t"

/* `add pc` branches to its own address plus four plus the register, so the 16-bit nop after it is
 * never executed and every 4-byte filler skipped is one cycle fewer. */
#define ASM_FILL(skip)            \
    "add pc, %[" skip "]\n\t"     \
    "nop\n\t"                     \
    ".rept " STR(FILL_REPT) "\n\t" \
    "mov.w r12, r12\n\t"          \
    ".endr\n\t"

#define ASM_TIMING_IN                                                                           \
    [clk] "r"(t->clk), [clr] "r"(t->clr), [set] "r"(t->set), [pin] "r"(t->pin), [low] "r"(t->low)

__attribute__((always_inline)) static inline uint32_t clock_read(const struct timing* t, bool after) {
    uint32_t bit;
    uint32_t n;
    if (after) {
        __asm volatile("str %[clk], [%[clr]]\n\t" ASM_LOW "str %[clk], [%[set]]\n\t" ASM_FILL(
                           "s1") "ldrb %[bit], [%[pin]]\n\t" ASM_FILL("s2")
                       : [bit] "=&r"(bit), [n] "=&r"(n)
                       : ASM_TIMING_IN, [s1] "r"(t->skip_at), [s2] "r"(t->skip_rest)
                       : "r12", "cc", "memory");
    } else {
        __asm volatile("str %[clk], [%[clr]]\n\t" ASM_LOW "ldrb %[bit], [%[pin]]\n\t"
                       "str %[clk], [%[set]]\n\t" ASM_FILL("s1") ASM_FILL("s2")
                       : [bit] "=&r"(bit), [n] "=&r"(n)
                       : ASM_TIMING_IN, [s1] "r"(t->skip_fill), [s2] "r"(t->skip_none)
                       : "r12", "cc", "memory");
    }
    return bit;
}

/** A clock that drives SWDIO first, as SW_WRITE_BIT does, then the same edges as a read. */
__attribute__((always_inline)) static inline void clock_write(const struct timing* t, uint32_t level) {
    uint32_t n;
    __asm volatile("strb %[dio], [%[pin]]\n\tstr %[clk], [%[clr]]\n\t" ASM_LOW
                   "str %[clk], [%[set]]\n\t" ASM_FILL("s1") ASM_FILL("s2")
                   : [n] "=&r"(n)
                   : ASM_TIMING_IN, [dio] "r"(level), [s1] "r"(t->skip_fill), [s2] "r"(t->skip_none)
                   : "r12", "cc", "memory");
}

__attribute__((always_inline)) static inline void clock_plain(const struct timing* t) {
    uint32_t n;
    __asm volatile("str %[clk], [%[clr]]\n\t" ASM_LOW "str %[clk], [%[set]]\n\t" ASM_FILL("s1")
                       ASM_FILL("s2")
                   : [n] "=&r"(n)
                   : ASM_TIMING_IN, [s1] "r"(t->skip_fill), [s2] "r"(t->skip_none)
                   : "r12", "cc", "memory");
}

/** The data phase, and also what the dry run times, so the two clock at the same rate. */
__attribute__((always_inline)) static inline uint32_t read_word(const struct timing* t, bool after) {
    uint32_t val = 0U;
    // Unrolled, each clock carries its own two filler runs: 13 KB of SRAM for nothing.
#pragma GCC unroll 1
    for (uint32_t i = 0U; i < 32U; i++) {
        val |= clock_read(t, after) << i;
    }
    return val;
}

/** One DPIDR read. The acknowledge in bits 0-2, and bit 3 set if the parity was wrong. */
__attribute__((always_inline)) static inline uint32_t read_dpidr(const struct timing* t, bool after,
                                                                 uint32_t* word) {
#pragma GCC unroll 1
    for (uint32_t i = 0U; i < 8U; i++) {
        clock_write(t, (DPIDR_READ_REQUEST >> i) & 1U);
    }
    PIN_SWDIO_OUT_DISABLE();
    clock_plain(t); // turnaround; DAP_SWD_Configure is ignored here and one cycle assumed

    uint32_t ack = 0U;
#pragma GCC unroll 1
    for (uint32_t i = 0U; i < 3U; i++) {
        ack |= clock_read(t, after) << i;
    }
    const uint32_t val = read_word(t, after);
    const uint32_t parity = clock_read(t, after);

    clock_plain(t); // turnaround
    PIN_SWDIO_OUT_ENABLE();
    PIN_SWDIO_OUT(1U);

    *word = val;
    return ack | (((parity ^ (uint32_t)__builtin_parity(val)) & 1U) << 3);
}

/* Each read runs with interrupts masked. An interrupt between the rising edge and a sample after
 * it would make that sample late, and the sweep would then measure the interrupt. */

static PROBE_CODE uint32_t read_after(const struct timing* t, uint32_t* word) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const uint32_t result = read_dpidr(t, true, word);
    __set_PRIMASK(primask);
    return result;
}

static PROBE_CODE uint32_t read_before(const struct timing* t, uint32_t* word) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const uint32_t result = read_dpidr(t, false, word);
    __set_PRIMASK(primask);
    return result;
}

/** Cycles for one data phase with no pin moving: the clock is a zero mask. */
__attribute__((always_inline)) static inline uint32_t dry_cycles(const struct timing* t, bool after) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const uint32_t start = DWT->CYCCNT;
    const uint32_t sink = read_word(t, after);
    const uint32_t cycles = DWT->CYCCNT - start;
    __set_PRIMASK(primask);
    (void)sink;
    return cycles;
}

static PROBE_CODE uint32_t dry_after(const struct timing* t) {
    return dry_cycles(t, true);
}

static PROBE_CODE uint32_t dry_before(const struct timing* t) {
    return dry_cycles(t, false);
}

static void put16(uint8_t* out, uint32_t v) {
    out[0] = (uint8_t)v;
    out[1] = (uint8_t)(v >> 8);
}

static void put32(uint8_t* out, uint32_t v) {
    put16(out, v);
    put16(out + 2, v >> 16);
}

uint32_t sample_probe_command(const uint8_t* request, uint8_t* response) {
    const uint32_t low = request[1];
    const uint32_t fill = request[2];
    const uint32_t at = request[3];
    const uint32_t count = (uint32_t)request[4] | ((uint32_t)request[5] << 8);
    const uint32_t expected = (uint32_t)request[6] | ((uint32_t)request[7] << 8) |
                              ((uint32_t)request[8] << 16) | ((uint32_t)request[9] << 24);
    const bool after = (at != SAMPLE_BEFORE_EDGE);

    memset(response + 1, 0, SAMPLE_REPLY_LEN - 1U);
    if (fill > SAMPLE_FILL_MAX || (after && at > fill) || DAP_Data.debug_port != DAP_PORT_SWD ||
        cJTAG_Active()) {
        response[1] = DAP_ERROR;
        return (SAMPLE_REQUEST_LEN << 16) | SAMPLE_REPLY_LEN;
    }

    struct timing t = {
        .set = &DBGIF_TCK_SWCLK_GPIO->SET[DBGIF_TCK_SWCLK_PORT],
        .clr = &DBGIF_TCK_SWCLK_GPIO->CLR[DBGIF_TCK_SWCLK_PORT],
        .pin = &DBGIF_TMS_SWDIO_GPIO->B[DBGIF_TMS_SWDIO_PORT][DBGIF_TMS_SWDIO_PIN],
        .clk = 0U,
        .low = low + 1U,
        .skip_at = (SAMPLE_FILL_MAX - (after ? at : 0U)) * 4U,
        .skip_rest = (SAMPLE_FILL_MAX - (after ? fill - at : 0U)) * 4U,
        .skip_fill = (SAMPLE_FILL_MAX - fill) * 4U,
        .skip_none = SAMPLE_FILL_MAX * 4U,
    };
    const uint32_t cycles = after ? dry_after(&t) : dry_before(&t);

    t.clk = 1U << DBGIF_TCK_SWCLK_PIN;
    uint32_t good = 0U;
    uint32_t bad_ack = 0U;
    uint32_t bad_word = 0U;
    uint32_t last_ack = 0U;
    uint32_t last_word = 0U;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t word;
        const uint32_t result = after ? read_after(&t, &word) : read_before(&t, &word);
        const uint32_t ack = result & 7U;
        if (ack != DAP_TRANSFER_OK) {
            bad_ack++;
            last_ack = ack;
        } else if ((result & 8U) != 0U || word != expected) {
            bad_word++;
            last_word = word;
        } else {
            good++;
        }
    }

    response[1] = DAP_OK;
    put16(response + 2, good);
    put16(response + 4, bad_ack);
    put16(response + 6, bad_word);
    response[8] = (uint8_t)last_ack;
    put32(response + 9, last_word);
    put32(response + 13, cycles);
    return (SAMPLE_REQUEST_LEN << 16) | SAMPLE_REPLY_LEN;
}
