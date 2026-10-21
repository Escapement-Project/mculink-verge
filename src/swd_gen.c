/** Generate the SWD transfer paths at run time, placed to the cycle.
 *
 * TIMING.md has the rules this places by and CODEGEN.md the reasoning. In brief:
 *
 * - Generated code runs from SRAMX, which the core fetches over C-AHB. There every 32-bit
 *   instruction costs one cycle, and GPIO loads and stores pipeline back to back. A schedule is
 *   therefore a list of (cycle, instruction), and the code is that list with the gaps filled: a
 *   filler a cycle, or a counted loop of three cycles an iteration.
 * - A clock falls on a store to the masked port register, which carries the probe's SWDIO bit in
 *   the same store, and rises on a store to SET. The data bit changes with the falling edge, so it
 *   has half a period either side of the rising edge the target samples it on, and on the wire it
 *   comes before the fall, as ARM's does.
 * - A bit the target launches on the rising-edge store at cycle R is on the pin for loads from
 *   R + latency until the next rising-edge store plus latency, less one. Up to a period of
 *   latency + SAFE_MARGIN cycles a sample goes in the middle of that window, the later of two
 *   middles, which can be after the next rising edge. Those transfers run masked: an interrupt
 *   between that edge and the sample would make it read the next bit. Longer periods sample just
 *   before the next rising edge, where an interrupt only stretches the clock.
 * - No clock is ever shorter than the period. Where an instruction needs a cycle a clock would
 *   have used, the clock waits.
 *
 * The functions follow ARM's SWD_Transfer pin for pin, which tools/equiv.py checks: the same
 * edges in the same order, with SWDIO at the same level on each, for every acknowledge, every
 * turnaround and both data-phase settings. Reads with CTimer 0's clock match on every rising edge
 * (equiv's --ignore-falls): the timer stops with SWCLK low, so falls move and SWCLK rests low.
 */
#include "swd_gen.h"

#include <stddef.h>
#include <string.h>

#include "DAP_config.h"

#include "DAP.h"
#include "gen.h"
#include "swd_timer.h"
#include "thumb.h"

/* What the generated functions keep in registers for their whole run. */
#define R_PKT 0U   // the request packet, bit n clocked on clock n
#define R_BUF 1U   // words read or written, post-incremented
#define R_FOUR 2U  // SWDIO's mask: also SWDIO high in a masked-port store
#define R_TXEN 3U  // the SWDIO buffer's direction pin
#define R_COUNT 4U // transfers still to do, the current one included
#define R_ONE 5U   // SWCLK's mask: also the 1 a byte-register store takes
#define R_ZERO 6U
#define R_DELAY 7U // delay loops
#define R_GPIO 8U  // the byte pin registers
#define R_PORT 9U  // MPIN0, with SET0, CLR0, DIRSET0 and DIRCLR0 at the offsets below
#define R_WORD 10U
#define R_ACK 11U
#define R_T0 12U
#define R_T1 T_LR

#define SWCLK (1U << DBGIF_TCK_SWCLK_PIN)
#define SWDIO_PIN DBGIF_TMS_SWDIO_PIN
#define SWDIO (1U << SWDIO_PIN)
#define TXEN (1U << DBGIF_TMS_SWDIO_TXEN_PIN)

_Static_assert((DBGIF_TCK_SWCLK_PORT == 0U) && (DBGIF_TMS_SWDIO_PORT == 0U) && (DBGIF_TMS_SWDIO_TXEN_PORT == 0U),
               "the generated code reaches port 0 only");
_Static_assert(SWCLK == 1U, "R_ONE is the clock mask and also the 1 a byte-register store takes");
_Static_assert(SWDIO < 256U, "R_FOUR is loaded as an 8-bit immediate");

#define PORT_BASE ((uint32_t)&DBGIF_GPIO->MPIN[0])
#define PORT_OFFSET(field) ((uint32_t)(offsetof(GPIO_Type, field) - offsetof(GPIO_Type, MPIN)))
#define OFF_MPIN 0U
#define OFF_SET PORT_OFFSET(SET)
#define OFF_CLR PORT_OFFSET(CLR)
#define OFF_DIRSET PORT_OFFSET(DIRSET)
#define OFF_DIRCLR PORT_OFFSET(DIRCLR)
#define OFF_B_SWDIO ((uint32_t)offsetof(GPIO_Type, B[0][SWDIO_PIN]))

/** How much early margin a sample taken before the next rising edge must have before a period is
 * run unmasked, in cycles. Below it the window's middle, after that edge, is worth the mask. */
#define SAFE_MARGIN 5U

/** Idle clocks scheduled inline. More go in a loop after the transfer. */
#define IDLE_INLINE 4U

#define FILLER t_mov(R_ZERO, R_ZERO)

struct gen_cfg {
    // Clocks the target drives, and the turnarounds and idle clocks: a read clock is four
    // instructions, so never shorter than SWD_GEN_PERIOD_MIN.
    uint32_t period;
    uint32_t low; // cycles from a falling edge to the rising edge
    // Clocks the probe drives: an edge store each and the next bit's extraction, so three.
    uint32_t wperiod;
    uint32_t wlow;
    uint32_t latency;
    uint32_t turnaround;
    uint32_t data_phase;
    uint32_t idle;
    bool masked;
    bool timer; // reads clocked by CTimer 0 (src/swd_timer.h)
};

/* -- schedule ---------------------------------------------------------------------------------- */

struct sched {
    const struct gen_cfg* cfg;
    uint32_t t; // the earliest cycle the next clock may fall on
    uint32_t last_fall;
    uint32_t last_rise;
    // The last clock's period and low phase, which the clocks still to come are assumed to share.
    uint32_t grid_period;
    uint32_t grid_low;
    uint32_t last_load;
    // Over every sample so far: the fewest cycles after the edge that launched its bit, and the
    // fewest before the edge that launches the next, negative for a sample after that edge.
    int32_t after_launch;
    int32_t before_next;
    struct gen_schedule q;
};

/* Static rather than on the stack: it is the largest thing here. */
static struct sched sched;
static struct gen_event sched_events[256];

static void sched_reset(struct sched* s, const struct gen_cfg* cfg) {
    s->cfg = cfg;
    gen_schedule_reset(&s->q, sched_events, sizeof sched_events / sizeof sched_events[0]);
    s->t = 0U;
    s->last_fall = 0U;
    s->last_rise = 0U;
    s->last_load = 0U;
    s->after_launch = INT32_MAX;
    s->before_next = INT32_MAX;
    s->grid_period = cfg->wperiod;
    s->grid_low = cfg->wlow;
}

static bool taken(const struct sched* s, uint32_t cycle) { return gen_taken(&s->q, cycle); }

static void put(struct sched* s, uint32_t cycle, uint32_t insn) { gen_put(&s->q, cycle, insn); }

/** Whether a clock not laid yet puts an edge on `cycle`, if the clocks from s->t on are like the
 * last one. Where they are not, a clock that finds its cycle taken waits a cycle. */
static bool on_grid(const struct sched* s, uint32_t cycle) {
    if (cycle < s->t) {
        return false;
    }
    const uint32_t phase = (cycle - s->t) % s->grid_period;
    return (phase == 0U) || (phase == s->grid_low);
}

/** An instruction the next clock has to wait for: the first free cycle from `earliest`. */
static uint32_t op(struct sched* s, uint32_t earliest, uint32_t insn) {
    uint32_t cycle = earliest;
    while (taken(s, cycle)) {
        cycle++;
    }
    put(s, cycle, insn);
    if (cycle >= s->t) {
        s->t = cycle + 1U;
    }
    return cycle;
}

/** An instruction that must not hold up a clock: the first free cycle from `earliest` that no
 * clock still to come will want. */
static uint32_t side(struct sched* s, uint32_t earliest, uint32_t insn) {
    uint32_t cycle = earliest;
    while (taken(s, cycle) || on_grid(s, cycle)) {
        cycle++;
    }
    put(s, cycle, insn);
    return cycle;
}

/** One clock, falling on `fall` at the first cycle from s->t where both of its edges fit.
 * Returns the cycle of its rising edge. */
static uint32_t clock(struct sched* s, uint32_t fall, uint32_t period, uint32_t low) {
    uint32_t cycle = s->t;
    while (taken(s, cycle) || taken(s, cycle + low)) {
        cycle++;
    }
    put(s, cycle, fall);
    put(s, cycle + low, t_str(R_ONE, R_PORT, OFF_SET));
    s->t = cycle + period;
    s->last_fall = cycle;
    s->last_rise = cycle + low;
    s->grid_period = period;
    s->grid_low = low;
    return cycle + low;
}

/** A clock with the probe driving `bit`, a register holding SWDIO's mask or zero. */
static uint32_t drive(struct sched* s, uint32_t bit) {
    return clock(s, t_str(bit, R_PORT, OFF_MPIN), s->cfg->wperiod, s->cfg->wlow);
}

/** A clock with the wire left as it is. */
static uint32_t idle_clock(struct sched* s) {
    return clock(s, t_str(R_ONE, R_PORT, OFF_CLR), s->cfg->period, s->cfg->low);
}

/** Bit `bit` of `rm` into SWDIO's position in `rd`, everything else clear. */
static uint32_t extract(uint32_t rd, uint32_t rm, uint32_t bit) {
    // A shift of zero has to be LSL: LSR with a zero amount encodes LSR #32.
    if (bit > SWDIO_PIN) {
        return t_and(rd, R_FOUR, rm, T_LSR, bit - SWDIO_PIN);
    }
    return t_and(rd, R_FOUR, rm, T_LSL, SWDIO_PIN - bit);
}

/** The load that samples the bit launched by the rising edge at `launch`, whose window the rising
 * edge at `end` closes, into `reg`, no earlier than `not_before`. */
static uint32_t sample(struct sched* s, uint32_t launch, uint32_t end, uint32_t reg, uint32_t not_before) {
    const struct gen_cfg* c = s->cfg;
    const uint32_t first = launch + c->latency;
    uint32_t best = 0U;
    bool found = false;
    if (c->masked) {
        // The middle of the window, which the next clocks may not have reached yet. Of two cycles
        // with the same margin the later: a bit that arrives later than measured, as some do after
        // a reset, is the likelier mistake.
        const uint32_t last = end + c->latency - 1U;
        uint32_t best_margin = 0U;
        for (uint32_t cycle = first; cycle <= last; cycle++) {
            if ((cycle < not_before) || taken(s, cycle) || on_grid(s, cycle)) {
                continue;
            }
            const uint32_t early = cycle - first;
            const uint32_t late = last - cycle;
            const uint32_t margin = early < late ? early : late;
            if (!found || (margin >= best_margin)) {
                best = cycle;
                best_margin = margin;
                found = true;
            }
        }
    } else {
        // As late as possible before the edge that ends the window: every cycle there is laid.
        for (uint32_t cycle = end; cycle-- > first;) {
            if ((cycle >= not_before) && !taken(s, cycle)) {
                best = cycle;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        s->q.failed = true;
        return s->last_load;
    }
    put(s, best, t_ldrb(reg, R_GPIO, OFF_B_SWDIO));
    if (best > s->last_load) {
        s->last_load = best;
    }
    const int32_t after = (int32_t)(best - launch);
    const int32_t before = (int32_t)(end - best);
    s->after_launch = after < s->after_launch ? after : s->after_launch;
    s->before_next = before < s->before_next ? before : s->before_next;
    return best;
}

/** Lets go of SWDIO after the packet's last rising edge, before the next clock. */
static void release(struct sched* s) {
    const uint32_t at = op(s, s->last_rise + 1U, t_str(R_TXEN, R_PORT, OFF_CLR));
    op(s, at + 1U, t_str(R_FOUR, R_PORT, OFF_DIRCLR));
}

/** Takes SWDIO back, no earlier than `earliest`. Returns the cycle of the second store. */
static uint32_t enable(struct sched* s, uint32_t earliest) {
    const uint32_t at = op(s, earliest, t_str(R_FOUR, R_PORT, OFF_DIRSET));
    return op(s, at + 1U, t_str(R_TXEN, R_PORT, OFF_SET));
}

/** Start, APnDP, RnW, A2, A3, parity, stop, park. */
static void packet(struct sched* s) {
    drive(s, R_FOUR);
    for (uint32_t k = 1U; k < 8U; k++) {
        uint32_t bit = R_FOUR;
        if (k == 6U) {
            bit = R_ZERO;
        } else if (k <= 5U) {
            // Two registers in turn, so none is written while the store before reads it.
            bit = (k & 1U) ? R_ACK : R_T0;
            op(s, s->last_fall + 1U, extract(bit, R_PKT, k));
        }
        drive(s, bit);
    }
    release(s);
}

/** The acknowledge bits, and for a read the data and parity: bit k into its place. */
static uint32_t consume_insn(uint32_t k, uint32_t reg) {
    if (k == 0U) {
        return t_mov(R_ACK, reg);
    }
    if (k < 3U) {
        return t_orr(R_ACK, R_ACK, reg, T_LSL, k);
    }
    if (k == 3U) {
        return t_mov(R_WORD, reg);
    }
    if (k < 35U) {
        return t_orr(R_WORD, R_WORD, reg, T_LSL, k - 3U);
    }
    return t_mov(R_ACK, reg);
}

#define LABEL_LOOP 1U
#define LABEL_FAIL 2U
#define LABEL_PARITY 3U
#define LABEL_WAIT_FAULT 4U
#define LABEL_EXIT 5U
#define LABEL_CLAIM 6U

/** Where a body leaves its schedule, for what comes after it. */
struct body {
    uint32_t branch;      // the cycle of the branch to LABEL_FAIL
    uint32_t branch_rise; // the last rising edge before that branch
    bool idle_loop;       // idle clocks still to run in a loop after the schedule
};

/** Clocks the target drives, sampling bits `from` up to `to`, from the rising edge at `launch`.
 * `chain` holds the last consume of R_ACK and of R_WORD, and `busy` the last use of each sample
 * register. A read decides on the acknowledge after its turnaround clocks, at bit 2 +
 * turnaround. */
struct sampling {
    uint32_t launch;
    uint32_t chain_ack;
    uint32_t chain_word;
    uint32_t busy[2];
    uint32_t decided; // the cmp of the acknowledge, once placed
};

/** Branches to LABEL_FAIL unless the acknowledge is OK. Every path clocks the turnaround after the
 * acknowledge, so the branch comes after the last of those clocks: a path that leaves here has
 * clocked all of them, and nothing a path does differently can come before one. */
static void decide(struct sched* s, struct sampling* p, struct body* b) {
    // The cmp goes in the cycle before the branch: a delay loop between them would set the flags.
    uint32_t at = p->chain_ack + 2U > s->last_rise + 1U ? p->chain_ack + 2U : s->last_rise + 1U;
    while (taken(s, at - 1U) || taken(s, at)) {
        at++;
    }
    put(s, at - 1U, t_cmp_imm(R_ACK, DAP_TRANSFER_OK));
    put(s, at, GEN_BRANCH(T_COND_NE, LABEL_FAIL));
    if (at >= s->t) {
        s->t = at + 1U;
    }
    p->decided = at - 1U;
    b->branch = at;
    b->branch_rise = s->last_rise;
}

static void target_bits(struct sched* s, struct sampling* p, struct body* b, uint32_t from, uint32_t to,
                        bool read) {
    const uint32_t decide_at = 2U + s->cfg->turnaround;
    for (uint32_t k = from; k < to; k++) {
        const uint32_t rise = idle_clock(s);
        const uint32_t reg = (k & 1U) ? R_T1 : R_T0;
        const uint32_t load = sample(s, p->launch, rise, reg, p->busy[k & 1U] + 1U);
        uint32_t earliest = load + 2U;
        uint32_t* chain = (k < 3U) || (k == 35U) ? &p->chain_ack : &p->chain_word;
        if ((k != 0U) && (k != 3U) && (earliest <= *chain)) {
            earliest = *chain + 1U;
        }
        if ((k == 35U) && (earliest <= p->decided)) {
            earliest = p->decided + 1U;
        }
        *chain = side(s, earliest, consume_insn(k, reg));
        p->busy[k & 1U] = *chain;
        p->launch = rise;
        if (read && (k == decide_at)) {
            decide(s, p, b);
        }
    }
}

/** Idle clocks after a transfer, with SWDIO low, then SWDIO high. `earliest` is after the last
 * wire event before them. Long runs are left to a loop after the schedule. */
static void idle(struct sched* s, struct body* b, uint32_t earliest, bool out_high) {
    const uint32_t n = s->cfg->idle;
    b->idle_loop = n > IDLE_INLINE;
    if (n == 0U) {
        if (out_high) {
            op(s, earliest, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));
        }
        return;
    }
    op(s, earliest, t_strb(R_ZERO, R_GPIO, OFF_B_SWDIO));
    if (b->idle_loop) {
        return;
    }
    for (uint32_t i = 0U; i < n; i++) {
        idle_clock(s);
    }
    op(s, s->last_rise + 1U, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));
}

static void schedule_read(struct sched* s, struct body* b) {
    const struct gen_cfg* c = s->cfg;
    if (c->masked) {
        op(s, 0U, t_msr_primask(R_ONE));
    }
    packet(s);
    struct sampling p = {0};
    for (uint32_t i = 0U; i < c->turnaround; i++) {
        p.launch = idle_clock(s);
    }
    target_bits(s, &p, b, 0U, 36U, true);
    for (uint32_t i = 0U; i < c->turnaround; i++) {
        idle_clock(s);
    }
    // The probe takes the wire back once the turnaround is over and every sample is in.
    uint32_t earliest = (s->last_rise > s->last_load ? s->last_rise : s->last_load) + 1U;
    earliest = enable(s, earliest) + 1U;
    // SWDIO is still high from the park bit, so nothing raises it unless idle clocks lowered it.
    idle(s, b, earliest, false);
}

/** The write parity, folded into R_T1 a step at a time wherever a cycle is free. */
struct fold {
    uint32_t next;
    uint32_t ready;
};

static uint32_t fold_insn(uint32_t step) {
    if (step == 0U) {
        return t_eor(R_T1, R_WORD, R_WORD, T_LSR, 16U);
    }
    return t_eor(R_T1, R_T1, R_T1, T_LSR, 16U >> step);
}

static void fold_fill(struct sched* s, struct fold* f, uint32_t from) {
    for (uint32_t cycle = from > f->ready ? from : f->ready; (cycle < s->t) && (f->next < 5U); cycle++) {
        if (!taken(s, cycle)) {
            put(s, cycle, fold_insn(f->next++));
            f->ready = cycle + 1U;
        }
    }
}

static void schedule_write(struct sched* s, struct body* b) {
    const struct gen_cfg* c = s->cfg;
    if (c->masked) {
        op(s, 0U, t_msr_primask(R_ONE));
    }
    const uint32_t loaded = side(s, 0U, t_ldr_post(R_WORD, R_BUF, 4U));
    packet(s);
    struct sampling p = {0};
    for (uint32_t i = 0U; i < c->turnaround; i++) {
        p.launch = idle_clock(s);
    }
    target_bits(s, &p, b, 0U, 3U, false);
    for (uint32_t i = 0U; i < c->turnaround; i++) {
        idle_clock(s);
    }
    decide(s, &p, b);
    enable(s, b->branch + 1U);

    struct fold f = {0U, (loaded + 2U > b->branch + 1U) ? loaded + 2U : b->branch + 1U};
    op(s, f.ready, extract(R_T0, R_WORD, 0U));
    for (uint32_t j = 0U; j < 33U; j++) {
        drive(s, (j & 1U) ? R_ACK : R_T0);
        if (j < 32U) {
            const uint32_t next = j + 1U;
            const uint32_t reg = (next & 1U) ? R_ACK : R_T0;
            uint32_t earliest = s->last_fall + 1U;
            uint32_t insn = extract(reg, R_WORD, next);
            if (next == 32U) {
                while (f.next < 5U) {
                    f.ready = op(s, f.ready, fold_insn(f.next++)) + 1U;
                }
                if (earliest < f.ready) {
                    earliest = f.ready;
                }
                insn = t_and(reg, R_FOUR, R_T1, T_LSL, SWDIO_PIN);
            }
            op(s, earliest, insn);
        }
        fold_fill(s, &f, s->last_fall + 1U);
    }
    idle(s, b, s->last_rise + 1U, true);
}

/* -- code ------------------------------------------------------------------------------------- */

/** `n` clocks with the wire left as it is, `since` cycles after the last rising edge. Returns the
 * cycles from the last of them to the instruction after the loop. Not timing-critical: these
 * clocks only have to be no shorter than the period. */
static uint32_t clock_loop(struct gen_emitter* e, const struct gen_cfg* c, uint32_t n, uint32_t since) {
    const uint32_t high = c->period - c->low;
    if (since + 1U < high) {
        gen_delay(e, high - since - 1U); // and the counter's load is the last cycle of it
    }
    gen_emit(e, t_movw(R_T0, n));
    const uint32_t head = e->pos;
    gen_emit(e, t_str(R_ONE, R_PORT, OFF_CLR));
    gen_delay(e, c->low - 1U);
    gen_emit(e, t_str(R_ONE, R_PORT, OFF_SET));
    // The subs and a taken bne put the next fall four cycles after the delay.
    const uint32_t gap = high > 4U ? high - 4U : 0U;
    gen_delay(e, gap);
    gen_emit(e, t_subs_imm(R_T0, R_T0, 1U));
    gen_emit(e, t_bcond(T_COND_NE, ((int32_t)head - (int32_t)e->pos) * 4));
    return gap + 3U;
}

static void emit_enable(struct gen_emitter* e) {
    gen_emit(e, t_str(R_FOUR, R_PORT, OFF_DIRSET));
    gen_emit(e, t_str(R_TXEN, R_PORT, OFF_SET));
}

/** Returns the acknowledge in `ack`'s instruction and the count left. */
static void emit_return(struct gen_emitter* e, uint32_t ack) {
    gen_emit(e, ack);
    gen_emit(e, t_mov(1U, R_COUNT));
    gen_emit(e, t_pop(0x8FF0U)); // r4-r11, pc
}

/** After an acknowledge other than OK, `since` cycles after the last rising edge, with the
 * turnaround clocked: what ARM's SWD_Transfer clocks for WAIT, FAULT or a protocol error. */
static void emit_fail(struct gen_emitter* e, const struct gen_cfg* c, bool read, uint32_t since) {
    gen_emit(e, t_cmp_imm(R_ACK, DAP_TRANSFER_WAIT));
    gen_emit_branch(e, T_COND_EQ, LABEL_WAIT_FAULT);
    gen_emit(e, t_cmp_imm(R_ACK, DAP_TRANSFER_FAULT));
    gen_emit_branch(e, T_COND_EQ, LABEL_WAIT_FAULT);
    // A protocol error backs off a whole data phase, then takes the wire back.
    clock_loop(e, c, 32U + 1U, since + 4U);
    emit_enable(e);
    gen_emit(e, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));
    gen_emit_branch(e, T_AL, LABEL_EXIT);

    gen_bind(e, LABEL_WAIT_FAULT);
    const uint32_t after = since + 3U; // a cmp and a taken branch at least
    if (read) {
        if (c->data_phase) {
            clock_loop(e, c, 32U + 1U, after);
        }
        emit_enable(e);
    } else {
        emit_enable(e);
        if (c->data_phase) {
            gen_emit(e, t_strb(R_ZERO, R_GPIO, OFF_B_SWDIO));
            clock_loop(e, c, 32U + 1U, after + 3U);
        }
    }
    gen_emit(e, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));

    gen_bind(e, LABEL_EXIT);
    if (c->masked) {
        gen_emit(e, t_msr_primask(R_ZERO));
    }
    emit_return(e, t_mov(0U, R_ACK));
}

/** The target latencies, TIMING.md's A, for which every sample the last schedule placed reads
 * the bit it wants: a sample `after` cycles after its bit's launch and `before` cycles before the
 * next launch reads it for latencies from 1 - before to after. */
static void latencies_served(const struct sched* s, int32_t* first, int32_t* last) {
    *first = 1 - s->before_next;
    *last = s->after_launch;
}

/** One function: entry, the transfer loop, and its exits. */
static bool emit_function(struct gen_emitter* e, const struct gen_cfg* c, bool read) {
    struct sched* s = &sched;
    struct body b = {0};
    sched_reset(s, c);
    if (read) {
        schedule_read(s, &b);
    } else {
        schedule_write(s, &b);
    }
    if (s->q.failed) {
        return false;
    }
    uint32_t first_fall = 0U;
    for (uint32_t i = 0U; i < s->q.n; i++) {
        if (s->q.ev[i].insn == t_str(R_FOUR, R_PORT, OFF_MPIN)) {
            first_fall = s->q.ev[i].cycle;
            break;
        }
    }
    // The body starts on a packet clock and ends on whatever clock it ends on: the high phase
    // between them is the longer of the two kinds, which covers both.
    const uint32_t read_high = c->period - c->low;
    const uint32_t write_high = c->wperiod - c->wlow;
    const uint32_t high = read_high > write_high ? read_high : write_high;

    gen_emit(e, t_push(0x4FF0U)); // r4-r11, lr
    gen_emit(e, t_mov(R_COUNT, 2U));
    gen_load_const(e, R_GPIO, (uint32_t)DBGIF_GPIO);
    gen_load_const(e, R_PORT, PORT_BASE);
    gen_emit(e, t_mov_imm(R_ONE, SWCLK));
    gen_emit(e, t_mov_imm(R_ZERO, 0U));
    gen_emit(e, t_mov_imm(R_FOUR, SWDIO));
    gen_load_const(e, R_TXEN, TXEN);
    // Nothing says how long ago the last rising edge was.
    gen_delay(e, high);

    gen_bind(e, LABEL_LOOP);
    const uint32_t length = gen_emit_schedule(e, &s->q);
    uint32_t since = length - s->last_rise;
    if (b.idle_loop) {
        since = clock_loop(e, c, c->idle, since);
        gen_emit(e, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));
        since++;
    }
    if (c->masked) {
        gen_emit(e, t_msr_primask(R_ZERO));
        since++;
    }
    if (read) {
        // The parity, off the finished word: R_ACK holds the sampled bit.
        gen_emit(e, t_eor(R_T1, R_ACK, R_WORD, T_LSL, 0U));
        for (uint32_t shift = 16U; shift != 0U; shift >>= 1) {
            gen_emit(e, t_eor(R_T1, R_T1, R_T1, T_LSR, shift));
        }
        gen_emit(e, t_tst_imm(R_T1, 1U));
        gen_emit_branch(e, T_COND_NE, LABEL_PARITY);
        gen_emit(e, t_str_post(R_WORD, R_BUF, 4U));
        since += 9U;
    }
    // subs, then the taken branch back, then the next transfer's first fall.
    const uint32_t to_fall = since + 3U + first_fall;
    if (to_fall < high) {
        gen_delay(e, high - to_fall);
    }
    gen_emit(e, t_subs_imm(R_COUNT, R_COUNT, 1U));
    gen_emit_branch(e, T_COND_NE, LABEL_LOOP);
    emit_return(e, t_mov_imm(0U, DAP_TRANSFER_OK));

    if (read) {
        gen_bind(e, LABEL_PARITY);
        emit_return(e, t_mov_imm(0U, DAP_TRANSFER_ERROR));
    }

    gen_bind(e, LABEL_FAIL);
    emit_fail(e, c, read, b.branch + 2U - b.branch_rise);
    gen_resolve(e);
    return !e->failed;
}

/* -- reads with the clock from CTimer 0 -------------------------------------------------------- */

/* A read is two runs of the timer's clock, each started by the core and stopped by DMA1. The first
 * clocks the packet, the turnaround and the acknowledge, and stops after the clock that follows it:
 * on OK that is data bit 0's clock, and on anything else the turnaround ARM's path clocks before
 * deciding. The second clocks the rest of the data, the parity and the turnaround. Between runs
 * SWCLK stands low, so the core can decide, and run the GPIO tails, at its own pace.
 *
 * Cycles in a run count from the store that starts CTimer 0, with the stop's start the cycle after.
 * Every instruction from there is one cycle, as the generated code always is, and nothing in a run
 * touches SRAM or a peripheral but GPIO. */

/** Rising edges in each run, 46 between them. Edge 8 of the first, the turnaround's, launches the
 * acknowledge's first bit, and each edge after it launches the next bit. */
#define RUN_ACK_EDGES 13U
#define RUN_DATA_EDGES 33U
/** Rising edge k of a run, as the cycle of the store that would have made it on GPIO: a load sees
 * it three cycles later, and the target's latency counts from here as it does for a store. */
#define RUN_EDGE(k) (1U + 3U * (k))
/** The stop's match that ends a run after `edges` rising edges. DMA1's write lands 26 cycles after
 * the match, which this puts on the falling edge after the last rising one: a cycle earlier cuts
 * that clock's high phase to one cycle, and a cycle later lets one more edge through. */
#define RUN_STOP(edges) (3U * (edges) - 23U)
/** The earliest cycle of a run the next run's start may come on: after the stop has landed, since a
 * start before it would be undone. */
#define RUN_SETTLED(edges) (3U * (edges) + 6U)

/** Puts `insn` on the first free cycle from `earliest`. */
static uint32_t place_free(struct sched* s, uint32_t earliest, uint32_t insn) {
    uint32_t cycle = earliest;
    while (taken(s, cycle)) {
        cycle++;
    }
    put(s, cycle, insn);
    return cycle;
}

/** CTimer 0's start, from lr, and the stop's, from r12. */
static void run_start(struct sched* s) {
    put(s, 0U, t_str(R_ONE, R_T1, SWD_TIMER_TCR));
    put(s, 1U, t_str(R_ONE, R_T0, SWD_TIMER_TCR));
}

/** Samples the bit edge `k` launches into `reg`, at the middle of its window when that cycle is free.
 * A bit with no edge after it in the run stays on the pin, and its window has no end. */
static uint32_t run_sample(struct sched* s, uint32_t k, uint32_t reg, uint32_t not_before, bool stays) {
    const uint32_t launch = RUN_EDGE(k);
    const uint32_t first = launch + s->cfg->latency;
    const uint32_t last = RUN_EDGE(k + 1U) + s->cfg->latency - 1U;
    uint32_t best = 0U;
    uint32_t best_margin = 0U;
    bool found = false;
    for (uint32_t cycle = first; cycle <= (stays ? first + 2U : last); cycle++) {
        if ((cycle < not_before) || taken(s, cycle)) {
            continue;
        }
        const uint32_t early = cycle - first;
        const uint32_t late = stays ? early : last - cycle;
        const uint32_t margin = early < late ? early : late;
        if (!found || (margin > best_margin)) {
            best = cycle;
            best_margin = margin;
            found = true;
        }
    }
    if (!found) {
        s->q.failed = true;
        return 0U;
    }
    put(s, best, t_ldrb(reg, R_GPIO, OFF_B_SWDIO));
    if (best > s->last_load) {
        s->last_load = best;
    }
    const int32_t after = (int32_t)(best - launch);
    s->after_launch = after < s->after_launch ? after : s->after_launch;
    if (!stays) {
        const int32_t before = (int32_t)(RUN_EDGE(k + 1U) - best);
        s->before_next = before < s->before_next ? before : s->before_next;
    }
    return best;
}

/** The packet, the turnaround and the acknowledge into R_ACK, then data bits 0 and 1 into R_WORD:
 * bit 1 stands on the pin while the clock is stopped. */
static void schedule_run_ack(struct sched* s) {
    run_start(s);
    // The start bit is stored before the run starts, and SWDIO is driven. Each other bit is
    // stored on the falling edge, a cycle before its rising edge, as ARM's path changes it: the
    // target took the bits there and a cycle earlier, but not two. The first is extracted into
    // R_ACK before the run starts.
    uint32_t free_from[2] = {RUN_EDGE(1U) - 1U, 2U}; // r11, r12: r12 holds the stop's base until its start
    for (uint32_t k = 1U; k < 8U; k++) {
        const uint32_t at = RUN_EDGE(k) - 1U;
        uint32_t bit = R_ONE;
        if (k == 6U) {
            bit = R_ZERO;
        } else if (k == 1U) {
            bit = R_ACK;
        } else if (k <= 5U) {
            bit = (k & 1U) ? R_ACK : R_T0;
            const uint32_t extracted = place_free(s, free_from[k & 1U ? 0U : 1U], t_and(bit, R_ONE, R_PKT, T_LSR, k));
            if (extracted >= at) {
                s->q.failed = true;
            }
        }
        put(s, at, t_strb(bit, R_GPIO, OFF_B_SWDIO));
        free_from[k & 1U ? 0U : 1U] = at + 1U;
    }
    // Let go after the park bit's edge, before the turnaround's.
    const uint32_t txen = place_free(s, RUN_EDGE(7U) + 1U, t_str(R_TXEN, R_PORT, OFF_CLR));
    const uint32_t dir = place_free(s, txen + 1U, t_str(R_FOUR, R_PORT, OFF_DIRCLR));
    if (dir >= RUN_EDGE(8U)) {
        s->q.failed = true;
    }
    uint32_t busy[2] = {0U, 0U};
    uint32_t chain[2] = {0U, 0U}; // R_ACK's last consume, and R_WORD's
    for (uint32_t j = 0U; j < 5U; j++) {
        const uint32_t reg = (j & 1U) ? R_T1 : R_T0;
        const uint32_t load = run_sample(s, 8U + j, reg, busy[j & 1U] + 1U, j == 4U);
        const bool word = j >= 3U;
        uint32_t earliest = load + 2U;
        if ((j != 0U) && (j != 3U) && (earliest <= chain[word])) {
            earliest = chain[word] + 1U;
        }
        // Acknowledge bits 0 to 2, then data bit 0 as R_WORD and bit 1 into it.
        const uint32_t insn = (j == 4U) ? t_orr(R_WORD, R_WORD, reg, T_LSL, 1U) : consume_insn(j, reg);
        chain[word] = place_free(s, earliest, insn);
        busy[j & 1U] = chain[word];
    }
    place_free(s, s->last_load + 1U, t_msr_primask(R_ZERO));
}

/** Data bits 2 to 31 into R_WORD and the parity folded into bit 0 of R_ACK, which holds bits 0 and
 * 1's on entry: that bit ends as zero for a word whose parity is right. Then the probe takes the
 * wire back. */
static void schedule_run_data(struct sched* s) {
    run_start(s);
    // Every load first, each at the middle of its window, then their uses in the cycles between.
    // Edge k launches data bit k + 2, and edge 30 the parity.
    uint32_t load[31];
    uint32_t busy[2] = {1U, 0U}; // r12 holds the stop's base until its start, lr CTimer 0's
    for (uint32_t k = 0U; k < 31U; k++) {
        load[k] = run_sample(s, k, (k & 1U) ? R_T1 : R_T0, busy[k & 1U] + 1U, false);
        busy[k & 1U] = load[k];
    }
    for (uint32_t k = 0U; k < 31U; k++) {
        const uint32_t reg = (k & 1U) ? R_T1 : R_T0;
        uint32_t last = load[k];
        if (k < 30U) {
            last = place_free(s, load[k] + 2U, t_orr(R_WORD, R_WORD, reg, T_LSL, k + 2U));
        }
        last = place_free(s, (load[k] + 2U > last + 1U) ? load[k] + 2U : last + 1U, t_eor(R_ACK, R_ACK, reg, T_LSL, 0U));
        // The register's next load, two bits on, must come after its uses.
        if ((k + 2U < 31U) && (last >= load[k + 2U])) {
            s->q.failed = true;
        }
    }
    // The turnaround is the run's last edge. After it the target has let go, and every sample is in.
    const uint32_t from = (RUN_EDGE(32U) > s->last_load ? RUN_EDGE(32U) : s->last_load) + 1U;
    const uint32_t dir = place_free(s, from, t_str(R_FOUR, R_PORT, OFF_DIRSET));
    place_free(s, dir + 1U, t_str(R_TXEN, R_PORT, OFF_SET));
    place_free(s, s->last_load + 1U, t_msr_primask(R_ZERO));
}

/** Loads the two timer bases a run's start stores use, then reads CTimer 0. The read waits out any
 * store still on its way through the peripheral bridge, which would otherwise move the start a
 * cycle. */
static void emit_run_bases(struct gen_emitter* e, uint32_t stop) {
    gen_load_const(e, R_T0, stop);
    gen_load_const(e, R_T1, SWD_TIMER_CLOCK);
    gen_emit(e, t_ldr(R_DELAY, R_T1, SWD_TIMER_TCR));
}

/** SWCLK's pin to `value`, through r12 and lr. Back to GPIO, the output is lowered first: the
 * timer leaves SWCLK low, and a rising edge here would be a clock. */
static void emit_pin(struct gen_emitter* e, uint32_t value, bool gpio) {
    if (gpio) {
        gen_emit(e, t_str(R_ONE, R_PORT, OFF_CLR));
    }
    gen_load_const(e, R_T0, SWD_TIMER_PIN);
    gen_load_const(e, R_T1, value);
    gen_emit(e, t_str(R_T1, R_T0, 0U));
}

/** Takes the peripheral bridge from CPU 0's LED (dap_shared.swd_timer_busy), waiting out a write
 * it has started. */
static void emit_claim(struct gen_emitter* e) {
    gen_load_const(e, R_T0, SWD_TIMER_BUSY);
    gen_emit(e, t_strb(R_ONE, R_T0, 0U));
    gen_emit(e, t_dmb());
    gen_bind(e, LABEL_CLAIM);
    gen_emit(e, t_ldrb(R_T1, R_T0, 1U));
    gen_emit(e, t_cmp_imm(R_T1, 0U));
    gen_emit_branch(e, T_COND_NE, LABEL_CLAIM);
}

/** SWCLK back to GPIO and the bridge back to CPU 0: every exit. */
static void emit_leave(struct gen_emitter* e, uint32_t pin_gpio) {
    emit_pin(e, pin_gpio, true);
    gen_load_const(e, R_T0, SWD_TIMER_BUSY);
    gen_emit(e, t_strb(R_ZERO, R_T0, 0U));
}

/** Pads so that the run that started `length` cycles ago, `edges` edges long, has stopped by the time
 * `more` further cycles have passed. */
static void emit_settle(struct gen_emitter* e, uint32_t length, uint32_t more, uint32_t edges) {
    if (length + more < RUN_SETTLED(edges)) {
        gen_delay(e, RUN_SETTLED(edges) - length - more);
    }
}

/** The read function with the clock from CTimer 0, served latencies left in `first` and `last`. */
static bool emit_timer_read(struct gen_emitter* e, const struct gen_cfg* c, uint32_t pin_gpio, int32_t* first,
                            int32_t* last) {
    struct sched* s = &sched;
    const uint32_t pin_timer = (pin_gpio & ~0xFU) | SWD_TIMER_PIN_FUNC;
    int32_t after = INT32_MAX;
    int32_t before = INT32_MAX;

    gen_emit(e, t_push(0x4FF0U)); // r4-r11, lr
    gen_emit(e, t_mov(R_COUNT, 2U));
    gen_load_const(e, R_GPIO, (uint32_t)DBGIF_GPIO);
    gen_load_const(e, R_PORT, PORT_BASE);
    gen_emit(e, t_mov_imm(R_ONE, SWCLK));
    gen_emit(e, t_mov_imm(R_ZERO, 0U));
    gen_emit(e, t_mov_imm(R_FOUR, SWDIO));
    gen_load_const(e, R_TXEN, TXEN);
    emit_claim(e);
    // CTimer 0 holds SWCLK low until the first run: a falling edge, which clocks nothing.
    emit_pin(e, pin_timer, false);

    gen_bind(e, LABEL_LOOP);
    // The start bit, since a sequence may have left SWDIO low. Well before the start: a GPIO store
    // just before it holds the start store up.
    gen_emit(e, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));
    gen_emit(e, t_and(R_ACK, R_ONE, R_PKT, T_LSR, 1U));
    emit_run_bases(e, SWD_TIMER_STOP_ACK);
    gen_emit(e, t_msr_primask(R_ONE));
    sched_reset(s, c);
    schedule_run_ack(s);
    if (s->q.failed) {
        return false;
    }
    after = s->after_launch;
    before = s->before_next;
    const uint32_t ack_length = gen_emit_schedule(e, &s->q);
    // To the next start: a cmp, the branch, an eor, five to load the bases and the mask.
    emit_settle(e, ack_length, 9U, RUN_ACK_EDGES);
    gen_emit(e, t_cmp_imm(R_ACK, DAP_TRANSFER_OK));
    gen_emit_branch(e, T_COND_NE, LABEL_FAIL);
    // The parity so far in bit 0, from data bits 0 and 1.
    gen_emit(e, t_eor(R_ACK, R_WORD, R_WORD, T_LSR, 1U));
    emit_run_bases(e, SWD_TIMER_STOP_DATA);
    gen_emit(e, t_msr_primask(R_ONE));

    sched_reset(s, c);
    schedule_run_data(s);
    if (s->q.failed) {
        return false;
    }
    after = s->after_launch < after ? s->after_launch : after;
    before = s->before_next < before ? s->before_next : before;
    const uint32_t data_length = gen_emit_schedule(e, &s->q);
    // To the next start, the shortest way: tst, branch, store, subs, a taken branch of two, and the
    // eight before the start. The exit's pin store comes later than that.
    emit_settle(e, data_length, 14U, RUN_DATA_EDGES);
    gen_emit(e, t_tst_imm(R_ACK, 1U));
    gen_emit_branch(e, T_COND_NE, LABEL_PARITY);
    gen_emit(e, t_str_post(R_WORD, R_BUF, 4U));
    gen_emit(e, t_subs_imm(R_COUNT, R_COUNT, 1U));
    gen_emit_branch(e, T_COND_NE, LABEL_LOOP);
    emit_leave(e, pin_gpio);
    emit_return(e, t_mov_imm(0U, DAP_TRANSFER_OK));

    gen_bind(e, LABEL_PARITY);
    emit_leave(e, pin_gpio);
    emit_return(e, t_mov_imm(0U, DAP_TRANSFER_ERROR));

    // The clock has stopped after the turnaround that follows the acknowledge, where the GPIO path
    // decides too. SWCLK is low, and the tail's first clock starts with a fall that is already
    // there.
    gen_bind(e, LABEL_FAIL);
    emit_leave(e, pin_gpio);
    emit_fail(e, c, true, c->period);
    gen_resolve(e);
    *first = 1 - before;
    *last = after;
    return !e->failed;
}

bool swd_gen_build(const struct swd_gen_config* config, swd_gen_fn* read, swd_gen_fn* write) {
    if ((config->period < SWD_GEN_PERIOD_MIN) || (config->period > SWD_GEN_PERIOD_MAX) || (config->latency == 0U) ||
        (config->latency > 64U) || (config->turnaround < 1U) || (config->turnaround > 4U) ||
        (config->idle_cycles > 255U)) {
        return false;
    }
    const uint32_t period = config->period < SWD_GEN_READ_PERIOD_MIN ? SWD_GEN_READ_PERIOD_MIN : config->period;
    const bool timer = (config->timer != 0U) && (config->period == SWD_GEN_PERIOD_MIN) && (config->turnaround == 1U) &&
                       (config->idle_cycles == 0U);
    const struct gen_cfg c = {
        .period = period,
        .low = period / 2U,
        .wperiod = config->period,
        // At three cycles the edge rises two after the fall: the data has two cycles to settle
        // and holds for one, the shape the bench measured writes landing with.
        .wlow = config->period == 3U ? 2U : config->period / 2U,
        .latency = config->latency,
        .turnaround = config->turnaround,
        .data_phase = config->data_phase != 0U,
        .idle = config->idle_cycles,
        .masked = period < config->latency + 1U + SAFE_MARGIN,
        .timer = timer,
    };
    struct gen_emitter e = {
        .code = (uint32_t*)SWD_GEN_BASE, .cap = SWD_GEN_SIZE / 4U, .delay_reg = R_DELAY, .filler = FILLER};

    struct swd_gen_info info = {
        .write_period = c.wperiod,
        .read_period = c.period,
        .masked_from = period > SAFE_MARGIN ? period - SAFE_MARGIN : 1U,
        .masked = c.masked,
        .ok = true,
    };
    const uint32_t read_at = e.pos;
    if (timer) {
        info.read_period = config->period;
        info.masked_from = 1U;
        info.timer = true;
        if (!emit_timer_read(&e, &c, swd_timer_pin_gpio(), &info.read_first, &info.read_last)) {
            return false;
        }
    } else {
        if (!emit_function(&e, &c, true)) {
            return false;
        }
        latencies_served(&sched, &info.read_first, &info.read_last);
    }
    const uint32_t write_at = e.pos;
    if (!emit_function(&e, &c, false)) {
        return false;
    }
    latencies_served(&sched, &info.write_first, &info.write_last);
    info.code_bytes = 4U * e.pos;
    swd_gen_info = info;
    // Only SWCLK and SWDIO answer a masked-port store. Nothing else in the image uses MPIN.
    DBGIF_GPIO->MASK[0] = ~(SWCLK | SWDIO);
    if (timer) {
        swd_timer_setup(RUN_STOP(RUN_ACK_EDGES), RUN_STOP(RUN_DATA_EDGES));
    }
    // The code was written as data; the core has to fetch what is there now.
    __DSB();
    __ISB();
    *read = (swd_gen_fn)(SWD_GEN_BASE + 4U * read_at + 1U);
    *write = (swd_gen_fn)(SWD_GEN_BASE + 4U * write_at + 1U);
    return true;
}

/* -- when to generate ------------------------------------------------------------------------- */

uint32_t swd_gen_clock_hz = DAP_DEFAULT_SWJ_CLOCK;
bool swd_gen_timer = true;
struct swd_gen_info swd_gen_info;
uint32_t swd_gen_latency = SWD_GEN_LATENCY_DEFAULT;
swd_gen_fn swd_gen_read;
swd_gen_fn swd_gen_write;

static struct swd_gen_config built;
static bool built_once;
static bool built_ok;
/** Cleared by swd_gen_changed: until then the inputs are the ones `built` was made from. */
static bool current;
uint32_t swd_gen_builds;
uint32_t swd_gen_failures;

/** The shortest period no faster than the clock asked for. */
static uint32_t period_for(uint32_t hz) {
    const uint32_t period = ((CPU_CLOCK - 1U) / hz) + 1U;
    return period < SWD_GEN_PERIOD_MIN ? SWD_GEN_PERIOD_MIN : period;
}

void swd_gen_changed(void) { current = false; }

struct swd_gen_config swd_gen_inputs(void) {
    return (struct swd_gen_config){
        .period = period_for(swd_gen_clock_hz),
        .latency = swd_gen_latency,
        .turnaround = DAP_Data.swd_conf.turnaround,
        .data_phase = DAP_Data.swd_conf.data_phase,
        .idle_cycles = DAP_Data.transfer.idle_cycles,
        .timer = swd_gen_timer ? 1U : 0U,
    };
}

/** Out of line, so the check every command makes does not pay this frame. */
static __attribute__((noinline)) bool regenerate(void) {
    current = true;
    const struct swd_gen_config want = swd_gen_inputs();
    if (built_once && (gen_owner == GEN_OWNER_SWD) && (memcmp(&want, &built, sizeof want) == 0)) {
        return built_ok;
    }
    built = want;
    built_once = true;
    gen_owner = GEN_OWNER_SWD;
    swd_gen_info.ok = false;
    built_ok = swd_gen_build(&want, &swd_gen_read, &swd_gen_write);
    if (built_ok) {
        swd_gen_builds++;
    } else {
        swd_gen_failures++;
    }
    return built_ok;
}

bool swd_gen_ready(void) {
    if (current && (gen_owner == GEN_OWNER_SWD)) {
        return built_ok;
    }
    return regenerate();
}
