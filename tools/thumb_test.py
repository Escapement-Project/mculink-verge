# /// script
# requires-python = ">=3.11"
# ///
"""Check src/thumb.h's encoders against arm-none-eabi-as.

Compiles a host program that prints each encoder's output, assembles the instruction it should be
with the GNU assembler, and compares the bytes.

    uv run tools/thumb_test.py
"""

import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")

R = {f"r{n}": n for n in range(13)} | {"lr": 14}

CASES: list[tuple[str, str]] = []


def case(c: str, asm: str) -> None:
    CASES.append((c, asm))


for rt, rn, imm in (("r5", "r9", 0x100), ("r11", "r9", 0), ("r2", "r9", 0x280), ("lr", "r1", 4095)):
    case(f"t_str({R[rt]}, {R[rn]}, {imm})", f"str.w {rt}, [{rn}, #{imm}]")
    case(f"t_strb({R[rt]}, {R[rn]}, {imm})", f"strb.w {rt}, [{rn}, #{imm}]")
    case(f"t_ldr({R[rt]}, {R[rn]}, {imm})", f"ldr.w {rt}, [{rn}, #{imm}]")
    case(f"t_ldrb({R[rt]}, {R[rn]}, {imm})", f"ldrb.w {rt}, [{rn}, #{imm}]")
for rt, rn in (("r10", "r1"), ("r0", "r12")):
    case(f"t_str_post({R[rt]}, {R[rn]}, 4)", f"str.w {rt}, [{rn}], #4")
    case(f"t_ldr_post({R[rt]}, {R[rn]}, 4)", f"ldr.w {rt}, [{rn}], #4")
for op in ("and", "bic", "orr", "eor"):
    for rd, rn, rm, kind, amount in (
        ("r11", "r2", "r0", "lsr", 3),
        ("r12", "r2", "r10", "lsl", 2),
        ("r10", "r10", "lr", "lsl", 31),
        ("lr", "lr", "lr", "lsr", 16),
        ("r11", "r11", "r12", "lsr", 1),
    ):
        t = "T_LSL" if kind == "lsl" else "T_LSR"
        case(f"t_{op}({R[rd]}, {R[rn]}, {R[rm]}, {t}, {amount})", f"{op}.w {rd}, {rn}, {rm}, {kind} #{amount}")
    case(f"t_{op}(11, 2, 0, T_LSL, 0)", f"{op}.w r11, r2, r0")
for rd, rm in (("r6", "r6"), ("r10", "r12"), ("r0", "r11"), ("r1", "r4")):
    case(f"t_mov({R[rd]}, {R[rm]})", f"mov.w {rd}, {rm}")
for rd, rm, amount in (("r0", "r0", 1), ("r10", "r4", 31), ("lr", "r12", 16)):
    case(f"t_lsr({R[rd]}, {R[rm]}, {amount})", f"lsr.w {rd}, {rm}, #{amount}")
for rd, rn, rm in (("r0", "r0", "r12"), ("r4", "r11", "lr"), ("lr", "r1", "r2")):
    case(f"t_lsr_reg({R[rd]}, {R[rn]}, {R[rm]})", f"lsr.w {rd}, {rn}, {rm}")
    case(f"t_lsl_reg({R[rd]}, {R[rn]}, {R[rm]})", f"lsl.w {rd}, {rn}, {rm}")
for rd, rm in (("r0", "r4"), ("r12", "lr"), ("r4", "r4")):
    case(f"t_rbit({R[rd]}, {R[rm]})", f"rbit {rd}, {rm}")
for rd, rn, imm in (("r11", "r10", 4), ("r10", "r3", 6), ("lr", "r4", 255)):
    case(f"t_orr_imm({R[rd]}, {R[rn]}, {imm})", f"orr.w {rd}, {rn}, #{imm}")
for rd, rm, amount in (("r11", "r12", 1), ("r10", "r3", 1), ("r4", "lr", 31)):
    case(f"t_lsl({R[rd]}, {R[rm]}, {amount})", f"lsl.w {rd}, {rm}, #{amount}")
for rd, rn, imm in (("r12", "r1", 32), ("r0", "r0", 0), ("lr", "r4", 255)):
    case(f"t_rsb_imm({R[rd]}, {R[rn]}, {imm})", f"rsb.w {rd}, {rn}, #{imm}")
for rn, imm in (("r11", 1), ("r11", 4), ("r4", 255)):
    case(f"t_cmp_imm({R[rn]}, {imm})", f"cmp.w {rn}, #{imm}")
    case(f"t_tst_imm({R[rn]}, {imm})", f"tst.w {rn}, #{imm}")
    case(f"t_subs_imm({R[rn]}, {R[rn]}, {imm})", f"subs.w {rn}, {rn}, #{imm}")
    case(f"t_mov_imm({R[rn]}, {imm})", f"mov.w {rn}, #{imm}")
for rd, value in (("r7", 1), ("r7", 0xFFFF), ("r8", 0xC000), ("r9", 0x4008), ("r3", 0x1000), ("r12", 0x8A5C)):
    case(f"t_movw({R[rd]}, {value})", f"movw {rd}, #{value}")
    case(f"t_movt({R[rd]}, {value})", f"movt {rd}, #{value}")
for offset in (-8, -4, 4, 8, 0x100, -0x1000, 0x7FFFC, -0x80000):
    case(f"t_bcond(T_COND_NE, {offset})", f"bne.w .{offset:+d}")
    case(f"t_bcond(T_COND_EQ, {offset})", f"beq.w .{offset:+d}")
for offset in (-8, 4, 0x100, -0x1000, 0x3FFFFC, -0x400000, 0xFFFFFC, -0xFFFFFC):
    case(f"t_b({offset})", f"b.w .{offset:+d}")
for rn in ("r5", "r6", "lr"):
    case(f"t_msr_primask({R[rn]})", f"msr primask, {rn}")
for rd, rn, rm, ra in (("r3", "r1", "r7", "r3"), ("r12", "r0", "lr", "r2")):
    case(f"t_mla({R[rd]}, {R[rn]}, {R[rm]}, {R[ra]})", f"mla {rd}, {rn}, {rm}, {ra}")
for rm in ("r3", "r12", "lr"):
    case(f"t_bx_pad({R[rm]})", f"bx {rm}\n    nop")
case("t_dmb()", "dmb sy")
case("t_push(0x4FF0)", "push.w {r4-r11, lr}")
case("t_pop(0x8FF0)", "pop.w {r4-r11, pc}")


def main() -> int:
    with tempfile.TemporaryDirectory() as tmp:
        c = os.path.join(tmp, "t.c")
        exe = os.path.join(tmp, "t")
        with open(c, "w") as fh:
            fh.write('#include <stdio.h>\n#include "thumb.h"\nint main(void) {\n')
            for expr, _ in CASES:
                fh.write(f'    printf("%08x\\n", (unsigned)({expr}));\n')
            fh.write("    return 0;\n}\n")
        subprocess.run(["cc", "-std=c11", "-Wall", "-Werror", "-I", SRC, "-o", exe, c], check=True)
        words = [int(line, 16) for line in subprocess.run([exe], capture_output=True, text=True, check=True).stdout.split()]

        src = os.path.join(tmp, "t.s")
        obj = os.path.join(tmp, "t.o")
        binary = os.path.join(tmp, "t.bin")
        with open(src, "w") as fh:
            fh.write(".syntax unified\n.thumb\n.cpu cortex-m33\n.text\n")
            for _, asm in CASES:
                fh.write(f"    {asm}\n")
        subprocess.run(["arm-none-eabi-as", "-o", obj, src], check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text", obj, binary], check=True)
        data = open(binary, "rb").read()

    failures = 0
    if len(data) != 4 * len(CASES):
        print(f"assembler produced {len(data)} bytes for {len(CASES)} instructions; one is not 32-bit")
        return 1
    for i, ((expr, asm), word) in enumerate(zip(CASES, words)):
        want = int.from_bytes(data[4 * i : 4 * i + 4], "little")
        if want != word:
            failures += 1
            print(f"  FAIL  {asm:<32} {expr}: {word:08x}, as says {want:08x}")
    print(f"{len(CASES) - failures}/{len(CASES)} encodings match the assembler")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
