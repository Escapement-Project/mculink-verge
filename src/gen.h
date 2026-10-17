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

#define GEN_EVENTS 256U
#define GEN_LABELS 8U
#define GEN_FIXUPS 16U

struct gen_event {
    uint32_t cycle;
    uint32_t insn;
};

/** Instructions on cycles, sorted by cycle, at most one a cycle. */
struct gen_schedule {
    uint32_t n;
    bool failed; // something did not fit, and the schedule is unusable
    struct gen_event ev[GEN_EVENTS];
};

void gen_schedule_reset(struct gen_schedule* s);
bool gen_taken(const struct gen_schedule* s, uint32_t cycle);
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

#endif
