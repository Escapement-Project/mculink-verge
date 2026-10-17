/** Generate the two-pin (IEEE 1149.7 OScan1) wire path at run time, placed to the cycle.
 *
 * CJTAGGEN.md has the wire this places by, measured on a CC1312R1. TIMING.md has the cycle rules,
 * and src/swd_gen.c is the SWD counterpart. In brief:
 *
 * - A JTAG clock is a frame of three TCKC packets: nTDI and TMS, which the probe drives and the
 *   target reads on the rising edge, then TDO, which the target drives. TMSC moves only while
 *   TCKC is low, because a move while it is high is an escape, which the target's adapter acts
 *   on. So a packet the probe drives is a falling-edge store, a data store and a rising-edge
 *   store, and no store moves both pins.
 * - The probe lets go of TMSC right after the fall that starts the TDO packet, buffer first, and
 *   takes it back right after the fall that ends it, pin first. TDO is on the pin from `latency`
 *   cycles after the first of those falls until after the second, so the sample goes just
 *   before the second: an interrupt can only make it later, and that edge waits for it.
 *   Interrupts are masked around the release, where one would leave the probe driving against
 *   the target, or for the whole run where a frame has no cycle to spare for that.
 * - No packet is shorter than the period, and neither phase shorter than its share of it.
 *
 * The generated function is a loop of one frame. What a frame does for the next one, its nTDI,
 * folding in the last sample and the loop count, fills the cycles the TDO latency leaves idle.
 */
#include "jtag_gen.h"

#include <stddef.h>
#include <string.h>

#include "DAP_config.h"

#include "gen.h"
#include "thumb.h"

/* What the generated function keeps in registers for its whole run. */
#define R_TDI 0U    // TDI still to send, the next frame's bit lowest; then the result
#define R_COUNT 1U  // frames still to clock
#define R_TMS 2U    // TMS for every frame, 0 or 1
#define R_TXEN 3U   // the TMSC buffer's direction pin
#define R_ACC 4U    // TDO so far, the first frame's bit highest
#define R_ONE 5U    // TCKC's mask: also the 1 a byte-register store takes
#define R_TMSC 6U   // TMSC's mask, for the direction registers
#define R_DELAY 7U  // delay loops
#define R_GPIO 8U   // the byte pin registers
#define R_PORT 9U   // MPIN0, with SET0, CLR0, DIRSET0 and DIRCLR0 at the offsets below
#define R_NTDI 10U  // the next frame's nTDI, 0 or 1
#define R_SAMPLE 11U
#define R_SHIFT 12U // 32 less the frame count, where the reversed result starts
#define R_ZERO T_LR

#define TCKC (1U << DBGIF_TCK_SWCLK_PIN)
#define TMSC_PIN DBGIF_TMS_SWDIO_PIN
#define TMSC (1U << TMSC_PIN)
#define TXEN (1U << DBGIF_TMS_SWDIO_TXEN_PIN)

_Static_assert((DBGIF_TCK_SWCLK_PORT == 0U) && (DBGIF_TMS_SWDIO_PORT == 0U) && (DBGIF_TMS_SWDIO_TXEN_PORT == 0U),
               "the generated code reaches port 0 only");
_Static_assert(TCKC == 1U, "R_ONE is the clock mask and also the 1 a byte-register store takes");
_Static_assert(TMSC < 256U, "R_TMSC is loaded as an 8-bit immediate");

#define PORT_BASE ((uint32_t)&GPIO->MPIN[0])
#define PORT_OFFSET(field) ((uint32_t)(offsetof(GPIO_Type, field) - offsetof(GPIO_Type, MPIN)))
#define OFF_SET PORT_OFFSET(SET)
#define OFF_CLR PORT_OFFSET(CLR)
#define OFF_DIRSET PORT_OFFSET(DIRSET)
#define OFF_DIRCLR PORT_OFFSET(DIRCLR)
#define OFF_B_TMSC ((uint32_t)offsetof(GPIO_Type, B[0][TMSC_PIN]))

#define FILLER t_mov(R_TMSC, R_TMSC)

/** Cycles a sample waits past the first that sees TDO. The latency was measured on one die. */
#define EARLY_MARGIN 1U

/** Periods below this run with interrupts masked for the whole call. A frame there has no cycle
 * to spare for masking around each release, and a whole call takes a few microseconds. */
#define MASK_WHOLE_BELOW 9U

#define LABEL_LOOP 0U

struct gen_cfg {
    uint32_t period;
    uint32_t low; // cycles from a falling edge to the rising edge, at least
    uint32_t high;
    uint32_t latency;
    bool masked;
};

/** Where one frame's edges fell, in cycles from the start of the loop body. */
struct frame {
    uint32_t length;   // from one pass's first instruction to the next pass's
    uint32_t tdo_fall; // the fall that starts the TDO packet
    uint32_t tdo_rise;
    uint32_t sample;
};

/* Static rather than on the stack: it is the largest thing here. */
static struct gen_schedule body;

static uint32_t later(uint32_t a, uint32_t b) { return a > b ? a : b; }

/** `insn` on the first free cycle from `earliest`. */
static uint32_t place(struct gen_schedule* s, uint32_t earliest, uint32_t insn) {
    while (gen_taken(s, earliest)) {
        earliest++;
    }
    gen_put(s, earliest, insn);
    return earliest;
}

/** One frame as the body of the loop, starting at the fall that ends the last frame's TDO packet.
 * On the first pass the clock is already low and the wire already driven, so that fall and the
 * take-back after it change nothing. */
static void schedule_frame(struct gen_schedule* s, const struct gen_cfg* c, struct frame* f) {
    const uint32_t p = c->period;
    gen_schedule_reset(s);

    // Packet 1, nTDI: the wire back, pin then buffer, then the bit.
    gen_put(s, 0U, t_str(R_ONE, R_PORT, OFF_CLR));
    gen_put(s, 1U, t_str(R_TMSC, R_PORT, OFF_DIRSET));
    gen_put(s, 2U, t_str(R_TXEN, R_PORT, OFF_SET));
    gen_put(s, 3U, t_strb(R_NTDI, R_GPIO, OFF_B_TMSC));
    const uint32_t r1 = later(4U, c->low);
    gen_put(s, r1, t_str(R_ONE, R_PORT, OFF_SET));

    // Packet 2, TMS.
    const uint32_t f1 = later(r1 + c->high, p);
    gen_put(s, f1, t_str(R_ONE, R_PORT, OFF_CLR));
    gen_put(s, f1 + 1U, t_strb(R_TMS, R_GPIO, OFF_B_TMSC));
    const uint32_t r2 = later(f1 + later(c->low, 2U), r1 + p);
    gen_put(s, r2, t_str(R_ONE, R_PORT, OFF_SET));

    // Packet 3, TDO: let go of the wire, buffer first, as soon as the clock is low.
    const uint32_t f2 = later(r2 + c->high, f1 + p);
    if (!c->masked) {
        // As late as possible, so that as little as possible runs masked.
        uint32_t at = f2 - 1U;
        while (gen_taken(s, at)) {
            at--;
        }
        gen_put(s, at, t_msr_primask(R_ONE));
    }
    gen_put(s, f2, t_str(R_ONE, R_PORT, OFF_CLR));
    gen_put(s, f2 + 1U, t_str(R_TXEN, R_PORT, OFF_CLR));
    gen_put(s, f2 + 2U, t_str(R_TMSC, R_PORT, OFF_DIRCLR));
    const uint32_t r3 = later(f2 + later(c->low, 3U), r2 + p);
    gen_put(s, r3, t_str(R_ONE, R_PORT, OFF_SET));
    if (!c->masked) {
        place(s, f2 + 3U, t_msr_primask(R_ZERO));
    }

    // The next frame's nTDI, and the last frame's sample folded in.
    const uint32_t shifted = place(s, f2 + 3U, t_lsr(R_TDI, R_TDI, 1U));
    place(s, shifted + 1U, t_bic(R_NTDI, R_ONE, R_TDI, T_LSL, 0U));
    place(s, f2 + 3U, t_orr(R_ACC, R_SAMPLE, R_ACC, T_LSL, 1U));

    // The sample, then the branch back, whose taken path puts the next fall three cycles after the
    // sample. The subs sits right before them, because a delay loop in between would set the flags.
    uint32_t sample = later(f2 + c->latency + EARLY_MARGIN, s->ev[s->n - 1U].cycle + 2U);
    // The TDO packet is a whole period, its high phase a whole share, and the next frame's first
    // rising edge a whole period after this one's last.
    sample = later(sample + 3U, f2 + p) - 3U;
    sample = later(sample + 3U, r3 + c->high) - 3U;
    sample = later(sample + 3U + r1, r3 + p) - 3U - r1;
    gen_put(s, sample - 1U, t_subs_imm(R_COUNT, R_COUNT, 1U));
    gen_put(s, sample, t_ldrb(R_SAMPLE, R_GPIO, OFF_B_TMSC));
    gen_put(s, sample + 1U, GEN_BRANCH(T_COND_NE, LABEL_LOOP));

    *f = (struct frame){.length = sample + 3U, .tdo_fall = f2, .tdo_rise = r3, .sample = sample};
}

static bool emit_scan(struct gen_emitter* e, const struct gen_cfg* c, struct frame* f) {
    schedule_frame(&body, c, f);
    if (body.failed) {
        return false;
    }
    gen_emit(e, t_push(0x4FF0U)); // r4-r11, lr
    gen_load_const(e, R_GPIO, (uint32_t)GPIO);
    gen_load_const(e, R_PORT, PORT_BASE);
    gen_load_const(e, R_TXEN, TXEN);
    gen_emit(e, t_mov_imm(R_ONE, TCKC));
    gen_emit(e, t_mov_imm(R_TMSC, TMSC));
    gen_emit(e, t_mov_imm(R_ZERO, 0U));
    gen_emit(e, t_mov_imm(R_ACC, 0U));
    // The first pass folds this in, a zero above every real sample.
    gen_emit(e, t_mov_imm(R_SAMPLE, 0U));
    gen_emit(e, t_rsb_imm(R_SHIFT, R_COUNT, 32U));
    gen_emit(e, t_bic(R_NTDI, R_ONE, R_TDI, T_LSL, 0U));
    if (c->masked) {
        gen_emit(e, t_msr_primask(R_ONE));
    }

    gen_bind(e, LABEL_LOOP);
    // The branch falls through a cycle after it issues, which is where this count ends.
    const uint32_t now = gen_emit_schedule(e, &body);
    const uint32_t fall = later(later(now, f->tdo_fall + c->period), f->tdo_rise + c->high);
    gen_delay(e, fall - now);
    gen_emit(e, t_str(R_ONE, R_PORT, OFF_CLR));
    gen_emit(e, t_str(R_TMSC, R_PORT, OFF_DIRSET));
    gen_emit(e, t_str(R_TXEN, R_PORT, OFF_SET));
    gen_emit(e, t_orr(R_ACC, R_SAMPLE, R_ACC, T_LSL, 1U));
    if (c->masked) {
        gen_emit(e, t_msr_primask(R_ZERO));
    }
    gen_emit(e, t_rbit(R_TDI, R_ACC));
    gen_emit(e, t_lsr_reg(R_TDI, R_TDI, R_SHIFT));
    gen_emit(e, t_pop(0x8FF0U)); // r4-r11, pc
    gen_resolve(e);
    return !e->failed;
}

bool jtag_gen_build(const struct jtag_gen_config* config, jtag_gen_scan_fn* scan) {
    if ((config->period < JTAG_GEN_PERIOD_MIN) || (config->period > JTAG_GEN_PERIOD_MAX) ||
        (config->latency == 0U) || (config->latency > 64U)) {
        return false;
    }
    // At three cycles a packet rises two after its fall: the data has a cycle to settle and holds
    // for one, which the bench measured accepted.
    const uint32_t low = config->period == 3U ? 2U : config->period / 2U;
    const struct gen_cfg c = {
        .period = config->period,
        .low = low,
        .high = config->period - low,
        .latency = config->latency,
        .masked = config->period < MASK_WHOLE_BELOW,
    };
    struct gen_emitter e = {
        .code = (uint32_t*)JTAG_GEN_BASE, .cap = JTAG_GEN_SIZE / 4U, .delay_reg = R_DELAY, .filler = FILLER};
    struct frame f;

    const uint32_t scan_at = e.pos;
    if (!emit_scan(&e, &c, &f)) {
        return false;
    }

    jtag_gen_info = (struct jtag_gen_info){
        .period = c.period,
        .frame = f.length,
        .code_bytes = 4U * e.pos,
        .masked = c.masked,
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
uint32_t jtag_gen_latency = JTAG_GEN_LATENCY_DEFAULT;
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

bool jtag_gen_ready(void) {
    const struct jtag_gen_config want = {
        .period = period_for(jtag_gen_clock_hz),
        .latency = jtag_gen_latency,
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
