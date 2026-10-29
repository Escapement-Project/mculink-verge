/** What the run-time generators share: a schedule of instructions placed on core cycles, and the
 * emitter that writes one out as code with the gaps filled.
 *
 * src/swd_gen.c and src/jtag_gen.c each decide where their own instructions go. TIMING.md has the
 * rules a cycle count follows from SRAMX: every 32-bit instruction one cycle, so a schedule's code
 * is its instructions in order with a filler or a counted loop in every gap.
 */
#ifndef GEN_H
#define GEN_H

#include <stdbool.h>
#include <stdint.h>

/** A branch to a label is scheduled as a placeholder whose low halfword is zero, which no 32-bit
 * encoding has: the label in the top byte, the condition below it (T_AL for b.w). */
#define T_AL 0xFU
#define GEN_BRANCH(cond, label) (((uint32_t)(label) << 24) | ((uint32_t)(cond) << 16))
#define GEN_IS_BRANCH(insn) (((insn) & 0xFFFFU) == 0U)

/** Which generator's code is in the SRAMX region they share. SWD and JTAG are never in use at
 * once, so each regenerates when it finds the other's code there. */
#define GEN_OWNER_NONE 0U
#define GEN_OWNER_SWD 1U
#define GEN_OWNER_JTAG 2U
extern uint32_t gen_owner;

/** Cycles another master can hold a load on the bit paths' AHB port (BUSES.md). A sample placed
 * after the edge that ends its bit's window reads the next bit if held past the window, so both
 * generators keep this many cycles clear at such a window's end. src/energy_monitor.c sets it while
 * a capture is asked for. */
extern uint32_t gen_stall;

#define GEN_LABELS 8U
#define GEN_FIXUPS 16U

struct gen_event {
    uint32_t cycle;
    uint32_t insn;
};

/** Instructions on cycles, sorted by cycle, at most one a cycle, in storage its user provides. */
struct gen_schedule {
    struct gen_event* ev;
    uint32_t cap;
    uint32_t n;
    bool failed; // something did not fit, and the schedule is unusable
};

/** Empties `s`, which keeps its events in `ev`, `cap` of them. */
void gen_schedule_reset(struct gen_schedule* s, struct gen_event* ev, uint32_t cap);
bool gen_taken(const struct gen_schedule* s, uint32_t cycle);
/** What is on `cycle`, or 0 when nothing is: no instruction or branch placeholder is 0. */
uint32_t gen_at(const struct gen_schedule* s, uint32_t cycle);
/** Puts `insn` on `cycle`. A cycle already taken, or a full schedule, fails the schedule. */
void gen_put(struct gen_schedule* s, uint32_t cycle, uint32_t insn);

struct gen_emitter {
    uint32_t* code;
    uint32_t pos; // words written
    uint32_t cap;
    uint32_t delay_reg; // the counter a delay loop runs on, which it leaves zero
    uint32_t filler;    // one cycle that changes nothing the generated code reads
    uint32_t label[GEN_LABELS];
    uint32_t fix[GEN_FIXUPS];
    uint32_t nfix;
    bool failed;
};

void gen_emit(struct gen_emitter* e, uint32_t insn);
/** A branch to `label`, which gen_resolve fills in once every label is bound. */
void gen_emit_branch(struct gen_emitter* e, uint32_t cond, uint32_t label);
void gen_bind(struct gen_emitter* e, uint32_t label);
void gen_resolve(struct gen_emitter* e);
/** movw, and movt when the top half is not zero. */
void gen_load_const(struct gen_emitter* e, uint32_t rd, uint32_t value);
/** Exactly `cycles` cycles. A delay of six or more is a counted loop, which sets the flags. */
void gen_delay(struct gen_emitter* e, uint32_t cycles);
/** The schedule as code. Returns its length in cycles. */
uint32_t gen_emit_schedule(struct gen_emitter* e, const struct gen_schedule* s);
/** The events on cycles `from` to `to`, not including `to`, as code that starts on `from`. Returns
 * the cycle after the last, or `from` when there are none. */
uint32_t gen_emit_span(struct gen_emitter* e, const struct gen_schedule* s, uint32_t from, uint32_t to);

#endif
