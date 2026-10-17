/** Generate the JTAG wire paths at run time, placed to the cycle: four-wire, and two-pin
 * (IEEE 1149.7 OScan1).
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

#include "gen.h"
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

#define PORT_BASE ((uint32_t)&GPIO->MPIN[0])
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
};

static struct gen_schedule layout;

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
    gen_schedule_reset(&layout);
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
    return finish(b, cycles);
}

/** A four-wire clock. The shortest clock that fits wins. Within one, a sample before the next
 * fall, which no interrupt can overtake, as late as possible; failing that, a masked one after
 * it, as near the middle of the window as fits. The window is [fall + latency, next fall +
 * FOUR_WIRE_HOLD], less MARGIN at each end. */
static bool layout_four_wire(struct block* b, uint32_t period, uint32_t latency) {
    const uint32_t first = latency + MARGIN;
    for (uint32_t cycles = later(period, 5U); cycles <= later(period, 5U) + first; cycles++) {
        for (uint32_t at = cycles - 1U; (at >= first) && (at < cycles); at--) {
            if (try_four_wire(b, cycles, at)) {
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
static bool layout_two_pin(struct block* b, uint32_t period, uint32_t latency) {
    const uint32_t p = period;
    const uint32_t low = p == 3U ? 2U : p / 2U;
    const uint32_t high = p - low;
    b->masked = p < TWO_PIN_MASK_WHOLE_BELOW;
    gen_schedule_reset(&layout);
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
    place(f2 + 3U, UINT32_MAX, S_CONSUME);
    // The sample goes in the block's last cycle, just before the next block's fall. That fall
    // ends the TDO packet, which is a whole period with a whole high phase, and the next frame's
    // first rising edge comes a whole period after this one's last.
    const uint32_t used = layout.ev[layout.n - 1U].cycle;
    uint32_t cycles = later(f2 + latency + MARGIN + 1U, used + 2U);
    cycles = later(later(cycles, f2 + p), later(r3 + high, r3 + p - r1));
    b->sample = cycles - 1U;
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
    gen_load_const(e, R_GPIO, (uint32_t)GPIO);
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
        gen_load_const(e, R_TXEN, (uint32_t)&GPIO->MASK[0]);
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
        gen_load_const(e, R_TXEN, (uint32_t)&GPIO->MASK[0]);
        gen_emit(e, t_str(R_TMS, R_TXEN, 0U));
    }
    if (b->masked) {
        gen_emit(e, t_msr_primask(R_ZERO));
    }
    gen_emit(e, t_pop(0x8FF0U)); // r4-r11, pc
    gen_resolve(e);
    return !e->failed;
}

bool jtag_gen_build(const struct jtag_gen_config* config, jtag_gen_scan_fn* scan) {
    if ((config->period < JTAG_GEN_PERIOD_MIN) || (config->period > JTAG_GEN_PERIOD_MAX) || (config->latency == 0U) ||
        (config->latency > 64U) || (config->wire > JTAG_GEN_TWO_PIN)) {
        return false;
    }
    const bool two_pin = config->wire == JTAG_GEN_TWO_PIN;
    static struct block b;
    if (!(two_pin ? layout_two_pin(&b, config->period, config->latency)
                  : layout_four_wire(&b, config->period, config->latency))) {
        return false;
    }
    struct gen_emitter e = {
        .code = (uint32_t*)JTAG_GEN_BASE, .cap = JTAG_GEN_SIZE / 4U, .delay_reg = R_DELAY, .filler = FILLER};
    const uint32_t scan_at = e.pos;
    if (!emit_scan(&e, &b, two_pin)) {
        return false;
    }

    jtag_gen_info = (struct jtag_gen_info){
        .period = two_pin ? config->period : b.cycles,
        .frame = b.cycles,
        .code_bytes = 4U * e.pos,
        .masked = b.masked,
        .ok = true,
    };
    // The code was written as data; the core has to fetch what is there now.
    __DSB();
    __ISB();
    *scan = (jtag_gen_scan_fn)(JTAG_GEN_BASE + 4U * scan_at + 1U);
    return true;
}

/* -- when to generate ------------------------------------------------------------------------- */

uint32_t jtag_gen_clock_hz = DAP_DEFAULT_SWJ_CLOCK;
struct jtag_gen_info jtag_gen_info;
uint32_t jtag_gen_latency[2] = {JTAG_GEN_LATENCY_DEFAULT, JTAG_GEN_LATENCY_DEFAULT};
jtag_gen_scan_fn jtag_gen_scan;

static struct jtag_gen_config built;
static bool built_once;
static bool built_ok;
uint32_t jtag_gen_builds;
uint32_t jtag_gen_failures;

/** The shortest period no faster than the clock asked for. */
static uint32_t period_for(uint32_t hz) {
    const uint32_t period = ((CPU_CLOCK - 1U) / hz) + 1U;
    return period < JTAG_GEN_PERIOD_MIN ? JTAG_GEN_PERIOD_MIN : period;
}

bool jtag_gen_ready(uint32_t wire) {
    const struct jtag_gen_config want = {
        .wire = wire,
        .period = period_for(jtag_gen_clock_hz),
        .latency = jtag_gen_latency[wire == JTAG_GEN_TWO_PIN ? JTAG_GEN_TWO_PIN : JTAG_GEN_FOUR_WIRE],
    };
    if (built_once && (memcmp(&want, &built, sizeof want) == 0)) {
        return built_ok;
    }
    built = want;
    built_once = true;
    jtag_gen_info.ok = false;
    built_ok = jtag_gen_build(&want, &jtag_gen_scan);
    if (built_ok) {
        jtag_gen_builds++;
    } else {
        jtag_gen_failures++;
    }
    return built_ok;
}
