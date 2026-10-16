# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn"]
# ///
"""Check src/mem.S's memcpy on an emulated Cortex-M: every length to 600 at every pair of offsets.

Assembles the routine on its own, runs each copy between guard bytes, and checks the copied
bytes, that nothing either side moved, that it returns the destination and that it preserves the
registers the AAPCS says it must.

    uv run tools/mem_test.py
"""

import os
import subprocess
import sys
import tempfile

from unicorn import UC_ARCH_ARM, UC_MODE_MCLASS, UC_MODE_THUMB, Uc
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R4,
    UC_ARM_REG_R5,
    UC_ARM_REG_R6,
    UC_ARM_REG_R7,
    UC_ARM_REG_R8,
    UC_ARM_REG_R9,
    UC_ARM_REG_R10,
    UC_ARM_REG_R11,
    UC_ARM_REG_SP,
)

HERE = os.path.dirname(os.path.abspath(__file__))
SOURCE = os.path.join(HERE, "..", "src", "mem.S")

CODE = 0x0000_1000
RAM = 0x2000_0000
RAM_SIZE = 0x0001_0000
SRC = RAM + 0x1000
DST = RAM + 0x4000
STACK_TOP = RAM + RAM_SIZE
RETURN = CODE + 0x0F00  # a halfword the routine never reaches; run until it

SAVED = (UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8, UC_ARM_REG_R9,
         UC_ARM_REG_R10, UC_ARM_REG_R11)
GUARD = 16


def assemble() -> bytes:
    with tempfile.TemporaryDirectory() as tmp:
        obj = os.path.join(tmp, "mem.o")
        out = os.path.join(tmp, "mem.bin")
        subprocess.run(["arm-none-eabi-gcc", "-mcpu=cortex-m33", "-mthumb", "-c", SOURCE, "-o", obj],
                       check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".text.memcpy", obj, out],
                       check=True)
        with open(out, "rb") as f:
            return f.read()


def main() -> int:
    code = assemble()
    uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
    uc.mem_map(CODE, 0x1000)
    uc.mem_write(CODE, code)
    uc.mem_map(RAM, RAM_SIZE)

    failures = 0
    cases = 0
    for src_off in range(4):
        for dst_off in range(4):
            for n in range(601):
                pattern = bytes((i * 7 + n + src_off) & 0xFF for i in range(n))
                uc.mem_write(SRC - GUARD, bytes(range(256))[: GUARD] + pattern + bytes(GUARD + 4))
                fill = bytes([0xA5]) * (n + 2 * GUARD + 4)
                uc.mem_write(DST - GUARD, fill)
                src = SRC + src_off
                dst = DST + dst_off
                uc.mem_write(src, pattern)
                uc.reg_write(UC_ARM_REG_R0, dst)
                uc.reg_write(UC_ARM_REG_R1, src)
                uc.reg_write(UC_ARM_REG_R2, n)
                for k, reg in enumerate(SAVED):
                    uc.reg_write(reg, 0x1111_0000 + k)
                uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
                uc.reg_write(UC_ARM_REG_LR, RETURN | 1)
                uc.emu_start(CODE | 1, RETURN, count=100_000)
                cases += 1

                problems = []
                if uc.reg_read(UC_ARM_REG_R0) != dst:
                    problems.append(f"returned {uc.reg_read(UC_ARM_REG_R0):#x}")
                got = bytes(uc.mem_read(dst, n))
                if got != pattern:
                    problems.append("data differs")
                before = bytes(uc.mem_read(dst - GUARD, GUARD))
                after = bytes(uc.mem_read(dst + n, GUARD))
                if before != bytes([0xA5]) * GUARD or after != bytes([0xA5]) * GUARD:
                    problems.append("wrote outside the destination")
                for k, reg in enumerate(SAVED):
                    if uc.reg_read(reg) != 0x1111_0000 + k:
                        problems.append(f"clobbered r{4 + k}")
                if uc.reg_read(UC_ARM_REG_SP) != STACK_TOP:
                    problems.append("stack pointer moved")
                if problems:
                    failures += 1
                    if failures <= 20:
                        print(f"src+{src_off} dst+{dst_off} n={n}: {', '.join(problems)}")
    print(f"{cases - failures}/{cases} copies correct")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
