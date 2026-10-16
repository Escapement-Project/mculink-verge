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
from dataclasses import dataclass

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
        subprocess.run(["arm-none-eabi-as", "-o", obj, src], check=True)
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
    starts as the caller's second argument. `pad` halfwords of `mov r12, r12` before the first
    timestamp move the body's alignment.
    """
    return (
        "push {r4-r11, lr}\n    mov r5, r0\n    mov r3, r1\n"
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
        + "\n    ldr r7, [r4]\n    subs r0, r7, r6\n    mov r1, r3\n    pop {r4-r11, pc}\n"
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

    def __init__(self, elf: str, model: "cy.Model | None" = None):
        super().__init__(elf, cy.Target([]), restart_at=set(), model=model)
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
        return (self.cycle & 0xFFFFFFFF) if offset == 4 else 0

    def _dwt_write(self, uc, offset, size, value, user):
        pass

    def write(self, arena: int, offset: int, data: bytes) -> None:
        base, _ = self.arenas[arena]
        self.uc.mem_write(base + offset, data)
        # unicorn keeps translated blocks across a write to the memory they came from.
        self.uc.ctl_remove_cache(base + offset, base + offset + len(data))
        self._decoded.clear()

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


def measure(lab, arena: int, case: Case, pad: int = 0, offset: int = 0, arg: int = 0) -> tuple[int, int]:
    base, _ = lab.arenas[arena]
    code = assemble(timed(case.body, case.setup, pad), base + offset)
    lab.write(arena, offset, code)
    return lab.run(arena, offset, (case.mask, arg, 0, 0))


def rep(text: str, n: int) -> str:
    return "\n".join(["    " + text] * n)


def corpus() -> list[Case]:
    cases = [Case("empty", "")]
    for n in (1, 4, 16):
        cases.append(Case(f"mov.w r12 x{n}", rep("mov.w r12, r12", n)))
        cases.append(Case(f"mov r12 (16-bit) x{n}", rep("mov r12, r12", n)))
        cases.append(Case(f"nop x{n}", rep("nop", n)))
        cases.append(Case(f"adds r0,#1 x{n}", rep("adds r0, #1", n)))
        cases.append(Case(f"str SET x{n}", rep("str r5, [r9]", n)))
        cases.append(Case(f"str SET/CLR alternating x{n}", "\n".join(
            ["    str r5, [r9]" if i % 2 == 0 else "    str r5, [r10]" for i in range(n)])))
        cases.append(Case(f"strb B x{n}", rep("strb r5, [r8, #2]", n)))
        cases.append(Case(f"ldrb B x{n}", rep("ldrb r0, [r8, #2]", n)))
        cases.append(Case(f"ldr sram x{n}", rep("ldr r0, [sp]", n)))
        cases.append(Case(f"str sram x{n}", rep("str r0, [sp, #-8]", n)))
    for k in range(0, 5):
        cases.append(Case(f"str SET; {k} mov.w; ldrb B", "    str r5, [r9]\n" + rep("mov.w r12, r12", k) + "\n    ldrb r0, [r8, #2]"))
        cases.append(Case(f"ldrb B; {k} mov.w; str SET", "    ldrb r0, [r8, #2]\n" + rep("mov.w r12, r12", k) + "\n    str r5, [r9]"))
        cases.append(Case(f"str SET; {k} mov.w; str CLR", "    str r5, [r9]\n" + rep("mov.w r12, r12", k) + "\n    str r5, [r10]"))
        cases.append(Case(f"ldrb B; use; {k} mov.w", "    ldrb r0, [r8, #2]\n" + rep("mov.w r12, r12", k) + "\n    orr r1, r1, r0"))
    for n in (1, 2, 3, 5):
        cases.append(Case(f"delay loop {n}", f"    movs r0, #{n}\n1:  subs r0, #1\n    bne 1b"))
    cases.append(Case("b to next (taken, aligned)", "    b 1f\n    .balign 4\n1:"))
    cases.append(Case("b over 2 bytes", "    b 1f\n    mov r12, r12\n1:  mov.w r12, r12"))
    cases.append(Case("b over 4 bytes", "    b 1f\n    mov.w r12, r12\n1:  mov.w r12, r12"))
    cases.append(Case("bne not taken", "    cmp r0, r0\n    bne 1f\n1:"))
    cases.append(Case("add pc skip 0", "    movs r0, #0\n    add pc, r0\n    nop\n" + rep("mov.w r12, r12", 4)))
    cases.append(Case("add pc skip 4", "    movs r0, #4\n    add pc, r0\n    nop\n" + rep("mov.w r12, r12", 4)))
    cases.append(Case("add pc skip 16", "    movs r0, #16\n    add pc, r0\n    nop\n" + rep("mov.w r12, r12", 4)))
    return cases


def run_corpus(labs: dict, arena: int, offset: int, pads: list[int], repeats: int) -> None:
    cases = corpus()
    names = list(labs)
    header = f"  {'case':<36}" + "".join(f"{n + ' ' + str(p):>12}" for p in pads for n in names)
    print(header)
    for case in cases:
        cells = []
        for pad in pads:
            for name in names:
                lab = labs[name]
                base, _ = lab.arenas[arena]
                code = assemble(timed(case.body, case.setup, pad), base + offset)
                lab.write(arena, offset, code)
                seen = sorted({lab.run(arena, offset, (case.mask, 0, 0, 0))[0] for _ in range(repeats)})
                cells.append(f"{'/'.join(str(s) for s in seen):>12}")
        print(f"  {case.name:<36}" + "".join(cells))


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
    args = ap.parse_args()

    labs = {}
    if not args.hw_only:
        labs["emu"] = EmulatedLab(args.elf)
    if not args.emu_only:
        labs["hw"] = HardwareLab(args.serial)
    arena = ARENA_SRAM if args.arena == "sram" else ARENA_SRAMX
    pads = [int(p) for p in args.pads.split(",")]
    run_corpus(labs, arena, args.offset, pads, args.repeats)
    return 0


if __name__ == "__main__":
    sys.exit(main())
