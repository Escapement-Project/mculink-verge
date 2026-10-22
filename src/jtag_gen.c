/** Generate the JTAG wire paths at run time, placed to the cycle: four-wire, and two-pin
 * (IEEE 1149.7 OScan1 and OScan2).
 *
 * CJTAGGEN.md has the wire this places by, measured on a CC1312R1. TIMING.md has the cycle rules,
 * and src/swd_gen.c is the SWD counterpart. In brief:
 *
 * - The generated function is 32 blocks of straight-line code, one JTAG clock each, entered at the
 *   block that leaves `count` of them to run. Every block has the same instructions in the same
 *   cycles and only their shift amounts differ, so the entry is a multiply away and no clock pays
 *   for a loop.
 * - Four-wire: a clock is a store to the masked port register that lowers TCK and sets TDI
 *   together, then a rising-edge store; TMS is set once for the call. TDO is on its pin from
 *   `latency` cycles after the falling edge that launched it until a few cycles past the next
 *   one, so a short clock samples it after that next edge and runs masked.
 * - Two-pin: a clock is a frame of three TCKC packets, nTDI and TMS from the probe, then TDO from
 *   the target. TMSC moves only while TCKC is low, because a move while it is high is an escape,
 *   which the target's adapter acts on: so a packet the probe drives is a falling-edge store, a
 *   data store and a rising-edge store. The probe lets go right after the fall that starts the TDO
 *   packet, buffer first, and takes the wire back right after the next fall, pin first. TDO stays
 *   on the pin past that fall, so the sample goes just before it, where an interrupt can only
 *   make it later; interrupts are masked only around the release, where one would leave the probe
 *   driving against the target, or for the whole call where a frame has no cycle to spare.
 * - No clock or packet is shorter than the period, and neither phase shorter than its share.
 */
#include "jtag_gen.h"

#include <stddef.h>
#include <string.h>

#include "DAP_config.h"

#include "DAP.h"
#include "gen.h"
#include "hot.h"
#include "thumb.h"

/* What the generated function keeps in registers. */
#define R_TDI 0U   // TDI, shifted so that block k sends bit k; the result at the end
#define R_SKIP 1U  // the count on entry; then the blocks skipped, 32 less it
#define R_TMS 2U   // TMS, 0 or 1; four-wire keeps the caller's port mask here after setting TMS
#define R_TXEN 3U  // two-pin: the TMSC buffer's direction pin. Four-wire: a scratch register
#define R_ACC 4U   // TDO, bit k from block k
#define R_ONE 5U   // TCK's mask: also the 1 a byte-register store takes
#define R_PIN 6U   // two-pin: TMSC's mask. Four-wire: TDI's
#define R_DELAY 7U // delay loops
#define R_GPIO 8U  // the byte pin registers
#define R_PORT 9U  // MPIN0, with SET0, CLR0, DIRSET0 and DIRCLR0 at the offsets below
#define R_DATA 10U // the next clock's TDI as the port wants it
#define R_ENTRY 11U
#define R_SAMPLE 12U
#define R_ZERO T_LR

#define TCK (1U << DBGIF_TCK_SWCLK_PIN)
#define TDI_PIN DBGIF_TDI_PIN
#define TDI (1U << TDI_PIN)
#define TDO_PIN DBGIF_TDO_SWO_PIN
#define TMS_PIN DBGIF_TMS_SWDIO_PIN
#define TMSC (1U << TMS_PIN)
#define TXEN (1U << DBGIF_TMS_SWDIO_TXEN_PIN)

_Static_assert((DBGIF_TCK_SWCLK_PORT == 0U) && (DBGIF_TMS_SWDIO_PORT == 0U) && (DBGIF_TMS_SWDIO_TXEN_PORT == 0U) &&
                   (DBGIF_TDI_PORT == 0U) && (DBGIF_TDO_SWO_PORT == 0U),
               "the generated code reaches port 0 only");
_Static_assert(TCK == 1U, "R_ONE is the clock mask and also the 1 a byte-register store takes");
_Static_assert((TMSC < 256U) && (TDI < 256U), "R_PIN is loaded as an 8-bit immediate");

#define PORT_BASE ((uint32_t)&DBGIF_GPIO->MPIN[0])
#define PORT_OFFSET(field) ((uint32_t)(offsetof(GPIO_Type, field) - offsetof(GPIO_Type, MPIN)))
#define OFF_MPIN 0U
#define OFF_SET PORT_OFFSET(SET)
#define OFF_CLR PORT_OFFSET(CLR)
#define OFF_DIRSET PORT_OFFSET(DIRSET)
#define OFF_DIRCLR PORT_OFFSET(DIRCLR)
#define OFF_B_TMS ((uint32_t)offsetof(GPIO_Type, B[0][TMS_PIN]))
#define OFF_B_TDO ((uint32_t)offsetof(GPIO_Type, B[0][TDO_PIN]))

#define FILLER t_mov(R_PIN, R_PIN)

/** Cycles a sample keeps from either end of its window. The latencies were measured on one die. */
#define MARGIN 1U
/** Four-wire: cycles past the next falling edge that TDO still holds the bit (measured). */
#define FOUR_WIRE_HOLD 6U
/** Four-wire: the acknowledge's last bit is loaded, folded in and compared, and the branch taken,
 * in this many cycles before the next falling edge. */
#define FOUR_WIRE_DECIDE 5U
/** Four-wire: cycles past the latency that the acknowledge's last bit is sampled on a clock long
 * enough to run unmasked, where the margin costs at most a few cycles a transfer. */
#define FOUR_WIRE_DECIDE_SLACK 3U
/** Two-pin periods below this run with interrupts masked for the whole call. A frame there has no
 * cycle to spare for masking around each release, and a call takes a few microseconds. */
#define TWO_PIN_MASK_WHOLE_BELOW 9U

#define BLOCKS JTAG_GEN_FRAMES_MAX

/** What a block's instructions do. Their cycles are the same in every block; their operands are
 * not. */
enum slot {
    S_FALL,      // four-wire: the masked-port store; two-pin: the fall that ends a TDO packet
    S_TAKE_PIN,  // two-pin: the wire back, pin first
    S_TAKE_TXEN, // then its buffer
    S_NTDI,      // two-pin: packet 1's bit
    S_RISE,
    S_TMS,       // two-pin: packet 2's bit
    S_RELEASE_TXEN,
    S_RELEASE_PIN,
    S_MASK,
    S_UNMASK,
    S_DATA,    // the next clock's TDI
    S_CONSUME, // a sample into its bit
    S_SAMPLE,
};

#define SLOTS_MAX 24U

struct block {
    uint32_t cycles; // from one block's first cycle to the next one's
    uint32_t n;
    struct {
        uint32_t cycle;
        uint8_t slot;
    } at[SLOTS_MAX];
    uint32_t lag;        // the clock a block's sample belongs to: the block's own less this
    uint32_t consume_lag; // the same for the sample a block folds in
    uint32_t sample;      // the sample's cycle within its block
    bool masked;          // the whole call runs masked
    uint32_t offset;      // the sample's cycles after the fall that launched its bit
    bool bound;           // the latency decided where something went
};

static struct gen_schedule layout;
static struct gen_event layout_events[SLOTS_MAX + 8U];

static uint32_t later(uint32_t a, uint32_t b) { return a > b ? a : b; }

static void put(uint32_t cycle, enum slot slot) { gen_put(&layout, cycle, (uint32_t)slot); }

/** `slot` on the first free cycle from `earliest`, below `before`. */
static uint32_t place(uint32_t earliest, uint32_t before, enum slot slot) {
    while (gen_taken(&layout, earliest)) {
        earliest++;
    }
    if (earliest >= before) {
        layout.failed = true;
        return earliest;
    }
    put(earliest, slot);
    return earliest;
}

static bool finish(struct block* b, uint32_t cycles) {
    if (layout.failed || (layout.n > SLOTS_MAX)) {
        return false;
    }
    b->cycles = cycles;
    b->n = layout.n;
    for (uint32_t i = 0U; i < layout.n; i++) {
        b->at[i].cycle = layout.ev[i].cycle;
        b->at[i].slot = (uint8_t)layout.ev[i].insn;
    }
    return true;
}

/** A four-wire clock of `cycles` with its sample `at` cycles after the clock's fall, if all five
 * instructions fit. */
static bool try_four_wire(struct block* b, uint32_t cycles, uint32_t at) {
    const uint32_t sample = at % cycles;
    gen_schedule_reset(&layout, layout_events, sizeof layout_events / sizeof layout_events[0]);
    put(0U, S_FALL);
    put(cycles / 2U, S_RISE);
    if (gen_taken(&layout, sample)) {
        return false;
    }
    put(sample, S_SAMPLE);
    place(1U, cycles, S_DATA);
    // A consume before the sample folds in the previous block's, which the sample is about to
    // overwrite; one after it folds in this block's, which it has to wait two cycles for.
    uint32_t consume = 1U;
    while (gen_taken(&layout, consume)) {
        consume++;
    }
    const bool previous = consume < sample;
    if (previous ? cycles + consume < sample + 2U : consume < sample + 2U) {
        if (previous) {
            return false;
        }
        consume = sample + 2U;
    }
    place(consume, cycles, S_CONSUME);
    b->lag = at / cycles;
    b->consume_lag = b->lag + (previous ? 1U : 0U);
    b->sample = sample;
    b->masked = b->lag != 0U;
    b->offset = at;
    return finish(b, cycles);
}

/** A four-wire clock. The shortest clock that fits wins. Within one, a sample before the next
 * fall, which no interrupt can overtake, as late as possible; failing that, a masked one after
 * it, as near the middle of the window as fits. The window is [fall + latency, next fall +
 * FOUR_WIRE_HOLD], less MARGIN at each end. With `earliest`, the sample goes as early in it as fits. */
static bool layout_four_wire(struct block* b, uint32_t period, uint32_t latency, bool earliest) {
    const uint32_t first = latency + MARGIN;
    // Only the first clock length's unmasked search, from the latest cycle down, is the same at a
    // lower latency.
    b->bound = true;
    for (uint32_t cycles = later(period, 5U); earliest && (cycles <= later(period, 5U) + first); cycles++) {
        for (uint32_t at = first; at <= cycles + FOUR_WIRE_HOLD - MARGIN; at++) {
            if (try_four_wire(b, cycles, at)) {
                return true;
            }
        }
    }
    for (uint32_t cycles = later(period, 5U); !earliest && (cycles <= later(period, 5U) + first); cycles++) {
        for (uint32_t at = cycles - 1U; (at >= first) && (at < cycles); at--) {
            if (try_four_wire(b, cycles, at)) {
                b->bound = cycles != later(period, 5U);
                return true;
            }
        }
        const uint32_t lo = later(first, cycles);
        const uint32_t hi = cycles + FOUR_WIRE_HOLD - MARGIN;
        for (uint32_t d = 0U; lo + d <= hi; d++) {
            const uint32_t mid = (lo + hi) / 2U;
            if ((mid + d <= hi) && try_four_wire(b, cycles, mid + d)) {
                return true;
            }
            if ((d != 0U) && (mid >= lo + d) && try_four_wire(b, cycles, mid - d)) {
                return true;
            }
        }
    }
    return false;
}

/** A two-pin frame, from the fall that ends the last frame's TDO packet. */
static bool layout_two_pin(struct block* b, uint32_t period, uint32_t latency, bool earliest) {
    const uint32_t p = period;
    const uint32_t low = p == 3U ? 2U : p / 2U;
    const uint32_t high = p - low;
    b->masked = p < TWO_PIN_MASK_WHOLE_BELOW;
    gen_schedule_reset(&layout, layout_events, sizeof layout_events / sizeof layout_events[0]);
    put(0U, S_FALL);
    put(1U, S_TAKE_PIN);
    put(2U, S_TAKE_TXEN);
    put(3U, S_NTDI);
    const uint32_t r1 = later(4U, low);
    put(r1, S_RISE);
    const uint32_t f1 = later(r1 + high, p);
    put(f1, S_FALL);
    put(f1 + 1U, S_TMS);
    const uint32_t r2 = later(f1 + later(low, 2U), r1 + p);
    put(r2, S_RISE);
    const uint32_t f2 = later(r2 + high, f1 + p);
    if (!b->masked) {
        // As late as possible, so that as little as possible runs masked.
        uint32_t at = f2 - 1U;
        while ((at > 0U) && gen_taken(&layout, at)) {
            at--;
        }
        put(at, S_MASK);
    }
    put(f2, S_FALL);
    put(f2 + 1U, S_RELEASE_TXEN);
    put(f2 + 2U, S_RELEASE_PIN);
    const uint32_t r3 = later(f2 + later(low, 3U), r2 + p);
    put(r3, S_RISE);
    if (!b->masked) {
        place(f2 + 3U, UINT32_MAX, S_UNMASK);
    }
    place(f2 + 3U, UINT32_MAX, S_DATA);
    const uint32_t consume = place(f2 + 3U, UINT32_MAX, S_CONSUME);
    // The sample goes in the block's last cycle, just before the next block's fall. That fall
    // ends the TDO packet, which is a whole period with a whole high phase, and the next frame's
    // first rising edge comes a whole period after this one's last.
    const uint32_t used = layout.ev[layout.n - 1U].cycle;
    const uint32_t otherwise = later(later(used + 2U, f2 + p), later(r3 + high, r3 + p - r1));
    const uint32_t cycles = later(f2 + latency + MARGIN + 1U, otherwise);
    b->bound = earliest || (cycles != otherwise);
    b->sample = cycles - 1U;
    if (earliest) {
        // As early as the latency allows, and after this block's fold-in of the last one's.
        b->sample = later(f2 + latency + MARGIN, consume + 1U);
        while (gen_taken(&layout, b->sample)) {
            b->sample++;
        }
    }
    b->offset = b->sample - f2;
    put(b->sample, S_SAMPLE);
    b->lag = 0U;
    b->consume_lag = 1U;
    return finish(b, cycles);
}

/** The instruction `slot` stands for in block `k`, of the clock count `blocks`. */
static uint32_t insn(enum slot slot, uint32_t k, const struct block* b, bool two_pin) {
    switch (slot) {
    case S_FALL:
        return two_pin ? t_str(R_ONE, R_PORT, OFF_CLR) : t_str(R_DATA, R_PORT, OFF_MPIN);
    case S_TAKE_PIN:
        return t_str(R_PIN, R_PORT, OFF_DIRSET);
    case S_TAKE_TXEN:
        return t_str(R_TXEN, R_PORT, OFF_SET);
    case S_NTDI:
        return t_strb(R_DATA, R_GPIO, OFF_B_TMS);
    case S_RISE:
        return t_str(R_ONE, R_PORT, OFF_SET);
    case S_TMS:
        return t_strb(R_TMS, R_GPIO, OFF_B_TMS);
    case S_RELEASE_TXEN:
        return t_str(R_TXEN, R_PORT, OFF_CLR);
    case S_RELEASE_PIN:
        return t_str(R_PIN, R_PORT, OFF_DIRCLR);
    case S_MASK:
        return t_msr_primask(R_ONE);
    case S_UNMASK:
        return t_msr_primask(R_ZERO);
    case S_DATA:
        // Bit k + 1 for the next block. Two-pin wants its inverse as 0 or 1; four-wire wants it
        // at TDI's place in the port, where a zero shift has to be LSL: LSR #0 encodes LSR #32.
        if (k + 1U >= BLOCKS) {
            return FILLER;
        }
        if (two_pin) {
            return t_bic(R_DATA, R_ONE, R_TDI, T_LSR, k + 1U);
        }
        return k + 1U > TDI_PIN ? t_and(R_DATA, R_PIN, R_TDI, T_LSR, k + 1U - TDI_PIN)
                                : t_and(R_DATA, R_PIN, R_TDI, T_LSL, TDI_PIN - (k + 1U));
    case S_CONSUME:
        if (k < b->consume_lag) {
            return FILLER;
        }
        return t_orr(R_ACC, R_ACC, R_SAMPLE, T_LSL, k - b->consume_lag);
    case S_SAMPLE:
        return t_ldrb(R_SAMPLE, R_GPIO, two_pin ? OFF_B_TMS : OFF_B_TDO);
    }
    return FILLER;
}

/** Block `k`, or past the last block only what is left of the earlier clocks: their samples and
 * the folding in. */
static void emit_block(struct gen_emitter* e, const struct block* b, uint32_t k, bool two_pin) {
    uint32_t now = 0U;
    for (uint32_t i = 0U; i < b->n; i++) {
        const enum slot slot = (enum slot)b->at[i].slot;
        uint32_t word = insn(slot, k, b, two_pin);
        if (k >= BLOCKS) {
            const bool sample_left = (slot == S_SAMPLE) && (k - b->lag < BLOCKS);
            const bool consume_left = (slot == S_CONSUME) && (k - b->consume_lag < BLOCKS);
            // The fall that ends the last TDO packet, and the take-back after it, are a frame's
            // own, and two-pin owes them once more.
            const bool two_pin_tail =
                two_pin && (k == BLOCKS) && (b->at[i].cycle <= 2U) && (slot != S_NTDI);
            if (!sample_left && !consume_left && !two_pin_tail) {
                continue;
            }
        }
        gen_delay(e, b->at[i].cycle - now);
        gen_emit(e, word);
        now = b->at[i].cycle + 1U;
    }
    if (k < BLOCKS) {
        gen_delay(e, b->cycles - now);
    }
}

/** The constant a movw/movt pair at `at` loads, filled in once it is known. */
static void patch_const(struct gen_emitter* e, uint32_t at, uint32_t rd, uint32_t value) {
    if (at + 1U < e->pos) {
        e->code[at] = t_movw(rd, value & 0xFFFFU);
        e->code[at + 1U] = t_movt(rd, value >> 16);
    }
}

static bool emit_scan(struct gen_emitter* e, const struct block* b, bool two_pin) {
    gen_emit(e, t_push(0x4FF0U)); // r4-r11, lr
    gen_load_const(e, R_GPIO, (uint32_t)DBGIF_GPIO);
    gen_load_const(e, R_PORT, PORT_BASE);
    gen_emit(e, t_mov_imm(R_ONE, TCK));
    gen_emit(e, t_mov_imm(R_ZERO, 0U));
    gen_emit(e, t_mov_imm(R_ACC, 0U));
    gen_emit(e, t_rsb_imm(R_SKIP, R_SKIP, BLOCKS));
    if (two_pin) {
        gen_load_const(e, R_TXEN, TXEN);
        gen_emit(e, t_mov_imm(R_PIN, TMSC));
        gen_emit(e, t_bic(R_DATA, R_ONE, R_TDI, T_LSL, 0U));
    } else {
        gen_emit(e, t_mov_imm(R_PIN, TDI));
        // TMS for the whole call, while TCK is still high from the last clock, as ARM sets it.
        gen_emit(e, t_strb(R_TMS, R_GPIO, OFF_B_TMS));
        // Only TCK and TDI answer the masked port; the caller's mask comes back at the end.
        gen_load_const(e, R_TXEN, (uint32_t)&DBGIF_GPIO->MASK[0]);
        gen_emit(e, t_ldr(R_TMS, R_TXEN, 0U));
        gen_load_const(e, R_ENTRY, ~(TCK | TDI));
        gen_emit(e, t_str(R_ENTRY, R_TXEN, 0U));
        gen_emit(e, t_and(R_DATA, R_PIN, R_TDI, T_LSL, TDI_PIN));
    }
    gen_emit(e, t_lsl_reg(R_TDI, R_TDI, R_SKIP));
    // The entry: the first block plus the skipped ones, both filled in once the blocks exist.
    const uint32_t entry_at = e->pos;
    gen_load_const(e, R_ENTRY, 0x10001U);
    const uint32_t size_at = e->pos;
    gen_load_const(e, R_SAMPLE, 0x10001U);
    gen_emit(e, t_mla(R_ENTRY, R_SKIP, R_SAMPLE, R_ENTRY));
    // The first block folds in a sample from a clock that never ran, into a bit the final shift
    // drops: it has to be one bit.
    gen_emit(e, t_mov_imm(R_SAMPLE, 0U));
    if (b->masked) {
        gen_emit(e, t_msr_primask(R_ONE));
    }
    if (!two_pin) {
        // The first block falls at once, and nothing says how long ago the last clock rose.
        gen_delay(e, b->cycles - b->cycles / 2U);
    }
    gen_emit(e, t_bx_pad(R_ENTRY));

    const uint32_t first = e->pos;
    for (uint32_t k = 0U; k < BLOCKS; k++) {
        emit_block(e, b, k, two_pin);
        if ((k == 0U) && !e->failed) {
            patch_const(e, size_at, R_SAMPLE, 4U * (e->pos - first));
        }
    }
    const uint32_t size = e->pos - first;
    patch_const(e, entry_at, R_ENTRY, JTAG_GEN_BASE + 4U * first + 1U);
    for (uint32_t k = BLOCKS; k <= BLOCKS + b->consume_lag; k++) {
        emit_block(e, b, k, two_pin);
    }
    if ((size % BLOCKS) != 0U) {
        return false; // the blocks are not all one size, and the entry would miss them
    }

    gen_emit(e, t_lsr_reg(R_TDI, R_ACC, R_SKIP));
    if (!two_pin) {
        gen_load_const(e, R_TXEN, (uint32_t)&DBGIF_GPIO->MASK[0]);
        gen_emit(e, t_str(R_TMS, R_TXEN, 0U));
    }
    if (b->masked) {
        gen_emit(e, t_msr_primask(R_ZERO));
    }
    gen_emit(e, t_pop(0x8FF0U)); // r4-r11, pc
    gen_resolve(e);
    return !e->failed;
}

/** OScan2's frames outside Shift: blocks of one packet, a fall, the TMS store and a rise, entered
 * as emit_scan's are. Nothing is released, so nothing runs masked: an interrupt only lengthens a
 * phase. The first block's fall finds TCKC already low; the last packet's fall comes after the
 * blocks, so the function leaves TCKC low. */
static bool emit_tms(struct gen_emitter* e, uint32_t period) {
    const uint32_t low = period == 3U ? 2U : period / 2U;
    const uint32_t high = period - low;
    const uint32_t rise = later(2U, low);
    const uint32_t cycles = later(rise + high, period);
    gen_emit(e, t_push(0x4FF0U)); // r4-r11, lr
    gen_load_const(e, R_GPIO, (uint32_t)DBGIF_GPIO);
    gen_load_const(e, R_PORT, PORT_BASE);
    gen_emit(e, t_mov_imm(R_ONE, TCK));
    gen_emit(e, t_rsb_imm(R_SKIP, R_SKIP, BLOCKS));
    const uint32_t entry_at = e->pos;
    gen_load_const(e, R_ENTRY, 0x10001U);
    const uint32_t size_at = e->pos;
    gen_load_const(e, R_SAMPLE, 0x10001U);
    gen_emit(e, t_mla(R_ENTRY, R_SKIP, R_SAMPLE, R_ENTRY));
    gen_emit(e, t_bx_pad(R_ENTRY));
    const uint32_t first = e->pos;
    for (uint32_t k = 0U; k < BLOCKS; k++) {
        gen_emit(e, t_str(R_ONE, R_PORT, OFF_CLR));
        gen_emit(e, t_strb(R_TMS, R_GPIO, OFF_B_TMS));
        gen_delay(e, rise - 2U);
        gen_emit(e, t_str(R_ONE, R_PORT, OFF_SET));
        gen_delay(e, cycles - rise - 1U);
        if ((k == 0U) && !e->failed) {
            patch_const(e, size_at, R_SAMPLE, 4U * (e->pos - first));
        }
    }
    if (((e->pos - first) % BLOCKS) != 0U) {
        return false;
    }
    patch_const(e, entry_at, R_ENTRY, JTAG_GEN_BASE + 4U * first + 1U);
    gen_emit(e, t_str(R_ONE, R_PORT, OFF_CLR));
    gen_load_const(e, R_TDI, 0xFFFFFFFFU);
    gen_emit(e, t_pop(0x8FF0U)); // r4-r11, pc
    gen_resolve(e);
    return !e->failed;
}

/* -- whole transfers ------------------------------------------------------------------------- */

/* OScan2 sends the clocks a transfer takes outside Shift as one TMS packet each, and the rest as
 * OScan1 frames. */

/* A transfer function runs `count` transfers of one request back to back, each as ARM's
 * JTAG_Transfer clocks it: Select-DR, Capture-DR and Shift-DR with ARM's held TDI and the bypass
 * bits before the DAP, then RnW, A2 and A3 with the acknowledge coming back. On OK the data and the
 * bypass bits after it follow, then Exit1-DR, Update-DR, Run-Test/Idle, TDI high from there, and
 * the idle cycles. Any other acknowledge leaves through Exit1-DR at once, as ARM's does, in
 * sequential code after the loop, and the function returns it. */

/* Its registers. */
#define X_REQ 0U   // RnW, A2 and A3 in bits 0 to 2
#define X_BUF 1U   // words read or written, post-incremented
#define X_COUNT 2U // transfers still to do, the current one included
#define X_TXEN 3U  // two-pin: the TMSC buffer's direction pin
#define X_WORD 4U
#define X_ONE 5U   // TCK's mask: also the 1 a byte-register store takes
#define X_PIN 6U   // four-wire: TDI's mask; two-pin: TMSC's
#define X_DELAY 7U
#define X_GPIO 8U
#define X_PORT 9U
#define X_DATA 10U // the next clock's TDI as the wire takes it: a port word, or nTDI
#define X_ACK 11U  // the acknowledge; on OK, a scratch register
#define X_S0 12U   // samples
#define X_S1 T_LR  // four-wire: a second sample register; two-pin: zero
#define X_ZERO T_LR

/** Four-wire: TMS's bit in a port word. */
#define TMS_WORD TMSC

#define XLABEL_LOOP 0U
#define XLABEL_FAIL 1U
#define XLABEL_BITS 2U

/** Idle cycles clocked inside the schedule; more go in a loop after it. */
#define X_IDLE_INLINE 4U

struct xfer {
    struct gen_schedule q;
    bool two_pin;
    bool oscan2;   // two-pin frames outside Shift are one TMS packet
    bool released; // two-pin: the last frame ended with the wire released
    bool masked;
    uint32_t p, low, high; // a four-wire clock, or a two-pin packet the probe drives
    uint32_t latency;
    uint32_t t;      // the earliest cycle the next clock or frame may start on
    uint32_t ready;  // the earliest cycle the next clock's data is in its register
    uint32_t fall;   // four-wire: the last clock's falling edge
    uint32_t rise;   // the last rising edge
    uint32_t d1;     // two-pin: the last frame's nTDI store
    uint32_t first;  // the first clock's falling edge, or the first frame's start
    uint32_t start;  // the last clock's falling edge, or the last frame's start
    uint32_t data_at[32]; // and each data bit's
    uint32_t word_ready; // the earliest cycle X_WORD may be read: a write's word loaded, or shifted
    // The data bits by shifting X_WORD, so that every one is the same instructions and they can
    // run as a loop, with X_ACK counting once the acknowledge is known; its mov is on `counter`.
    bool uniform;
    uint32_t counter;
    bool pend;       // the last clock's TDO waits for its sample
    uint32_t pend_at; // four-wire: that clock's fall; two-pin: the latest cycle it may be sampled on
    uint32_t pend_dst, pend_bit;
    uint32_t sreg;
    uint32_t chain;   // the last fold-in, which the next one follows
    uint32_t busy[2]; // the fold-in of each sample register's last sample
    uint32_t branch;
    uint32_t tdo_fall; // two-pin: the fall that starts the last frame's TDO packet
    uint32_t offset;   // the fewest cycles any sample comes after the fall that launched its bit
    bool bound;        // the latency decided where something went
};

static struct xfer xs;
/* A two-pin transfer is up to 59 frames of about twenty events. */
static struct gen_event xfer_events[1280];

/** Whether a four-wire clock not laid yet puts an edge on `cycle`, if the clocks to come are like
 * the last one. */
static bool x_grid(const struct xfer* x, uint32_t cycle) {
    if (x->two_pin || (cycle < x->t)) {
        return false;
    }
    const uint32_t phase = (cycle - x->t) % x->p;
    return (phase == 0U) || (phase == x->low);
}

static uint32_t x_free(const struct xfer* x, uint32_t cycle, bool grid) {
    while (gen_taken(&x->q, cycle) || (grid && x_grid(x, cycle))) {
        cycle++;
    }
    return cycle;
}

/** An instruction the next clock waits for. */
static uint32_t x_op(struct xfer* x, uint32_t earliest, uint32_t insn) {
    const uint32_t cycle = x_free(x, earliest, false);
    gen_put(&x->q, cycle, insn);
    if (cycle >= x->t) {
        x->t = cycle + 1U;
    }
    if (cycle >= x->ready) {
        x->ready = cycle + 1U;
    }
    return cycle;
}

/** An instruction that must not hold up a clock. */
static uint32_t x_side(struct xfer* x, uint32_t earliest, uint32_t insn) {
    const uint32_t cycle = x_free(x, earliest, true);
    gen_put(&x->q, cycle, insn);
    return cycle;
}

/** A sample folded into the acknowledge (bits as ARM assembles it: the first request clock's is
 * bit 1, then bit 0, then bit 2) or into the word. */
static uint32_t fold(const struct xfer* x, uint32_t dst, uint32_t bit, uint32_t reg) {
    if (dst == X_ACK) {
        static const uint8_t place[3] = {1U, 0U, 2U};
        return bit == 0U ? t_lsl(X_ACK, reg, 1U) : t_orr(X_ACK, X_ACK, reg, T_LSL, place[bit]);
    }
    if (x->uniform) {
        return t_orr(X_WORD, reg, X_WORD, T_LSL, 1U); // reversed by an rbit after the last
    }
    return bit == 0U ? t_mov(X_WORD, reg) : t_orr(X_WORD, X_WORD, reg, T_LSL, bit);
}

static void x_capture(struct xfer* x, uint32_t dst, uint32_t bit, uint32_t at) {
    x->pend = true;
    x->pend_at = at;
    x->pend_dst = dst;
    x->pend_bit = bit;
}

/** The pending sample on `at`, and its fold-in once the load has landed. */
static void x_take(struct xfer* x, uint32_t at) {
    const uint32_t reg = x->sreg == 0U ? X_S0 : X_S1;
    if (gen_taken(&x->q, at) || (at <= x->busy[x->sreg])) {
        x->q.failed = true;
    }
    gen_put(&x->q, at, t_ldrb(reg, X_GPIO, x->two_pin ? OFF_B_TMS : OFF_B_TDO));
    const uint32_t offset = at - (x->two_pin ? x->tdo_fall : x->pend_at);
    x->offset = offset < x->offset ? offset : x->offset;
    x->chain = x_side(x, later(at + 2U, x->chain + 1U), fold(x, x->pend_dst, x->pend_bit, reg));
    x->busy[x->sreg] = x->chain;
    if (!x->two_pin) {
        x->sreg ^= 1U;
    }
    x->pend = false;
}

/* Four-wire. */

/** Bit `bit` of `rm` into TDI's place, everything else clear. */
static uint32_t tdi_word(uint32_t rd, uint32_t rm, uint32_t bit) {
    return bit > TDI_PIN ? t_and(rd, X_PIN, rm, T_LSR, bit - TDI_PIN) : t_and(rd, X_PIN, rm, T_LSL, TDI_PIN - bit);
}

/** The last clock's TDO, now that the fall that ends its window is known. */
static void fx_sample(struct xfer* x, uint32_t next) {
    const uint32_t first = x->pend_at + x->latency + MARGIN;
    // Before the next fall, where no interrupt can make it late: as late as possible.
    for (uint32_t c = next; c-- > first;) {
        if (!gen_taken(&x->q, c)) {
            x_take(x, c);
            return;
        }
    }
    // After it, in the middle of what is left of the window, off the clocks still to come. Only a
    // masked function may sample there.
    const uint32_t last = next + FOUR_WIRE_HOLD - MARGIN;
    uint32_t best = 0U;
    uint32_t best_margin = 0U;
    bool found = false;
    for (uint32_t c = later(first, next + 1U); x->masked && (c <= last); c++) {
        if (gen_taken(&x->q, c) || x_grid(x, c)) {
            continue;
        }
        const uint32_t margin = (c - first) < (last - c) ? (c - first) : (last - c);
        if (!found || (margin > best_margin)) {
            best = c;
            best_margin = margin;
            found = true;
        }
    }
    if (!found) {
        x->q.failed = true;
        x->pend = false;
        return;
    }
    x_take(x, best);
}

static void fx_clock(struct xfer* x, uint32_t data) {
    uint32_t c = later(x->t, x->ready);
    while (gen_taken(&x->q, c) || gen_taken(&x->q, c + x->low)) {
        c++;
    }
    gen_put(&x->q, c, t_str(data, X_PORT, OFF_MPIN));
    gen_put(&x->q, c + x->low, t_str(X_ONE, X_PORT, OFF_SET));
    if (x->first == UINT32_MAX) {
        x->first = c;
    }
    x->start = c;
    x->t = c + x->p;
    if (x->pend) {
        fx_sample(x, c);
    }
    x->fall = c;
    x->rise = c + x->low;
}

/** The branch to XLABEL_FAIL, once the acknowledge's last bit is folded in, before anything the
 * two outcomes do differently. */
static void x_decide(struct xfer* x) {
    uint32_t at = x->chain + 1U;
    while (gen_taken(&x->q, at) || gen_taken(&x->q, at + 1U)) {
        at++;
    }
    // The cmp goes in the cycle before the branch: a delay loop between them would set the flags.
    gen_put(&x->q, at, t_cmp_imm(X_ACK, DAP_TRANSFER_OK));
    gen_put(&x->q, at + 1U, GEN_BRANCH(T_COND_NE, XLABEL_FAIL));
    x->branch = at + 1U;
    x->t = later(x->t, at + 2U);
    x->ready = later(x->ready, at + 2U);
    if (x->uniform) {
        x->counter = x_side(x, at + 2U, t_mov_imm(X_ACK, 0U)); // the count goes in once it is known
    }
}

/** A write's next bit, now that its clock or frame has used this one. */
static void x_shift(struct xfer* x, uint32_t used) {
    if (x->uniform) {
        x->word_ready = x_side(x, used + 1U, t_lsr(X_WORD, X_WORD, 1U)) + 1U;
    }
}

/** A read's word starts from zero, and a write's is loaded. */
static void x_word(struct xfer* x, bool read) {
    if (!read) {
        x->word_ready = x_side(x, 0U, t_ldr_post(X_WORD, X_BUF, 4U)) + 2U;
    } else if (x->uniform) {
        x_side(x, 0U, t_mov_imm(X_WORD, 0U));
    }
}

static void fx_transfer(struct xfer* x, bool read, uint32_t before, uint32_t after, uint32_t idle) {
    if (x->masked) {
        x_op(x, 0U, t_msr_primask(X_ONE));
    }
    x_word(x, read);
    // Select-DR with TMS high, then Capture-DR, Shift-DR and the bypass before, all with TDI as
    // ARM's pin holds it, which X_DATA has.
    x_op(x, 0U, t_orr_imm(X_ACK, X_DATA, TMS_WORD));
    fx_clock(x, X_ACK);
    for (uint32_t i = 0U; i < 2U + before; i++) {
        fx_clock(x, X_DATA);
    }
    for (uint32_t r = 0U; r < 3U; r++) {
        x_op(x, x->fall + 1U, tdi_word(X_DATA, X_REQ, r));
        fx_clock(x, X_DATA);
        x_capture(x, X_ACK, r, x->fall);
        if (r == 2U) {
            // Before the next clock falls, which already carries the next TMS, and otherwise as
            // late as the decision allows, as every other sample is.
            const uint32_t first = x->fall + x->latency + MARGIN;
            const uint32_t decide = x->fall + x->p - FOUR_WIRE_DECIDE;
            const uint32_t at = later(decide, x->masked ? first : first + FOUR_WIRE_DECIDE_SLACK);
            x->bound = x->bound || (at != decide);
            x_take(x, x_free(x, at, false));
            x_decide(x);
        }
    }
    // A read holds A3 on TDI, as ARM's pin does; a write sends the word.
    for (uint32_t j = 0U; j < 32U; j++) {
        const bool exits = (j == 31U) && (after == 0U);
        uint32_t used = 0U;
        if (!read) {
            used = x_op(x, later(x->fall + 1U, x->word_ready), tdi_word(X_DATA, X_WORD, x->uniform ? 0U : j));
        }
        if (exits) {
            x_op(x, x->fall + 1U, t_orr_imm(X_ACK, X_DATA, TMS_WORD));
        }
        fx_clock(x, exits ? X_ACK : X_DATA);
        x->data_at[j] = x->start;
        if (read) {
            x_capture(x, X_WORD, j, x->fall);
        } else {
            x_shift(x, used);
        }
    }
    if (after != 0U) {
        for (uint32_t i = 1U; i < after; i++) {
            fx_clock(x, X_DATA);
        }
        x_op(x, x->fall + 1U, t_orr_imm(X_ACK, X_DATA, TMS_WORD));
        fx_clock(x, X_ACK); // Exit1-DR
    }
    fx_clock(x, X_ACK);  // Update-DR
    fx_clock(x, X_DATA); // Run-Test/Idle
    // TDI high from here, where ARM's PIN_TDI_OUT(1) puts it.
    x_op(x, x->fall + 1U, t_mov_imm(X_DATA, TDI));
    for (uint32_t i = 0U; (i < idle) && (idle <= X_IDLE_INLINE); i++) {
        fx_clock(x, X_DATA);
    }
}

/* Two-pin. */

/** A frame from the fall that ends the last TDO packet: nTDI from `ntdi`, TMS from `tms` (a
 * register holding 1 or 0). With `decide`, the acknowledge is checked before its first packet,
 * whose TDI a failing transfer does not share. */
static void cx_frame(struct xfer* x, uint32_t ntdi, uint32_t tms, bool decide) {
    const uint32_t p = x->p;
    uint32_t s = x->t;
    while (gen_taken(&x->q, s) || gen_taken(&x->q, s + 1U) || gen_taken(&x->q, s + 2U) ||
           (x->pend && gen_taken(&x->q, s - 1U))) {
        s++;
    }
    gen_put(&x->q, s, t_str(X_ONE, X_PORT, OFF_CLR));
    gen_put(&x->q, s + 1U, t_str(X_PIN, X_PORT, OFF_DIRSET));
    gen_put(&x->q, s + 2U, t_str(X_TXEN, X_PORT, OFF_SET));
    if (x->first == UINT32_MAX) {
        x->first = s;
    }
    x->start = s;
    if (x->pend) {
        // The last frame's TDO, in its last cycle, now that its end is known.
        x_take(x, s - 1U);
    }
    if (decide) {
        x_decide(x);
    }
    const uint32_t d1 = x_free(x, later(s + 3U, x->ready), false);
    gen_put(&x->q, d1, t_strb(ntdi, X_GPIO, OFF_B_TMS));
    const uint32_t r1 = x_free(x, later(s + later(4U, x->low), d1 + 1U), false);
    gen_put(&x->q, r1, t_str(X_ONE, X_PORT, OFF_SET));
    const uint32_t f1 = x_free(x, later(r1 + x->high, s + p), false);
    gen_put(&x->q, f1, t_str(X_ONE, X_PORT, OFF_CLR));
    const uint32_t d2 = x_free(x, f1 + 1U, false);
    gen_put(&x->q, d2, t_strb(tms, X_GPIO, OFF_B_TMS));
    const uint32_t r2 = x_free(x, later(later(f1 + later(x->low, 2U), r1 + p), d2 + 1U), false);
    gen_put(&x->q, r2, t_str(X_ONE, X_PORT, OFF_SET));
    uint32_t f2 = later(r2 + x->high, f1 + p);
    while (gen_taken(&x->q, f2) || gen_taken(&x->q, f2 + 1U) || gen_taken(&x->q, f2 + 2U)) {
        f2++;
    }
    if (!x->masked) {
        uint32_t at = f2 - 1U;
        while ((at > d2) && gen_taken(&x->q, at)) {
            at--;
        }
        if (gen_taken(&x->q, at)) {
            x->q.failed = true;
        } else {
            gen_put(&x->q, at, t_msr_primask(X_ONE));
        }
    }
    gen_put(&x->q, f2, t_str(X_ONE, X_PORT, OFF_CLR));
    gen_put(&x->q, f2 + 1U, t_str(X_TXEN, X_PORT, OFF_CLR));
    gen_put(&x->q, f2 + 2U, t_str(X_PIN, X_PORT, OFF_DIRCLR));
    const uint32_t r3 = x_free(x, later(f2 + later(x->low, 3U), r2 + p), false);
    gen_put(&x->q, r3, t_str(X_ONE, X_PORT, OFF_SET));
    uint32_t last = r3;
    if (!x->masked) {
        const uint32_t unmask = x_free(x, f2 + 3U, false);
        gen_put(&x->q, unmask, t_msr_primask(X_ZERO));
        last = later(last, unmask);
    }
    // The next frame starts once this TDO packet has a whole period, a whole high phase and the
    // latency the sample in its last cycle needs, and its first rising edge comes a whole period
    // after this frame's last. The sample's cycle has to be free.
    const uint32_t otherwise = later(later(f2 + p, last + 2U), later(r3 + x->high, r3 + p - later(4U, x->low)));
    x->t = later(f2 + x->latency + MARGIN + 1U, otherwise);
    x->bound = x->bound || (x->t != otherwise);
    x->d1 = d1;
    x->rise = r3;
    x->tdo_fall = f2;
    x->released = true;
}

/** OScan2's frame outside Shift: one TMS packet from `tms` (a register holding 1 or 0), with the
 * take-back first if the last frame released the wire. */
static void cx_tms_frame(struct xfer* x, uint32_t tms) {
    const uint32_t take = x->released ? 2U : 0U;
    uint32_t s = x->t;
    while (gen_taken(&x->q, s) || (take && (gen_taken(&x->q, s + 1U) || gen_taken(&x->q, s + 2U))) ||
           (x->pend && gen_taken(&x->q, s - 1U))) {
        s++;
    }
    gen_put(&x->q, s, t_str(X_ONE, X_PORT, OFF_CLR));
    if (take != 0U) {
        gen_put(&x->q, s + 1U, t_str(X_PIN, X_PORT, OFF_DIRSET));
        gen_put(&x->q, s + 2U, t_str(X_TXEN, X_PORT, OFF_SET));
    }
    if (x->first == UINT32_MAX) {
        x->first = s;
    }
    x->start = s;
    if (x->pend) {
        x_take(x, s - 1U);
    }
    const uint32_t d = x_free(x, later(s + 1U + take, x->ready), false);
    gen_put(&x->q, d, t_strb(tms, X_GPIO, OFF_B_TMS));
    const uint32_t r = x_free(x, later(s + x->low, d + 1U), false);
    gen_put(&x->q, r, t_str(X_ONE, X_PORT, OFF_SET));
    x->t = later(r + x->high, s + x->p);
    x->d1 = d;
    x->rise = r;
    x->released = false;
}

/** A frame outside Shift: OScan2's one packet, or OScan1's three. */
static void cx_idle_frame(struct xfer* x, uint32_t tms) {
    if (x->oscan2) {
        cx_tms_frame(x, tms);
    } else {
        cx_frame(x, X_DATA, tms, false);
    }
}

static void cx_transfer(struct xfer* x, bool read, uint32_t before, uint32_t after, uint32_t idle) {
    if (x->masked) {
        x_op(x, 0U, t_msr_primask(X_ONE));
    }
    x_word(x, read);
    // X_DATA holds the nTDI of ARM's held TDI.
    cx_idle_frame(x, X_ONE); // Select-DR
    // Capture-DR and Shift-DR from outside Shift, then the bypass bits before the DAP in it.
    for (uint32_t i = 0U; i < 2U + before; i++) {
        if (i < 2U) {
            cx_idle_frame(x, X_ZERO);
        } else {
            cx_frame(x, X_DATA, X_ZERO, false);
        }
    }
    for (uint32_t r = 0U; r < 3U; r++) {
        x_op(x, x->d1 + 1U, t_bic(X_DATA, X_ONE, X_REQ, r == 0U ? T_LSL : T_LSR, r));
        cx_frame(x, X_DATA, X_ZERO, false);
        x_capture(x, X_ACK, r, 0U);
    }
    for (uint32_t j = 0U; j < 32U; j++) {
        const bool exits = (j == 31U) && (after == 0U);
        const uint32_t bit = x->uniform ? 0U : j;
        uint32_t used = 0U;
        if (!read) {
            used = x_op(x, later(x->d1 + 1U, x->word_ready), t_bic(X_DATA, X_ONE, X_WORD, bit == 0U ? T_LSL : T_LSR, bit));
        }
        cx_frame(x, X_DATA, exits ? X_ONE : X_ZERO, j == 0U);
        x->data_at[j] = x->start;
        if (read) {
            x_capture(x, X_WORD, j, 0U);
        } else {
            x_shift(x, used);
        }
    }
    if (after != 0U) {
        for (uint32_t i = 1U; i < after; i++) {
            cx_frame(x, X_DATA, X_ZERO, false);
        }
        cx_frame(x, X_DATA, X_ONE, false); // Exit1-DR
    }
    cx_idle_frame(x, X_ONE);  // Update-DR
    cx_idle_frame(x, X_ZERO); // Run-Test/Idle
    // TDI high from here, where ARM's PIN_TDI_OUT(1) puts it: nTDI 0.
    x_op(x, x->d1 + 1U, t_mov_imm(X_DATA, 0U));
    for (uint32_t i = 0U; (i < idle) && (idle <= X_IDLE_INLINE); i++) {
        cx_idle_frame(x, X_ZERO);
    }
}

/* Sequential code: clocks only no shorter than the period. */

static void slow_clock(struct gen_emitter* e, const struct xfer* x, uint32_t data) {
    gen_emit(e, t_str(data, X_PORT, OFF_MPIN));
    gen_delay(e, x->low - 1U);
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_SET));
    gen_delay(e, x->high - 1U);
}

/** A frame that samples nothing. `fall` with the fall that ends the last TDO packet and the
 * take-back, which a frame begins with but the failing path has already had. */
static void slow_frame(struct gen_emitter* e, const struct xfer* x, uint32_t ntdi, uint32_t tms, bool fall) {
    const uint32_t low1 = later(x->low, 4U);
    if (fall) {
        gen_emit(e, t_str(X_ONE, X_PORT, OFF_CLR));
        gen_emit(e, t_str(X_PIN, X_PORT, OFF_DIRSET));
        gen_emit(e, t_str(X_TXEN, X_PORT, OFF_SET));
    }
    gen_emit(e, t_strb(ntdi, X_GPIO, OFF_B_TMS));
    gen_delay(e, low1 - 4U);
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_SET));
    gen_delay(e, x->high - 1U);
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_CLR));
    gen_emit(e, t_strb(tms, X_GPIO, OFF_B_TMS));
    gen_delay(e, later(x->low, 2U) - 2U);
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_SET));
    gen_delay(e, x->high - 1U);
    if (!x->masked) {
        gen_emit(e, t_msr_primask(X_ONE));
    }
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_CLR));
    gen_emit(e, t_str(X_TXEN, X_PORT, OFF_CLR));
    gen_emit(e, t_str(X_PIN, X_PORT, OFF_DIRCLR));
    if (!x->masked) {
        gen_emit(e, t_msr_primask(X_ZERO));
    }
    gen_delay(e, later(x->low, 3U) - (x->masked ? 3U : 4U));
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_SET));
    gen_delay(e, later(x->p, x->high) - 1U);
}

/** OScan2's frame outside Shift, which samples nothing: the take-back, harmless where the wire is
 * already driven, then one TMS packet. */
static void slow_tms_frame(struct gen_emitter* e, const struct xfer* x, uint32_t tms) {
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_CLR));
    gen_emit(e, t_str(X_PIN, X_PORT, OFF_DIRSET));
    gen_emit(e, t_str(X_TXEN, X_PORT, OFF_SET));
    gen_emit(e, t_strb(tms, X_GPIO, OFF_B_TMS));
    gen_delay(e, later(x->low, 4U) - 4U);
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_SET));
    gen_delay(e, later(x->p, x->high) - 1U);
}

/** The fall that ends the last TDO packet and the take-back, which leave a two-pin call as it
 * found the wire. */
static void slow_tail(struct gen_emitter* e) {
    gen_emit(e, t_str(X_ONE, X_PORT, OFF_CLR));
    gen_emit(e, t_str(X_PIN, X_PORT, OFF_DIRSET));
    gen_emit(e, t_str(X_TXEN, X_PORT, OFF_SET));
}

/** `n` idle clocks or frames in a loop, TDI high. */
static void slow_idle(struct gen_emitter* e, const struct xfer* x, uint32_t n) {
    gen_emit(e, t_movw(X_S0, n));
    const uint32_t head = e->pos;
    if (x->oscan2) {
        slow_tms_frame(e, x, X_ZERO);
    } else if (x->two_pin) {
        slow_frame(e, x, X_DATA, X_ZERO, true);
    } else {
        slow_clock(e, x, X_DATA);
    }
    gen_emit(e, t_subs_imm(X_S0, X_S0, 1U));
    gen_emit(e, t_bcond(T_COND_NE, ((int32_t)head - (int32_t)e->pos) * 4));
}

struct xfer_cfg {
    bool two_pin;
    bool oscan2;
    uint32_t period, latency, before, after, idle;
    bool loop; // the data bits as a loop, which is smaller and takes the same cycles
};

/** `count` repeats of the `period` cycles from `from`. */
struct span {
    uint32_t from, period, count;
};

/** The longest run of the schedule around the middle data bits that repeats every data bit, cut
 * where its last three cycles are free for the loop's subs and taken branch. */
static bool find_loop(const struct xfer* x, struct span* loop) {
    const struct gen_schedule* q = &x->q;
    const uint32_t middle = x->data_at[16];
    const uint32_t period = x->data_at[17] - middle;
    if (period < 4U) {
        return false;
    }
    // The cycles c for which what is on c differs from what is on c + period: the nearest on each
    // side of the middle bound the run.
    uint32_t lo = 0U;
    uint32_t hi = UINT32_MAX;
    for (uint32_t i = 0U; i < q->n; i++) {
        const uint32_t c = q->ev[i].cycle;
        const uint32_t insn = q->ev[i].insn;
        const uint32_t ends[2] = {c, c - period};
        const bool differs[2] = {gen_at(q, c + period) != insn, (c >= period) && (gen_at(q, c - period) != insn)};
        for (uint32_t k = 0U; k < 2U; k++) {
            if (!differs[k]) {
                continue;
            }
            if (ends[k] < middle) {
                lo = later(lo, ends[k] + 1U);
            } else if (ends[k] < hi) {
                hi = ends[k];
            }
        }
    }
    if (hi == UINT32_MAX) {
        return false;
    }
    const uint32_t end = hi + period; // the run repeats from lo up to here
    for (uint32_t from = lo; (from < lo + period) && (from + 2U * period <= end); from++) {
        const uint32_t tail = from + period - 3U;
        if ((gen_at(q, tail) == 0U) && (gen_at(q, tail + 1U) == 0U) && (gen_at(q, tail + 2U) == 0U)) {
            *loop = (struct span){from, period, (end - from) / period};
            return true;
        }
    }
    return false;
}

static void x_patch(struct xfer* x, uint32_t cycle, uint32_t insn) {
    for (uint32_t i = 0U; i < x->q.n; i++) {
        if (x->q.ev[i].cycle == cycle) {
            x->q.ev[i].insn = insn;
        }
    }
}

/** Lays out one transfer in `xs` with a four-wire clock or two-pin packet of `p` cycles. */
static bool lay_xfer(const struct xfer_cfg* c, bool read, uint32_t p) {
    struct xfer* x = &xs;
    *x = (struct xfer){.two_pin = c->two_pin,
                       .oscan2 = c->oscan2,
                       .latency = c->latency,
                       .first = UINT32_MAX,
                       .uniform = c->loop,
                       .offset = UINT32_MAX};
    gen_schedule_reset(&x->q, xfer_events, sizeof xfer_events / sizeof xfer_events[0]);
    x->p = p;
    if (c->two_pin) {
        x->low = p == 3U ? 2U : p / 2U;
        x->masked = p < TWO_PIN_MASK_WHOLE_BELOW;
    } else {
        x->low = p / 2U;
        x->masked = p <= c->latency + MARGIN + 1U;
        // A masked four-wire clock samples from the latency on, after the next fall.
        x->bound = x->masked;
    }
    x->high = p - x->low;
    if (c->two_pin) {
        cx_transfer(x, read, c->before, c->after, c->idle);
    } else {
        fx_transfer(x, read, c->before, c->after, c->idle);
    }
    if (read && x->uniform) {
        x_side(x, x->chain + 1U, t_rbit(X_WORD, X_WORD));
    }
    return !x->q.failed && !x->pend;
}

static bool emit_xfer(struct gen_emitter* e, const struct xfer_cfg* c, bool read) {
    struct xfer* x = &xs;
    // A four-wire clock too short for the TDO window runs longer, as the scan engine's does. A
    // two-pin frame finds its own room.
    const uint32_t shortest = c->two_pin ? c->period : later(c->period, 5U);
    const uint32_t longest = c->two_pin ? shortest : later(shortest, c->latency + FOUR_WIRE_HOLD);
    uint32_t p = shortest;
    while (!lay_xfer(c, read, p)) {
        if (p++ == longest) {
            return false;
        }
    }
    x->bound = x->bound || (p != shortest);
    struct span loop = {0U, 0U, 0U};
    if (x->uniform) {
        if (!find_loop(x, &loop) || (loop.count > 255U)) {
            return false;
        }
        x_patch(x, x->counter, t_mov_imm(X_ACK, loop.count));
    }

    gen_emit(e, t_push(0x4FF0U)); // r4-r11, lr
    gen_load_const(e, X_GPIO, (uint32_t)DBGIF_GPIO);
    gen_load_const(e, X_PORT, PORT_BASE);
    gen_emit(e, t_mov_imm(X_ONE, TCK));
    gen_emit(e, t_lsr(X_REQ, X_REQ, 1U));
    if (c->two_pin) {
        gen_emit(e, t_mov_imm(X_PIN, TMSC));
        gen_emit(e, t_mov_imm(X_ZERO, 0U));
        gen_emit(e, t_bic(X_DATA, X_ONE, 3U, T_LSL, 0U)); // the caller's TDI, in r3
        gen_load_const(e, X_TXEN, TXEN);
    } else {
        gen_emit(e, t_mov_imm(X_PIN, TDI));
        gen_emit(e, t_lsl(X_DATA, 3U, TDI_PIN));
        // TMS moves with the falling edge here, so it answers the masked port too.
        gen_load_const(e, X_ACK, (uint32_t)&DBGIF_GPIO->MASK[0]);
        gen_load_const(e, X_S0, ~(TCK | TDI | TMSC));
        gen_emit(e, t_str(X_S0, X_ACK, 0U));
        // Nothing says how long ago the last clock rose.
        gen_delay(e, x->high);
    }

    gen_bind(e, XLABEL_LOOP);
    uint32_t length;
    if (x->uniform) {
        // Every pass takes the cycles the unrolled bits would have: the subs and the taken branch
        // fill the three it leaves free, and a filler after the last pass the one its untaken
        // branch does not.
        gen_delay(e, loop.from - gen_emit_span(e, &x->q, 0U, loop.from));
        gen_bind(e, XLABEL_BITS);
        const uint32_t tail = loop.from + loop.period - 3U;
        gen_delay(e, tail - gen_emit_span(e, &x->q, loop.from, tail));
        gen_emit(e, t_subs_imm(X_ACK, X_ACK, 1U));
        gen_emit_branch(e, T_COND_NE, XLABEL_BITS);
        gen_delay(e, 1U);
        length = gen_emit_span(e, &x->q, loop.from + (loop.count * loop.period), UINT32_MAX);
    } else {
        length = gen_emit_schedule(e, &x->q);
    }
    // Cycles from the last rising edge to here, and the most the next clock or frame has to wait
    // after it: the schedule's own x->t, which is past every constraint of the last one.
    uint32_t since = length - x->rise;
    uint32_t need = x->t - x->rise;
    if (c->idle > X_IDLE_INLINE) {
        gen_delay(e, need > since ? need - since : 0U);
        slow_idle(e, x, c->idle);
        since = need; // the loop's clocks end with whole phases of their own
    }
    if (read) {
        gen_emit(e, t_str_post(X_WORD, X_BUF, 4U));
        since++;
    }
    if (x->masked) {
        gen_emit(e, t_mov_imm(X_ACK, 0U));
        gen_emit(e, t_msr_primask(X_ACK));
        since += 2U;
    }
    // The subs and the taken branch, then the next transfer's first edge.
    const uint32_t to_first = since + 3U + x->first;
    if (to_first < need) {
        gen_delay(e, need - to_first);
        since += need - to_first;
    }
    gen_emit(e, t_subs_imm(X_COUNT, X_COUNT, 1U));
    gen_emit_branch(e, T_COND_NE, XLABEL_LOOP);
    gen_emit(e, t_mov_imm(X_ACK, DAP_TRANSFER_OK));
    if (c->two_pin) {
        // The fall that ends the last TDO packet, as the next frame's would have.
        gen_delay(e, need);
        slow_tail(e);
    }
    const uint32_t exit = e->pos;
    // ARM leaves TDI high after a transfer, on the pin itself on four wires.
    if (!c->two_pin) {
        gen_emit(e, t_strb(X_ONE, X_GPIO, (uint32_t)offsetof(GPIO_Type, B[0][TDI_PIN])));
    }
    gen_emit(e, t_mov(0U, X_ACK));
    gen_emit(e, t_mov(1U, X_COUNT));
    gen_emit(e, t_pop(0x8FF0U)); // r4-r11, pc

    // Any other acknowledge: Exit1-DR with A3 still on TDI, Update-DR, Run-Test/Idle, TDI high,
    // the idle cycles.
    gen_bind(e, XLABEL_FAIL);
    gen_delay(e, x->high);
    if (c->two_pin) {
        gen_emit(e, t_bic(X_DATA, X_ONE, X_REQ, T_LSR, 2U));
        slow_frame(e, x, X_DATA, X_ONE, false); // from Shift-DR, so three packets in either format
        if (c->oscan2) {
            slow_tms_frame(e, x, X_ONE);
            slow_tms_frame(e, x, X_ZERO);
        } else {
            slow_frame(e, x, X_DATA, X_ONE, true);
            slow_frame(e, x, X_DATA, X_ZERO, true);
        }
        gen_emit(e, t_mov_imm(X_DATA, 0U));
    } else {
        gen_emit(e, tdi_word(X_DATA, X_REQ, 2U));
        gen_emit(e, t_orr_imm(X_WORD, X_DATA, TMS_WORD));
        slow_clock(e, x, X_WORD);
        slow_clock(e, x, X_WORD);
        slow_clock(e, x, X_DATA);
        gen_emit(e, t_mov_imm(X_DATA, TDI));
    }
    if (c->idle != 0U) {
        slow_idle(e, x, c->idle);
    }
    if (c->two_pin) {
        slow_tail(e);
    }
    if (x->masked) {
        gen_emit(e, t_mov_imm(X_S0, 0U));
        gen_emit(e, t_msr_primask(X_S0));
    }
    gen_emit(e, t_b(((int32_t)exit - (int32_t)e->pos) * 4));
    gen_resolve(e);
    return !e->failed;
}

bool jtag_gen_build(const struct jtag_gen_config* config, jtag_gen_scan_fn* scan, jtag_gen_xfer_fn* read,
                    jtag_gen_xfer_fn* write) {
    if ((config->period < JTAG_GEN_PERIOD_MIN) || (config->period > JTAG_GEN_PERIOD_MAX) || (config->latency == 0U) ||
        (config->latency > 64U) || (config->wire > JTAG_GEN_OSCAN2) || (config->before > 7U) ||
        (config->after > 7U) || (config->idle > 255U)) {
        return false;
    }
    const bool two_pin = config->wire != JTAG_GEN_FOUR_WIRE;
    const bool oscan2 = config->wire == JTAG_GEN_OSCAN2;
    static struct block b;
    const bool earliest = config->earliest != 0U;
    if (earliest && (read != NULL)) {
        return false;
    }
    if (!(two_pin ? layout_two_pin(&b, config->period, config->latency, earliest)
                  : layout_four_wire(&b, config->period, config->latency, earliest))) {
        return false;
    }
    struct gen_emitter e = {
        .code = (uint32_t*)JTAG_GEN_BASE, .cap = JTAG_GEN_SIZE / 4U, .delay_reg = R_DELAY, .filler = FILLER};
    const uint32_t scan_at = e.pos;
    if (!emit_scan(&e, &b, two_pin)) {
        return false;
    }
    const uint32_t tms_at = e.pos;
    if (oscan2 && !emit_tms(&e, config->period)) {
        return false;
    }

    // The transfers, if they fit beside it. The delay register is X_DELAY, the filler the same.
    struct xfer_cfg xc = {
        .two_pin = two_pin,
        .oscan2 = oscan2,
        .period = config->period,
        .latency = config->latency,
        .before = config->before,
        .after = config->after,
        .idle = config->idle,
    };
    const uint32_t scan_end = e.pos;
    uint32_t read_at = scan_end;
    uint32_t write_at = scan_end;
    bool transfers = false;
    uint32_t read_offset = 0U;
    bool read_bound = false;
    // Unrolled where that fits in JTAG_GEN_UNROLLED_MAX, which leaves the fastest clocks the fewest
    // instructions; the data bits in a loop otherwise; unrolled into the whole region where the
    // loop does not fit either.
    for (uint32_t loop = 0U; (read != NULL) && (loop < 3U) && !transfers; loop++) {
        xc.loop = loop == 1U;
        e.cap = loop == 0U ? JTAG_GEN_UNROLLED_MAX / 4U : JTAG_GEN_SIZE / 4U;
        e.pos = scan_end;
        e.failed = false;
        read_at = e.pos;
        transfers = emit_xfer(&e, &xc, true);
        read_offset = xs.offset;
        read_bound = xs.bound;
        write_at = e.pos;
        transfers = transfers && emit_xfer(&e, &xc, false);
    }
    e.cap = JTAG_GEN_SIZE / 4U;
    if (!transfers) {
        e.pos = scan_end;
        e.failed = false;
    }

    jtag_gen_info = (struct jtag_gen_info){
        .period = two_pin ? config->period : b.cycles,
        .frame = b.cycles,
        .code_bytes = 4U * e.pos,
        .masked = b.masked,
        .ok = true,
        .transfers = transfers,
        .looped = transfers && xc.loop,
        // Without transfers, ARM's run as scans.
        .scan_last = (int32_t)b.offset,
        .read_last = (int32_t)(transfers ? read_offset : b.offset),
        .write_last = (int32_t)(transfers ? xs.offset : b.offset),
        // Transfers that did not fit at this latency might at a lower one.
        .same_below =
            (!b.bound && ((read == NULL) || (transfers && !read_bound && !xs.bound))) ? config->latency + 1U : 0U,
    };
    // The code was written as data; the core has to fetch what is there now.
    __DSB();
    __ISB();
    *scan = (jtag_gen_scan_fn)(JTAG_GEN_BASE + 4U * scan_at + 1U);
    jtag_gen_tms = oscan2 ? (jtag_gen_scan_fn)(JTAG_GEN_BASE + 4U * tms_at + 1U) : NULL;
    if (read != NULL) {
        *read = transfers ? (jtag_gen_xfer_fn)(JTAG_GEN_BASE + 4U * read_at + 1U) : NULL;
        *write = transfers ? (jtag_gen_xfer_fn)(JTAG_GEN_BASE + 4U * write_at + 1U) : NULL;
    }
    return true;
}

/* -- when to generate ------------------------------------------------------------------------- */

uint32_t jtag_gen_clock_hz = DAP_DEFAULT_SWJ_CLOCK;
struct jtag_gen_info jtag_gen_info;
uint32_t jtag_gen_latency[2] = {JTAG_GEN_FOUR_WIRE_LATENCY_DEFAULT, JTAG_GEN_LATENCY_DEFAULT};
jtag_gen_scan_fn jtag_gen_scan;
jtag_gen_scan_fn jtag_gen_tms;
jtag_gen_xfer_fn jtag_gen_read;
jtag_gen_xfer_fn jtag_gen_write;

static struct jtag_gen_config built;
static bool built_once;
static bool built_ok;
static bool built_transfers; // the last build has its transfers, those that fit
// The idle clocks each wire learned last, and the host's configuration they were learned for, before
// pacing. A host passes through other configurations, and other wires, on its way back to it: an
// OScan2 attach frames OScan1 and a one-TAP chain before its own.
static struct jtag_gen_config learned_for[JTAG_GEN_OSCAN2 + 1U];
static uint32_t learned[JTAG_GEN_OSCAN2 + 1U];
static bool learning; // jtag_gen_paced has one more clock for regenerate to record
uint32_t jtag_gen_builds;
uint32_t jtag_gen_failures;
uint32_t jtag_gen_restores;
uint8_t jtag_gen_command;
struct jtag_gen_log_entry jtag_gen_log[JTAG_GEN_LOG_SIZE];

// The last build that asked for transfers, whether or not they fitted, kept so that a host coming
// back to its configuration gets it back with a copy rather than a build: a session switches to its
// two-pin wire and back to four-wire at either end, and each switch writes over SRAMX. A build
// depends on its configuration alone, which regenerate already relies on when it keeps the code in
// place. Its scans serve a scan-only request as well.
static COLD_BSS struct {
    bool valid;
    struct jtag_gen_config config;
    struct jtag_gen_info info;
    jtag_gen_scan_fn scan;
    jtag_gen_scan_fn tms;
    jtag_gen_xfer_fn read;
    jtag_gen_xfer_fn write;
    uint32_t code[JTAG_GEN_SIZE / 4U];
} saved;

/** The shortest period no faster than the clock asked for. */
static uint32_t period_for(uint32_t hz) {
    const uint32_t period = ((CPU_CLOCK - 1U) / hz) + 1U;
    return period < JTAG_GEN_PERIOD_MIN ? JTAG_GEN_PERIOD_MIN : period;
}

uint32_t jtag_gen_paced;
bool jtag_gen_pacing = true;
static bool waited;

COMMAND_PATH void jtag_gen_waited(void) { waited = true; }

COMMAND_PATH void jtag_gen_learn(uint32_t wire) {
    if (waited && jtag_gen_pacing && (jtag_gen_paced < JTAG_GEN_PACED_MAX)) {
        jtag_gen_paced++;
        learning = true;
        (void)jtag_gen_ready_transfers(wire);
    }
    waited = false;
}

/** The configuration the host asked for, before pacing. */
static struct jtag_gen_config host_config(uint32_t wire) {
    return (struct jtag_gen_config){
        .wire = wire,
        .period = period_for(jtag_gen_clock_hz),
        .latency = jtag_gen_latency[JTAG_GEN_PINS(wire)],
        .before = DAP_Data.jtag_dev.index,
        .after = DAP_Data.jtag_dev.index < DAP_Data.jtag_dev.count
                     ? DAP_Data.jtag_dev.count - DAP_Data.jtag_dev.index - 1U
                     : 0U,
        .idle = DAP_Data.transfer.idle_cycles,
    };
}

static uint32_t paced_idle(uint32_t idle) { return idle + jtag_gen_paced > 255U ? 255U : idle + jtag_gen_paced; }

struct jtag_gen_config jtag_gen_inputs(uint32_t wire) {
    struct jtag_gen_config config = host_config(wire);
    config.idle = paced_idle(config.idle);
    return config;
}

/** Out of line, so the check every transfer command makes does not pay its frame or its divide. */
static __attribute__((noinline)) bool regenerate(uint32_t wire, bool transfers) {
    struct jtag_gen_config want = host_config(wire);
    const bool recorded = learning;
    if (learning) {
        learned_for[wire] = want;
        learned[wire] = jtag_gen_paced;
        learning = false;
    }
    jtag_gen_paced = (jtag_gen_pacing && (memcmp(&want, &learned_for[wire], sizeof want) == 0)) ? learned[wire] : 0U;
    want.idle = paced_idle(want.idle);
    if (built_once && (gen_owner == GEN_OWNER_JTAG) && (memcmp(&want, &built, sizeof want) == 0) &&
        (built_transfers || !transfers)) {
        return built_ok;
    }
    struct jtag_gen_log_entry* entry =
        &jtag_gen_log[(jtag_gen_builds + jtag_gen_failures + jtag_gen_restores) % JTAG_GEN_LOG_SIZE];
    *entry = (struct jtag_gen_log_entry){
        .command = jtag_gen_command,
        .wire = (uint8_t)want.wire,
        .latency = (uint8_t)want.latency,
        .before = (uint8_t)want.before,
        .after = (uint8_t)want.after,
        .idle = (uint8_t)want.idle,
        .paced = (uint8_t)jtag_gen_paced,
        .period = (uint16_t)want.period,
        .at = TIMESTAMP_GET(),
    };
    if (!built_once) {
        entry->changed = JTAG_GEN_CHANGED_FIRST;
    } else {
        entry->changed = (gen_owner != GEN_OWNER_JTAG ? JTAG_GEN_CHANGED_OWNER : 0U) |
                         (want.wire != built.wire ? JTAG_GEN_CHANGED_WIRE : 0U) |
                         (want.period != built.period ? JTAG_GEN_CHANGED_PERIOD : 0U) |
                         (want.latency != built.latency ? JTAG_GEN_CHANGED_LATENCY : 0U) |
                         ((want.before != built.before) || (want.after != built.after) ? JTAG_GEN_CHANGED_CHAIN : 0U) |
                         (want.idle != built.idle ? JTAG_GEN_CHANGED_IDLE : 0U) |
                         (transfers && !built_transfers ? JTAG_GEN_CHANGED_TRANSFERS : 0U);
    }
    entry->flags = (transfers ? JTAG_GEN_LOG_TRANSFERS : 0U) | (recorded ? JTAG_GEN_LOG_LEARNED : 0U);
    built = want;
    built_once = true;
    built_transfers = transfers;
    gen_owner = GEN_OWNER_JTAG;
    jtag_gen_info.ok = false;
    if (saved.valid && (memcmp(&want, &saved.config, sizeof want) == 0)) {
        memcpy((void*)JTAG_GEN_BASE, saved.code, saved.info.code_bytes);
        __DSB();
        __ISB();
        jtag_gen_info = saved.info;
        jtag_gen_scan = saved.scan;
        jtag_gen_tms = saved.tms;
        jtag_gen_read = saved.read;
        jtag_gen_write = saved.write;
        built_transfers = true;
        built_ok = true;
        entry->cycles = TIMESTAMP_GET() - entry->at;
        entry->flags |= JTAG_GEN_LOG_TRANSFERS | JTAG_GEN_LOG_OK | JTAG_GEN_LOG_RESTORED;
        jtag_gen_restores++;
        return true;
    }
    // A scan-only build writes over the transfers the last one left.
    jtag_gen_read = NULL;
    jtag_gen_write = NULL;
    built_ok = jtag_gen_build(&want, &jtag_gen_scan, transfers ? &jtag_gen_read : NULL,
                              transfers ? &jtag_gen_write : NULL);
    entry->cycles = TIMESTAMP_GET() - entry->at;
    entry->flags |= built_ok ? JTAG_GEN_LOG_OK : 0U;
    if (built_ok) {
        jtag_gen_builds++;
    } else {
        jtag_gen_failures++;
    }
    if (built_ok && transfers) {
        saved.config = want;
        saved.info = jtag_gen_info;
        saved.scan = jtag_gen_scan;
        saved.tms = jtag_gen_tms;
        saved.read = jtag_gen_read;
        saved.write = jtag_gen_write;
        memcpy(saved.code, (const void*)JTAG_GEN_BASE, jtag_gen_info.code_bytes);
        saved.valid = true;
    }
    return built_ok;
}

/** Everything regenerate reads, as the last call found it. The chain position comes with every
 *  transfer command, so a flag cleared by the configuration commands would not do: this compares
 *  the inputs themselves, and needs no list of what writes them. */
struct jtag_gen_inputs {
    uint32_t wire;
    uint32_t hz;
    uint32_t latency;
    uint32_t index;
    uint32_t count;
    uint32_t idle;
    uint32_t paced;
    bool pacing;
};

static struct jtag_gen_inputs seen;
static bool seen_once;
static bool seen_transfers;

static inline bool same_inputs(const struct jtag_gen_inputs* a, const struct jtag_gen_inputs* b) {
    return (a->wire == b->wire) && (a->hz == b->hz) && (a->latency == b->latency) && (a->index == b->index) &&
           (a->count == b->count) && (a->idle == b->idle) && (a->paced == b->paced) && (a->pacing == b->pacing);
}

__attribute__((always_inline)) static inline bool ready(uint32_t wire, bool transfers) {
    const struct jtag_gen_inputs now = {
        .wire = wire,
        .hz = jtag_gen_clock_hz,
        .latency = jtag_gen_latency[JTAG_GEN_PINS(wire)],
        .index = DAP_Data.jtag_dev.index,
        .count = DAP_Data.jtag_dev.count,
        .idle = DAP_Data.transfer.idle_cycles,
        .paced = jtag_gen_paced,
        .pacing = jtag_gen_pacing,
    };
    if (seen_once && (gen_owner == GEN_OWNER_JTAG) && same_inputs(&now, &seen) && (seen_transfers || !transfers)) {
        return built_ok;
    }
    const bool ok = regenerate(wire, transfers);
    seen = now;
    seen.paced = jtag_gen_paced; // regenerate keeps each wire's pacing, and restarts a changed one's
    seen_once = true;
    seen_transfers = built_transfers;
    return ok;
}

COMMAND_PATH bool jtag_gen_ready(uint32_t wire) { return ready(wire, false); }

COMMAND_PATH bool jtag_gen_ready_transfers(uint32_t wire) { return ready(wire, true); }
