# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn", "capstone"]
# ///
"""Count what one debug-port bit costs, by emulating the built image.

The bench cannot answer this yet and a disassembly listing cannot either: the bit paths are
unrolled, the delay loop length is computed at run time from the requested clock, and the compiler
interleaves the two. So run the real image on an emulated Cortex-M33, drive it through the real
DAP command layer, and count what actually executes between one clock edge and the next.

What comes out is an instruction count per bit, broken down by class, and a cycle estimate from a
cost model whose constants are command-line arguments. The counts are facts about the image. The
cycles are a model, and the bench settles it: one measurement of the real clock period at two
delay settings solves for the store and branch costs.
"""

import argparse
import itertools
import os
import re
import subprocess
import sys
import tempfile
from dataclasses import dataclass, field

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs
from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_CODE,
    UC_HOOK_MEM_READ,
    UC_HOOK_MEM_WRITE,
    UC_MODE_MCLASS,
    UC_MODE_THUMB,
    Uc,
    UcError,
)
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m33 import Engine, Insn  # noqa: E402

# LPC55S69, non-secure aliases, which is what the image was built against.
GPIO_BASE = 0x4008C000
GPIO_SIZE = 0x4000
# Byte-addressable pin registers live at the base; the rest start 0x2000 in. Port 0 is the only
# port this firmware touches, so every offset below is that port's word.
REG_B0 = 0x0000
REG_DIR = 0x2000
REG_MASK = 0x2080
REG_PIN = 0x2100
REG_MPIN = 0x2180
REG_SET = 0x2200
REG_CLR = 0x2280
REG_NOT = 0x2300
REG_DIRSET = 0x2380
REG_DIRCLR = 0x2400

PIN_TCK = 0
PIN_TDI = 1
PIN_SWDIO = 2
PIN_TDO = 3
PIN_TXEN = 28

SWCLK_BIT = 1 << PIN_TCK
SWDIO_BIT = 1 << PIN_SWDIO

FLASH = (0x00000000, 0x00100000)
# Fetched over C-AHB. The image leaves it empty; the SWD paths are generated into it at run time.
SRAMX = (0x04000000, 0x00008000)
SRAM = (0x20000000, 0x00050000)
USB_RAM = (0x40100000, 0x00004000)
PERIPH_LOW = (0x40000000, GPIO_BASE - 0x40000000)
PERIPH_HIGH = (GPIO_BASE + GPIO_SIZE, 0x40100000 - (GPIO_BASE + GPIO_SIZE))
PPB = (0xE0000000, 0x00100000)
STACK_TOP = SRAM[0] + SRAM[1] - 0x100
SCRATCH = SRAM[0] + SRAM[1] - 0x4000  # request/response buffers, clear of .bss and the stack
RETURN_MAGIC = 0x00F00000  # inside flash, nothing is linked there


def sh(*args: str) -> str:
    return subprocess.run(args, capture_output=True, text=True, check=True).stdout


def base_name(symbol: str) -> str:
    """The name as written, with whatever the compiler appended when it privatised it.

    GCC's link-time optimiser renames a static it has made local to `name.lto_priv.0`, and clang's
    renames one to `name.llvm.<hash>`. Both are the same symbol a person wrote, and a tool that
    looks one up by name finds nothing in one toolchain's build and everything in the other's.
    """
    for marker in (".lto_priv.", ".llvm.", ".constprop.", ".isra."):
        cut = symbol.find(marker)
        if cut > 0:
            return symbol[:cut]
    return symbol


def symbols(elf: str) -> dict[str, int]:
    out = {}
    for line in sh("arm-none-eabi-nm", elf).splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
            out.setdefault(base_name(parts[2]), int(parts[0], 16))
    return out


def sections(elf: str) -> dict[str, tuple[int, int, int]]:
    """name -> (size, vma, lma)"""
    out = {}
    for line in sh("arm-none-eabi-objdump", "-h", elf).splitlines():
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            out[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
    return out


@dataclass
class Model:
    """Cycles per instruction class, Cortex-M33 defaults from its TRM."""

    other: int = 1
    load: int = 2
    store: int = 1
    taken_branch: int = 2
    gpio_store: int = 1  # a store to the GPIO block, which sits on its own AHB port
    # A conditional branch that falls through costs nothing beyond the instruction it sits in
    # front of: the bench measured a 14-instruction data bit the model priced at 17 cycles taking
    # exactly 15, and the two fall-through `bne`s of its two delay loops are the whole difference.
    not_taken_branch: int = 0
    # Time instructions with tools/m33.py instead of the constants above.
    engine: bool = False
    # With the engine, a SWD target that answers by time, readable this many cycles after the
    # rising-edge store that launched a bit. None keeps the target that answers in order.
    latency: int | None = None

    def total(self, c: "Counts") -> int:
        if self.engine:
            return c.span
        return (
            c.other * self.other
            + c.load * self.load
            + (c.store - c.gpio_store) * self.store
            + c.gpio_store * self.gpio_store
            + c.taken_branch * (self.taken_branch - self.other)
            + c.not_taken_branch * (self.not_taken_branch - self.other)
        )


@dataclass
class Counts:
    instructions: int = 0
    other: int = 0
    load: int = 0
    store: int = 0
    gpio_store: int = 0
    taken_branch: int = 0
    not_taken_branch: int = 0
    delay_iterations: int = 0
    tck_rise: int = 0
    tck_fall: int = 0
    swdio_release: int = 0
    # A wire transition while the clock is high. Harmless on SWD and four-wire JTAG; on a 1149.7
    # wire it is an escape edge, which is the whole reason this firmware exists.
    moves_while_high: int = 0
    trace: list[str] = field(default_factory=list)
    # The running cycle count at each rising clock edge. Differences are wire periods, which is
    # what an analyser measures and what Set_Clock_Delay is trying to control.
    edge_cycles: list[int] = field(default_factory=list)
    # Cycles from begin() to end(), as the timing engine counts them.
    span: int = 0
    # With a timed SWD target, every read and where it sat in its window.
    reads: list = field(default_factory=list)


# The 16 TAP states, and where TMS takes each one. Named as IEEE 1149.1 names them.
TAP_NEXT = {
    "tlr": ("rti", "tlr"),
    "rti": ("rti", "seldr"),
    "seldr": ("capdr", "selir"),
    "capdr": ("shdr", "ex1dr"),
    "shdr": ("shdr", "ex1dr"),
    "ex1dr": ("paudr", "upddr"),
    "paudr": ("paudr", "ex2dr"),
    "ex2dr": ("shdr", "upddr"),
    "upddr": ("rti", "seldr"),
    "selir": ("capir", "tlr"),
    "capir": ("shir", "ex1ir"),
    "shir": ("shir", "ex1ir"),
    "ex1ir": ("pauir", "updir"),
    "pauir": ("pauir", "ex2ir"),
    "ex2ir": ("shir", "updir"),
    "updir": ("rti", "seldr"),
}


class JtagTap:
    """A JTAG target that walks the TAP with the probe and shifts a chosen word out of TDO.

    A flat list of bits cannot model this: which bit the probe reads depends on where the TAP is,
    and that is exactly what a rewritten transfer could get wrong. Tracking the state machine from
    TMS means the model notices a wrong TAP walk as well as a wrong bit.
    """

    def __init__(self, dr: int, dr_bits: int):
        # ARM's transfer starts its walk from Run-Test/Idle, which the host is expected to have
        # reached with a JTAG sequence before any transfer. Starting in Test-Logic-Reset instead
        # leaves TMS=1 looping in place and the walk never reaches a shift state.
        self.state = "rti"
        self.initial = (dr, dr_bits)
        self.dr = dr
        self.left = dr_bits

    def tdo(self) -> int:
        # Only the data register is modelled; the probe does not read TDO while shifting IR.
        return self.dr & 1 if self.state == "shdr" else 0

    def clock(self, tms: int) -> None:
        # Every rising edge in a shift state shifts, including the one that leaves it.
        if self.state == "shdr":
            self.dr >>= 1
        self.state = TAP_NEXT[self.state][1 if tms else 0]
        if self.state == "capdr":
            self.dr = self.initial[0]


class Target:
    """A SWD or JTAG target that always acknowledges OK and shifts out zeros.

    The firmware only reads the wire during the acknowledge and data phases, and it reads them in
    order, so a per-transfer list of bits is enough. The list is restarted whenever the emulator
    enters a transfer function, which is what keeps it in step without guessing from bus traffic.
    """

    def __init__(self, pattern: list[int]):
        self.pattern = pattern
        self.index = 0

    def restart(self):
        self.index = 0

    def bit(self) -> int:
        if self.index < len(self.pattern):
            value = self.pattern[self.index]
        else:
            value = 0
        self.index += 1
        return value


@dataclass
class Read:
    """One SWDIO sample, placed against the window of the bit it was meant to read.

    The bit a read wants is the one the target launched on the rising edge `want` edges after the
    turnaround began, counting reads in order. It is on the probe's pin for loads from `latency`
    cycles after that edge's store until `latency` cycles after the next one's. `early` is how far
    into that window the read landed and `late` how far before its end, both in cycles; either
    one negative means the read saw a neighbouring bit. `late` stays None while no later edge has
    happened, which leaves the window open on that side.
    """

    cycle: int
    want: int
    early: int | None = None
    late: int | None = None
    got: int = 0
    wanted: int = 0

    @property
    def margin(self) -> int | None:
        sides = [m for m in (self.early, self.late) if m is not None]
        return min(sides) if sides else None


class TimedSwdTarget:
    """A SWD target that answers by time rather than by the order of the probe's reads.

    The target launches each bit on a rising SWCLK edge, and the probe sees it on SWDIO from
    `latency` cycles after the store that raised the clock. A read before that sees the previous
    bit; a read from `latency` cycles after the next edge sees the next. `latency` is a store to
    load distance in issue cycles of tools/m33.py's engine: the probe's own path out and back in
    plus the target's, so it depends on the target and the cable as well as on this probe.

    `serve` picks what a read returns: "wire", what the timing puts on the pin, so the firmware
    reacts as it would on hardware; or "intended", the bit the read was meant to get, so a run
    keeps going and every read's margin is still recorded.
    """

    def __init__(self, pattern: list[int], latency: int, turnaround: int = 1, serve: str = "wire", idle: int = 1):
        self.pattern = pattern
        self.latency = latency
        self.turnaround = turnaround
        self.serve = serve
        self.idle = idle
        self.released = False
        self.launches: list[tuple[int, int]] = []  # (store cycle, bit), in order
        self.reads: list[Read] = []
        self.transfer_reads: list[Read] = []
        self.rises = 0

    def restart(self) -> None:
        self.released = True
        self.launches = []
        self.transfer_reads = []
        self.rises = 0

    def take_back(self) -> None:
        self.released = False

    def rise(self, cycle: int) -> None:
        if not self.released:
            return
        self.rises += 1
        k = self.rises - self.turnaround
        if k < 0:
            return
        bit = self.pattern[k] if k < len(self.pattern) else self.idle
        self.launches.append((cycle, bit))
        # A read that came before this edge now knows where its window closed.
        for read in self.transfer_reads:
            if read.late is None and read.want + 1 == k:
                read.late = cycle + self.latency - 1 - read.cycle

    def read(self, cycle: int) -> int:
        on_wire = self.idle
        for at, bit in self.launches:
            if at + self.latency <= cycle:
                on_wire = bit
        want = len(self.transfer_reads)
        wanted = self.pattern[want] if want < len(self.pattern) else self.idle
        r = Read(cycle, want, got=on_wire, wanted=wanted)
        if want < len(self.launches):
            r.early = cycle - (self.launches[want][0] + self.latency)
        if want + 1 < len(self.launches):
            r.late = self.launches[want + 1][0] + self.latency - 1 - cycle
        self.transfer_reads.append(r)
        self.reads.append(r)
        return on_wire if self.serve == "wire" else wanted

    def bit(self) -> int:
        raise RuntimeError("a timed target answers read(cycle); this machine has no timing engine")


# Acknowledge bits in the order each protocol reads them, then the data phase.
SWD_OK = [1, 0, 0]  # ack bit0 first, DAP_TRANSFER_OK == 1
SWD_READ = SWD_OK + [0] * 33  # 32 data bits and parity
SWD_WRITE = SWD_OK

# The data register a modelled TAP shifts out: an OK acknowledge, then a word of zeros.
# JTAG_Transfer reassembles the acknowledge as bit1, bit0, bit2, so OK puts the one in the middle.
JTAG_DR_OK = 1 << 1


UNCONDITIONAL = {"b", "b.w", "b.n", "bl"}
CONDITIONAL_PREFIX = ("cbz", "cbnz")
CONDITIONS = ("eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le")


def condition_holds(cc: int, flags: int) -> bool:
    """Whether an ARM condition code passes, given NZCV in bits 31..28 of `flags`."""
    from capstone import arm as A

    n, z, c, v = (flags >> 31) & 1, (flags >> 30) & 1, (flags >> 29) & 1, (flags >> 28) & 1
    return {
        A.ARM_CC_EQ: z == 1,
        A.ARM_CC_NE: z == 0,
        A.ARM_CC_HS: c == 1,
        A.ARM_CC_LO: c == 0,
        A.ARM_CC_MI: n == 1,
        A.ARM_CC_PL: n == 0,
        A.ARM_CC_VS: v == 1,
        A.ARM_CC_VC: v == 0,
        A.ARM_CC_HI: c == 1 and z == 0,
        A.ARM_CC_LS: c == 0 or z == 1,
        A.ARM_CC_GE: n == v,
        A.ARM_CC_LT: n != v,
        A.ARM_CC_GT: z == 0 and n == v,
        A.ARM_CC_LE: z == 1 or n != v,
    }.get(cc, True)


def branch_kind(mnemonic: str, writes_pc: bool) -> str:
    base = mnemonic.split(".")[0]
    if mnemonic in UNCONDITIONAL:
        return "uncond"
    if base.startswith(CONDITIONAL_PREFIX) or (base[:1] == "b" and base[1:] in CONDITIONS):
        return "cond"
    if writes_pc or base in ("bx", "blx", "tbb", "tbh"):
        return "indirect"
    return ""


class Machine:
    def __init__(
        self,
        elf: str,
        target: Target,
        restart_at: set[int],
        model: "Model | None" = None,
        engine: "Engine | None" = None,
    ):
        self.sym = symbols(elf)
        self.model = model or Model()
        # When set, issue and bus cycles come from tools/m33.py instead of the per-class model.
        if engine is None and self.model.engine:
            engine = Engine()
        self.engine = engine
        self._insns: dict[int, Insn] = {}
        # How each branch decides, by address: a condition code, or the register cbz/cbnz tests.
        self._branch_test: dict[int, tuple[int, str | None]] = {}
        self._branch_taken = False
        self.cycle = 0
        self.target = target
        self.counting = False
        self.restart_at = restart_at
        self.resync_on_release = False
        self.tap: "JtagTap | None" = None
        # The same TAP reached through OScan1 framing instead of four wires.
        self.cjtag_tap: "JtagTap | None" = None
        self._packet = 0
        self._frame_tms = 0
        self.explain = False
        self._window: list[int] = []
        self._window_start = 0
        self._prev_was_branch = False
        self.longest: tuple[int, list[int]] = (0, [])
        # Every gap, not only the longest. The longest is the first word of a block, which carries
        # the command dispatch; the one that costs is the steady-state gap repeated once a word.
        self.windows: list[tuple[int, list[int]]] = []
        self.counts = Counts()
        self.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        self.md.detail = True
        self._decoded: dict[int, tuple[str, int]] = {}
        self._prev_end: int | None = None

        self.level = 0
        self.dir = 0
        self.mask = 0

        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for base, size in (FLASH, SRAMX, SRAM, USB_RAM, PERIPH_LOW, PERIPH_HIGH, PPB):
            self.uc.mem_map(base, size)
        self.uc.mmio_map(GPIO_BASE, GPIO_SIZE, self._gpio_read, None, self._gpio_write, None)

        with tempfile.NamedTemporaryFile(suffix=".bin") as raw:
            sh("arm-none-eabi-objcopy", "-O", "binary", elf, raw.name)
            image = open(raw.name, "rb").read()
        self.uc.mem_write(FLASH[0], image)
        # The startup code is not run, so do what it does for .data.
        secs = sections(elf)
        size, vma, lma = secs[".data"]
        if size:
            self.uc.mem_write(vma, bytes(self.uc.mem_read(lma, size)))

        self.uc.hook_add(UC_HOOK_CODE, self._code)
        if self.engine is not None:
            # Only over plain memory. A memory hook that also covers the MMIO-mapped GPIO block
            # changes what unicorn does there, and the JTAG path then takes a different branch;
            # GPIO and the PPB report their own accesses to the engine anyway.
            for base, size in (FLASH, SRAMX, SRAM, USB_RAM):
                self.uc.hook_add(UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE, self._mem, None, base, base + size - 1)

    def _mem(self, uc, access, address, size, value, user):
        # GPIO and the PPB report their own accesses, from their callbacks.
        if not self.counting or GPIO_BASE <= address < GPIO_BASE + GPIO_SIZE or address >= 0xE0000000:
            return
        self.engine.access(address, access == 17)  # UC_MEM_WRITE

    def _decide(self, insn: Insn) -> bool:
        """Whether a branch about to execute will be taken, from the state before it runs."""
        if insn.branch in ("uncond", "indirect"):
            return True
        if insn.branch != "cond":
            return False
        cc, reg = self._branch_test[insn.address]
        if reg is not None:
            from unicorn import arm_const

            value = self.uc.reg_read(getattr(arm_const, f"UC_ARM_REG_{reg.upper()}"))
            return (value == 0) == insn.mnemonic.startswith("cbz")
        from unicorn.arm_const import UC_ARM_REG_APSR

        return condition_holds(cc, self.uc.reg_read(UC_ARM_REG_APSR))

    def _insn_at(self, address: int) -> Insn:
        code = bytes(self.uc.mem_read(address, 4))
        insn = next(self.md.disasm(code, address), None)
        return self._insn(address, insn.size if insn else 2)

    def _insn(self, address: int, size: int) -> Insn:
        hit = self._insns.get(address)
        if hit is None:
            code = bytes(self.uc.mem_read(address, size))
            insn = next(self.md.disasm(code, address), None)
            if insn is None:
                hit = Insn(address, size, "?")
            else:
                read, written = insn.regs_access()
                reads = frozenset(insn.reg_name(r) for r in read)
                writes = frozenset(insn.reg_name(r) for r in written)
                data = frozenset()
                if insn.mnemonic.startswith(("str", "push", "stm")) and insn.operands:
                    # The registers stored, not the ones that form the address.
                    from capstone.arm import ARM_OP_REG

                    data = frozenset(insn.reg_name(op.reg) for op in insn.operands if op.type == ARM_OP_REG)
                hit = Insn(
                    address,
                    size,
                    insn.mnemonic,
                    reads,
                    writes,
                    branch=branch_kind(insn.mnemonic, "pc" in writes),
                    store_data=data,
                )
                if hit.branch == "cond":
                    reg = insn.reg_name(insn.operands[0].reg) if insn.mnemonic.startswith("cb") else None
                    self._branch_test[address] = (insn.cc, reg)
            self._insns[address] = hit
        return hit

    # -- GPIO ----------------------------------------------------------------
    def _gpio_read(self, uc, offset, size, user):
        if self.engine is not None and self.counting:
            self.cycle = self.engine.access(GPIO_BASE + offset, False)
        # B[0][n] is one byte per pin at the start of the block: SWDIO is pin 2, TDO pin 3.
        if offset in (REG_B0 + PIN_SWDIO, REG_B0 + PIN_TDO):
            if self.counting:
                self.counts.trace.append("S" if offset == REG_B0 + PIN_SWDIO else "T")
            if self.tap is not None and offset == REG_B0 + PIN_TDO:
                return self.tap.tdo()
            if self.cjtag_tap is not None and offset == REG_B0 + PIN_SWDIO:
                return self.cjtag_tap.tdo()
            if isinstance(self.target, TimedSwdTarget):
                # While the probe drives the wire, its pin reads back what it drives.
                driven = self._driven()
                if driven != "z":
                    return int(driven)
                return self.target.read(self.cycle)
            return self.target.bit()
        if offset == REG_PIN:
            return self.level & self.dir
        if offset == REG_DIR:
            return self.dir
        return 0

    def _driven(self) -> str:
        """What the target sees on the shared wire: our level, or z when we have let go.

        The buffer direction pin gates the line as much as the pin direction does, so a release
        needs both and so does taking it back.
        """
        if not (self.dir >> PIN_SWDIO) & 1 or not (self.level >> PIN_TXEN) & 1:
            return "z"
        return "1" if (self.level >> PIN_SWDIO) & 1 else "0"

    def _gpio_write(self, uc, offset, size, value, user):
        if self.engine is not None and self.counting:
            self.cycle = self.engine.access(GPIO_BASE + offset, True)
        before_clock = (self.level >> PIN_TCK) & 1
        before_wire = self._driven()
        before_tdi = (self.level >> PIN_TDI) & 1

        if offset == REG_SET:
            self.level |= value
        elif offset == REG_CLR:
            self.level &= ~value & 0xFFFFFFFF
        elif offset == REG_NOT:
            self.level ^= value
        elif offset == REG_MPIN:
            keep = self.mask
            self.level = (self.level & keep) | (value & ~keep & 0xFFFFFFFF)
        elif offset == REG_MASK:
            self.mask = value
        elif offset == REG_DIR:
            self.dir = value
        elif offset == REG_DIRSET:
            self.dir |= value
        elif offset == REG_DIRCLR:
            self.dir &= ~value & 0xFFFFFFFF
        elif REG_B0 <= offset < REG_B0 + 32:
            pin = offset - REG_B0
            self.level = (self.level & ~(1 << pin)) | ((1 if value & 1 else 0) << pin)
        else:
            return

        after_clock = (self.level >> PIN_TCK) & 1
        after_wire = self._driven()
        after_tdi = (self.level >> PIN_TDI) & 1

        # The SWD target tracks the wire whether or not anything is being counted. Keying it on the
        # measurement meant a run that never called begin() got a target that never acknowledged,
        # and a transfer that had failed was then reported as one that had been cheap. The two TAP
        # models stay behind the guard below: they decode OScan1 framing, which holds only once the
        # harness has driven the activation, and the activation's own clocks are not frames.
        if after_wire != before_wire and after_wire == "z" and self.resync_on_release:
            self.target.restart()
        if isinstance(self.target, TimedSwdTarget):
            if before_wire == "z" and after_wire != "z":
                self.target.take_back()
            if after_clock and not before_clock:
                self.target.rise(self.cycle)

        if not self.counting:
            return
        self.counts.gpio_store += 1

        # Level changes are recorded before the clock edge in the same store, so that fusing a
        # data write into the falling edge produces the same stream as doing them separately.
        if after_tdi != before_tdi:
            self.counts.trace.append(f"I{after_tdi}")
        if after_wire != before_wire:
            self.counts.trace.append(f"W{after_wire}")
            if after_wire == "z":
                self.counts.swdio_release += 1
                # A frame releases the wire exactly once, immediately before its third packet.
                # That is a marker on the wire, so the framing stays in step with the probe
                # whatever the optimiser does to the code that produces it.
                self._packet = 2
            if before_clock and after_clock:
                self.counts.moves_while_high += 1
        if after_clock and not before_clock:
            self.counts.tck_rise += 1
            self.counts.edge_cycles.append(self.cycle)
            if self.explain:
                span = self.cycle - self._window_start
                if span > self.longest[0]:
                    self.longest = (span, self._window)
                self.windows.append((span, self._window))
                self._window = []
                self._window_start = self.cycle
            self.counts.trace.append(f"^{after_wire}{after_tdi}")
            if self.tap is not None:
                self.tap.clock((self.level >> PIN_SWDIO) & 1)
            if self.cjtag_tap is not None:
                # Packet 1 carries nTDI, packet 2 carries TMS, packet 3 is the target's.
                if self._packet == 1:
                    self._frame_tms = (self.level >> PIN_SWDIO) & 1
                elif self._packet == 2:
                    self.cjtag_tap.clock(self._frame_tms)
                self._packet = (self._packet + 1) % 3
        elif before_clock and not after_clock:
            self.counts.tck_fall += 1
            self.counts.trace.append("v")

    # -- instruction accounting ---------------------------------------------
    def _classify(self, address: int, size: int) -> tuple[str, int]:
        hit = self._decoded.get(address)
        if hit is None:
            code = bytes(self.uc.mem_read(address, size))
            insn = next(self.md.disasm(code, address), None)
            mnemonic = insn.mnemonic if insn else "?"
            kind = "other"
            if mnemonic.startswith(("ldr", "ldm", "pop", "ldrex", "tbb", "tbh")):
                kind = "load"
            elif mnemonic.startswith(("str", "stm", "push", "strex")):
                kind = "store"
            elif mnemonic.startswith(("b", "cb")) and not mnemonic.startswith("bic"):
                kind = "branch"
            hit = (kind, size)
            self._decoded[address] = hit
        return hit

    def _code(self, uc, address, size, user):
        if address == RETURN_MAGIC:
            uc.emu_stop()
            return
        if address in self.restart_at:
            self.target.restart()
        if not self.counting:
            self._prev_end = address + size
            return

        if self.engine is not None:
            # A branch to the next instruction is taken all the same, and costs as much. Any
            # other jump past the next instruction is an IT block skipping one, which unicorn
            # does not run but the core still issues.
            taken = self._prev_end is not None and self._branch_taken
            if self._prev_end is not None and not taken and address != self._prev_end:
                skipped = self._prev_end
                while skipped < address and skipped - self._prev_end < 8:
                    insn = self._insn_at(skipped)
                    self.engine.issue(insn, None)
                    skipped += insn.size
            insn = self._insn(address, size)
            self.cycle = self.engine.issue(insn, address if taken else None)
            self._branch_taken = self._decide(insn)
            if taken:
                self.counts.taken_branch += 1
            self._prev_end = address + size
            if self.explain:
                self._window.append(address)
            self.counts.instructions += 1
            return

        if self._prev_was_branch and self._prev_end is not None and address == self._prev_end:
            # The branch before this one fell through. It was charged as an ordinary instruction
            # when it issued, because whether it was taken is only visible from where we land.
            self.counts.not_taken_branch += 1
            self.cycle += self.model.not_taken_branch - self.model.other

        if self._prev_end is not None and address != self._prev_end:
            self.counts.taken_branch += 1
            # The pacing loop is two instructions, so a backward branch this short is one of its
            # iterations. Counting them is what lets the delay be separated from everything else
            # without parsing the clock arithmetic out of the source.
            if 0 < self._prev_end - address <= 8:
                self.counts.delay_iterations += 1
            self.cycle += self.model.taken_branch - self.model.other
        self._prev_end = address + size

        if self.explain:
            self._window.append(address)
        kind, _ = self._classify(address, size)
        self._prev_was_branch = kind == "branch"
        c = self.counts
        c.instructions += 1
        m = self.model
        if kind == "load":
            c.load += 1
            self.cycle += m.load
        elif kind == "store":
            c.store += 1
            self.cycle += m.store
        else:
            c.other += 1
            self.cycle += m.other

    # -- running -------------------------------------------------------------
    def call(self, name: str, *args: int, timeout_insns: int = 50_000_000) -> int:
        addr = self.sym[name]
        self.uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN_MAGIC | 1)
        for i, value in enumerate(args):
            self.uc.reg_write([UC_ARM_REG_R0, UC_ARM_REG_R1][i], value)
        self._prev_end = None
        try:
            self.uc.emu_start(addr | 1, RETURN_MAGIC, count=timeout_insns)
        except UcError as exc:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            raise SystemExit(f"emulation fault in {name} at 0x{pc:08x}: {exc}") from exc
        return self.uc.reg_read(UC_ARM_REG_R0)

    def command(self, request: bytes) -> bytes:
        self.uc.mem_write(SCRATCH, request + bytes(600 - len(request)))
        self.uc.mem_write(SCRATCH + 0x400, bytes(600))
        # The entry point main() uses. DAP_ProcessCommand sits behind it and is not always a
        # symbol: a build that routes some commands of its own leaves a wrapper small enough to
        # inline away.
        entry = "DAP_ExecuteCommand" if "DAP_ExecuteCommand" in self.sym else "DAP_ProcessCommand"
        produced = self.call(entry, SCRATCH, SCRATCH + 0x400) & 0xFFFF
        if produced > 0x400:
            # A reply that long means the call did not finish: the usual cause is a pacing loop
            # counting down from zero, which takes about a minute of emulated time and leaves a
            # register holding whatever it held.
            raise SystemExit(f"{entry} returned a {produced}-byte reply, which means it never returned")
        return bytes(self.uc.mem_read(SCRATCH + 0x400, max(produced, 1)))

    def begin(self):
        self.counts = Counts()
        self.cycle = 0
        self.counting = True
        if self.engine is not None:
            self.engine.reset()

    def end(self) -> Counts:
        self.counting = False
        if self.engine is not None:
            self.counts.span = self.cycle
        if isinstance(self.target, TimedSwdTarget):
            self.counts.reads = list(self.target.reads)
            self.target.reads = []
        return self.counts


def u16(v: int) -> bytes:
    return v.to_bytes(2, "little")


def u32(v: int) -> bytes:
    return v.to_bytes(4, "little")


# DAP command ids used below.
ID_CONNECT, ID_TRANSFER_CONFIG, ID_TRANSFER, ID_TRANSFER_BLOCK = 0x02, 0x04, 0x05, 0x06
ID_SWJ_CLOCK, ID_SWJ_SEQUENCE, ID_SWD_CONFIGURE = 0x11, 0x12, 0x13
ID_JTAG_SEQUENCE, ID_JTAG_CONFIGURE, ID_SWD_SEQUENCE = 0x14, 0x15, 0x1D
ID_CJTAG_CONTROL = 0x83

AP_DRW_READ, AP_DRW_WRITE = 0x0F, 0x0D


def build(elf: str, wire: str, read: bool, model: Model) -> Machine:
    """A machine with a target model attached, found from the wire rather than from symbols.

    SWD re-synchronises on the turnaround, the only place the probe lets go. JTAG and the two-pin
    path drive a modelled TAP, which needs no marker at all. Keying any of it off a function entry
    worked until the optimiser inlined that function, and then the run silently took an error path
    and reported a much better number.
    """
    target = Target(SWD_READ if read else SWD_WRITE)
    if wire == "swd" and model.engine and model.latency is not None:
        # The bit the target answers with matters for nothing but the reply, so keep the order
        # target's pattern and serve what each read meant to get; the margins carry the timing.
        target = TimedSwdTarget(SWD_READ if read else SWD_WRITE, model.latency, serve="intended", idle=0)
    probe = Machine(elf, target, restart_at=set(), model=model)
    if wire == "swd":
        probe.resync_on_release = True
    elif wire == "jtag":
        probe.tap = JtagTap(JTAG_DR_OK, 35)
    else:
        probe.cjtag_tap = JtagTap(JTAG_DR_OK, 35)
    return probe


def measure(
    elf: str, wire: str, clock_hz: int, words: int, read: bool, model: Model, explain: bool = False
) -> tuple[Counts, bytes, "Machine"]:
    probe = build(elf, wire, read, model)
    # DAP_Setup only sets defaults that the commands below overwrite, and link-time optimisation
    # usually folds it into main, so run it when it still exists and skip it when it does not.
    if "DAP_Setup" in probe.sym:
        probe.call("DAP_Setup")
    probe.command(bytes([ID_CONNECT, 1 if wire == "swd" else 2]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(clock_hz))
    probe.command(bytes([ID_TRANSFER_CONFIG, 0]) + u16(100) + u16(0))
    if wire == "swd":
        probe.command(bytes([ID_SWD_CONFIGURE, 0x00]))
    else:
        probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
    if wire == "cjtag":
        probe.command(bytes([ID_CJTAG_CONTROL, 1]))

    request = AP_DRW_READ if read else AP_DRW_WRITE
    payload = bytes([ID_TRANSFER_BLOCK, 0]) + u16(words) + bytes([request])
    if not read:
        payload += u32(0xA5A5_5A5A) * words

    probe.explain = explain
    probe.begin()
    probe._window_start = 0
    response = probe.command(payload)
    return probe.end(), response, probe


def measure_transfer(
    elf: str, wire: str, clock_hz: int, count: int, read: bool, model: Model, explain: bool = False
) -> tuple[Counts, bytes, "Machine"]:
    """The same measurement through DAP_Transfer instead of DAP_TransferBlock.

    Not the throughput path -- a host reads memory in blocks -- but it is most of what an attach
    is made of, and of anything that touches one register at a time. The per-transfer gap is much
    larger here than in a block, which is what src/swd_transfer.h is about.
    """
    probe = build(elf, wire, read, model)
    if "DAP_Setup" in probe.sym:
        probe.call("DAP_Setup")
    probe.command(bytes([ID_CONNECT, 1 if wire == "swd" else 2]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(clock_hz))
    probe.command(bytes([ID_TRANSFER_CONFIG, 0]) + u16(100) + u16(0))
    if wire == "swd":
        probe.command(bytes([ID_SWD_CONFIGURE, 0x00]))
    else:
        probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
    if wire == "cjtag":
        probe.command(bytes([ID_CJTAG_CONTROL, 1]))

    if read:
        payload = bytes([ID_TRANSFER, 0, count]) + bytes([AP_DRW_READ]) * count
    else:
        payload = bytes([ID_TRANSFER, 0, count]) + (bytes([AP_DRW_WRITE]) + u32(0xA5A5_5A5A)) * count

    probe.explain = explain
    probe.begin()
    probe._window_start = 0
    response = probe.command(payload)
    return probe.end(), response, probe


def measure_bit_loop(elf: str, clock_hz: int, model: Model, bits: int = 248) -> Counts:
    """Cost of a bare clocked bit, with no transfer around it.

    DAP_SWJ_Sequence is SW_CLOCK_CYCLE with one data write per bit and nothing else: no packet
    request, no acknowledge, no turnaround, no per-word bookkeeping. It is the only scenario here
    whose cycles-per-bit is the quantity ARM's clock arithmetic is actually about.
    """
    probe = Machine(elf, Target([]), restart_at=set(), model=model)
    probe.command(bytes([ID_CONNECT, 1]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(clock_hz))
    probe.begin()
    probe.command(bytes([ID_SWJ_SEQUENCE, bits % 256]) + bytes([0xA5]) * ((bits + 7) // 8))
    return probe.end()


def explain_gap(probe: "Machine") -> None:
    """Name the code that runs in the longest stretch between two clock edges."""
    span, window = probe.longest
    if not window:
        return
    ordered = sorted((a, n) for n, a in probe.sym.items())
    addrs = [a for a, _ in ordered]
    import bisect

    tally: dict[str, int] = {}
    for pc in window:
        i = bisect.bisect_right(addrs, pc) - 1
        name = ordered[i][1] if i >= 0 else "?"
        tally[name] = tally.get(name, 0) + 1
    ranked = sorted(tally.items(), key=lambda kv: -kv[1])[:6]
    print(f"    the longest gap is {span} cycles, {len(window)} instructions, in:")
    for name, n in ranked:
        print(f"      {n:5d}  {name}")
    if os.environ.get("CYCLES_GAP_LISTING"):
        print("    the instructions, in order:")
        for pc in window:
            i = bisect.bisect_right(addrs, pc) - 1
            name = ordered[i][1] if i >= 0 else "?"
            print(f"      {pc:#010x}  {name}")


def report(name: str, c: Counts, model: Model, clock_hz: int, cpu_hz: int, words: int):
    clocks = c.tck_rise or 1
    cycles = model.total(c)
    bit_cycles = cycles / clocks
    achieved = cpu_hz / bit_cycles if bit_cycles else 0
    print(f"  {name}")
    print(
        f"    wire clocks {clocks}  ({clocks / words:.1f} per word)"
        f"   instructions {c.instructions}  ({c.instructions / clocks:.2f} per clock)"
    )
    print(
        f"    of those: {c.load} loads, {c.store} stores ({c.gpio_store} to GPIO),"
        f" {c.taken_branch} taken branches, {c.not_taken_branch} fell through, {c.other} other"
    )
    print(f"    wire moved {c.moves_while_high} times while the clock was high")
    periods = [b - a for a, b in zip(c.edge_cycles, c.edge_cycles[1:])]
    if periods:
        tally: dict[int, int] = {}
        for p in periods:
            tally[p] = tally.get(p, 0) + 1
        steady = max(tally.items(), key=lambda kv: kv[1])[0]
        print(
            f"    wire period: shortest {min(periods)}, longest {max(periods)},"
            f" held for most of a transfer {steady} cycles = {cpu_hz / steady / 1e6:.2f} MHz"
        )
        # The one safety property a debug probe owes its target. A transfer runs at several
        # periods and only the shortest one can break anything, so that is what has to be under
        # the clock the host asked for -- not the average, and not the common case.
        fastest = cpu_hz / min(periods)
        verdict = "within" if fastest <= clock_hz else "OVER"
        print(f"    fastest edge {fastest / 1e6:.2f} MHz, {verdict} the {clock_hz / 1e6:g} MHz asked")
        # Ranked by the cycles each class costs in total, which is where the time actually goes.
        total = sum(periods)
        ranked = sorted(tally.items(), key=lambda kv: -kv[0] * kv[1])[:5]
        shape = "   ".join(f"{p}cyc x{n} = {100 * p * n / total:.0f}%" for p, n in ranked)
        print(f"    where the time goes: {shape}")
    print(
        f"    modelled {cycles} cycles, {bit_cycles:.2f} per clock"
        f"  ->  {achieved / 1e6:.2f} MHz on the wire for {clock_hz / 1e6:.0f} MHz asked"
        f"   ({100 * achieved / clock_hz:.0f}%)"
    )
    if c.reads:
        margins = [r.margin for r in c.reads if r.margin is not None]
        wrong = sum(1 for r in c.reads if r.got != r.wanted)
        print(
            f"    sampled {len(c.reads)} target bits: least margin {min(margins)} cycles,"
            f" {sum(1 for m in margins if m < 0)} outside their window, {wrong} would read wrong"
        )
    # Wire time only. The host round trip between packets is not modelled and the bench measured
    # it at about 10% of a transfer at 14 MHz, so treat this as a ceiling.
    seconds = cycles / cpu_hz
    print(f"    {words * 4 / seconds / 1024:.0f} KiB/s of wire time for {words} words")


def dap_constants(path: str, cpu_hz: int) -> tuple[int, int, int]:
    """CPU_CLOCK, IO_PORT_WRITE_CYCLES and DELAY_SLOW_CYCLES, as the image was built with them.

    CPU_CLOCK is a macro from src/core_clock.h whose value turns on a build option, so the source
    alone cannot say which core clock an image was built for. When it does not resolve to a number
    here, `--cpu` is what decides, and CMake passes the figure the build used.
    """
    text = open(path).read()

    def value(name: str, default: int | None = None) -> int:
        m = re.search(rf"^#define\s+{name}\s+(\d+)U?", text, re.M)
        if m:
            return int(m.group(1))
        if default is None:
            raise SystemExit(f"{name} not found in {path}")
        return default

    return value("CPU_CLOCK", cpu_hz), value("IO_PORT_WRITE_CYCLES"), value("DELAY_SLOW_CYCLES", 3)


def calibrate(elf: str, wire: str, words: int, cpu_hz: int, model: Model, config: str) -> int:
    """Recover the two constants ARM's clock arithmetic assumes, from what the image actually runs.

    DAP.c models a half period as IO_PORT_WRITE_CYCLES + clock_delay * DELAY_SLOW_CYCLES, and picks
    clock_delay from the requested frequency with that model. Measure cycles per bit against delay
    iterations per bit and the line's slope and intercept are those two constants. Where they are
    wrong, every requested clock is wrong by the same amount.
    """
    cpu_clock, io_cycles, slow_cycles = dap_constants(config, cpu_hz)
    fast_above = (cpu_clock / 2) / io_cycles
    points = []
    print(f"{elf}, calibration against a bare clocked bit (DAP_SWJ_Sequence)")
    print(
        f"built with CPU_CLOCK {cpu_clock / 1e6:g} MHz, IO_PORT_WRITE_CYCLES {io_cycles},"
        f" DELAY_SLOW_CYCLES {slow_cycles}; the delay-free path starts at {fast_above / 1e6:.1f} MHz\n"
    )
    print(f"  {'asked':>8}  {'path':>5}  {'delay/bit':>10}  {'cycles/bit':>11}  {'MHz':>7}")
    for mhz in (0.5, 1, 2, 3, 5, 8, 10, 20, 50):
        counts = measure_bit_loop(elf, int(mhz * 1e6), model=model)
        clocks = counts.tck_rise or 1
        cycles = model.total(counts) / clocks
        # The loop runs its last pass with the branch falling through, so one iteration per delay
        # -- two per bit -- never shows up as a taken branch. The delay-free path has no loop.
        fast = mhz * 1e6 >= fast_above
        delays = counts.delay_iterations / clocks + (0.0 if fast else 2.0)
        if not fast:
            points.append((delays, cycles))
        print(
            f"  {mhz:8g}  {'fast' if fast else 'slow':>5}  {delays:10.2f}  {cycles:11.2f}"
            f"  {cpu_hz / cycles / 1e6:7.2f}"
        )

    n = len(points)
    sx = sum(x for x, _ in points)
    sy = sum(y for _, y in points)
    sxx = sum(x * x for x, _ in points)
    sxy = sum(x * y for x, y in points)
    slope = (n * sxy - sx * sy) / (n * sxx - sx * sx)
    intercept = (sy - slope * sx) / n
    print(f"\n  cycles per bit = {intercept:.2f} + {slope:.2f} * delay iterations per bit")
    print(f"  measured DELAY_SLOW_CYCLES {slope:.1f} against the {slow_cycles} built in")
    print(f"  measured IO_PORT_WRITE_CYCLES {intercept / 2:.1f} against the {io_cycles} built in")
    print("  a transfer costs more per bit than this: it also pays for the acknowledge, the")
    print("  turnaround and the bookkeeping, none of which the clock arithmetic is about")
    return 0


CHECK_WIRES = ("swd", "jtag", "cjtag")
CHECK_CLOCKS = (1, 5, 10, 20, 50)
BASELINE_DIR = os.path.join(os.path.dirname(os.path.abspath(__file__)), "baseline")


TABLE_CLOCKS = (1, 2, 3, 5, 8, 10, 12, 14, 16, 20, 25, 30, 40, 50)


def table(elf: str, model: Model, cpu_hz: int, words: int) -> int:
    """What the wire does for each rate a host can ask for.

    The clock delay is a whole number of loop iterations, so the rate goes up in steps and most
    requests land on a step below them. Printing the whole range is the only way to see which
    request is worth making: the steps matter far more than any remaining optimisation, and a
    change that flattens one would otherwise show up as a small loss at a single clock.
    """
    wires = [("SWD", "swd"), ("JTAG", "jtag"), ("two-pin", "cjtag")]
    print(f"| asked | {' | '.join(f'{n} {d}' for n, _ in wires for d in ('read', 'write'))} |")
    print("| --- " * (1 + 2 * len(wires)) + "|")
    for mhz in TABLE_CLOCKS:
        cells = []
        for _, wire in wires:
            for read in (True, False):
                counts, _, _ = measure(elf, wire, mhz * 1_000_000, words, read, model)
                seconds = model.total(counts) / cpu_hz
                cells.append(f"{words * 4 / seconds / 1024:.0f}")
        print(f"| {mhz} MHz | {' | '.join(cells)} |")
    return 0


def check(elf: str, model: Model, cpu_hz: int, bless: bool) -> int:
    """The gate: an optimisation may change the timing and must not change anything else.

    Three things are checked. The wire event stream has to match what is recorded, which is what
    makes a change to the bit paths reviewable without reading the generated code. The two-pin path
    has to emit no transition while the clock is high, because that is an escape edge and the
    reason this firmware exists. And no wire may clock faster than the host asked for, except where
    that is already recorded as known -- a new one is a regression.
    """
    failures = []

    # The activation sequence is the one thing here that has to match a published format rather
    # than only match itself, and it is driven by a table extracted from OpenOCD's. Recording what
    # the pin layer actually emits for it catches a change to the table, to the loop that walks it,
    # or to the buffer handling around it.
    probe = Machine(elf, Target([]), restart_at=set(), model=model)
    probe.command(bytes([ID_CONNECT, 2]))
    # The clock first: DAP_Setup normally leaves a default behind and the emulator skips it, so
    # without this the pacing loop counts down from zero.
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(10_000_000))
    probe.begin()
    probe.command(bytes([ID_CJTAG_CONTROL, 1]))
    activation = probe.end()
    path = os.path.join(BASELINE_DIR, "cjtag-activation.trace")
    body = "\n".join(activation.trace) + "\n"
    if bless:
        os.makedirs(BASELINE_DIR, exist_ok=True)
        open(path, "w").write(body)
    elif not os.path.exists(path):
        failures.append("cjtag-activation.trace: no baseline recorded")
    elif open(path).read() != body:
        failures.append("cjtag-activation.trace: the activation sequence changed on the wire")

    # The sequence commands are how a host resets a line, switches protocol and walks a TAP by
    # hand, so they run on every attach -- and no transfer reaches the code that serves them. In
    # particular the two-pin JTAG_Sequence entry point has nothing else exercising it.
    for wire, name, payload in (
        (
            "swd",
            "swj-sequence",
            bytes([ID_SWJ_SEQUENCE, 56]) + bytes([0xA5, 0x3C, 0xFF, 0x00, 0x5A, 0xC3, 0x69]),
        ),
        ("swd", "swd-sequence", bytes([ID_SWD_SEQUENCE, 2, 16, 0xA5, 0x3C, 0x80 | 16])),
        ("jtag", "jtag-sequence", bytes([ID_JTAG_SEQUENCE, 2, 0x08, 0xA5, 0x80 | 0x40 | 0x08, 0x3C])),
        ("cjtag", "jtag-sequence", bytes([ID_JTAG_SEQUENCE, 2, 0x08, 0xA5, 0x80 | 0x40 | 0x08, 0x3C])),
    ):
        probe = Machine(elf, Target([0, 1] * 64), restart_at=set(), model=model)
        probe.command(bytes([ID_CONNECT, 1 if wire == "swd" else 2]))
        probe.command(bytes([ID_SWJ_CLOCK]) + u32(10_000_000))
        if wire == "cjtag":
            probe.command(bytes([ID_CJTAG_CONTROL, 1]))
        probe.begin()
        reply = probe.command(payload)
        counts = probe.end()
        if reply[1] != 0:
            failures.append(f"{wire} {name}: answered 0x{reply[1]:02x}, wanted DAP_OK")
        path = os.path.join(BASELINE_DIR, f"{wire}-{name}.trace")
        body = "\n".join(counts.trace) + "\n"
        if bless:
            open(path, "w").write(body)
        elif not os.path.exists(path):
            failures.append(f"{wire}-{name}.trace: no baseline recorded")
        elif open(path).read() != body:
            failures.append(f"{wire}-{name}.trace: wire events differ from the baseline")

    # DAP_Transfer, which is how a host reaches a single register and most of what an attach is
    # made of. src/dap_dispatch.c routes every command, and until this nothing but the block was
    # checked, so a mistake in the forwarding would have been invisible.
    for wire, port in (("swd", 1), ("jtag", 2), ("cjtag", 2)):
        for label, payload in (
            ("transfer-read", bytes([ID_TRANSFER, 0, 4]) + bytes([0x0F]) * 4),
            ("transfer-write", bytes([ID_TRANSFER, 0, 2]) + (bytes([0x0D]) + u32(0xA5A5_5A5A)) * 2),
            (
                "transfer-match",
                bytes([ID_TRANSFER, 0, 2])
                + bytes([0x20])
                + u32(0xFFFFFFFF)
                + bytes([0x0F | 0x10])
                + u32(0x00000000),
            ),
        ):
            probe = build(elf, wire, True, model)
            probe.command(bytes([ID_CONNECT, port]))
            probe.command(bytes([ID_SWJ_CLOCK]) + u32(10_000_000))
            probe.command(bytes([ID_TRANSFER_CONFIG, 0]) + u16(2) + u16(0))
            if wire == "swd":
                probe.command(bytes([ID_SWD_CONFIGURE, 0]))
            else:
                probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
            if wire == "cjtag":
                probe.command(bytes([ID_CJTAG_CONTROL, 1]))
                probe.cjtag_tap = JtagTap(JTAG_DR_OK, 35)
            probe.begin()
            reply = probe.command(payload)
            counts = probe.end()
            if reply[0] != ID_TRANSFER:
                failures.append(f"{wire} {label}: reply echoes 0x{reply[0]:02x}, wanted 0x05")
            path = os.path.join(BASELINE_DIR, f"{wire}-{label}.trace")
            body = "\n".join(counts.trace) + "\n"
            if bless:
                open(path, "w").write(body)
            elif not os.path.exists(path):
                failures.append(f"{wire}-{label}.trace: no baseline recorded")
            elif open(path).read() != body:
                failures.append(f"{wire}-{label}.trace: wire events differ from the baseline")

    known_path = os.path.join(BASELINE_DIR, "known-overshoot.txt")
    known = set()
    if os.path.exists(known_path):
        known = {line.strip() for line in open(known_path) if line.strip() and not line.startswith("#")}
    seen_overshoot = set()

    # Two clocks, because the paced and delay-free paths are different code and a baseline taken
    # at one of them says nothing about the other.
    for wire, (tag, hz) in itertools.product(CHECK_WIRES, (("10MHz", 10_000_000), ("50MHz", 50_000_000))):
        for label, read in (("read", True), ("write", False)):
            counts, reply, _ = measure(elf, wire, hz, 16, read, model)

            # The command layer has to come back with a well formed answer, not just the right
            # waveform: the echoed id, the word count asked for, an OK status, and for a read the
            # bytes the modelled target shifted out.
            want = 4 + 64 if read else 4
            got_words = reply[1] | (reply[2] << 8)
            if len(reply) != want:
                failures.append(f"{wire} {label}: reply is {len(reply)} bytes, wanted {want}")
            elif reply[0] != ID_TRANSFER_BLOCK:
                failures.append(f"{wire} {label}: reply echoes 0x{reply[0]:02x}, wanted 0x06")
            elif got_words != 16:
                failures.append(f"{wire} {label}: reply says {got_words} words, wanted 16")
            elif reply[3] != 1:
                failures.append(f"{wire} {label}: transfer status 0x{reply[3]:02x}, wanted OK")
            elif read and any(reply[4:68]):
                failures.append(f"{wire} {label}: data does not match what the target shifted out")

            name = f"{wire}-{label}-{tag}.trace"
            path = os.path.join(BASELINE_DIR, name)
            body = "\n".join(counts.trace) + "\n"
            if bless:
                os.makedirs(BASELINE_DIR, exist_ok=True)
                open(path, "w").write(body)
            elif not os.path.exists(path):
                failures.append(f"{name}: no baseline recorded")
            elif open(path).read() != body:
                failures.append(f"{name}: wire events differ from the baseline")

    # The count a block reply carries, at the sizes the traces above do not reach. It is derived
    # from the response pointer rather than counted, so one word and a full packet are the two
    # that would show an off-by-one, and both are outside every other case here.
    for wire, (label, read) in itertools.product(CHECK_WIRES, (("read", True), ("write", False))):
        for words in (1, 2, 3, 127):
            _, reply, _ = measure(elf, wire, 10_000_000, words, read, model)
            want = 4 + (4 * words) if read else 4
            got_words = reply[1] | (reply[2] << 8)
            if got_words != words:
                failures.append(f"{wire} {label}: a {words}-word block replied {got_words} words")
            elif len(reply) != want:
                failures.append(f"{wire} {label}, {words} words: reply is {len(reply)} bytes, wanted {want}")
            elif reply[3] != 1:
                failures.append(f"{wire} {label}, {words} words: status 0x{reply[3]:02x}, wanted OK")

            if wire == "cjtag" and counts.moves_while_high:
                failures.append(
                    f"{wire} {label}: {counts.moves_while_high} transitions while the clock was"
                    " high, which are escape edges to a 1149.7 target"
                )

    for wire in CHECK_WIRES:
        for mhz in CHECK_CLOCKS:
            counts, _, _ = measure(elf, wire, int(mhz * 1e6), 16, True, model)
            periods = [b - a for a, b in zip(counts.edge_cycles, counts.edge_cycles[1:])]
            if not periods:
                continue
            fastest = cpu_hz / min(periods)
            if fastest > mhz * 1e6:
                tag = f"{wire}@{mhz}MHz"
                seen_overshoot.add(tag)
                if tag not in known:
                    failures.append(
                        f"{tag}: fastest edge {fastest / 1e6:.2f} MHz is over the clock asked for"
                    )

    if bless:
        os.makedirs(BASELINE_DIR, exist_ok=True)
        with open(known_path, "w") as fh:
            fh.write("# Wires that clock faster than the host asked for. See OPTIMISATIONS.md:\n")
            fh.write("# it is a decision rather than a slip, and both ways out cost throughput.\n")
            for tag in sorted(seen_overshoot):
                fh.write(tag + "\n")
        print(f"baseline rewritten in {BASELINE_DIR}")
        return 0

    for tag in sorted(known - seen_overshoot):
        print(f"  note: {tag} no longer overshoots; drop it from known-overshoot.txt")
    if failures:
        for f in failures:
            print(f"  FAIL  {f}")
        return 1
    print(
        "  all clear: every transfer answered OK with the right data, wire events match,"
        f" the activation is {activation.tck_rise} clocks unchanged, no escape edges,"
        f" {len(known)} known overshoots unchanged"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", nargs="?", default="build/mculink_cjtag.elf")
    ap.add_argument("--wire", choices=["swd", "jtag", "cjtag"], default="swd")
    ap.add_argument("--words", type=int, default=16, help="words in the measured DAP_TransferBlock")
    ap.add_argument(
        "--clock", type=float, nargs="*", default=[1, 5, 10, 14, 25, 50], help="requested wire clocks, MHz"
    )
    ap.add_argument("--cpu", type=float, default=150.0, help="core clock, MHz")
    ap.add_argument(
        "--engine",
        action="store_true",
        help="time instructions with tools/m33.py, which models fetch and the bus, not per-class constants",
    )
    ap.add_argument(
        "--latency",
        type=int,
        help="with --engine, a SWD target readable this many cycles after the store that launched"
        " each bit (7 against the U575 on the bench), and every read's margin reported",
    )
    ap.add_argument("--store-cycles", type=int, default=1)
    ap.add_argument("--gpio-store-cycles", type=int, default=1)
    ap.add_argument("--load-cycles", type=int, default=2)
    ap.add_argument(
        "--fallthrough-cycles",
        type=int,
        default=0,
        help="a conditional branch that falls through; measured as free on this part",
    )
    ap.add_argument(
        "--branch-cycles",
        type=int,
        default=2,
        help="a taken branch on this core is one issue plus a short refill",
    )
    ap.add_argument(
        "--config",
        default="src/DAP_config.h",
        help="the DAP_config.h the image was built from, for its clock constants",
    )
    ap.add_argument(
        "--check",
        action="store_true",
        help="gate: wire events match the baseline, no escape edges, no new overclocking",
    )
    ap.add_argument(
        "--bless",
        action="store_true",
        help="with --check, rewrite the baseline from this build",
    )
    ap.add_argument(
        "--explain",
        action="store_true",
        help="name the code running in the longest stretch between two clock edges",
    )
    ap.add_argument(
        "--trace",
        metavar="DIR",
        help="write the wire event stream per scenario, to diff across a change",
    )
    ap.add_argument(
        "--transfer",
        action="store_true",
        help="measure DAP_Transfer rather than DAP_TransferBlock",
    )
    ap.add_argument(
        "--table",
        action="store_true",
        help="KiB/s for every rate a host can ask for, as markdown",
    )
    ap.add_argument(
        "--calibrate",
        action="store_true",
        help="fit cycles per bit against delay iterations to recover ARM's two clock constants",
    )
    args = ap.parse_args()

    model = Model(
        load=args.load_cycles,
        store=args.store_cycles,
        taken_branch=args.branch_cycles,
        not_taken_branch=args.fallthrough_cycles,
        gpio_store=args.gpio_store_cycles,
        engine=args.engine or args.latency is not None,
        latency=args.latency,
    )
    cpu_hz = int(args.cpu * 1e6)
    if args.check:
        print(f"{args.elf}")
        return check(args.elf, model, cpu_hz, args.bless)
    if args.table:
        return table(args.elf, model, cpu_hz, args.words)
    if args.calibrate:
        return calibrate(args.elf, args.wire, args.words, cpu_hz, model, args.config)
    shape = f"{args.words}-transfer DAP_Transfer" if args.transfer else f"{args.words}-word block"
    print(f"{args.elf}, {args.wire}, {shape}, core {args.cpu:g} MHz")
    if model.engine:
        print("cost model: tools/m33.py" + (f", target latency {model.latency} cycles" if model.latency else ""))
    else:
        print(
            f"cost model: load {model.load}, store {model.store}, GPIO store {model.gpio_store},"
            f" taken branch {model.taken_branch}, fall-through {model.not_taken_branch}, other {model.other}"
        )
    for mhz in args.clock:
        clock_hz = int(mhz * 1e6)
        print(f"\n{mhz:g} MHz requested")
        for label, read in (("read", True), ("write", False)):
            drive = measure_transfer if args.transfer else measure
            counts, _, probe = drive(
                args.elf, args.wire, clock_hz, args.words, read, model, explain=args.explain
            )
            report(label, counts, model, clock_hz, cpu_hz, args.words)
            if args.explain:
                explain_gap(probe)
            if args.trace:
                os.makedirs(args.trace, exist_ok=True)
                path = os.path.join(args.trace, f"{args.wire}-{label}-{mhz:g}MHz.trace")
                with open(path, "w") as fh:
                    fh.write("\n".join(counts.trace) + "\n")
                print(f"    wire events written to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
