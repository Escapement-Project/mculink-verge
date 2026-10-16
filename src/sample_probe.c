/** Vendor 0x87: where a read has to sample SWDIO, measured rather than assumed.
 *
 * Every production read samples just before its rising edge, as ARM's code does. The target
 * drives on that edge, so the bit the next sample needs was launched one clock earlier, and the
 * question is how long it takes to arrive: out through the probe's pin and buffer, through the
 * target, and back in through the buffer and the GPIO input.
 *
 * This runs DPIDR reads with the sample placed by the host, in one of two shapes:
 *
 * - **Lead.** Production's own pattern: the clock stays high for `h` cycles after its rising
 *   edge, then the falling-edge store, the delay loop and the sample, then whatever is left of the
 *   period. The sample needs the bit the previous rising edge launched, so shortening `h` until
 *   reads come back one bit behind finds how soon after an edge that bit arrives. With `h` equal
 *   to the fill this is production's placement exactly.
 * - **After.** The sample `at` cycles after the rising-edge store, still in the high phase. It
 *   needs the bit the previous edge launched, and reads come back one bit ahead once the new one
 *   has arrived, which bounds the window from the other side.
 *
 * Exact cycles come from a computed branch into a run of single-cycle instructions. The branch
 * costs the same however far in it lands, and a second run takes up whatever the first did not,
 * so moving the sample leaves the clock period where it was. A dry run moves no pin and times the
 * same clock with the cycle counter, which is how the host learns the period it asked for.
 *
 * Request, after the id:
 *
 *     [0]     low      delay-loop iterations in the low phase, less one
 *     [1]     fill     filler cycles in a clock, at most SAMPLE_FILL_MAX
 *     [2]     at       0..fill: after the edge by that many cycles; SAMPLE_LEAD | h: lead with a
 *                      high phase of h, h at most fill; SAMPLE_BEFORE_EDGE: lead with h = fill
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
#include <stddef.h>
#include <string.h>

#include "DAP_config.h"

#include "DAP.h"
#include "cjtag.h"
#include "sample_probe.h"

// The filler's repeat count in the asm, which has to be a bare number.
#define FILL_REPT 32
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
    volatile uint32_t* gpio; // SET for the clock's port; CLR follows it
    volatile uint8_t* pin;   // SWDIO's byte register
    uint32_t clk;            // the SWCLK mask, or zero for a dry run
    uint32_t low;            // delay-loop iterations, at least one
    uint32_t skip1;          // filler bytes skipped by a read clock's first run
    uint32_t skip2;          // and by its second, so the two add up to the fill
    uint32_t skip_fill;      // for a clock that does not read
    uint32_t skip_none;
};

// CLR sits 0x80 past SET in every port's block, which lets one base register reach both.
#define GPIO_CLR_OFFSET 0x80
_Static_assert(offsetof(GPIO_Type, CLR) - offsetof(GPIO_Type, SET) == GPIO_CLR_OFFSET,
               "the GPIO layout the clock stores assume");
#define ASM_RISE "str %[clk], [%[gpio]]\n\t"
#define ASM_FALL "str %[clk], [%[gpio], #" STR(GPIO_CLR_OFFSET) "]\n\t"
#define ASM_LOW "mov %[n], %[low]\n\t1: subs %[n], #1\n\tbne 1b\n\t"
#define ASM_SAMPLE "ldrb %[bit], [%[pin]]\n\t"

/* `add pc` branches to its own address plus four plus the register, so the 16-bit nop after it is
 * never executed and every 4-byte filler skipped is one cycle fewer. */
#define ASM_FILL(skip)            \
    "add pc, %[" skip "]\n\t"     \
    "nop\n\t"                     \
    ".rept " STR(FILL_REPT) "\n\t" \
    "mov.w r12, r12\n\t"          \
    ".endr\n\t"

/* The operands come in as plain values, so the compiler can keep them in registers across a
 * whole transfer. Read through the struct, each clock reloaded six of them between its rising
 * edge and the next sample, which is the interval the lead shape exists to shorten. */
struct regs {
    volatile uint32_t* gpio;
    volatile uint8_t* pin;
    uint32_t clk;
    uint32_t low;
};

#define ASM_IN [gpio] "r"(r.gpio), [pin] "r"(r.pin), [clk] "r"(r.clk), [low] "r"(r.low)

/** The after shape: the sample sits in the high phase, `s1`'s worth of filler past the edge. */
__attribute__((always_inline)) static inline uint32_t clock_read_after(struct regs r, uint32_t s1, uint32_t s2) {
    uint32_t bit;
    uint32_t n;
    __asm volatile(ASM_FALL ASM_LOW ASM_RISE ASM_FILL("s1") ASM_SAMPLE ASM_FILL("s2")
                   : [bit] "=&r"(bit), [n] "=&r"(n)
                   : ASM_IN, [s1] "r"(s1), [s2] "r"(s2)
                   : "r12", "cc", "memory");
    return bit;
}

/* The lead shape: the rest of the high phase, then the falling edge, production's delay loop and
 * the sample, folded straight into its bit; then the rest of the low phase and the rising edge
 * that launches the next bit. Unrolled, so nothing but the filler and production's own pattern
 * sits between a rising edge and the sample after it. Looped, the back-branch and the bit
 * bookkeeping put seven cycles there, more than production has. */
#define LEAD_CLOCK(dst, shift)                                                                     \
    __asm volatile(ASM_FILL("s1") ASM_FALL ASM_LOW ASM_SAMPLE                                     \
                   "orr %[acc], %[acc], %[bit], lsl #" #shift "\n\t" ASM_FILL("s2") ASM_RISE       \
                   : [acc] "+r"(dst), [bit] "=&r"(bit), [n] "=&r"(n)                               \
                   : ASM_IN, [s1] "r"(s1), [s2] "r"(s2)                                           \
                   : "r12", "cc", "memory")

#define LEAD_CLOCK_8(dst, base)        \
    LEAD_CLOCK(dst, base + 0);         \
    LEAD_CLOCK(dst, base + 1);         \
    LEAD_CLOCK(dst, base + 2);         \
    LEAD_CLOCK(dst, base + 3);         \
    LEAD_CLOCK(dst, base + 4);         \
    LEAD_CLOCK(dst, base + 5);         \
    LEAD_CLOCK(dst, base + 6);         \
    LEAD_CLOCK(dst, base + 7)

struct lead_bits {
    uint32_t ack;
    uint32_t word;
    uint32_t parity;
};

/** The acknowledge, data and parity of one read in the lead shape: 36 clocks. */
static PROBE_CODE void lead_read(struct regs r, uint32_t s1, uint32_t s2, struct lead_bits* out) {
    uint32_t ack = 0U;
    uint32_t word = 0U;
    uint32_t parity = 0U;
    uint32_t bit;
    uint32_t n;
    LEAD_CLOCK(ack, 0);
    LEAD_CLOCK(ack, 1);
    LEAD_CLOCK(ack, 2);
    LEAD_CLOCK_8(word, 0);
    LEAD_CLOCK_8(word, 8);
    LEAD_CLOCK_8(word, 16);
    LEAD_CLOCK_8(word, 24);
    LEAD_CLOCK(parity, 0);
    out->ack = ack;
    out->word = word;
    out->parity = parity;
}
#define LEAD_CLOCKS 36U

/** A clock that drives SWDIO first, as SW_WRITE_BIT does, then the same edges as a read. */
__attribute__((always_inline)) static inline void clock_write(struct regs r, uint32_t level, uint32_t s1,
                                                              uint32_t s2) {
    uint32_t n;
    __asm volatile("strb %[dio], [%[pin]]\n\t" ASM_FALL ASM_LOW ASM_RISE ASM_FILL("s1") ASM_FILL("s2")
                   : [n] "=&r"(n)
                   : ASM_IN, [dio] "r"(level), [s1] "r"(s1), [s2] "r"(s2)
                   : "r12", "cc", "memory");
}

__attribute__((always_inline)) static inline void clock_plain(struct regs r, uint32_t s1, uint32_t s2) {
    uint32_t n;
    __asm volatile(ASM_FALL ASM_LOW ASM_RISE ASM_FILL("s1") ASM_FILL("s2")
                   : [n] "=&r"(n)
                   : ASM_IN, [s1] "r"(s1), [s2] "r"(s2)
                   : "r12", "cc", "memory");
}

/** The data phase, and also what the dry run times, so the two clock at the same rate. */
__attribute__((always_inline)) static inline uint32_t read_word_after(struct regs r, uint32_t s1, uint32_t s2) {
    uint32_t val = 0U;
    // Unrolled, each clock carries its own two filler runs: 9 KB of SRAM the after shape can do without.
#pragma GCC unroll 1
    for (uint32_t i = 0U; i < 32U; i++) {
        val |= clock_read_after(r, s1, s2) << i;
    }
    return val;
}

/** One DPIDR read. The acknowledge in bits 0-2, and bit 3 set if the parity was wrong. */
__attribute__((always_inline)) static inline uint32_t read_dpidr(const struct timing* t, bool after,
                                                                 uint32_t* word) {
    const struct regs r = {t->gpio, t->pin, t->clk, t->low};
    const uint32_t s1 = t->skip1;
    const uint32_t s2 = t->skip2;
    const uint32_t fill = t->skip_fill;
    const uint32_t none = t->skip_none;
#pragma GCC unroll 1
    for (uint32_t i = 0U; i < 8U; i++) {
        clock_write(r, (DPIDR_READ_REQUEST >> i) & 1U, fill, none);
    }
    PIN_SWDIO_OUT_DISABLE();
    clock_plain(r, fill, none); // turnaround; DAP_SWD_Configure is ignored here and one cycle assumed

    struct lead_bits got = {0U, 0U, 0U};
    if (after) {
#pragma GCC unroll 1
        for (uint32_t i = 0U; i < 3U; i++) {
            got.ack |= clock_read_after(r, s1, s2) << i;
        }
        got.word = read_word_after(r, s1, s2);
        got.parity = clock_read_after(r, s1, s2);
    } else {
        lead_read(r, s1, s2, &got);
    }
    const uint32_t ack = got.ack;
    const uint32_t val = got.word;
    const uint32_t parity = got.parity;

    clock_plain(r, fill, none); // turnaround
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

static PROBE_CODE uint32_t read_lead(const struct timing* t, uint32_t* word) {
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const uint32_t result = read_dpidr(t, false, word);
    __set_PRIMASK(primask);
    return result;
}

/* Cycles for 32 read clocks with no pin moving: the clock is a zero mask. */

static PROBE_CODE uint32_t dry_after(const struct timing* t) {
    const struct regs r = {t->gpio, t->pin, t->clk, t->low};
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const uint32_t start = DWT->CYCCNT;
    volatile uint32_t sink = read_word_after(r, t->skip1, t->skip2);
    const uint32_t cycles = DWT->CYCCNT - start;
    __set_PRIMASK(primask);
    (void)sink;
    return cycles;
}

static PROBE_CODE uint32_t dry_lead(const struct timing* t) {
    const struct regs r = {t->gpio, t->pin, t->clk, t->low};
    struct lead_bits sink;
    const uint32_t primask = __get_PRIMASK();
    __disable_irq();
    const uint32_t start = DWT->CYCCNT;
    lead_read(r, t->skip1, t->skip2, &sink);
    const uint32_t cycles = DWT->CYCCNT - start;
    __set_PRIMASK(primask);
    // 36 clocks timed, reported as 32 so both shapes answer in the same unit.
    return (cycles * 32U + LEAD_CLOCKS / 2U) / LEAD_CLOCKS;
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
    const bool after = (at & SAMPLE_LEAD) == 0U;
    // Where the first filler run stops: `at` after the edge, or the lead's high phase.
    const uint32_t first = after ? at : (at == SAMPLE_BEFORE_EDGE) ? fill : (at & ~SAMPLE_LEAD);

    memset(response + 1, 0, SAMPLE_REPLY_LEN - 1U);
    if (fill > SAMPLE_FILL_MAX || first > fill || DAP_Data.debug_port != DAP_PORT_SWD || cJTAG_Active()) {
        response[1] = DAP_ERROR;
        return (SAMPLE_REQUEST_LEN << 16) | SAMPLE_REPLY_LEN;
    }

    struct timing t = {
        .gpio = &DBGIF_TCK_SWCLK_GPIO->SET[DBGIF_TCK_SWCLK_PORT],
        .pin = &DBGIF_TMS_SWDIO_GPIO->B[DBGIF_TMS_SWDIO_PORT][DBGIF_TMS_SWDIO_PIN],
        .clk = 0U,
        .low = low + 1U,
        .skip1 = (SAMPLE_FILL_MAX - first) * 4U,
        .skip2 = (SAMPLE_FILL_MAX - (fill - first)) * 4U,
        .skip_fill = (SAMPLE_FILL_MAX - fill) * 4U,
        .skip_none = SAMPLE_FILL_MAX * 4U,
    };
    const uint32_t cycles = after ? dry_after(&t) : dry_lead(&t);

    t.clk = 1U << DBGIF_TCK_SWCLK_PIN;
    uint32_t good = 0U;
    uint32_t bad_ack = 0U;
    uint32_t bad_word = 0U;
    uint32_t last_ack = 0U;
    uint32_t last_word = 0U;
    for (uint32_t i = 0U; i < count; i++) {
        uint32_t word;
        const uint32_t result = after ? read_after(&t, &word) : read_lead(&t, &word);
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
