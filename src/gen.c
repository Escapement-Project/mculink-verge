#include "gen.h"

#include <string.h>

#include "thumb.h"

uint32_t gen_owner = GEN_OWNER_NONE;

void gen_schedule_reset(struct gen_schedule* s, struct gen_event* ev, uint32_t cap) {
    s->ev = ev;
    s->cap = cap;
    s->n = 0U;
    s->failed = false;
}

static uint32_t lower_bound(const struct gen_schedule* s, uint32_t cycle) {
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

bool gen_taken(const struct gen_schedule* s, uint32_t cycle) {
    const uint32_t i = lower_bound(s, cycle);
    return (i < s->n) && (s->ev[i].cycle == cycle);
}

uint32_t gen_at(const struct gen_schedule* s, uint32_t cycle) {
    const uint32_t i = lower_bound(s, cycle);
    return (i < s->n) && (s->ev[i].cycle == cycle) ? s->ev[i].insn : 0U;
}

void gen_put(struct gen_schedule* s, uint32_t cycle, uint32_t insn) {
    if ((s->n == s->cap) || gen_taken(s, cycle)) {
        s->failed = true;
        return;
    }
    const uint32_t i = lower_bound(s, cycle);
    memmove(&s->ev[i + 1U], &s->ev[i], (s->n - i) * sizeof s->ev[0]);
    s->ev[i] = (struct gen_event){cycle, insn};
    s->n++;
}

void gen_emit(struct gen_emitter* e, uint32_t insn) {
    if (e->pos >= e->cap) {
        e->failed = true;
        return;
    }
    e->code[e->pos++] = insn;
}

void gen_emit_branch(struct gen_emitter* e, uint32_t cond, uint32_t label) {
    if (e->nfix == GEN_FIXUPS) {
        e->failed = true;
        return;
    }
    e->fix[e->nfix++] = e->pos;
    gen_emit(e, GEN_BRANCH(cond, label));
}

void gen_bind(struct gen_emitter* e, uint32_t label) { e->label[label] = e->pos; }

void gen_resolve(struct gen_emitter* e) {
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

void gen_load_const(struct gen_emitter* e, uint32_t rd, uint32_t value) {
    gen_emit(e, t_movw(rd, value & 0xFFFFU));
    if ((value >> 16) != 0U) {
        gen_emit(e, t_movt(rd, value >> 16));
    }
}

/* A counted loop of three cycles an iteration, and fillers for the rest. The loop's head is
 * word-aligned like everything here, which is what makes it three. */
void gen_delay(struct gen_emitter* e, uint32_t cycles) {
    if (cycles >= 6U) {
        uint32_t n = cycles / 3U;
        if (n > 0xFFFFU) {
            n = (cycles - 1U) / 3U; // movt costs a cycle
        }
        const bool wide = n > 0xFFFFU;
        gen_load_const(e, e->delay_reg, n);
        gen_emit(e, t_subs_imm(e->delay_reg, e->delay_reg, 1U));
        gen_emit(e, t_bcond(T_COND_NE, -4));
        cycles -= 3U * n + (wide ? 1U : 0U);
    }
    while (cycles-- != 0U) {
        gen_emit(e, e->filler);
    }
}

uint32_t gen_emit_span(struct gen_emitter* e, const struct gen_schedule* s, uint32_t from, uint32_t to) {
    uint32_t now = from;
    for (uint32_t i = lower_bound(s, from); (i < s->n) && (s->ev[i].cycle < to); i++) {
        const struct gen_event* ev = &s->ev[i];
        gen_delay(e, ev->cycle - now);
        if (GEN_IS_BRANCH(ev->insn)) {
            gen_emit_branch(e, (ev->insn >> 16) & 0xFU, ev->insn >> 24);
        } else {
            gen_emit(e, ev->insn);
        }
        now = ev->cycle + 1U;
    }
    return now;
}

uint32_t gen_emit_schedule(struct gen_emitter* e, const struct gen_schedule* s) {
    return gen_emit_span(e, s, 0U, UINT32_MAX);
}
