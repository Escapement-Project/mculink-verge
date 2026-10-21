/** Thumb-2 encoders for the code src/swd_gen.c and src/jtag_gen.c write into SRAMX.
 *
 * Every encoding here is 32-bit, or a 16-bit one padded to 32. Generated code is all 32-bit so that every instruction sits on a
 * word boundary, which is what makes each one cost one cycle and every loop head cost the same
 * (TIMING.md). An encoding is returned as the word a little-endian store puts in memory: the
 * first halfword in the low half.
 *
 * tools/thumb_test.py assembles the same instructions with arm-none-eabi-as and compares.
 */
#ifndef THUMB_H
#define THUMB_H

#include <stdint.h>

#define T_LR 14U
#define T_PC 15U

#define T_COND_EQ 0x0U
#define T_COND_NE 0x1U

#define T_LSL 0U
#define T_LSR 1U

static inline uint32_t t32(uint32_t hw1, uint32_t hw2) { return (hw1 & 0xFFFFU) | (hw2 << 16); }

/** str.w, strb.w, ldr.w and ldrb.w with a 12-bit positive offset. */
static inline uint32_t t_str(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return t32(0xF8C0U | rn, (rt << 12) | imm12);
}
static inline uint32_t t_strb(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return t32(0xF880U | rn, (rt << 12) | imm12);
}
static inline uint32_t t_ldr(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return t32(0xF8D0U | rn, (rt << 12) | imm12);
}
static inline uint32_t t_ldrb(uint32_t rt, uint32_t rn, uint32_t imm12) {
    return t32(0xF890U | rn, (rt << 12) | imm12);
}

/** str.w and ldr.w post-indexed, adding imm8: `[rn], #imm8`. */
static inline uint32_t t_str_post(uint32_t rt, uint32_t rn, uint32_t imm8) {
    return t32(0xF840U | rn, (rt << 12) | 0xB00U | imm8);
}
static inline uint32_t t_ldr_post(uint32_t rt, uint32_t rn, uint32_t imm8) {
    return t32(0xF850U | rn, (rt << 12) | 0xB00U | imm8);
}

/** Data processing with a shifted register operand: `op rd, rn, rm, <type> #amount`. */
static inline uint32_t t_dp_reg(uint32_t op, uint32_t s, uint32_t rd, uint32_t rn, uint32_t rm, uint32_t type,
                                uint32_t amount) {
    return t32(0xEA00U | (op << 5) | (s << 4) | rn,
               ((amount >> 2) << 12) | (rd << 8) | ((amount & 3U) << 6) | (type << 4) | rm);
}
static inline uint32_t t_and(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t type, uint32_t amount) {
    return t_dp_reg(0x0U, 0U, rd, rn, rm, type, amount);
}
static inline uint32_t t_orr(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t type, uint32_t amount) {
    return t_dp_reg(0x2U, 0U, rd, rn, rm, type, amount);
}
static inline uint32_t t_eor(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t type, uint32_t amount) {
    return t_dp_reg(0x4U, 0U, rd, rn, rm, type, amount);
}
static inline uint32_t t_bic(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t type, uint32_t amount) {
    return t_dp_reg(0x1U, 0U, rd, rn, rm, type, amount);
}
/** mov.w rd, rm, and the shifts, which are the same encoding with rn = pc. */
static inline uint32_t t_mov(uint32_t rd, uint32_t rm) { return t_dp_reg(0x2U, 0U, rd, T_PC, rm, T_LSL, 0U); }
/** lsr.w rd, rm, #amount, 1 to 31. */
static inline uint32_t t_lsr(uint32_t rd, uint32_t rm, uint32_t amount) {
    return t_dp_reg(0x2U, 0U, rd, T_PC, rm, T_LSR, amount);
}
/** lsl.w and lsr.w rd, rn, rm: shifts by a register. */
static inline uint32_t t_lsl_reg(uint32_t rd, uint32_t rn, uint32_t rm) { return t32(0xFA00U | rn, 0xF000U | (rd << 8) | rm); }
static inline uint32_t t_lsr_reg(uint32_t rd, uint32_t rn, uint32_t rm) { return t32(0xFA20U | rn, 0xF000U | (rd << 8) | rm); }
/** mla rd, rn, rm, ra: rn * rm + ra. Its result is usable two cycles after it issues. */
static inline uint32_t t_mla(uint32_t rd, uint32_t rn, uint32_t rm, uint32_t ra) {
    return t32(0xFB00U | rn, (ra << 12) | (rd << 8) | rm);
}
/** rbit: rm's bits in the reverse order. */
static inline uint32_t t_rbit(uint32_t rd, uint32_t rm) { return t32(0xFA90U | rm, 0xF0A0U | (rd << 8) | rm); }

/** Data processing with a small immediate (0..255): cmp.w, subs.w, tst.w, mov.w, rsb.w. */
static inline uint32_t t_cmp_imm(uint32_t rn, uint32_t imm8) { return t32(0xF1B0U | rn, 0x0F00U | imm8); }
static inline uint32_t t_subs_imm(uint32_t rd, uint32_t rn, uint32_t imm8) {
    return t32(0xF1B0U | rn, (rd << 8) | imm8);
}
static inline uint32_t t_tst_imm(uint32_t rn, uint32_t imm8) { return t32(0xF010U | rn, 0x0F00U | imm8); }
static inline uint32_t t_mov_imm(uint32_t rd, uint32_t imm8) { return t32(0xF04FU, (rd << 8) | imm8); }
/** orr.w rd, rn, #imm8. */
static inline uint32_t t_orr_imm(uint32_t rd, uint32_t rn, uint32_t imm8) { return t32(0xF040U | rn, (rd << 8) | imm8); }
/** lsl.w rd, rm, #amount, 1 to 31. */
static inline uint32_t t_lsl(uint32_t rd, uint32_t rm, uint32_t amount) {
    return t_dp_reg(0x2U, 0U, rd, T_PC, rm, T_LSL, amount);
}
/** rsb.w rd, rn, #imm8: imm8 less rn. */
static inline uint32_t t_rsb_imm(uint32_t rd, uint32_t rn, uint32_t imm8) { return t32(0xF1C0U | rn, (rd << 8) | imm8); }

/** movw and movt: the low and high halves of a constant. */
static inline uint32_t t_movw_movt(uint32_t top, uint32_t rd, uint32_t imm16) {
    return t32((top ? 0xF2C0U : 0xF240U) | (((imm16 >> 11) & 1U) << 10) | (imm16 >> 12),
               (((imm16 >> 8) & 7U) << 12) | (rd << 8) | (imm16 & 0xFFU));
}
static inline uint32_t t_movw(uint32_t rd, uint32_t imm16) { return t_movw_movt(0U, rd, imm16); }
static inline uint32_t t_movt(uint32_t rd, uint32_t imm16) { return t_movw_movt(1U, rd, imm16); }

/** b<cond>.w, `offset` bytes from the branch's own address. Range +-1 MB. */
static inline uint32_t t_bcond(uint32_t cond, int32_t offset) {
    const uint32_t v = (uint32_t)(offset - 4);
    return t32(0xF000U | (((v >> 20) & 1U) << 10) | (cond << 6) | ((v >> 12) & 0x3FU),
               0x8000U | (((v >> 18) & 1U) << 13) | (((v >> 19) & 1U) << 11) | ((v >> 1) & 0x7FFU));
}

/** b.w, `offset` bytes from the branch's own address. Range +-16 MB. */
static inline uint32_t t_b(int32_t offset) {
    const uint32_t v = (uint32_t)(offset - 4);
    const uint32_t s = (v >> 24) & 1U;
    const uint32_t j1 = (~((v >> 23) ^ s)) & 1U;
    const uint32_t j2 = (~((v >> 22) ^ s)) & 1U;
    return t32(0xF000U | (s << 10) | ((v >> 12) & 0x3FFU), 0x9000U | (j1 << 13) | (j2 << 11) | ((v >> 1) & 0x7FFU));
}

/** bx rm, then a 16-bit nop that never runs, so the next instruction keeps its word boundary. */
static inline uint32_t t_bx_pad(uint32_t rm) { return t32(0x4700U | (rm << 3), 0xBF00U); }

/** msr primask, rn. */
static inline uint32_t t_msr_primask(uint32_t rn) { return t32(0xF380U | rn, 0x8810U); }
/** dmb sy */
static inline uint32_t t_dmb(void) { return t32(0xF3BFU, 0x8F5FU); }

/** push.w and pop.w of a register list (bit n = rn). */
static inline uint32_t t_push(uint32_t list) { return t32(0xE92DU, list); }
static inline uint32_t t_pop(uint32_t list) { return t32(0xE8BDU, list); }

#endif
