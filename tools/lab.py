# /// script
# requires-python = ">=3.11"
# dependencies = ["pyusb", "unicorn", "capstone"]
# ///
"""Time Thumb code on the probe and in the emulator, at the same address, and compare.

The emulator's cost model has to be exact for code a generator emits, and the only reference for
that is the core itself. Vendor 0x88 (src/code_lab.c, built with -DCODE_LAB=ON) runs a function
the host writes into RAM, with interrupts masked. Each case here is a body of instructions the
template times with DWT->CYCCNT, assembled once and run in both places.

    uv run tools/lab.py corpus --elf build-lab/mculink_cjtag.elf
    uv run tools/lab.py corpus --emu-only
"""

import argparse
import os
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cycles as cy  # noqa: E402

VENDOR_LAB = 0x88
LAB_INFO, LAB_WRITE, LAB_READ, LAB_RUN = 0, 1, 2, 3
ARENA_SRAM, ARENA_SRAMX = 0, 1
ARENA_NAMES = {ARENA_SRAM: "sram", ARENA_SRAMX: "sramx"}

SRAMX = (0x04000000, 0x8000)
DWT_BASE = 0xE0001000
CYCCNT = DWT_BASE + 4

# GPIO port 0 on the non-secure alias, as the image uses it.
GPIO = 0x4008C000
GPIO_SET0 = GPIO + 0x2200
GPIO_CLR0 = GPIO + 0x2280
GPIO_NOT0 = GPIO + 0x2300
GPIO_PIN0 = GPIO + 0x2100
GPIO_B0 = GPIO  # one byte per pin
GPIO_W0 = GPIO + 0x1000  # one word per pin


def assemble(source: str, base: int) -> bytes:
    """Thumb source to bytes, linked at `base` so a branch or literal resolves where it will run."""
    head = ".syntax unified\n.thumb\n.cpu cortex-m33\n.text\n.global lab\n.thumb_func\nlab:\n"
    with tempfile.TemporaryDirectory() as tmp:
        src = os.path.join(tmp, "lab.s")
        obj = os.path.join(tmp, "lab.o")
        elf = os.path.join(tmp, "lab.elf")
        binary = os.path.join(tmp, "lab.bin")
        with open(src, "w") as fh:
            fh.write(head + source + "\n")
        subprocess.run(["arm-none-eabi-as", "-o", obj, src], check=True, capture_output=True)
        subprocess.run(["arm-none-eabi-ld", "-Ttext", hex(base), "-e", "lab", "-o", elf, obj], check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", elf, binary], check=True)
        return open(binary, "rb").read()


def const(reg: str, value: int) -> str:
    return f"movw {reg}, #{value & 0xFFFF}\n    movt {reg}, #{value >> 16}\n"


def timed(body: str, setup: str = "", pad: int = 0) -> str:
    """A function that returns the cycles `body` takes in r0, and whatever it left in r3 in r1.

    r4 holds CYCCNT's address. r8 is GPIO's B block, r9 SET, r10 CLR, r11 the word registers, r5 a
    pin mask the caller passes in r0 (zero moves nothing and still makes the bus access), r6 and
    r7 the two timestamps. The body may use r0-r3, r12 and lr freely and must leave the rest; r3
    starts as the caller's second argument. [sp, #0] to [sp, #28] is scratch: anything above it
    is the caller's saved registers, and a store there returns into a corrupted caller with
    interrupts off. `pad` halfwords of `mov r12, r12` before the first
    timestamp move the body's alignment.
    """
    return (
        "push {r4-r11, lr}\n    sub sp, #32\n    mov r5, r0\n    mov r3, r1\n"
        + const("r4", CYCCNT)
        + const("r8", GPIO_B0)
        + const("r9", GPIO_SET0)
        + const("r10", GPIO_CLR0)
        + const("r11", GPIO_W0)
        + setup
        + "    .balign 4\n"
        + "    mov r12, r12\n" * pad
        + "    ldr r6, [r4]\n"
        + body
        + "\n    ldr r7, [r4]\n    subs r0, r7, r6\n    mov r1, r3\n    add sp, #32\n    pop {r4-r11, pc}\n"
    )


class HardwareLab:
    def __init__(self, serial: str | None):
        import dap

        class Args:
            pass

        args = Args()
        args.serial = serial
        self.probe = dap.one(args)
        reply = self.probe.command(bytes([VENDOR_LAB, LAB_INFO]))
        if len(reply) < 3 or reply[1] != 0:
            raise SystemExit("vendor 0x88 is not in this image; flash a -DCODE_LAB=ON build")
        self.arenas = {}
        for i in range(reply[2]):
            base = int.from_bytes(reply[3 + 8 * i : 7 + 8 * i], "little")
            size = int.from_bytes(reply[7 + 8 * i : 11 + 8 * i], "little")
            self.arenas[i] = (base, size)

    def write(self, arena: int, offset: int, data: bytes) -> None:
        for at in range(0, len(data), 448):
            chunk = data[at : at + 448]
            req = bytes([VENDOR_LAB, LAB_WRITE, arena]) + (offset + at).to_bytes(2, "little")
            req += len(chunk).to_bytes(2, "little") + chunk
            if self.probe.command(req)[1] != 0:
                raise SystemExit(f"write refused at {arena}:{offset + at:#x}")

    def run(self, arena: int, entry: int, args: tuple[int, int, int, int]) -> tuple[int, int]:
        req = bytes([VENDOR_LAB, LAB_RUN, arena]) + entry.to_bytes(2, "little")
        req += b"".join(a.to_bytes(4, "little") for a in args)
        reply = self.probe.command(req)
        if reply[1] != 0:
            raise SystemExit(f"run refused at {arena}:{entry:#x}")
        return int.from_bytes(reply[2:6], "little"), int.from_bytes(reply[6:10], "little")


class EmulatedLab(cy.Machine):
    """The same image on unicorn, with CYCCNT answering the model's cycle count."""

    def __init__(self, elf: str, model: "cy.Model | None" = None, engine=None):
        super().__init__(elf, cy.Target([]), restart_at=set(), model=model, engine=engine)
        self.uc.mem_map(*SRAMX)
        self.uc.mem_unmap(DWT_BASE, 0x1000)
        self.uc.mmio_map(DWT_BASE, 0x1000, self._dwt_read, None, self._dwt_write, None)
        reply = self.command(bytes([VENDOR_LAB, LAB_INFO]))
        if len(reply) < 3 or reply[1] != 0:
            raise SystemExit(f"{elf} has no vendor 0x88; build it with -DCODE_LAB=ON")
        self.arenas = {}
        for i in range(reply[2]):
            base = int.from_bytes(reply[3 + 8 * i : 7 + 8 * i], "little")
            size = int.from_bytes(reply[7 + 8 * i : 11 + 8 * i], "little")
            self.arenas[i] = (base, size)

    def _dwt_read(self, uc, offset, size, user):
        if self.engine is not None and self.counting:
            self.cycle = self.engine.access(DWT_BASE + offset, False)
        return (self.cycle & 0xFFFFFFFF) if offset == 4 else 0

    def _dwt_write(self, uc, offset, size, value, user):
        pass

    def write(self, arena: int, offset: int, data: bytes) -> None:
        base, _ = self.arenas[arena]
        self.uc.mem_write(base + offset, data)
        # unicorn keeps translated blocks across a write to the memory they came from.
        self.uc.ctl_remove_cache(base + offset, base + offset + len(data))
        self._decoded.clear()
        self._insns.clear()

    def run(self, arena: int, entry: int, args: tuple[int, int, int, int]) -> tuple[int, int]:
        req = bytes([VENDOR_LAB, LAB_RUN, arena]) + entry.to_bytes(2, "little")
        req += b"".join(a.to_bytes(4, "little") for a in args)
        self.begin()
        reply = self.command(req)
        self.end()
        return int.from_bytes(reply[2:6], "little"), int.from_bytes(reply[6:10], "little")


@dataclass
class Case:
    name: str
    body: str
    setup: str = ""
    mask: int = 0  # pin mask in r5; zero makes every GPIO store a bus access that moves nothing
    # Filler around the body, so neither timestamp waits on the body's last bus access. The
    # count then times the body alone, plus the filler, which is one cycle an instruction.
    drained: bool = False


DRAIN_BEFORE = 2
DRAIN_AFTER = 4


def case_body(case: Case) -> str:
    if not case.drained:
        return case.body
    return rep("mov.w r12, r12", DRAIN_BEFORE) + "\n" + case.body + "\n" + rep("mov.w r12, r12", DRAIN_AFTER)


def measure(lab, arena: int, case: Case, pad: int = 0, offset: int = 0, arg: int = 0) -> tuple[int, int]:
    base, _ = lab.arenas[arena]
    code = assemble(timed(case_body(case), case.setup, pad), base + offset)
    lab.write(arena, offset, code)
    return lab.run(arena, offset, (case.mask, arg, 0, 0))


def rep(text: str, n: int) -> str:
    return "\n".join(["    " + text] * n)


def corpus() -> list[Case]:
    """Every case the model is fitted and checked against, grouped by what each group isolates."""
    cases = [Case("empty", "")]
    forms = {
        "mov.w r12": "mov.w r12, r12",
        "mov r12 (16)": "mov r12, r12",
        "nop": "nop",
        "movs lo imm (16)": "movs r0, #1",
        "adds r0 chain (16)": "adds r0, #1",
        "add.w r0 chain": "add.w r0, r0, #1",
        "lsls r0 chain (16)": "lsls r0, r0, #1",
        "str.w GPIO SET": "str.w r5, [r9]",
        "strb.w B": "strb.w r5, [r8, #2]",
        "ldrb.w B": "ldrb.w r0, [r8, #2]",
        "ldr sp (16)": "ldr r0, [sp]",
        "ldr.w sp": "ldr.w r0, [sp, #4]",
        "str sp (16)": "str r0, [sp]",
        "str.w sp": "str.w r0, [sp, #8]",
        "ldr PIN word": "ldr.w r0, [r1]",
    }
    for name, insn in forms.items():
        setup = "    sub.w r1, r9, #0x100\n" if "PIN" in name else ""
        for n in (1, 2, 3, 4, 8, 16):
            cases.append(Case(f"G1 {name} x{n}", rep(insn, n), setup))
    pairs = {
        "adds r0/adds r1": ("adds r0, #1", "adds r1, #1"),
        "movs r0/movs r1": ("movs r0, #1", "movs r1, #2"),
        "lsls r0/lsls r1": ("lsls r0, r0, #1", "lsls r1, r1, #1"),
        "mov r0,r1/mov r2,r3": ("mov r0, r1", "mov r2, r3"),
        "ldr sp/adds r1": ("ldr r0, [sp]", "adds r1, #1"),
        "adds r1/ldr sp": ("adds r1, #1", "ldr r0, [sp]"),
        "str sp/adds r1": ("str r0, [sp]", "adds r1, #1"),
        "nop/adds r1": ("nop", "adds r1, #1"),
        "adds r1/nop": ("adds r1, #1", "nop"),
        "mov r12/nop": ("mov r12, r12", "nop"),
        "cmp/bne nt": ("cmp r0, r0", "bne 9f\n9:"),
    }
    for name, (a, b) in pairs.items():
        for n in (1, 4, 8):
            cases.append(Case(f"G6 {name} x{n}", "\n".join(f"    {a}\n    {b}" for _ in range(n))))
    for k in (0, 1, 2, 3, 4, 6, 8, 12, 16):
        for m in (1, 2, 3, 4, 6):
            cases.append(Case(f"G2 {k} mov16 then {m} str.w", rep("mov r12, r12", k) + "\n" + rep("str.w r5, [r9]", m)))
    for k in (0, 2, 4, 8):
        for m in (1, 2, 4):
            cases.append(Case(f"G2 {k} nop then {m} str.w", rep("nop", k) + "\n" + rep("str.w r5, [r9]", m)))
    for k in range(0, 4):
        filler = rep("mov.w r12, r12", k)
        cases.append(Case(f"G3 ldrb B; {k}; use", f"    ldrb.w r0, [r8, #2]\n{filler}\n    orr.w r1, r1, r0"))
        cases.append(Case(f"G3 ldr sp; {k}; use", f"    ldr r0, [sp]\n{filler}\n    orr.w r1, r1, r0"))
        cases.append(Case(f"G3 ldr sp; {k}; base", f"    mov r0, sp\n    str r0, [sp]\n    ldr r0, [sp]\n{filler}\n    ldr r1, [r0]"))
        cases.append(Case(f"G5 str SET; {k}; ldrb B", f"    str.w r5, [r9]\n{filler}\n    ldrb.w r0, [r8, #2]"))
        cases.append(Case(f"G5 ldrb B; {k}; str SET", f"    ldrb.w r0, [r8, #2]\n{filler}\n    str.w r5, [r9]"))
        cases.append(Case(f"G5 str SET; {k}; str CLR", f"    str.w r5, [r9]\n{filler}\n    str.w r5, [r10]"))
    for skip in (0, 2, 4, 6, 8):
        for target in ("mov.w r12, r12", "mov r12, r12"):
            tsize = 32 if ".w" in target else 16
            body = "    b.n 9f\n" + ("    mov r12, r12\n" * (skip // 2)) + f"9:  {target}"
            cases.append(Case(f"G4 b.n skip {skip} to {tsize}-bit", body))
            body = "    b.w 9f\n" + ("    mov r12, r12\n" * (skip // 2)) + f"9:  {target}"
            cases.append(Case(f"G4 b.w skip {skip} to {tsize}-bit", body))
            body = "    cmp r0, r0\n    beq 9f\n" + ("    mov r12, r12\n" * (skip // 2)) + f"9:  {target}"
            cases.append(Case(f"G4 beq taken skip {skip} to {tsize}-bit", body))
            body = "    movs r0, #0\n    cbz r0, 9f\n" + ("    mov r12, r12\n" * (skip // 2)) + f"9:  {target}"
            cases.append(Case(f"G4 cbz taken skip {skip} to {tsize}-bit", body))
    for n in (1, 2, 3, 4, 6):
        cases.append(Case(f"G4 loop subs/bne x{n}", f"    movs r0, #{n}\n9:  subs r0, #1\n    bne 9b"))
        cases.append(Case(f"G4 loop subs.w/bne.w x{n}", f"    mov.w r0, #{n}\n9:  subs.w r0, r0, #1\n    bne.w 9b"))
        cases.append(Case(f"G4 loop aligned subs/bne x{n}", f"    movs r0, #{n}\n    .balign 4\n9:  subs r0, #1\n    bne 9b"))
    for skip in (0, 4, 8, 16):
        cases.append(Case(f"G4 add pc skip {skip}", f"    movs r0, #{skip}\n    add pc, r0\n    nop\n" + rep("mov.w r12, r12", 4)))
    cases.append(Case("G4 bx to label", "    adr r0, 9f + 1\n    bx r0\n    .balign 4\n9:  mov.w r12, r12"))
    for n in (1, 2, 4):
        regs = ",".join(f"r{i}" for i in range(n))
        cases.append(Case(f"G7 push/pop {n} regs", f"    push {{{regs}}}\n    pop {{{regs}}}"))
    return cases


def corpus3() -> list[Case]:
    """The generator's vocabulary, drained so each count is the body's own issue span."""
    d = lambda name, body, setup="": Case(name, body, setup, drained=True)  # noqa: E731
    w = lambda n: rep("mov.w r12, r12", n)  # noqa: E731
    cases = [d("D empty", "")]
    for n in (1, 2, 4, 8):
        cases.append(d(f"D mov.w x{n}", w(n)))
        cases.append(d(f"D str.w GPIO x{n}", rep("str.w r5, [r9]", n)))
        cases.append(d(f"D ldrb.w GPIO x{n}", rep("ldrb.w r0, [r8, #2]", n)))
        cases.append(d(f"D strb.w B x{n}", rep("strb.w r5, [r8, #2]", n)))
    for pre in ("mov r12, r12", "mov.w r12, r12", "adds r1, #1", "add.w r1, r1, #1", "nop"):
        for mem in ("str.w r5, [r9]", "ldrb.w r0, [r8, #2]", "ldr r0, [sp]", "str r0, [sp]"):
            for n in (1, 4):
                cases.append(d(f"D ({pre}; {mem}) x{n}", "\n".join(f"    {pre}\n    {mem}" for _ in range(n))))
    for k in range(0, 5):
        cases.append(d(f"D ldrb.w; {k}; orr.w use", f"    ldrb.w r0, [r8, #2]\n{w(k)}\n    orr.w r1, r1, r0"))
        cases.append(d(f"D ldrb.w; {k}; orr.w use; orr.w r2", f"    ldrb.w r0, [r8, #2]\n{w(k)}\n    orr.w r1, r1, r0\n    orr.w r2, r2, r1"))
        cases.append(d(f"D ldrb.w; {k}; bfi use", f"    ldrb.w r0, [r8, #2]\n{w(k)}\n    bfi r1, r0, #5, #1"))
        cases.append(d(f"D ldrb.w; {k}; str.w r0 GPIO", f"    ldrb.w r0, [r8, #2]\n{w(k)}\n    str.w r0, [r9]"))
        cases.append(d(f"D ldr.w sp; {k}; orr.w use", f"    ldr.w r0, [sp, #4]\n{w(k)}\n    orr.w r1, r1, r0"))
        cases.append(d(f"D str.w; {k}; ldrb.w", f"    str.w r5, [r9]\n{w(k)}\n    ldrb.w r0, [r8, #2]"))
        cases.append(d(f"D str.w; {k}; str.w", f"    str.w r5, [r9]\n{w(k)}\n    str.w r5, [r10]"))
    # Forward conditional branches, both encodings, taken and not, flags set 0 to 2 cycles before.
    for enc in ("", ".w"):
        for taken in (True, False):
            cond = "beq" if taken else "bne"
            for gap in (0, 1, 2):
                for skip in (0, 4, 8):
                    body = f"    cmp.w r0, r0\n{w(gap)}\n    {cond}{enc} 9f\n{w(skip // 4)}\n9:  mov.w r12, r12"
                    cases.append(d(f"D fwd {cond}{enc} {'taken' if taken else 'not'} gap {gap} skip {skip}", body))
    # Unconditional, forward and to aligned and unaligned targets.
    for enc in ("b.n", "b.w"):
        for skip in (0, 2, 4, 6, 8):
            body = f"    {enc} 9f\n" + "    mov r12, r12\n" * (skip // 2) + "9:  mov.w r12, r12"
            cases.append(d(f"D {enc} fwd skip {skip}", body))
    # Backward loops, 32-bit forms as a generator would emit them, at each alignment of the head.
    for n in (1, 2, 3, 4, 8):
        for body_fill in (0, 1, 2):
            loop = f"    mov.w r0, #{n}\n9:\n{w(body_fill)}\n    subs.w r0, r0, #1\n    bne.w 9b"
            cases.append(d(f"D loop.w fill {body_fill} x{n}", loop))
            loop = f"    movs r0, #{n}\n9:\n{w(body_fill)}\n    subs r0, #1\n    bne 9b"
            cases.append(d(f"D loop.n fill {body_fill} x{n}", loop))
    # A loop around a store, the shape of a per-word block loop.
    for n in (1, 2, 4):
        cases.append(d(f"D loop store x{n}", f"    mov.w r0, #{n}\n9:  str.w r5, [r9]\n{w(3)}\n    subs.w r0, r0, #1\n    bne.w 9b"))
    cases.append(d("D cbz taken", "    movs r0, #0\n    cbz r0, 9f\n    mov.w r12, r12\n9:  mov.w r12, r12"))
    cases.append(d("D cbz not", "    movs r0, #1\n    cbz r0, 9f\n    mov.w r12, r12\n9:  mov.w r12, r12"))
    return cases


def corpus4() -> list[Case]:
    """What else a generator might emit from SRAMX: literals, calls, stack, masking, arithmetic."""
    d = lambda name, body, setup="": Case(name, body, setup, drained=True)  # noqa: E731
    w = lambda n: rep("mov.w r12, r12", n)  # noqa: E731
    cases = []
    for n in (1, 2, 4):
        # A literal in the code itself is a data access on the bus the code is fetched over.
        cases.append(d(f"E ldr literal x{n}", rep("ldr.w r0, 8f", n) + "\n    b.w 9f\n    .balign 4\n8:  .word 0x12345678\n9:"))
        cases.append(d(f"E ldr literal; 2; use x{n}", "\n".join(["    ldr.w r0, 8f\n" + w(2) + "\n    orr.w r1, r1, r0"] * n) + "\n    b.w 9f\n    .balign 4\n8:  .word 0x12345678\n9:"))
        cases.append(d(f"E push/pop {n}", f"    push {{{','.join(f'r{i}' for i in range(n))}}}\n    pop {{{','.join(f'r{i}' for i in range(n))}}}"))
        if n > 1:
            cases.append(d(f"E ldm sp {n}", f"    ldm.w sp, {{{','.join(f'r{i}' for i in range(n))}}}"))
            cases.append(d(f"E stm sp {n}", f"    stm.w sp, {{{','.join(f'r{i}' for i in range(n))}}}"))
        cases.append(d(f"E ldrd/strd x{n}", rep("ldrd r0, r1, [sp, #8]", n) + "\n" + rep("strd r0, r1, [sp, #8]", n)))
        cases.append(d(f"E mov movw/movt x{n}", rep("movw r0, #0x1234\n    movt r0, #0x5678", n)))
        cases.append(d(f"E mul x{n}", rep("mul r0, r0, r0", n)))
        cases.append(d(f"E ubfx/bfi x{n}", rep("ubfx r0, r1, #3, #1\n    bfi r1, r0, #7, #1", n)))
    for k in (0, 1, 2):
        cases.append(d(f"E bl; {k}; bx lr", f"    mov r3, lr\n    bl 7f\n    b.w 9f\n7:\n{w(k)}\n    bx lr\n9:  mov lr, r3"))
        cases.append(d(f"E blx reg; {k}; bx lr", f"    mov r3, lr\n    adr r0, 7f + 1\n    blx r0\n    b.w 9f\n    .balign 4\n7:\n{w(k)}\n    bx lr\n9:  mov lr, r3"))
    cases.append(d("E cpsid/cpsie", "    cpsid i\n    cpsie i"))
    cases.append(d("E mrs/msr primask", "    mrs r0, primask\n    cpsid i\n    msr primask, r0"))
    cases.append(d("E cpsid; str GPIO", "    cpsid i\n    str.w r5, [r9]\n    cpsie i"))
    cases.append(d("E udiv", "    movs r0, #100\n    movs r1, #7\n    udiv r2, r0, r1"))
    cases.append(d("E it eq; moveq x2", "    cmp r0, r0\n    it eq\n    moveq r1, #1\n    it ne\n    movne r1, #2"))
    return cases


def run_corpus(labs: dict, arena: int, offset: int, pads: list[int], repeats: int, select: str = "") -> dict:
    """Every case on every lab at every padding; returns {case: {"lab pad": "cycles[/other]"}}."""
    results: dict[str, dict[str, str]] = {}
    names = list(labs)
    cases = [c for c in (corpus() + corpus3() + corpus4()) if select in c.name]
    print(f"  {'case':<44}" + "".join(f"{n + ' ' + str(p):>10}" for p in pads for n in names))
    for case in cases:
        row = {}
        for pad in pads:
            for name in names:
                lab = labs[name]
                base, _ = lab.arenas[arena]
                try:
                    code = assemble(timed(case_body(case), case.setup, pad), base + offset)
                except subprocess.CalledProcessError:
                    row[f"{name} {pad}"] = "asm?"
                    continue
                lab.write(arena, offset, code)
                seen = sorted({lab.run(arena, offset, (case.mask, 0, 0, 0))[0] for _ in range(repeats)})
                row[f"{name} {pad}"] = "/".join(str(v) for v in seen)
        results[case.name] = row
        print(f"  {case.name:<44}" + "".join(f"{row[f'{n} {p}']:>10}" for p in pads for n in names))
    return results


DPIDR_REQUEST = 0xA5  # start, DP, read, A2=0, A3=0, parity, stop, park
GPIO_DIRSET0 = GPIO + 0x2380
PIN_SWDIO = 2
PIN_TXEN = 28


def swd_read_asm(period: int, low: int, sample: int) -> str:
    """A DPIDR read as straight-line code, one instruction a cycle, every store and sample placed.

    Every clock is `period` cycles: the falling-edge store at 0, the rising-edge store at `low`.
    Each of the 36 clocks that reads the target samples SWDIO at `sample` cycles past its falling
    edge: before `low` that is production's place, in the low phase; after it, in the high phase.
    Every instruction is a 32-bit encoding that costs one cycle from SRAMX, so a slot is a cycle
    there. Returns the 36 sampled bits, ack first, in r0 and the low byte of r1, and the cycles the
    read took in r1's upper bits.
    """
    if not (3 <= low < period and 1 <= sample < period and sample != low):
        raise ValueError("need 3 <= low < period, and a sample inside the period off the rising edge")
    fixed: dict[int, str] = {}
    floating: list[tuple[int, str]] = []  # (earliest cycle, instruction)
    clock = 0

    def edges(at: int) -> None:
        fixed[at] = "str.w r5, [r10]"
        fixed[at + low] = "str.w r5, [r9]"

    for i in range(8):
        at = clock * period
        edges(at)
        fixed[at + 1] = f"mov.w r0, #{(DPIDR_REQUEST >> i) & 1}"
        fixed[at + 2] = "strb.w r0, [r8, #2]"
        clock += 1
    # Turnaround: let go of SWDIO in the low phase, before the rising edge the target answers on.
    at = clock * period
    edges(at)
    fixed[at + 1] = "str.w r3, [r10]"  # TXEN off
    fixed[at + 2] = "str.w r2, [r11, #0x80]"  # SWDIO input
    clock += 1
    for bit in range(36):
        at = clock * period
        edges(at)
        fixed[at + sample] = "ldrb.w r0, [r8, #2]"
        acc, shift = ("r1", bit) if bit < 32 else ("lr", bit - 32)
        floating.append((at + sample + 2, f"orr.w {acc}, {acc}, r0, lsl #{shift}"))
        clock += 1
    at = clock * period
    edges(at)
    clock += 1
    end = clock * period
    floating.append((end, "str.w r2, [r11]"))  # SWDIO output
    floating.append((end, "str.w r3, [r9]"))  # TXEN on
    floating.append((end, "mov.w r0, #1"))
    floating.append((end, "strb.w r0, [r8, #2]"))

    slots: dict[int, str] = dict(fixed)
    for earliest, insn in floating:
        at = earliest
        while at in slots:
            at += 1
        # A floating op must not move a sample's register before the orr that reads it.
        slots[at] = insn
    last = max(slots)
    lines = [slots.get(c, "mov.w r12, r12") for c in range(last + 1)]
    return (
        "push {r4-r11, lr}\n    mov r5, r0\n"
        + const("r4", CYCCNT)
        + const("r8", GPIO_B0)
        + const("r9", GPIO_SET0)
        + const("r10", GPIO_CLR0)
        + const("r11", GPIO_DIRSET0)
        + f"    mov.w r2, #{1 << PIN_SWDIO}\n    mov.w r3, #{1 << PIN_TXEN}\n"
        + "    mov.w r1, #0\n    mov.w lr, #0\n    .balign 4\n    mov r12, r12\n"
        + "    ldr r6, [r4]\n"
        + "\n".join("    " + line for line in lines)
        + "\n    ldr r7, [r4]\n    subs r7, r7, r6\n    mov r0, r1\n    orr r1, lr, r7, lsl #8\n"
        + "    pop {r4-r11, pc}\n"
    )


def swd_read_pipelined(period: int, low: int, delay: int) -> str:
    """A DPIDR read whose samples are placed by their distance from the edge that launched them.

    The clock runs at `period` cycles, falling at 0 and rising at `low` in each. Every target bit
    is sampled `delay` cycles after the rising-edge store that launched it, which at a short
    period is several clocks later: the sample is pipelined behind the clock rather than tied to
    it. Each sample is stored to the buffer the caller passes in r1, one byte a bit, by the next
    free slot; a store of a just-loaded value does not wait for it. Every instruction is a 32-bit
    encoding costing one cycle from SRAMX, so a slot is a cycle there.

    Returns the cycles the read took in r0. A placement that collides with a clock edge raises.
    """
    if not (2 <= low <= period - 2) or period < 4:
        raise ValueError("need period >= 4 and 2 <= low <= period - 2")
    fixed: dict[int, str] = {}

    def put(at: int, insn: str) -> None:
        if at in fixed:
            raise ValueError(f"cycle {at} is taken by {fixed[at]!r}; cannot place {insn!r}")
        fixed[at] = insn

    def edges(clock: int) -> None:
        put(clock * period, "str.w r5, [r10]")
        put(clock * period + low, "str.w r5, [r9]")

    for i in range(8):
        edges(i)
        put(i * period + 1, "strb.w r0, [r8, #2]")
        put(i * period - 1, f"mov.w r0, #{(DPIDR_REQUEST >> i) & 1}")
    put(7 * period + low + 1, "str.w r3, [r10]")  # TXEN off, after the park bit's rising edge
    turnaround = 8
    edges(turnaround)
    put(turnaround * period + 1, "str.w r2, [r11, #0x80]")  # SWDIO input
    for k in range(37):  # 36 target bits, then the edge that hands the wire back
        edges(turnaround + 1 + k)
    regs = ("r0", "r12", "lr")
    last = 0
    for k in range(36):
        at = (turnaround + k) * period + low + delay
        put(at, f"ldrb.w {regs[k % 3]}, [r8, #2]")
        st = at + 1
        while st in fixed:
            st += 1
        nxt = (turnaround + k + 3) * period + low + delay  # the next load into this register
        if st >= nxt:
            raise ValueError(f"no slot to store sample {k} before its register is reused")
        put(st, f"strb.w {regs[k % 3]}, [r1, #{k}]")
        last = max(last, st)
    end = max(last, (turnaround + 38) * period) + 1
    for insn in ("str.w r2, [r11]", "str.w r3, [r9]", "mov.w r0, #1", "strb.w r0, [r8, #2]"):
        while end in fixed:
            end += 1
        put(end, insn)
    first = min(fixed)
    lines = [fixed.get(c, "mov.w r7, r7") for c in range(first, max(fixed) + 1)]
    return (
        "push {r4-r11, lr}\n    mov r5, r0\n"
        + const("r4", CYCCNT)
        + const("r8", GPIO_B0)
        + const("r9", GPIO_SET0)
        + const("r10", GPIO_CLR0)
        + const("r11", GPIO_DIRSET0)
        + f"    mov.w r2, #{1 << PIN_SWDIO}\n    mov.w r3, #{1 << PIN_TXEN}\n"
        + "    .balign 4\n    mov r12, r12\n"
        + "    ldr r6, [r4]\n"
        + "\n".join("    " + line for line in lines)
        + "\n    ldr r7, [r4]\n    subs r0, r7, r6\n    movs r1, #0\n    pop {r4-r11, pc}\n"
    )


def swd_write_asm(period: int, lead: int, value: int, request: int = 0x05) -> str:
    """A SWD write as straight-line code, every probe-driven bit set `lead` cycles before its edge.

    Each probe-driven clock is `period` cycles with the rising-edge store last; the SWDIO store
    sits `lead` cycles before it, and the falling-edge store takes the first free cycle. A lead of
    period - 1 changes SWDIO one cycle after the previous rising edge, which is how close to an
    edge the target holds its sample. The acknowledge is clocked but not sampled: a request the
    target misread shows up as a write that did not land. The default request writes AP TAR.
    One instruction a cycle from SRAMX; registers r6 and r7 hold 0 and 1.
    """
    if not 1 <= lead <= period - 1 or period < 3:
        raise ValueError("need period >= 3 and 1 <= lead <= period - 1")
    lines: list[str] = []

    def clock(data_reg: str | None) -> None:
        slots = ["mov.w r12, r12"] * period
        slots[period - 1] = "str.w r5, [r9]"  # rise
        if data_reg is not None:
            slots[period - 1 - lead] = f"strb.w {data_reg}, [r8, #2]"
        fall = next(i for i in range(period - 1) if slots[i] == "mov.w r12, r12")
        slots[fall] = "str.w r5, [r10]"
        lines.extend(slots)

    parity = bin(request & 0x0F).count("1") & 1
    packet = [1, request & 1, (request >> 1) & 1, (request >> 2) & 1, (request >> 3) & 1, parity, 0, 1]
    for bit in packet:
        clock("r7" if bit else "r6")
    lines += ["str.w r3, [r10]", "str.w r2, [r11, #0x80]"]  # let go: TXEN off, SWDIO input
    for _ in range(1 + 3 + 1):  # turnaround, acknowledge, turnaround
        clock(None)
    lines += ["str.w r2, [r11]", "str.w r3, [r9]"]  # take it back: SWDIO output, TXEN on
    data = [(value >> i) & 1 for i in range(32)] + [bin(value).count("1") & 1]
    for bit in data:
        clock("r7" if bit else "r6")
    # Idle clocks with SWDIO low, so the write completes; high would be a start bit. Then high.
    lines += ["strb.w r6, [r8, #2]"]
    for _ in range(8):
        clock(None)
    lines += ["strb.w r7, [r8, #2]"]
    return (
        "push {r4-r11, lr}\n    mov r5, r0\n"
        + const("r4", CYCCNT)
        + const("r8", GPIO_B0)
        + const("r9", GPIO_SET0)
        + const("r10", GPIO_CLR0)
        + const("r11", GPIO_DIRSET0)
        + f"    mov.w r2, #{1 << PIN_SWDIO}\n    mov.w r3, #{1 << PIN_TXEN}\n"
        + "    mov.w r6, #0\n    mov.w r7, #1\n"
        + "    .balign 4\n    mov r12, r12\n"
        + "    ldr r1, [r4]\n"
        + "\n".join("    " + line for line in lines)
        + "\n    ldr r0, [r4]\n    subs r0, r0, r1\n    movs r1, #0\n    pop {r4-r11, pc}\n"
    )


@dataclass
class Schedule:
    """A SWD sequence laid out cycle by cycle, from the timing model alone.

    Every instruction is a 32-bit encoding costing one cycle from SRAMX, so a slot is a cycle.
    `samples` lists, for each transfer, where its target bits land in the sample buffer.
    """

    period: int
    latency: int
    slots: dict = field(default_factory=dict)
    samples: list = field(default_factory=list)
    t: int = 0  # the cycle the next clock starts on
    nbuf: int = 0

    def put(self, at: int, insn: str) -> None:
        if at in self.slots:
            raise ValueError(f"cycle {at} is taken by {self.slots[at]!r}; cannot place {insn!r}")
        self.slots[at] = insn

    def free(self, at: int, low: int | None = None) -> int:
        """The first empty cycle from `at`. With `low`, also keep off the edge and data cycles
        of the uniform clocks that have not been laid yet, which start at `self.t`."""
        while at in self.slots or (low is not None and at >= self.t and (at - self.t) % self.period in (0, low)):
            at += 1
        return at


def swd_schedule(period: int, transfers: list, latency: int = 7) -> Schedule:
    """Place a list of SWD transfers on a clock of `period` cycles.

    `transfers` holds ("w", request, value) and ("r", request). Every clock is `period` cycles,
    falling at its start and rising `low` cycles in. A probe-driven bit's SWDIO store sits one
    cycle after the falling edge. A target bit is sampled at the centre of the window the model
    gives, `latency` to `period + latency - 1` cycles after the store that launched it, moved
    off any cycle an edge already holds, and stored to the buffer at r1 by the next free cycle.
    Where the probe hands the wire over or takes it back, the next clock waits for every sample
    still outstanding: that lengthens one clock and never shortens any.
    """
    if period < 4:
        raise ValueError("a read clock needs four instructions: period >= 4")
    low = period // 2
    sched = Schedule(period, latency)
    regs = ("r0", "r12", "lr")
    outstanding = []  # (register, cycle of its load) not yet stored

    def clock(data_reg=None):
        start = sched.t
        sched.put(start, "str.w r5, [r10]")
        if data_reg is not None:
            sched.put(start + 1, f"strb.w {data_reg}, [r8, #2]")
        sched.put(start + low, "str.w r5, [r9]")
        sched.t = start + period
        return start + low  # the rising edge

    def sample(rise: int, index: int):
        # Nearest free cycle to the centre of the window, never outside it. Later clocks are
        # uniform, so their edges fall on known cycles: keep off those too. They are the
        # target's clocks, with no SWDIO store: the probe drives again only after settle().
        def edge_like(c):
            return (c - (sched.t - period)) % period in (0, low)
        window = range(latency, period + latency)
        for d in sorted(window, key=lambda d: (-min(d - latency, period + latency - 1 - d), d)):
            at = rise + d
            if at in sched.slots or (at >= sched.t and edge_like(at)):
                continue
            reg = regs[index % 3]
            sched.put(at, f"ldrb.w {reg}, [r8, #2]")
            outstanding.append((reg, at, sched.nbuf))
            sched.nbuf += 1
            return at
        raise ValueError(f"no cycle in the window for a sample after the edge at {rise}")

    def settle():
        # Store every outstanding sample, then start the next clock after the last one.
        last = sched.t - 1
        for reg, at, slot in outstanding:
            st = sched.free(at + 1, low)
            sched.put(st, f"strb.w {reg}, [r1, #{slot}]")
            last = max(last, st)
        outstanding.clear()
        sched.t = max(sched.t, last + 1)

    def flush_due():
        # A sample register is reused three samples later; store any sample that has to go.
        while len(outstanding) > 2:
            reg, at, slot = outstanding.pop(0)
            st = sched.free(at + 1, low)
            sched.put(st, f"strb.w {reg}, [r1, #{slot}]")

    def op(insn):
        sched.put(sched.free(sched.t), insn)
        sched.t = max(sched.t, max(c for c, i in sched.slots.items() if i == insn) + 1)

    for tr in transfers:
        kind, request = tr[0], tr[1]
        parity = bin(request & 0x0F).count("1") & 1
        packet = [1, request & 1, (request >> 1) & 1, (request >> 2) & 1, (request >> 3) & 1, parity, 0, 1]
        for bit in packet:
            clock("r5" if bit else "r6")
        op("str.w r3, [r10]")  # TXEN off
        op("str.w r2, [r11, #0x80]")  # SWDIO input
        first = sched.nbuf
        rise = clock()  # turnaround: the target launches ack0 on this edge
        bits = 3 if kind == "w" else 36
        for k in range(bits):
            nxt = clock()
            sample(rise, k)
            flush_due()
            rise = nxt
        clock()  # the turnaround after the acknowledge or after parity
        settle()
        if kind == "r":
            op("str.w r2, [r11]")
            op("str.w r3, [r9]")
            op("strb.w r5, [r8, #2]")
        else:
            op("str.w r2, [r11]")
            op("str.w r3, [r9]")
            value = tr[2]
            data = [(value >> i) & 1 for i in range(32)] + [bin(value).count("1") & 1]
            for bit in data:
                clock("r5" if bit else "r6")
            op("strb.w r5, [r8, #2]")
        sched.samples.append((kind, first, sched.nbuf - first))
    # Idle clocks with SWDIO low so the last write completes, then leave it high.
    op("strb.w r6, [r8, #2]")
    for _ in range(8):
        clock()
    op("strb.w r5, [r8, #2]")
    return sched


def schedule_asm(sched: Schedule) -> str:
    """The schedule as code. A run of three or more empty cycles becomes a counted loop on r7,
    which costs exactly three cycles an iteration: every instruction here is 32-bit, so every
    loop head is word-aligned. The data registers are r6 for 0 and r5, the clock mask, for 1."""
    first, last = min(sched.slots), max(sched.slots)
    lines = []
    c = first
    while c <= last:
        if c in sched.slots:
            lines.append(sched.slots[c])
            c += 1
            continue
        run = 0
        while c + run <= last and c + run not in sched.slots:
            run += 1
        n = run // 3
        if n >= 1:
            label = f"L{c}"
            lines += [f"movw r7, #{n}", f"{label}: subs.w r7, r7, #1", f"bne.w {label}"]
        lines += ["mov.w r6, r6"] * (run - 3 * n)
        c += run
    return (
        "push {r4-r11, lr}\n    mov r5, r0\n"
        + const("r4", CYCCNT)
        + const("r8", GPIO_B0)
        + const("r9", GPIO_SET0)
        + const("r10", GPIO_CLR0)
        + const("r11", GPIO_DIRSET0)
        + f"    mov.w r2, #{1 << PIN_SWDIO}\n    mov.w r3, #{1 << PIN_TXEN}\n"
        + "    mov.w r6, #0\n"
        + "    .balign 4\n    mov r12, r12\n"
        + "    ldr r0, [r4]\n    push {r0}\n    .balign 4\n"
        + "\n".join(("" if ":" in line else "    ") + line for line in lines)
        + "\n    ldr r0, [r4]\n    pop {r1}\n    subs r0, r0, r1\n    movs r1, #0\n    pop {r4-r11, pc}\n"
    )


def swd_stream(value: int) -> int:
    """The 36 bits a correct DPIDR read samples: OK, the word, its parity."""
    parity = bin(value).count("1") & 1
    return 0b001 | (value << 3) | (parity << 35)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--serial")
    ap.add_argument("--elf", default="build-lab/mculink_cjtag.elf")
    sub = ap.add_subparsers(dest="command", required=True)
    c = sub.add_parser("corpus", help="time every case on the probe and in the emulator")
    c.add_argument("--arena", choices=["sram", "sramx"], default="sram")
    c.add_argument("--offset", type=lambda s: int(s, 0), default=0)
    c.add_argument("--pads", default="0,1", help="halfwords of padding before the timed body")
    c.add_argument("--repeats", type=int, default=3)
    c.add_argument("--emu-only", action="store_true")
    c.add_argument("--hw-only", action="store_true")
    c.add_argument("--select", default="", help="only cases whose name contains this")
    c.add_argument("--save", help="write the results as JSON, keyed by arena")
    args = ap.parse_args()

    labs = {}
    if not args.hw_only:
        labs["emu"] = EmulatedLab(args.elf)
    if not args.emu_only:
        labs["hw"] = HardwareLab(args.serial)
    arena = ARENA_SRAM if args.arena == "sram" else ARENA_SRAMX
    pads = [int(p) for p in args.pads.split(",")]
    results = run_corpus(labs, arena, args.offset, pads, args.repeats, args.select)
    if args.save:
        import json

        saved = json.load(open(args.save)) if os.path.exists(args.save) else {}
        saved[args.arena] = results
        with open(args.save, "w") as fh:
            json.dump(saved, fh, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
