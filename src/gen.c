#include "gen.h"

#include "hot.h"
#include "thumb.h"

uint32_t gen_owner = GEN_OWNER_NONE;
uint32_t gen_stall;

#if defined(SWD_CODEGEN) && !defined(JTAG_CODEGEN)
struct gen_event gen_events[256];
#else
struct gen_event gen_events[1280];
#endif

GEN_PATH void gen_schedule_reset(struct gen_schedule* s, struct gen_event* ev, uint32_t cap) {
    s->ev = ev;
    s->cap = cap;
    s->n = 0U;
    s->failed = false;
}

/** Events a lookup checks from the end before it searches the rest. The generators lay events
 *  close to the order of their cycles and ask mostly about cycles near the last. */
#define GEN_TAIL 4U

GEN_PATH static uint32_t lower_bound(const struct gen_schedule* s, uint32_t cycle) {
    uint32_t hi = s->n;
    for (uint32_t k = 0U; (k < GEN_TAIL) && (hi != 0U); k++, hi--) {
        if (s->ev[hi - 1U].cycle < cycle) {
            return hi;
        }
    }
    uint32_t lo = 0U;
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

GEN_PATH bool gen_taken(const struct gen_schedule* s, uint32_t cycle) {
    const uint32_t i = lower_bound(s, cycle);
    return (i < s->n) && (s->ev[i].cycle == cycle);
}

GEN_PATH uint32_t gen_at(const struct gen_schedule* s, uint32_t cycle) {
    const uint32_t i = lower_bound(s, cycle);
    return (i < s->n) && (s->ev[i].cycle == cycle) ? s->ev[i].insn : 0U;
}

GEN_PATH void gen_put(struct gen_schedule* s, uint32_t cycle, uint32_t insn) {
    if (s->n == s->cap) {
        s->failed = true;
        return;
    }
    const uint32_t i = lower_bound(s, cycle);
    if ((i < s->n) && (s->ev[i].cycle == cycle)) {
        s->failed = true;
        return;
    }
    // Rarely more than an event or two, and newlib-nano's memmove copies bytes.
    for (uint32_t k = s->n; k > i; k--) {
        s->ev[k] = s->ev[k - 1U];
    }
    s->ev[i] = (struct gen_event){cycle, insn};
    s->n++;
}

GEN_PATH void gen_emit(struct gen_emitter* e, uint32_t insn) {
    if (e->pos >= e->cap) {
        e->failed = true;
        return;
    }
    e->code[e->pos++] = insn;
}

GEN_PATH void gen_emit_branch(struct gen_emitter* e, uint32_t cond, uint32_t label) {
    if (e->nfix == GEN_FIXUPS) {
        e->failed = true;
        return;
    }
    e->fix[e->nfix++] = e->pos;
    gen_emit(e, GEN_BRANCH(cond, label));
}

GEN_PATH void gen_bind(struct gen_emitter* e, uint32_t label) { e->label[label] = e->pos; }

GEN_PATH void gen_resolve(struct gen_emitter* e) {
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

GEN_PATH void gen_load_const(struct gen_emitter* e, uint32_t rd, uint32_t value) {
    gen_emit(e, t_movw(rd, value & 0xFFFFU));
    if ((value >> 16) != 0U) {
        gen_emit(e, t_movt(rd, value >> 16));
    }
}

/* A counted loop of three cycles an iteration, and fillers for the rest. The loop's head is
 * word-aligned like everything here, which is what makes it three. */
GEN_PATH void gen_delay(struct gen_emitter* e, uint32_t cycles) {
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

GEN_PATH uint32_t gen_emit_span(struct gen_emitter* e, const struct gen_schedule* s, uint32_t from, uint32_t to) {
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

GEN_PATH uint32_t gen_emit_schedule(struct gen_emitter* e, const struct gen_schedule* s) {
    return gen_emit_span(e, s, 0U, UINT32_MAX);
}
