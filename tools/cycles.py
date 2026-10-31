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
import concurrent.futures
import itertools
import os
import re
import subprocess
import sys
import tempfile
from functools import lru_cache
from dataclasses import dataclass, field, replace

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs, arm as ARM
from unicorn import (
    UC_ARCH_ARM,
    UC_HOOK_CODE,
    UC_HOOK_MEM_WRITE,
    UC_MODE_MCLASS,
    UC_MODE_THUMB,
    Uc,
    UcError,
)
from unicorn.arm_const import (
    UC_ARM_REG_LR,
    UC_ARM_REG_PC,
    UC_ARM_REG_R0,
    UC_ARM_REG_R1,
    UC_ARM_REG_R2,
    UC_ARM_REG_R3,
    UC_ARM_REG_SP,
)

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from m33 import Engine, Insn  # noqa: E402
from host_resources import available_cpus  # noqa: E402

# LPC55S69, non-secure aliases, which is what the image was built against. GPIO_BASE is the block the
# debug interface is driven through: the secure GPIO, with port 0's register layout (DBGIF_GPIO in
# src/board_pins.h). The ordinary GPIO, which only J8 and target sensing still use, is plain memory.
GPIO_BASE = 0x400A8000
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

# The ordinary GPIO, where CPU 0 reads the straps that tell the boards apart (src/board_straps.c).
ORDINARY_GPIO = 0x4008C000

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
PACKET_SIZE = 512  # DAP_PACKET_SIZE: the longest request USB delivers


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


@lru_cache(maxsize=16)
def symbols(elf: str) -> dict[str, int]:
    out = {}
    for line in sh("arm-none-eabi-nm", elf).splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
            out.setdefault(base_name(parts[2]), int(parts[0], 16))
    return out


@lru_cache(maxsize=16)
def sections(elf: str) -> dict[str, tuple[int, int, int]]:
    """name -> (size, vma, lma)"""
    out = {}
    for line in sh("arm-none-eabi-objdump", "-h", elf).splitlines():
        m = re.match(r"\s*\d+\s+(\S+)\s+([0-9a-f]+)\s+([0-9a-f]+)\s+([0-9a-f]+)", line)
        if m:
            out[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), int(m.group(4), 16))
    return out


@lru_cache(maxsize=16)
def load_address(elf: str) -> int:
    """Where `objcopy -O binary` output starts: the lowest load address of anything with bytes.
    CPU 0's image starts at 0 and CPU 1's at its own flash origin."""
    starts = []
    for line in sh("arm-none-eabi-readelf", "-lW", elf).splitlines():
        parts = line.split()
        if parts and parts[0] == "LOAD" and int(parts[4], 16) != 0:
            starts.append(int(parts[3], 16))
    return min(starts)


@lru_cache(maxsize=8)
def image_bytes(elf: str) -> bytes:
    """The load image is identical across every emulated machine for the same ELF."""
    with tempfile.NamedTemporaryFile(suffix=".bin") as raw:
        sh("arm-none-eabi-objcopy", "-O", "binary", elf, raw.name)
        with open(raw.name, "rb") as image_file:
            return image_file.read()


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
    escape_edges: int = 0  # of those, the EOT escapes OScan5 leaves Shift with
    trace: list[str] = field(default_factory=list)
    # The running cycle count at each rising clock edge. Differences are wire periods, which is
    # what an analyser measures and what Set_Clock_Delay is trying to control.
    edge_cycles: list[int] = field(default_factory=list)
    # The same at each falling edge, which with the rising ones gives both phases of a clock.
    fall_cycles: list[int] = field(default_factory=list)
    # Cycles from begin() to end(), as the timing engine counts them.
    span: int = 0
    # With a timed SWD target, every read and where it sat in its window.
    reads: list = field(default_factory=list)


def minimum_clock_phases(rises: list[int], falls: list[int]) -> tuple[int | None, int | None]:
    """Return the shortest low and high phase from ordered clock edges in one pass."""
    min_low = None
    last_fall = None
    fall_at = 0
    for rise in rises:
        while fall_at < len(falls) and falls[fall_at] < rise:
            last_fall = falls[fall_at]
            fall_at += 1
        if last_fall is not None:
            gap = rise - last_fall
            min_low = gap if min_low is None or gap < min_low else min_low

    min_high = None
    last_rise = None
    rise_at = 0
    for fall in falls:
        while rise_at < len(rises) and rises[rise_at] < fall:
            last_rise = rises[rise_at]
            rise_at += 1
        if last_rise is not None:
            gap = fall - last_rise
            min_high = gap if min_high is None or gap < min_high else min_high

    return min_low, min_high


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
    happened, which leaves the window open on that side. `after_next` says the edge that ends the
    window came before the read, so a load another master holds for more than `late` cycles reads
    the next bit; held before that edge, it holds the edge back with it.
    """

    cycle: int
    want: int
    early: int | None = None
    late: int | None = None
    got: int = 0
    wanted: int = 0
    after_next: bool = False

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
            r.after_next = True
        self.transfer_reads.append(r)
        self.reads.append(r)
        return on_wire if self.serve == "wire" else wanted

    def bit(self) -> int:
        raise RuntimeError("a timed target answers read(cycle); this machine has no timing engine")


class RecordingTap:
    """A TAP that shifts TDI in as well as a word out, and keeps what each update wrote.

    JtagTap shifts out only, which checks a read. A write needs the bits that went in, and on two
    pins every TDI bit has a packet of its own. Capture-DR loads `dr`; Capture-IR loads 0b01, the
    two low bits IEEE 1149.1 requires.
    """

    def __init__(self, dr: int, dr_bits: int, ir_bits: int = 4):
        self.state = "rti"
        self.dr, self.dr_bits, self.ir_bits = dr, dr_bits, ir_bits
        self.shift = 0
        self.updates: list[tuple[str, int]] = []

    def tdo(self) -> int:
        return self.shift & 1 if self.state in ("shdr", "shir") else 0

    def clock(self, tms: int, tdi: int = 1) -> None:
        if self.state == "shdr":
            self.shift = (self.shift >> 1) | (tdi << (self.dr_bits - 1))
        elif self.state == "shir":
            self.shift = (self.shift >> 1) | (tdi << (self.ir_bits - 1))
        self.state = TAP_NEXT[self.state][1 if tms else 0]
        if self.state == "capdr":
            self.shift = self.dr
        elif self.state == "capir":
            self.shift = 0b01
        elif self.state in ("upddr", "updir"):
            self.updates.append((self.state[3:], self.shift))


class IdcodeTap(RecordingTap):
    """A RecordingTap whose IDCODE instruction captures `idcode`, as a DAP's does."""

    def __init__(self, dr: int, dr_bits: int, idcode: int, ir_bits: int = 4):
        super().__init__(dr, dr_bits, ir_bits)
        self.idcode = idcode
        self.ir = 0

    def clock(self, tms: int, tdi: int = 1) -> None:
        super().clock(tms, tdi)
        if self.state == "updir":
            self.ir = self.shift & ((1 << self.ir_bits) - 1)
        elif (self.state == "capdr") and (self.ir == JTAG_IR_IDCODE):
            self.shift = self.idcode


class TimedCjtagTarget:
    """A two-pin target (IEEE 1149.7, OScan1, OScan2, OScan5 or OScan6) that answers by time.

    A JTAG clock is three TCKC packets; with `oscan2`, only in Shift-DR and Shift-IR, and one TMS
    packet everywhere else, with nothing driven back. With `oscan5`, two in Shift, nTDI and TDO,
    whose TMS is 1 only when TMSC moves twice while TCKC is high in the nTDI packet, an EOT escape
    (1149.7 Figure 25-11); each of those edges, and the fall after them, has to come `escape_gap`
    cycles or more after the one before. With `oscan6`, OScan2's frames outside Shift and OScan5's
    in it, the first after entering Shift with a PAD packet between nTDI and TDO. The target reads nTDI and TMS on the rising edges of the
    first two (the activation sets SEDGE=1), and drives TDO in the third: on the probe's pin from
    `latency` cycles after the falling edge that starts that packet, until `hold` cycles after the
    falling edge that ends it (CJTAGGEN.md, measured on a CC1312R1). A read outside that sees
    what the released wire held, which is the TMS level the probe drove last.

    It also records what hardware punishes and the order-only model cannot see: a probe still
    driving `release_by` cycles into a TDO packet, a TMSC move while TCKC is high or in the same
    store as a TCKC edge, a packet the probe should drive but has let go, and any entry into
    Test-Logic-Reset, which drops a CC13xx out of OScan1.
    """

    def __init__(self, tap: "RecordingTap", latency: int = 8, hold: int = 7, release_by: int = 3,
                 oscan2: bool = False, oscan5: bool = False, escape_gap: int = 0, oscan6: bool = False):
        self.tap = tap
        self.latency, self.hold, self.release_by = latency, hold, release_by
        self.oscan2 = oscan2 or oscan6  # one TMS packet outside Shift
        self.oscan5, self.escape_gap = oscan5 or oscan6, escape_gap  # two packets in it
        self.oscan6 = oscan6
        self.prev_shift = False  # the last frame was in a Shift state
        self.pad = False  # the frame under way has OScan6's PAD packet, still to come
        self.shift5 = False  # the frame under way is OScan5's two packets in Shift
        self.rose_at = 0
        self.escape: list[int] = []
        self.escape_edges = 0  # every TMSC move with TCKC high that was part of an EOT escape
        self.packet = 0  # the packet whose cycle this is: 0 nTDI, 1 TMS, 2 TDO
        self.single = False  # the frame under way is OScan2's one TMS packet
        self.tdi = 1
        self.wanted = 0
        self.tdo_fall: int | None = None
        self.drive_from: int | None = None
        self.drive_until: int | None = None
        self.held = 1
        self.driving = True
        self.released_at: int | None = None
        self.driven_at: int | None = None
        self.frames = 0
        self.reads: list[Read] = []
        self._open: list[Read] = []
        self.problems: list[str] = []

    def restart(self) -> None:
        pass

    def wire(self, cycle: int, driven: str, clock_high: bool, with_edge: bool) -> None:
        if clock_high and self.shift5 and self.packet == 0 and driven != "z":
            self.escape.append(cycle)
            self.escape_edges += 1
        elif clock_high:
            self.problems.append(f"TMSC moved to {driven} at {cycle} with TCKC high: an escape edge")
        elif with_edge:
            self.problems.append(f"TMSC moved to {driven} at {cycle} in the store of a TCKC edge")
        if driven == "z":
            self.driving, self.released_at = False, cycle
        else:
            if not self.driving:
                self.driven_at = cycle
            self.driving = True
            self.held = int(driven)

    def rise(self, cycle: int, driven: str) -> None:
        if self.packet == 2:
            return
        if driven == "z":
            self.problems.append(f"packet {self.packet + 1} rose at {cycle} with TMSC released")
            return
        self.single = self.packet == 0 and self.oscan2 and self.tap.state not in ("shdr", "shir")
        if self.packet == 0:
            shift = self.tap.state in ("shdr", "shir")
            self.shift5 = self.oscan5 and shift
            self.pad = self.oscan6 and shift and not self.prev_shift
            self.prev_shift = shift
            self.rose_at, self.escape = cycle, []
        elif self.packet == 1 and self.pad:
            return  # OScan6's PAD packet carries nothing
        if self.packet == 0 and not self.single:
            self.tdi = 1 - int(driven)
            return
        self.wanted = self.tap.tdo()
        self.tap.clock(int(driven), 1 if self.single else self.tdi)
        self.frames += 1
        if self.tap.state == "tlr":
            self.problems.append(f"frame {self.frames} entered Test-Logic-Reset, which drops OScan1")

    def fall(self, cycle: int) -> None:
        if self.single:
            # OScan2's one-packet frame is over; the next packet starts a frame.
            self.single = False
            return
        if self.shift5 and self.packet == 0:
            # OScan5's nTDI packet in Shift is over, and with it the frame's TMS. The TDO packet
            # follows at once, as the third packet does in OScan1.
            self.shift5 = False
            if len(self.escape) not in (0, 2):
                self.problems.append(f"frame {self.frames + 1}: {len(self.escape)} TMSC edges with TCKC high, "
                                     "where an EOT escape is 2")
            marks = [self.rose_at, *self.escape, cycle]
            if self.escape and min(b - a for a, b in zip(marks, marks[1:])) < self.escape_gap:
                self.problems.append(f"frame {self.frames + 1}: EOT escape edges {marks} closer than "
                                     f"{self.escape_gap} cycles")
            self.wanted = self.tap.tdo()
            self.tap.clock(1 if self.escape else 0, self.tdi)
            self.frames += 1
            if self.tap.state == "tlr":
                self.problems.append(f"frame {self.frames} entered Test-Logic-Reset, which drops OScan1")
            self.packet = 1
            if self.pad:
                return  # the PAD packet comes before TDO's
        if self.packet == 1:
            self.pad = False
            self.tdo_fall = cycle
            self.drive_from, self.drive_until = cycle + self.latency, None
        elif self.packet == 2:
            self.drive_until = cycle + self.hold
            for read in self._open:
                read.late = self.drive_until - read.cycle
            self._open = []
            start = self.tdo_fall + self.release_by
            let_go = self.released_at is not None and self.tdo_fall <= self.released_at <= start
            back = self.driving or (self.driven_at is not None and self.driven_at > self.released_at)
            if not let_go or back:
                self.problems.append(f"frame {self.frames}: TMSC driven by the probe during the TDO packet")
        self.packet = (self.packet + 1) % 3

    def read(self, cycle: int) -> int:
        if self.drive_from is None:
            return self.held
        late = None if self.drive_until is None else self.drive_until - cycle
        on_wire = self.held if cycle < self.drive_from else self.wanted
        r = Read(cycle, self.frames - 1, cycle - self.drive_from, late, got=on_wire, wanted=self.wanted,
                 after_next=late is not None)
        if late is None:
            self._open.append(r)
        self.reads.append(r)
        return on_wire

    def bit(self) -> int:
        raise RuntimeError("a timed target answers read(cycle); this machine has no timing engine")


class TimedJtagTarget:
    """A four-wire JTAG target that answers by time.

    It reads TMS and TDI on each rising TCK edge and launches TDO on each falling one. The probe
    sees a bit from `latency` cycles after the falling-edge store that launched it until `hold`
    cycles after the next one (a CC1312R1 in four-pin mode: 8, and 6); in between the two it reads
    the old bit, and the read's late margin says so.
    """

    def __init__(self, tap: "RecordingTap", latency: int = 8, hold: int = 6):
        self.tap = tap
        self.latency, self.hold = latency, hold
        # (falling-edge store cycle, bit). Before the first clock TDO idles high, as a target that
        # is not shifting leaves it.
        self.launches: list[tuple[int, int]] = [(-(10**9), 1)]
        self.reads: list[Read] = []
        self.problems: list[str] = []
        self._open: list[tuple[Read, int]] = []  # reads whose bit's window has not closed yet

    def restart(self) -> None:
        pass

    def elapse(self, cycles: int) -> None:
        """Moves everything recorded `cycles` into the past, for a cycle count that starts again:
        what one command left on TDO was launched before the next began."""
        self.launches = [(at - cycles, bit) for at, bit in self.launches]
        for read, _ in self._open:
            read.cycle -= cycles

    def fall(self, cycle: int) -> None:
        last = len(self.launches) - 1
        for read, k in self._open:
            if k == last:
                read.late = cycle + self.hold - read.cycle
        self._open = [(r, k) for r, k in self._open if k != last]
        self.launches.append((cycle, self.tap.tdo()))

    def rise(self, tms: int, tdi: int) -> None:
        self.tap.clock(tms, tdi)

    def read(self, cycle: int) -> int:
        k = len(self.launches) - 1
        while k > 0 and self.launches[k][0] + self.latency > cycle:
            k -= 1
        at, bit = self.launches[k]
        late = None
        if k + 1 < len(self.launches):
            late = self.launches[k + 1][0] + self.hold - cycle
        r = Read(cycle, k, cycle - (at + self.latency), late, got=bit, wanted=bit, after_next=late is not None)
        if late is None:
            self._open.append((r, k))
        self.reads.append(r)
        return bit

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
    nzcv = flags >> 28
    n, z, c, v = nzcv & 8, nzcv & 4, nzcv & 2, nzcv & 1
    if cc == ARM.ARM_CC_EQ:
        return z != 0
    if cc == ARM.ARM_CC_NE:
        return z == 0
    if cc == ARM.ARM_CC_HS:
        return c != 0
    if cc == ARM.ARM_CC_LO:
        return c == 0
    if cc == ARM.ARM_CC_MI:
        return n != 0
    if cc == ARM.ARM_CC_PL:
        return n == 0
    if cc == ARM.ARM_CC_VS:
        return v != 0
    if cc == ARM.ARM_CC_VC:
        return v == 0
    if cc == ARM.ARM_CC_HI:
        return c != 0 and z == 0
    if cc == ARM.ARM_CC_LS:
        return c == 0 or z != 0
    if cc == ARM.ARM_CC_GE:
        return n == (v << 3)
    if cc == ARM.ARM_CC_LT:
        return n != (v << 3)
    if cc == ARM.ARM_CC_GT:
        return z == 0 and n == (v << 3)
    if cc == ARM.ARM_CC_LE:
        return z != 0 or n != (v << 3)
    return True


def data_accesses(insn) -> tuple[int, str, bool] | None:
    """How many data accesses a capstone instruction makes, the register its address is based on,
    and whether they are writes; None when it touches no memory. pc as the base means the
    instruction's own region."""
    from capstone.arm import ARM_OP_MEM, ARM_OP_REG

    m = insn.mnemonic.split(".")[0]
    write = m.startswith(("push", "vpush", "stm", "vstm", "str", "vstr"))
    if m.startswith(("pop", "vpop", "push", "vpush")):
        words = sum(2 if insn.reg_name(op.reg).startswith("d") else 1 for op in insn.operands if op.type == ARM_OP_REG)
        return words, "sp", write
    if m.startswith(("ldm", "vldm", "stm", "vstm")):
        regs = [op for op in insn.operands[1:] if op.type == ARM_OP_REG]
        words = sum(2 if insn.reg_name(op.reg).startswith("d") else 1 for op in regs)
        return words, insn.reg_name(insn.operands[0].reg), write
    if m.startswith(("ldr", "tbb", "tbh", "vldr", "str", "vstr")):
        mem = next((op for op in insn.operands if op.type == ARM_OP_MEM), None)
        if mem is None:
            return None
        double = m.startswith(("ldrd", "strd")) or (
            m.startswith(("vldr", "vstr")) and insn.reg_name(insn.operands[0].reg).startswith("d")
        )
        return 2 if double else 1, insn.reg_name(mem.mem.base), write
    return None


def branch_kind(mnemonic: str, writes_pc: bool) -> str:
    base = mnemonic.split(".")[0]
    if mnemonic in UNCONDITIONAL:
        return "uncond"
    if base.startswith(CONDITIONAL_PREFIX) or (base[:1] == "b" and base[1:] in CONDITIONS):
        return "cond"
    if writes_pc or base in ("bx", "blx", "tbb", "tbh"):
        return "indirect"
    return ""


_INSTRUCTION_DECODER = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
_INSTRUCTION_DECODER.detail = True


@lru_cache(maxsize=131_072)
def decode_instruction(address: int, code: bytes) -> tuple[Insn, tuple[int, str | None] | None,
                                                          tuple[int, str, bool] | None]:
    """Decode immutable instruction bytes once per worker, across emulator instances."""
    insn = next(_INSTRUCTION_DECODER.disasm(code, address), None)
    if insn is None:
        return Insn(address, len(code), "?"), None, None

    read, written = insn.regs_access()
    reads = frozenset(insn.reg_name(r) for r in read)
    writes = frozenset(insn.reg_name(r) for r in written)
    data = frozenset()
    if insn.mnemonic.startswith(("str", "push", "stm")) and insn.operands:
        from capstone.arm import ARM_OP_REG

        data = frozenset(insn.reg_name(op.reg) for op in insn.operands if op.type == ARM_OP_REG)
    branch = branch_kind(insn.mnemonic, "pc" in writes)
    branch_target = None
    if branch == "cond":
        target = insn.operands[1] if insn.mnemonic.startswith(("cbz", "cbnz")) else insn.operands[0]
        if target.type == ARM.ARM_OP_IMM:
            branch_target = target.imm
    hit = Insn(address, insn.size, insn.mnemonic, reads, writes,
               branch=branch, branch_target=branch_target, store_data=data)
    branch_test = None
    if hit.branch == "cond":
        reg = insn.reg_name(insn.operands[0].reg) if insn.mnemonic.startswith("cb") else None
        branch_test = (insn.cc, reg)
    return hit, branch_test, data_accesses(insn)


# Cycles between two counted commands, which the count does not see. The probe spends about 15 us
# between commands (BENCH.md), over 2,000 cycles; any gap past the longest latency and hold leaves
# the same windows.
BETWEEN_COMMANDS = 2_000


class AllocationAttempt(RuntimeError):
    pass


class Machine:
    def __init__(
        self,
        elf: str,
        target: Target,
        restart_at: set[int],
        model: "Model | None" = None,
        engine: "Engine | None" = None,
        timed: bool = True,
        record_trace: bool = True,
        record_stats: bool = True,
    ):
        self.sym = symbols(elf)
        # False runs every instruction untimed: the wire's events and levels without their cycles,
        # which is all tools/equiv.py compares, at native speed.
        self.timed = timed
        self.record_trace = record_trace
        self.record_stats = record_stats
        self.model = model or Model()
        # When set, issue and bus cycles come from tools/m33.py instead of the per-class model.
        if engine is None and self.model.engine:
            engine = Engine()
        self.engine = engine
        self._insns: dict[int, Insn] = {}
        # How each branch decides, by address: a condition code, or the register cbz/cbnz tests.
        self._branch_test: dict[int, tuple[int, str | None]] = {}
        # Each load's or store's data accesses, by address: how many, the register whose value
        # says which bus they take, and whether they write. See _code for why these are not
        # counted from a memory hook.
        self._accesses: dict[int, tuple[int, int | None, bool]] = {}
        self._branch_taken = False
        self._branch_target: int | None = None
        self.cycle = 0
        self.target = target
        self.counting = False
        self.restart_at = restart_at
        self.resync_on_release = False
        self.tap: "JtagTap | None" = None
        # The same TAP reached through OScan1 framing instead of four wires, or OScan2's.
        self.cjtag_tap: "JtagTap | None" = None
        self.cjtag_oscan2 = False
        self.cjtag_oscan5 = False
        self.cjtag_oscan6 = False
        self._prev_shift = False  # the last frame was in a Shift state
        self._pad = False  # OScan6's PAD packet comes next
        self._shift5 = False  # OScan5's two-packet frame in Shift is under way
        self._escape = 0  # TMSC moves while TCKC is high in that frame's nTDI packet
        self._packet = 0
        self._frame_tms = 0
        # The TDO the target holds for the whole of a frame's third packet, rising edge included.
        self._frame_tdo = 0
        # Four-wire TDO from one falling edge to the next: a sample after the rising edge reads the
        # bit the fall launched, as on the wire, not the one the rising edge shifted in behind it.
        self._tdo_latch = 0
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

        self.uc.mem_write(load_address(elf), image_bytes(elf))
        # The startup code is not run, so do what it does for .data.
        secs = sections(elf)
        size, vma, lma = secs[".data"]
        if size:
            self.uc.mem_write(vma, bytes(self.uc.mem_read(lma, size)))
        # A write hook invalidates only instructions the generator overwrites in SRAMX. There is no
        # read hook: Unicorn 2.1.4 mis-executes loads in IT blocks when memory reads are hooked.
        self.uc.hook_add(
            UC_HOOK_MEM_WRITE,
            self._sramx_write,
            begin=SRAMX[0],
            end=SRAMX[0] + SRAMX[1] - 1,
        )
        self.allocator_entries = {}
        self._allocator_sources = {}
        self.install_allocator_traps(self.sym, elf)
        # The code hook runs once per instruction, so leave it off while generated setup code runs.
        # emu_start's `until` stops a call at RETURN_MAGIC without it.
        self._hook: int | None = None
        if restart_at:
            self._hook_code(True)

    def install_allocator_traps(self, linked_symbols: dict[str, int], elf: str) -> None:
        if os.environ.get("MCULINK_CHECK_ALLOCATIONS") != "1":
            return
        allocators = {"malloc", "calloc", "realloc", "free", "_malloc_r", "_calloc_r",
                      "_realloc_r", "_free_r", "_sbrk", "_sbrk_r"}
        def allocation(uc, address, size, user):
            raise AllocationAttempt(f"{self._allocator_sources[address]}: allocator entry "
                                    f"{self.allocator_entries[address]} at {address:#x}")
        for name, address in linked_symbols.items():
            if name.split(".")[0] in allocators and address not in self.allocator_entries:
                self.allocator_entries[address] = name
                self._allocator_sources[address] = elf
                self.uc.hook_add(UC_HOOK_CODE, allocation, begin=address, end=address)

    def _hook_code(self, on: bool) -> None:
        if on == (self._hook is not None) or (not on and self.restart_at):
            return
        if on:
            callback = self._code
            if self.engine is not None and not self.record_stats and not self.explain and not self.restart_at:
                callback = self._timing_code
            self._hook = self.uc.hook_add(UC_HOOK_CODE, callback)
        else:
            self.uc.hook_del(self._hook)
            self._hook = None
        # A code hook is compiled into each translated block, so blocks translated before the
        # change keep the old one.
        self.uc.ctl_flush_tb()

    def _sramx_write(self, uc, access, address: int, size: int, value: int, user) -> None:
        """Discard cached instructions whose bytes overlap a generated-code write."""
        first = max(SRAMX[0], (address - 3) & ~1)
        last = min(SRAMX[0] + SRAMX[1] - 2, (address + size - 1) & ~1)
        for instruction in range(first, last + 1, 2):
            self._insns.pop(instruction, None)
            self._decoded.pop(instruction, None)
            self._branch_test.pop(instruction, None)
            self._accesses.pop(instruction, None)

    @staticmethod
    @lru_cache(maxsize=32)
    def _reg_id(name: str) -> int:
        from unicorn import arm_const

        return getattr(arm_const, f"UC_ARM_REG_{name.upper()}")

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
        halfword = int.from_bytes(self.uc.mem_read(address, 2), "little")
        # Thumb-2 32-bit instructions start with one of the 11101, 11110 or 11111 prefixes.
        size = 4 if (halfword & 0xF800) >= 0xE800 else 2
        return self._insn(address, size)

    def _insn(self, address: int, size: int) -> Insn:
        hit = self._insns.get(address)
        if hit is None:
            code = bytes(self.uc.mem_read(address, size))
            hit, branch_test, accesses = decode_instruction(address, code)
            if branch_test is not None:
                self._branch_test[address] = branch_test
            if accesses is not None:
                count, base, write = accesses
                base_id = None if base == "pc" else self._reg_id(base)
                self._accesses[address] = (count, base_id, write)
            self._insns[address] = hit
        return hit

    # -- GPIO ----------------------------------------------------------------
    def _gpio_read(self, uc, offset, size, user):
        if self.engine is not None and self.counting:
            self.cycle = self.engine.access(GPIO_BASE + offset, False)
        # B[0][n] is one byte per pin at the start of the block: SWDIO is pin 2, TDO pin 3.
        if offset in (REG_B0 + PIN_SWDIO, REG_B0 + PIN_TDO):
            if self.counting and self.record_trace:
                self.counts.trace.append("S" if offset == REG_B0 + PIN_SWDIO else "T")
            if isinstance(self.target, TimedJtagTarget) and offset == REG_B0 + PIN_TDO:
                return self.target.read(self.cycle)
            if self.tap is not None and offset == REG_B0 + PIN_TDO:
                return self._tdo_latch
            if isinstance(self.target, TimedCjtagTarget) and offset == REG_B0 + PIN_SWDIO:
                driven = self._driven()
                return int(driven) if driven != "z" else self.target.read(self.cycle)
            if self.cjtag_tap is not None and offset == REG_B0 + PIN_SWDIO:
                return self._frame_tdo
            if isinstance(self.target, TimedSwdTarget):
                # While the probe drives the wire, its pin reads back what it drives.
                driven = self._driven()
                if driven != "z":
                    return int(driven)
                return self.target.read(self.cycle)
            return self.target.bit()
        if REG_B0 <= offset < REG_B0 + 32:
            # Any other pin reads its level, which for an output is the level it drives.
            return (self.level >> (offset - REG_B0)) & 1
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
        if isinstance(self.target, TimedCjtagTarget):
            if after_wire != before_wire:
                self.target.wire(
                    self.cycle, after_wire, bool(before_clock and after_clock), after_clock != before_clock
                )
            if after_clock and not before_clock:
                self.target.rise(self.cycle, after_wire)
            elif before_clock and not after_clock:
                self.target.fall(self.cycle)
        if isinstance(self.target, TimedJtagTarget):
            if after_clock and not before_clock:
                self.target.rise((self.level >> PIN_SWDIO) & 1, after_tdi)
            elif before_clock and not after_clock:
                self.target.fall(self.cycle)

        if not self.counting:
            return
        if self.record_stats:
            self.counts.gpio_store += 1

        # Level changes are recorded before the clock edge in the same store, so that fusing a
        # data write into the falling edge produces the same stream as doing them separately.
        if after_tdi != before_tdi:
            if self.record_trace:
                self.counts.trace.append(f"I{after_tdi}")
        if after_wire != before_wire:
            if self.record_trace:
                self.counts.trace.append(f"W{after_wire}")
            if after_wire == "z":
                self.counts.swdio_release += 1
                # A frame releases the wire exactly once, immediately before its third packet.
                # That is a marker on the wire, so the framing stays in step with the probe
                # whatever the optimiser does to the code that produces it.
                self._packet = 2
            if before_clock and after_clock:
                self.counts.moves_while_high += 1
                if self._shift5 and self._packet == 1:
                    self._escape += 1
                    self.counts.escape_edges += 1
        if after_clock and not before_clock:
            if self.record_stats:
                self.counts.tck_rise += 1
            self.counts.edge_cycles.append(self.cycle)
            if self.explain:
                span = self.cycle - self._window_start
                if span > self.longest[0]:
                    self.longest = (span, self._window)
                self.windows.append((span, self._window))
                self._window = []
                self._window_start = self.cycle
            if self.record_trace:
                self.counts.trace.append(f"^{after_wire}{after_tdi}")
            if self.tap is not None:
                self.tap.clock((self.level >> PIN_SWDIO) & 1)
            if self.cjtag_tap is not None:
                # Packet 1 carries nTDI, packet 2 carries TMS, packet 3 is the target's: it drives
                # the frame's TDO, the bit the TMS packet's clock shifted out, through the whole
                # packet, so a sample after its rising edge reads the same bit as one before.
                # OScan2 outside Shift sends TMS alone, and the target drives nothing.
                shift = self.cjtag_tap.state in ("shdr", "shir")
                single = (self.cjtag_oscan2 or self.cjtag_oscan6) and self._packet == 0 and not shift
                pad = self._pad and self._packet == 1
                if self._packet == 0:
                    self._shift5 = (self.cjtag_oscan5 or self.cjtag_oscan6) and shift
                    self._pad = self.cjtag_oscan6 and shift and not self._prev_shift
                    self._prev_shift = shift
                    self._escape = 0
                if pad:
                    self._pad = False  # OScan6's PAD packet carries nothing
                elif single:
                    self._frame_tdo = 1
                    self.cjtag_tap.clock((self.level >> PIN_SWDIO) & 1)
                    self._packet = 2
                elif self._packet == 1:
                    self._frame_tms = (self.level >> PIN_SWDIO) & 1
                    self._frame_tdo = self.cjtag_tap.tdo()
                    self.cjtag_tap.clock(self._frame_tms)
                self._packet = (self._packet + 1) % 3
        elif before_clock and not after_clock:
            if self.tap is not None:
                self._tdo_latch = self.tap.tdo()
            if self.cjtag_tap is not None and self._shift5 and self._packet == 1:
                # The end of OScan5's nTDI packet in Shift, and so of its TMS: 1 only by an EOT
                # escape. The TDO packet that follows drives what this clock shifted out.
                self._shift5 = False
                self._frame_tdo = self.cjtag_tap.tdo()
                self.cjtag_tap.clock(1 if self._escape >= 2 else 0)
            if self.record_stats:
                self.counts.tck_fall += 1
            self.counts.fall_cycles.append(self.cycle)
            if self.record_trace:
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

    def _timing_code(self, uc, address, size, user):
        """Code hook for counted engine runs without optional bookkeeping or reset addresses."""
        taken = self._prev_end is not None and (
            address == self._branch_target
            if self._branch_target is not None and self._branch_target != self._prev_end
            else self._branch_taken
        )
        if self._prev_end is not None and not taken and address != self._prev_end:
            # Unicorn skips IT-block instructions; the timing engine still issues them.
            skipped = self._prev_end
            while skipped < address and skipped - self._prev_end < 8:
                insn = self._insn_at(skipped)
                self.engine.issue(insn, None)
                skipped += insn.size
        insn = self._insn(address, size)
        self.cycle = self.engine.issue(insn, address if taken else None)
        spec = self._accesses.get(address)
        if spec is not None:
            count, base, write = spec
            where = address if base is None else self.uc.reg_read(base)
            if not (GPIO_BASE <= where < GPIO_BASE + GPIO_SIZE or where >= 0xE0000000):
                for _ in range(count):
                    self.engine.access(where, write)
        if insn.branch == "cond":
            self._branch_target = insn.branch_target
            self._branch_taken = (
                self._decide(insn)
                if insn.branch_target is None or insn.branch_target == address + size
                else False
            )
        else:
            self._branch_target = None
            self._branch_taken = bool(insn.branch)
        self._prev_end = address + size

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
            taken = self._prev_end is not None and (
                address == self._branch_target
                if self._branch_target is not None and self._branch_target != self._prev_end
                else self._branch_taken
            )
            if self._prev_end is not None and not taken and address != self._prev_end:
                skipped = self._prev_end
                while skipped < address and skipped - self._prev_end < 8:
                    insn = self._insn_at(skipped)
                    self.engine.issue(insn, None)
                    skipped += insn.size
            insn = self._insn(address, size)
            self.cycle = self.engine.issue(insn, address if taken else None)
            # A load's or store's accesses, from the address its base register gives, which is
            # the bus they take. GPIO and the PPB report their own, from their callbacks.
            spec = self._accesses.get(address)
            if spec is not None:
                count, base, write = spec
                where = address if base is None else self.uc.reg_read(base)
                if not (GPIO_BASE <= where < GPIO_BASE + GPIO_SIZE or where >= 0xE0000000):
                    for _ in range(count):
                        self.engine.access(where, write)
            if insn.branch == "cond":
                self._branch_target = insn.branch_target
                self._branch_taken = (
                    self._decide(insn)
                    if insn.branch_target is None or insn.branch_target == address + size
                    else False
                )
            else:
                self._branch_target = None
                self._branch_taken = bool(insn.branch)
            if self.record_stats and taken:
                self.counts.taken_branch += 1
            self._prev_end = address + size
            if self.explain:
                self._window.append(address)
            if self.record_stats:
                self.counts.instructions += 1
            return

        if self._prev_was_branch and self._prev_end is not None and address == self._prev_end:
            # The branch before this one fell through. It was charged as an ordinary instruction
            # when it issued, because whether it was taken is only visible from where we land.
            if self.record_stats:
                self.counts.not_taken_branch += 1
            self.cycle += self.model.not_taken_branch - self.model.other

        if self._prev_end is not None and address != self._prev_end:
            if self.record_stats:
                self.counts.taken_branch += 1
            # The pacing loop is two instructions, so a backward branch this short is one of its
            # iterations. Counting them is what lets the delay be separated from everything else
            # without parsing the clock arithmetic out of the source.
            if 0 < self._prev_end - address <= 8:
                if self.record_stats:
                    self.counts.delay_iterations += 1
            self.cycle += self.model.taken_branch - self.model.other
        self._prev_end = address + size

        if self.explain:
            self._window.append(address)
        kind, _ = self._classify(address, size)
        self._prev_was_branch = kind == "branch"
        c = self.counts
        if self.record_stats:
            c.instructions += 1
        m = self.model
        if kind == "load":
            if self.record_stats:
                c.load += 1
            self.cycle += m.load
        elif kind == "store":
            if self.record_stats:
                c.store += 1
            self.cycle += m.store
        else:
            if self.record_stats:
                c.other += 1
            self.cycle += m.other

    # -- running -------------------------------------------------------------
    def call(self, name: str, *args: int, timeout_insns: int = 50_000_000) -> int:
        addr = self.sym[name]
        self.uc.reg_write(UC_ARM_REG_SP, STACK_TOP)
        self.uc.reg_write(UC_ARM_REG_LR, RETURN_MAGIC | 1)
        for i, value in enumerate(args):
            self.uc.reg_write([UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3][i], value)
        self._prev_end = None
        try:
            self.uc.emu_start(addr | 1, RETURN_MAGIC, count=timeout_insns)
        except UcError as exc:
            pc = self.uc.reg_read(UC_ARM_REG_PC)
            raise SystemExit(f"emulation fault in {name} at 0x{pc:08x}: {exc}") from exc
        return self.uc.reg_read(UC_ARM_REG_R0)

    def command(self, request: bytes) -> bytes:
        request_at, response_at = SCRATCH, SCRATCH + 0x400
        if len(request) > PACKET_SIZE:
            raise SystemExit(f"a {len(request)}-byte request is longer than a {PACKET_SIZE}-byte packet")
        self.uc.mem_write(request_at, request + bytes(PACKET_SIZE - len(request)))
        self.uc.mem_write(response_at, bytes(600))
        # Run the same sized entry point as USB, including its received length and transport size.
        if "DAP_ExecuteCommandSized" in self.sym:
            entry = "DAP_ExecuteCommandSized"
            produced = self.call(entry, request_at, len(request), PACKET_SIZE, response_at) & 0xFFFF
        else:
            entry = "DAP_ExecuteCommand" if "DAP_ExecuteCommand" in self.sym else "DAP_ProcessCommand"
            produced = self.call(entry, request_at, response_at) & 0xFFFF
        if produced > 0x400:
            # A reply that long means the call did not finish: the usual cause is a pacing loop
            # counting down from zero, which takes about a minute of emulated time and leaves a
            # register holding whatever it held.
            raise SystemExit(
                f"{entry} returned a {produced}-byte reply to {request[:8].hex()}, which means it never returned"
            )
        return bytes(self.uc.mem_read(response_at, max(produced, 1)))

    def begin(self):
        if isinstance(self.target, TimedJtagTarget):
            self.target.elapse(self.cycle + BETWEEN_COMMANDS)
        self.counts = Counts()
        self.cycle = 0
        self.counting = True
        self._hook_code(self.timed)
        if self.engine is not None:
            self.engine.reset()

    def end(self) -> Counts:
        self.counting = False
        self._hook_code(False)
        if self.engine is not None:
            self.counts.span = self.cycle
        if isinstance(self.target, TimedSwdTarget):
            self.counts.reads = list(self.target.reads)
            self.target.reads = []
        return self.counts


def u16(v: int) -> bytes:
    return v.to_bytes(2, "little")


# The two-pin wires: OScan1, which the probe frames from vendor 0x83 mode 2, and OScan2, 5 and 6,
# which it frames from mode 3. Either way the TAP starts in Run-Test/Idle, as a host leaves it after
# driving the activation itself.
TWO_PIN = ("cjtag", "cjtag2", "cjtag5", "cjtag6")


def cjtag_enter(wire: str) -> bytes:
    if wire == "cjtag2":
        return bytes([ID_CJTAG_CONTROL, 3, 10, 1])
    if wire == "cjtag5":
        return bytes([ID_CJTAG_CONTROL, 3, 13, 1])
    if wire == "cjtag6":
        return bytes([ID_CJTAG_CONTROL, 3, 14, 1])
    return bytes([ID_CJTAG_CONTROL, 2])


def u32(v: int) -> bytes:
    return v.to_bytes(4, "little")


# DAP command ids used below.
ID_CONNECT, ID_TRANSFER_CONFIG, ID_TRANSFER, ID_TRANSFER_BLOCK = 0x02, 0x04, 0x05, 0x06
ID_SWJ_CLOCK, ID_SWJ_SEQUENCE, ID_SWD_CONFIGURE = 0x11, 0x12, 0x13
ID_JTAG_SEQUENCE, ID_JTAG_CONFIGURE, ID_SWD_SEQUENCE = 0x14, 0x15, 0x1D
ID_CJTAG_CONTROL = 0x83

AP_DRW_READ, AP_DRW_WRITE = 0x0F, 0x0D


def build(elf: str, wire: str, read: bool, model: Model, record_stats: bool = True) -> Machine:
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
    probe = Machine(elf, target, restart_at=set(), model=model, record_stats=record_stats)
    if wire == "swd":
        probe.resync_on_release = True
    elif wire == "jtag" and model.engine:
        # A generated path samples a short clock's TDO after the next falling edge, which only a
        # target that answers by time can serve.
        probe.target = TimedJtagTarget(RecordingTap(JTAG_DR_OK, 35))
    elif wire == "jtag":
        probe.tap = JtagTap(JTAG_DR_OK, 35)
    else:
        probe.cjtag_tap = JtagTap(JTAG_DR_OK, 35)
        probe.cjtag_oscan2 = wire == "cjtag2"
        probe.cjtag_oscan5 = wire == "cjtag5"
        probe.cjtag_oscan6 = wire == "cjtag6"
    return probe


def measure(
    elf: str,
    wire: str,
    clock_hz: int,
    words: int,
    read: bool,
    model: Model,
    explain: bool = False,
    record_stats: bool = True,
) -> tuple[Counts, bytes, "Machine"]:
    probe = build(elf, wire, read, model, record_stats)
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
    if wire in TWO_PIN:
        probe.command(cjtag_enter(wire))

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
    if wire in TWO_PIN:
        probe.command(cjtag_enter(wire))

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


DPIDR_MSPM0L1306 = 0x6BA02477
ID_VENDOR_SWD_GEN = 0x89


def timed_dpidr_target(elf: str, model: Model, latency: int, hz: int) -> "Machine":
    """A probe connected over SWD at `hz` to a target that answers DPIDR reads `latency` cycles
    after each edge, and answers with what the timing puts on the pin."""
    word = DPIDR_MSPM0L1306
    pattern = SWD_OK + [(word >> i) & 1 for i in range(32)] + [word.bit_count() & 1]
    model = replace(model, engine=True, latency=latency)
    probe = Machine(elf, TimedSwdTarget(pattern, latency, serve="wire", idle=1), restart_at=set(), model=model,
                    record_trace=False, record_stats=False)
    probe.resync_on_release = True
    if "DAP_Setup" in probe.sym:
        probe.call("DAP_Setup")
    probe.command(bytes([ID_CONNECT, 1]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(hz))
    probe.command(bytes([ID_TRANSFER_CONFIG, 0]) + u16(100) + u16(0))
    probe.command(bytes([ID_SWD_CONFIGURE, 0]))
    return probe


def connect_measures(elf: str, model: Model, latency: int, hz: int) -> list[str]:
    """A host connecting as probe-rs does, by line reset and DPIDR read, to a target the default
    latency reads wrong: the probe has to measure on its own and answer the read right, then use
    what it measured at the next connection rather than measure again."""
    probe = timed_dpidr_target(elf, model, latency, hz)
    tag = f"connecting at {hz / 1e6:g} MHz to latency {latency}"
    for attempt in (1, 2):
        # The timed target keeps time only while the machine counts.
        probe.begin()
        probe.command(bytes([ID_SWJ_SEQUENCE, 54]) + bytes([0xFF] * 6 + [0x07]))  # 51 high, 3 low
        reply = probe.command(bytes([ID_TRANSFER, 0, 1, 0x02]))
        probe.end()
        state = probe.command(bytes([ID_VENDOR_SWD_GEN, 0, 0]))
        measurements = int.from_bytes(state[42:46], "little")
        if reply[1:3] != bytes([1, 1]) or int.from_bytes(reply[3:7], "little") != DPIDR_MSPM0L1306:
            return [f"{tag}, attempt {attempt}: DPIDR read answered {reply[1:7].hex()}"]
        if state[2] != latency or measurements != 1:
            return [f"{tag}, attempt {attempt}: latency {state[2]} after {measurements} measurements"]
    return []


def connect_rescued(elf: str, model: Model, latency: int, hz: int) -> list[str]:
    """The same at a clock with margin enough that connecting measures nothing first, to a target
    the default latency reads wrong all the same: the failed DPIDR read has to be measured and run
    again, so the host sees it answer."""
    probe = timed_dpidr_target(elf, model, latency, hz)
    probe.begin()
    probe.command(bytes([ID_SWJ_SEQUENCE, 54]) + bytes([0xFF] * 6 + [0x07]))
    reply = probe.command(bytes([ID_TRANSFER, 0, 1, 0x02]))
    probe.end()
    state = probe.command(bytes([ID_VENDOR_SWD_GEN, 0, 0]))
    measurements, rescues = (int.from_bytes(state[i : i + 4], "little") for i in (42, 54))
    tag = f"connecting at {hz / 1e6:g} MHz to latency {latency}"
    if reply[1:3] != bytes([1, 1]) or int.from_bytes(reply[3:7], "little") != DPIDR_MSPM0L1306:
        return [f"{tag}: DPIDR read answered {reply[1:7].hex()}"]
    if state[2] != latency or measurements != 1 or rescues != 1:
        return [f"{tag}: latency {state[2]} after {measurements} measurements, {rescues} rescues"]
    return []


def measure_latency(elf: str, model: Model, latency: int, hz: int) -> list[str]:
    """Vendor 0x89's measurement against a target that answers `latency` cycles after each edge."""
    word = DPIDR_MSPM0L1306
    probe = timed_dpidr_target(elf, model, latency, hz)
    probe.begin()
    reply = probe.command(bytes([ID_VENDOR_SWD_GEN, 0, 1]))
    probe.end()
    tag = f"latency {latency} at {hz / 1e6:g} MHz"
    if reply[1] != 0 or reply[30] != 1 or reply[31] != latency or reply[2] != latency:
        return [f"{tag}: measured outcome {reply[30]}, latency {reply[31]}, in use {reply[2]}"]
    probe.begin()
    reply = probe.command(bytes([ID_TRANSFER, 0, 1, 0x02]))
    reads = probe.end().reads
    margins = [r.margin for r in reads if r.margin is not None]
    if reply[1:3] != bytes([1, 1]) or int.from_bytes(reply[3:7], "little") != word or not margins or min(margins) < 1:
        return [f"{tag}: the DPIDR read after it answered {reply[1:7].hex()}, least margin {min(margins, default=None)}"]
    return []


JTAG_IR_IDCODE = 0xE
DAP_IDCODE = 0x4BA00477
JTAG_WORD = 0xA5A5_5A5A
ID_VENDOR_JTAG_GEN = 0x8A


def timed_idcode_target(elf: str, model: Model, wire: str, latency: int, hz: int) -> "Machine":
    """A probe connected over `wire`, jtag or cjtag, at `hz` to one TAP whose TDO comes `latency`
    cycles after each falling edge: a DAP that answers IDCODE, and every DR scan with an OK
    acknowledge and JTAG_WORD."""
    probe = Machine(elf, Target([]), restart_at=set(), model=replace(model, engine=True), record_trace=False,
                    record_stats=False)
    if "DAP_Setup" in probe.sym:
        probe.call("DAP_Setup")
    probe.command(bytes([ID_CONNECT, 2]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(hz))
    probe.command(bytes([ID_TRANSFER_CONFIG, 0]) + u16(100) + u16(0))
    if wire in TWO_PIN:
        # Framing on without an activation, where a host leaves the wire after driving it.
        probe.command(cjtag_enter(wire))
    tap = IdcodeTap(JTAG_DR_OK | (JTAG_WORD << 3), 35, DAP_IDCODE)
    probe.target = (TimedCjtagTarget(tap, latency=latency, oscan2=wire == "cjtag2", oscan5=wire == "cjtag5",
                                     oscan6=wire == "cjtag6")
                    if wire in TWO_PIN
                    else TimedJtagTarget(tap, latency=latency))
    return probe


def jtag_read_right(probe: "Machine", tag: str) -> list[str]:
    """A DAP_Transfer AP read, posted and so read twice, with every sample a cycle inside its window."""
    probe.target.reads = []
    probe.begin()
    reply = probe.command(bytes([ID_TRANSFER, 0, 2, AP_DRW_READ, AP_DRW_READ]))
    probe.end()
    margins = [r.margin for r in probe.target.reads if r.margin is not None]
    if reply != bytes([ID_TRANSFER, 2, 1]) + u32(JTAG_WORD) * 2 or not margins or min(margins) < 1:
        return [f"{tag}: the read after it answered {reply.hex()}, least margin {min(margins, default=None)}"]
    return []


def latency_in_use(wire: str, measured: int) -> int:
    """What the probe builds for after measuring `measured`: a cycle later on two pins
    (src/jtag_sweep.c, TWO_PIN_SLACK)."""
    return measured + 1 if wire in TWO_PIN else measured


def jtag_measure_latency(elf: str, model: Model, wire: str, latency: int, hz: int) -> list[str]:
    """Vendor 0x8A's measurement against a TAP whose TDO comes `latency` cycles after each fall."""
    probe = timed_idcode_target(elf, model, wire, latency, hz)
    probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
    probe.begin()
    reply = probe.command(bytes([ID_VENDOR_JTAG_GEN, 0, 0, 1]))
    probe.end()
    tag = f"{wire} latency {latency} at {hz / 1e6:g} MHz"
    in_use = reply[2] if wire in TWO_PIN else reply[3]
    if reply[1] != 0 or reply[31] != 1 or reply[33] != latency or in_use != latency_in_use(wire, latency):
        return [f"{tag}: measured outcome {reply[31]}, latency {reply[33]}, in use {in_use}"]
    return jtag_read_right(probe, tag)


def jtag_connect(elf: str, model: Model, wire: str, latency: int, hz: int, rescued: bool) -> list[str]:
    """A host connecting as probe-rs does, DAP_JTAG_Configure then DAP_Transfer, to a TAP the
    default latency reads wrong. Where the default leaves too little margin the probe measures
    first; where it does not, the first transfer fails and the probe measures and runs it again.
    Either way the host's read answers right."""
    probe = timed_idcode_target(elf, model, wire, latency, hz)
    probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
    probe.begin()
    reply = probe.command(bytes([ID_TRANSFER, 0, 2, AP_DRW_READ, AP_DRW_READ]))
    probe.end()
    state = probe.command(bytes([ID_VENDOR_JTAG_GEN, 0, 0, 0]))
    measurements, rescues = (int.from_bytes(state[i : i + 4], "little") for i in (44, 56))
    in_use = state[2] if wire in TWO_PIN else state[3]
    tag = f"{wire} connecting at {hz / 1e6:g} MHz to latency {latency}"
    if reply != bytes([ID_TRANSFER, 2, 1]) + u32(JTAG_WORD) * 2:
        return [f"{tag}: the first DAP_Transfer answered {reply.hex()}"]
    if in_use != latency_in_use(wire, latency) or measurements != 1 or rescues != (1 if rescued else 0):
        return [f"{tag}: latency {in_use} after {measurements} measurements, {rescues} rescues"]
    return jtag_read_right(probe, tag)


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


CHECK_WIRES = ("swd", "jtag", "cjtag", "cjtag2", "cjtag5", "cjtag6")
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


def check_single_wire(args: tuple[str, Model, int, str]) -> int:
    elf, model, cpu_hz, wire = args
    return check(elf, model, cpu_hz, False, 1, (wire,))


def check(
    elf: str,
    model: Model,
    cpu_hz: int,
    bless: bool,
    jobs: int = 1,
    wires: tuple[str, ...] | None = None,
) -> int:
    """The gate: an optimisation may change the timing and must not change anything else.

    Three things are checked. The wire event stream has to match what is recorded, which is what
    makes a change to the bit paths reviewable without reading the generated code. The two-pin path
    has to emit no transition while the clock is high, because that is an escape edge and the
    reason this firmware exists. And no wire may clock faster than the host asked for, except where
    that is already recorded as known -- a new one is a regression.
    """
    if jobs < 1:
        raise ValueError("jobs must be at least 1")
    wires = CHECK_WIRES if wires is None else wires
    if jobs > 1 and not bless and len(wires) > 1:
        args = [(elf, model, cpu_hz, wire) for wire in wires]
        with concurrent.futures.ProcessPoolExecutor(max_workers=min(jobs, len(wires))) as pool:
            return 1 if any(pool.map(check_single_wire, args)) else 0

    failures = []

    # An image that generates its SWD paths runs them from SRAMX, which only the timing engine
    # prices correctly. One built with ARM's paths alone samples on other cycles, so its traces are
    # its own. Recognised by a variable: a function can be inlined out of the symbol table, and
    # clang did that to swd_gen_ready once its callers shrank to one.
    baseline_dir = BASELINE_DIR + "-armwire"
    if "swd_gen_read" in symbols(elf):
        baseline_dir = BASELINE_DIR
        if not model.engine:
            model = replace(model, engine=True)

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
        ("cjtag2", "jtag-sequence", bytes([ID_JTAG_SEQUENCE, 2, 0x08, 0xA5, 0x80 | 0x40 | 0x08, 0x3C])),
        ("cjtag5", "jtag-sequence", bytes([ID_JTAG_SEQUENCE, 2, 0x08, 0xA5, 0x80 | 0x40 | 0x08, 0x3C])),
        ("cjtag6", "jtag-sequence", bytes([ID_JTAG_SEQUENCE, 2, 0x08, 0xA5, 0x80 | 0x40 | 0x08, 0x3C])),
    ):
        if wire not in wires:
            continue
        probe = Machine(elf, Target([0, 1] * 64), restart_at=set(), model=model, record_stats=False)
        probe.command(bytes([ID_CONNECT, 1 if wire == "swd" else 2]))
        probe.command(bytes([ID_SWJ_CLOCK]) + u32(10_000_000))
        if wire in TWO_PIN:
            probe.command(cjtag_enter(wire))
        probe.begin()
        reply = probe.command(payload)
        counts = probe.end()
        if reply[1] != 0:
            failures.append(f"{wire} {name}: answered 0x{reply[1]:02x}, wanted DAP_OK")
        path = os.path.join(baseline_dir, f"{wire}-{name}.trace")
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
    for wire, port in (("swd", 1), ("jtag", 2), ("cjtag", 2), ("cjtag2", 2), ("cjtag5", 2), ("cjtag6", 2)):
        if wire not in wires:
            continue
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
            probe = build(elf, wire, True, model, record_stats=False)
            probe.command(bytes([ID_CONNECT, port]))
            probe.command(bytes([ID_SWJ_CLOCK]) + u32(10_000_000))
            probe.command(bytes([ID_TRANSFER_CONFIG, 0]) + u16(2) + u16(0))
            if wire == "swd":
                probe.command(bytes([ID_SWD_CONFIGURE, 0]))
            else:
                probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
            if wire in TWO_PIN:
                probe.command(cjtag_enter(wire))
                probe.cjtag_tap = JtagTap(JTAG_DR_OK, 35)
            probe.begin()
            reply = probe.command(payload)
            counts = probe.end()
            if reply[0] != ID_TRANSFER:
                failures.append(f"{wire} {label}: reply echoes 0x{reply[0]:02x}, wanted 0x05")
            path = os.path.join(baseline_dir, f"{wire}-{label}.trace")
            body = "\n".join(counts.trace) + "\n"
            if bless:
                open(path, "w").write(body)
            elif not os.path.exists(path):
                failures.append(f"{wire}-{label}.trace: no baseline recorded")
            elif open(path).read() != body:
                failures.append(f"{wire}-{label}.trace: wire events differ from the baseline")

    # The read latency, measured by the probe against targets that answer later and earlier than
    # the default: it has to find each one exactly, and leave every sample of the read that follows
    # inside its window.
    if "swd_sweep_last" in symbols(elf) and "swd" in wires:
        for latency, hz in ((6, 37_500_000), (9, 30_000_000)):
            failures += measure_latency(elf, model, latency, hz)
        failures += connect_measures(elf, model, 11, 37_500_000)
        failures += connect_rescued(elf, model, 12, 25_000_000)
    # The same with IDCODE reads, on four wires, OScan1, and OScan5 and OScan6 in Shift.
    if "jtag_sweep_last" in symbols(elf):
        if "jtag" in wires:
            for latency, hz in ((7, 30_000_000), (13, 25_000_000)):
                failures += jtag_measure_latency(elf, model, "jtag", latency, hz)
            failures += jtag_connect(elf, model, "jtag", 13, 30_000_000, rescued=False)
            failures += jtag_connect(elf, model, "jtag", 16, 10_000_000, rescued=True)
        for wire in ("cjtag", "cjtag5", "cjtag6"):
            if wire in wires:
                failures += jtag_measure_latency(elf, model, wire, 10, 50_000_000)
                failures += jtag_connect(elf, model, wire, 10, 50_000_000, rescued=False)

    known_path = os.path.join(baseline_dir, "known-overshoot.txt")
    known = set()
    if os.path.exists(known_path):
        known = {line.strip() for line in open(known_path) if line.strip() and not line.startswith("#")}
    known_for_wires = {tag for tag in known if any(tag.startswith(f"{wire}@") for wire in wires)}
    seen_overshoot = set()

    # Two clocks, because the paced and delay-free paths are different code and a baseline taken
    # at one of them says nothing about the other.
    for wire, (tag, hz) in itertools.product(wires, (("10MHz", 10_000_000), ("50MHz", 50_000_000))):
        for label, read in (("read", True), ("write", False)):
            counts, reply, _ = measure(elf, wire, hz, 16, read, model, record_stats=False)

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
            path = os.path.join(baseline_dir, name)
            body = "\n".join(counts.trace) + "\n"
            if bless:
                os.makedirs(baseline_dir, exist_ok=True)
                open(path, "w").write(body)
            elif not os.path.exists(path):
                failures.append(f"{name}: no baseline recorded")
            elif open(path).read() != body:
                failures.append(f"{name}: wire events differ from the baseline")

    # The count a block reply carries, at the sizes the traces above do not reach. It is derived
    # from the response pointer rather than counted, so one word and a full packet are the two
    # that would show an off-by-one, and both are outside every other case here. A full packet is
    # 127 words of reply, but 126 of request: a write's five header bytes take the 127th.
    for wire, (label, read) in itertools.product(wires, (("read", True), ("write", False))):
        for words in (1, 2, 3, 127 if read else 126):
            counts, reply, _ = measure(elf, wire, 10_000_000, words, read, model, record_stats=False)
            want = 4 + (4 * words) if read else 4
            got_words = reply[1] | (reply[2] << 8)
            if got_words != words:
                failures.append(f"{wire} {label}: a {words}-word block replied {got_words} words")
            elif len(reply) != want:
                failures.append(f"{wire} {label}, {words} words: reply is {len(reply)} bytes, wanted {want}")
            elif reply[3] != 1:
                failures.append(f"{wire} {label}, {words} words: status 0x{reply[3]:02x}, wanted OK")

            if wire in TWO_PIN and counts.moves_while_high != counts.escape_edges:
                failures.append(
                    f"{wire} {label}: {counts.moves_while_high - counts.escape_edges} transitions while the"
                    " clock was high outside an EOT escape, which are escape edges to a 1149.7 target"
                )

    for wire in wires:
        for mhz in CHECK_CLOCKS:
            counts, _, _ = measure(elf, wire, int(mhz * 1e6), 16, True, model, record_stats=False)
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
        os.makedirs(baseline_dir, exist_ok=True)
        with open(known_path, "w") as fh:
            fh.write("# Wires that clock faster than the host asked for. See OPTIMISATIONS.md:\n")
            fh.write("# it is a decision rather than a slip, and both ways out cost throughput.\n")
            for tag in sorted(seen_overshoot):
                fh.write(tag + "\n")
        print(f"baseline rewritten in {baseline_dir}")
        return 0

    for tag in sorted(known_for_wires - seen_overshoot):
        print(f"  note: {tag} no longer overshoots; drop it from known-overshoot.txt")
    if failures:
        for f in failures:
            print(f"  FAIL  {f}")
        return 1
    print(
        "  all clear: every transfer answered OK with the right data, wire events match,"
        " no escape edges,"
        f" {len(known_for_wires)} known overshoots unchanged"
    )
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", nargs="?", default="build/mculink_cjtag_core1.elf", help="CPU 1's image")
    ap.add_argument(
        "--wire",
        choices=["swd", "jtag", "cjtag", "cjtag2", "cjtag5", "cjtag6"],
        help="wire to measure; with --check, restrict the check to this wire (default: all wires)",
    )
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
    ap.add_argument("--jobs", type=int, default=min(4, available_cpus()),
                    help="independent wire checks to run at once")
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
    if args.jobs < 1:
        ap.error("--jobs must be at least 1")

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
        selected = None if args.wire is None else (args.wire,)
        print(f"{args.elf}, {args.wire or 'all wires'}")
        return check(args.elf, model, cpu_hz, args.bless, args.jobs, selected)
    if args.table:
        return table(args.elf, model, cpu_hz, args.words)
    wire = args.wire or "swd"
    if args.calibrate:
        return calibrate(args.elf, wire, args.words, cpu_hz, model, args.config)
    shape = f"{args.words}-transfer DAP_Transfer" if args.transfer else f"{args.words}-word block"
    print(f"{args.elf}, {wire}, {shape}, core {args.cpu:g} MHz")
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
                args.elf, wire, clock_hz, args.words, read, model, explain=args.explain
            )
            report(label, counts, model, clock_hz, cpu_hz, args.words)
            if args.explain:
                explain_gap(probe)
            if args.trace:
                os.makedirs(args.trace, exist_ok=True)
                path = os.path.join(args.trace, f"{wire}-{label}-{mhz:g}MHz.trace")
                with open(path, "w") as fh:
                    fh.write("\n".join(counts.trace) + "\n")
                print(f"    wire events written to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
