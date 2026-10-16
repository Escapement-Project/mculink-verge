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
 *   latency + SAFE_MARGIN cycles a sample goes in the middle of that window, which can be after
 *   the next rising edge. Those transfers run masked: an interrupt between that edge and the
 *   sample would make it read the next bit. Longer periods sample just before the next rising
 *   edge, where an interrupt only stretches the clock.
 * - No clock is ever shorter than the period. Where an instruction needs a cycle a clock would
 *   have used, the clock waits.
 *
 * The functions follow ARM's SWD_Transfer pin for pin, which tools/equiv.py checks: the same
 * edges in the same order, with SWDIO at the same level on each, for every acknowledge, every
 * turnaround and both data-phase settings.
 */
#include "swd_gen.h"

#include <stddef.h>
#include <string.h>

#include "DAP_config.h"

#include "DAP.h"
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

#define PORT_BASE ((uint32_t)&GPIO->MPIN[0])
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
};

/* -- schedule ---------------------------------------------------------------------------------- */

/** A branch to a label is scheduled as a placeholder whose low halfword is zero, which no 32-bit
 * encoding has: the label in the top byte, the condition below it (T_AL for b.w). */
#define T_AL 0xFU
#define BRANCH(cond, label) (((uint32_t)(label) << 24) | ((uint32_t)(cond) << 16))
#define IS_BRANCH(insn) (((insn) & 0xFFFFU) == 0U)

#define GEN_EVENTS 256U

struct event {
    uint32_t cycle;
    uint32_t insn;
};

struct sched {
    const struct gen_cfg* cfg;
    uint32_t n;
    uint32_t t; // the earliest cycle the next clock may fall on
    uint32_t last_fall;
    uint32_t last_rise;
    // The last clock's period and low phase, which the clocks still to come are assumed to share.
    uint32_t grid_period;
    uint32_t grid_low;
    uint32_t last_load;
    bool failed;
    struct event ev[GEN_EVENTS];
};

/* Static rather than on the stack: it is the largest thing here. */
static struct sched sched;

static void sched_reset(struct sched* s, const struct gen_cfg* cfg) {
    s->cfg = cfg;
    s->n = 0U;
    s->t = 0U;
    s->last_fall = 0U;
    s->last_rise = 0U;
    s->last_load = 0U;
    s->grid_period = cfg->wperiod;
    s->grid_low = cfg->wlow;
    s->failed = false;
}

static uint32_t lower_bound(const struct sched* s, uint32_t cycle) {
    uint32_t lo = 0U;
    uint32_t hi = s->n;
    while (lo < hi) {
        const uint32_t mid = (lo + hi) / 2U;
        if (s->ev[mid].cycle < cycle) {
            lo = mid + 1U;
        } else {
            hi = mid;
        }
    }
    return lo;
}

static bool taken(const struct sched* s, uint32_t cycle) {
    const uint32_t i = lower_bound(s, cycle);
    return (i < s->n) && (s->ev[i].cycle == cycle);
}

static void put(struct sched* s, uint32_t cycle, uint32_t insn) {
    if ((s->n == GEN_EVENTS) || taken(s, cycle)) {
        s->failed = true;
        return;
    }
    const uint32_t i = lower_bound(s, cycle);
    memmove(&s->ev[i + 1U], &s->ev[i], (s->n - i) * sizeof s->ev[0]);
    s->ev[i] = (struct event){cycle, insn};
    s->n++;
}

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
        // The middle of the window, which the next clocks may not have reached yet.
        const uint32_t last = end + c->latency - 1U;
        uint32_t best_margin = 0U;
        for (uint32_t cycle = first; cycle <= last; cycle++) {
            if ((cycle < not_before) || taken(s, cycle) || on_grid(s, cycle)) {
                continue;
            }
            const uint32_t early = cycle - first;
            const uint32_t late = last - cycle;
            const uint32_t margin = early < late ? early : late;
            if (!found || (margin > best_margin)) {
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
        s->failed = true;
        return s->last_load;
    }
    put(s, best, t_ldrb(reg, R_GPIO, OFF_B_SWDIO));
    if (best > s->last_load) {
        s->last_load = best;
    }
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
#define LABELS 6U

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
    put(s, at, BRANCH(T_COND_NE, LABEL_FAIL));
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

#define GEN_FIXUPS 16U

struct emitter {
    uint32_t* code;
    uint32_t pos;
    uint32_t cap;
    uint32_t label[LABELS];
    uint32_t fix[GEN_FIXUPS];
    uint32_t nfix;
    bool failed;
};

static void emit(struct emitter* e, uint32_t insn) {
    if (e->pos >= e->cap) {
        e->failed = true;
        return;
    }
    e->code[e->pos++] = insn;
}

static void emit_branch(struct emitter* e, uint32_t cond, uint32_t label) {
    if (e->nfix == GEN_FIXUPS) {
        e->failed = true;
        return;
    }
    e->fix[e->nfix++] = e->pos;
    emit(e, BRANCH(cond, label));
}

static void bind(struct emitter* e, uint32_t label) { e->label[label] = e->pos; }

static void resolve(struct emitter* e) {
    for (uint32_t i = 0U; i < e->nfix; i++) {
        const uint32_t at = e->fix[i];
        if (at >= e->pos) {
            continue; // the emitter ran out of room before writing it
        }
        const uint32_t insn = e->code[at];
        const int32_t offset = ((int32_t)e->label[insn >> 24] - (int32_t)at) * 4;
        const uint32_t cond = (insn >> 16) & 0xFU;
        e->code[at] = cond == T_AL ? t_b(offset) : t_bcond(cond, offset);
    }
    e->nfix = 0U;
}

static void load_const(struct emitter* e, uint32_t rd, uint32_t value) {
    emit(e, t_movw(rd, value & 0xFFFFU));
    if ((value >> 16) != 0U) {
        emit(e, t_movt(rd, value >> 16));
    }
}

/** Exactly `cycles` cycles: a counted loop of three cycles an iteration and fillers for the rest.
 * The loop's head is word-aligned like everything here, which is what makes it three. */
static void delay(struct emitter* e, uint32_t cycles) {
    if (cycles >= 6U) {
        uint32_t n = cycles / 3U;
        if (n > 0xFFFFU) {
            n = (cycles - 1U) / 3U; // movt costs a cycle
        }
        const bool wide = n > 0xFFFFU;
        load_const(e, R_DELAY, n);
        emit(e, t_subs_imm(R_DELAY, R_DELAY, 1U));
        emit(e, t_bcond(T_COND_NE, -4));
        cycles -= 3U * n + (wide ? 1U : 0U);
    }
    while (cycles-- != 0U) {
        emit(e, FILLER);
    }
}

/** The schedule as code. Returns its length in cycles. */
static uint32_t emit_schedule(struct emitter* e, const struct sched* s) {
    uint32_t now = 0U;
    for (uint32_t i = 0U; i < s->n; i++) {
        const struct event* ev = &s->ev[i];
        delay(e, ev->cycle - now);
        if (IS_BRANCH(ev->insn)) {
            emit_branch(e, (ev->insn >> 16) & 0xFU, ev->insn >> 24);
        } else {
            emit(e, ev->insn);
        }
        now = ev->cycle + 1U;
    }
    return now;
}

/** `n` clocks with the wire left as it is, `since` cycles after the last rising edge. Returns the
 * cycles from the last of them to the instruction after the loop. Not timing-critical: these
 * clocks only have to be no shorter than the period. */
static uint32_t clock_loop(struct emitter* e, const struct gen_cfg* c, uint32_t n, uint32_t since) {
    const uint32_t high = c->period - c->low;
    if (since + 1U < high) {
        delay(e, high - since - 1U); // and the counter's load is the last cycle of it
    }
    emit(e, t_movw(R_T0, n));
    const uint32_t head = e->pos;
    emit(e, t_str(R_ONE, R_PORT, OFF_CLR));
    delay(e, c->low - 1U);
    emit(e, t_str(R_ONE, R_PORT, OFF_SET));
    // The subs and a taken bne put the next fall four cycles after the delay.
    const uint32_t gap = high > 4U ? high - 4U : 0U;
    delay(e, gap);
    emit(e, t_subs_imm(R_T0, R_T0, 1U));
    emit(e, t_bcond(T_COND_NE, ((int32_t)head - (int32_t)e->pos) * 4));
    return gap + 3U;
}

static void emit_enable(struct emitter* e) {
    emit(e, t_str(R_FOUR, R_PORT, OFF_DIRSET));
    emit(e, t_str(R_TXEN, R_PORT, OFF_SET));
}

/** Returns the acknowledge in `ack`'s instruction and the count left. */
static void emit_return(struct emitter* e, uint32_t ack) {
    emit(e, ack);
    emit(e, t_mov(1U, R_COUNT));
    emit(e, t_pop(0x8FF0U)); // r4-r11, pc
}

/** After an acknowledge other than OK, `since` cycles after the last rising edge, with the
 * turnaround clocked: what ARM's SWD_Transfer clocks for WAIT, FAULT or a protocol error. */
static void emit_fail(struct emitter* e, const struct gen_cfg* c, bool read, uint32_t since) {
    emit(e, t_cmp_imm(R_ACK, DAP_TRANSFER_WAIT));
    emit_branch(e, T_COND_EQ, LABEL_WAIT_FAULT);
    emit(e, t_cmp_imm(R_ACK, DAP_TRANSFER_FAULT));
    emit_branch(e, T_COND_EQ, LABEL_WAIT_FAULT);
    // A protocol error backs off a whole data phase, then takes the wire back.
    clock_loop(e, c, 32U + 1U, since + 4U);
    emit_enable(e);
    emit(e, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));
    emit_branch(e, T_AL, LABEL_EXIT);

    bind(e, LABEL_WAIT_FAULT);
    const uint32_t after = since + 3U; // a cmp and a taken branch at least
    if (read) {
        if (c->data_phase) {
            clock_loop(e, c, 32U + 1U, after);
        }
        emit_enable(e);
    } else {
        emit_enable(e);
        if (c->data_phase) {
            emit(e, t_strb(R_ZERO, R_GPIO, OFF_B_SWDIO));
            clock_loop(e, c, 32U + 1U, after + 3U);
        }
    }
    emit(e, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));

    bind(e, LABEL_EXIT);
    if (c->masked) {
        emit(e, t_msr_primask(R_ZERO));
    }
    emit_return(e, t_mov(0U, R_ACK));
}

/** One function: entry, the transfer loop, and its exits. */
static bool emit_function(struct emitter* e, const struct gen_cfg* c, bool read) {
    struct sched* s = &sched;
    struct body b = {0};
    sched_reset(s, c);
    if (read) {
        schedule_read(s, &b);
    } else {
        schedule_write(s, &b);
    }
    if (s->failed) {
        return false;
    }
    uint32_t first_fall = 0U;
    for (uint32_t i = 0U; i < s->n; i++) {
        if (s->ev[i].insn == t_str(R_FOUR, R_PORT, OFF_MPIN)) {
            first_fall = s->ev[i].cycle;
            break;
        }
    }
    // The body starts on a packet clock and ends on whatever clock it ends on: the high phase
    // between them is the longer of the two kinds, which covers both.
    const uint32_t read_high = c->period - c->low;
    const uint32_t write_high = c->wperiod - c->wlow;
    const uint32_t high = read_high > write_high ? read_high : write_high;

    emit(e, t_push(0x4FF0U)); // r4-r11, lr
    emit(e, t_mov(R_COUNT, 2U));
    load_const(e, R_GPIO, (uint32_t)GPIO);
    load_const(e, R_PORT, PORT_BASE);
    emit(e, t_mov_imm(R_ONE, SWCLK));
    emit(e, t_mov_imm(R_ZERO, 0U));
    emit(e, t_mov_imm(R_FOUR, SWDIO));
    load_const(e, R_TXEN, TXEN);
    // Nothing says how long ago the last rising edge was.
    delay(e, high);

    bind(e, LABEL_LOOP);
    const uint32_t length = emit_schedule(e, s);
    uint32_t since = length - s->last_rise;
    if (b.idle_loop) {
        since = clock_loop(e, c, c->idle, since);
        emit(e, t_strb(R_ONE, R_GPIO, OFF_B_SWDIO));
        since++;
    }
    if (c->masked) {
        emit(e, t_msr_primask(R_ZERO));
        since++;
    }
    if (read) {
        // The parity, off the finished word: R_ACK holds the sampled bit.
        emit(e, t_eor(R_T1, R_ACK, R_WORD, T_LSL, 0U));
        for (uint32_t shift = 16U; shift != 0U; shift >>= 1) {
            emit(e, t_eor(R_T1, R_T1, R_T1, T_LSR, shift));
        }
        emit(e, t_tst_imm(R_T1, 1U));
        emit_branch(e, T_COND_NE, LABEL_PARITY);
        emit(e, t_str_post(R_WORD, R_BUF, 4U));
        since += 9U;
    }
    // subs, then the taken branch back, then the next transfer's first fall.
    const uint32_t to_fall = since + 3U + first_fall;
    if (to_fall < high) {
        delay(e, high - to_fall);
    }
    emit(e, t_subs_imm(R_COUNT, R_COUNT, 1U));
    emit_branch(e, T_COND_NE, LABEL_LOOP);
    emit_return(e, t_mov_imm(0U, DAP_TRANSFER_OK));

    if (read) {
        bind(e, LABEL_PARITY);
        emit_return(e, t_mov_imm(0U, DAP_TRANSFER_ERROR));
    }

    bind(e, LABEL_FAIL);
    emit_fail(e, c, read, b.branch + 2U - b.branch_rise);
    resolve(e);
    return !e->failed;
}

bool swd_gen_build(const struct swd_gen_config* config, swd_gen_fn* read, swd_gen_fn* write) {
    if ((config->period < SWD_GEN_PERIOD_MIN) || (config->period > SWD_GEN_PERIOD_MAX) || (config->latency == 0U) ||
        (config->latency > 64U) || (config->turnaround < 1U) || (config->turnaround > 4U) ||
        (config->idle_cycles > 255U)) {
        return false;
    }
    const uint32_t period = config->period < SWD_GEN_READ_PERIOD_MIN ? SWD_GEN_READ_PERIOD_MIN : config->period;
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
    };
    struct emitter e = {.code = (uint32_t*)SWD_GEN_BASE, .cap = SWD_GEN_SIZE / 4U};

    const uint32_t read_at = e.pos;
    if (!emit_function(&e, &c, true)) {
        return false;
    }
    const uint32_t write_at = e.pos;
    if (!emit_function(&e, &c, false)) {
        return false;
    }

    // Only SWCLK and SWDIO answer a masked-port store. Nothing else in the image uses MPIN.
    GPIO->MASK[0] = ~(SWCLK | SWDIO);
    // The code was written as data; the core has to fetch what is there now.
    __DSB();
    __ISB();
    *read = (swd_gen_fn)(SWD_GEN_BASE + 4U * read_at + 1U);
    *write = (swd_gen_fn)(SWD_GEN_BASE + 4U * write_at + 1U);
    return true;
}

/* -- when to generate ------------------------------------------------------------------------- */

uint32_t swd_gen_clock_hz = DAP_DEFAULT_SWJ_CLOCK;
uint32_t swd_gen_latency = SWD_GEN_LATENCY_DEFAULT;
swd_gen_fn swd_gen_read;
swd_gen_fn swd_gen_write;

static struct swd_gen_config built;
static bool built_once;
static bool built_ok;
uint32_t swd_gen_builds;
uint32_t swd_gen_failures;

/** The shortest period no faster than the clock asked for. */
static uint32_t period_for(uint32_t hz) {
    const uint32_t period = ((CPU_CLOCK - 1U) / hz) + 1U;
    return period < SWD_GEN_PERIOD_MIN ? SWD_GEN_PERIOD_MIN : period;
}

bool swd_gen_ready(void) {
    const struct swd_gen_config want = {
        .period = period_for(swd_gen_clock_hz),
        .latency = swd_gen_latency,
        .turnaround = DAP_Data.swd_conf.turnaround,
        .data_phase = DAP_Data.swd_conf.data_phase,
        .idle_cycles = DAP_Data.transfer.idle_cycles,
    };
    if (built_once && (memcmp(&want, &built, sizeof want) == 0)) {
        return built_ok;
    }
    built = want;
    built_once = true;
    built_ok = swd_gen_build(&want, &swd_gen_read, &swd_gen_write);
    if (built_ok) {
        swd_gen_builds++;
    } else {
        swd_gen_failures++;
    }
    return built_ok;
}
