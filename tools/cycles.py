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
    UC_MODE_MCLASS,
    UC_MODE_THUMB,
    Uc,
    UcError,
)
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_SP

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


def symbols(elf: str) -> dict[str, int]:
    out = {}
    for line in sh("arm-none-eabi-nm", elf).splitlines():
        parts = line.split()
        if len(parts) == 3:
            out[parts[2]] = int(parts[0], 16)
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

    def total(self, c: "Counts") -> int:
        return (
            c.other * self.other
            + c.load * self.load
            + (c.store - c.gpio_store) * self.store
            + c.gpio_store * self.gpio_store
            + c.taken_branch * (self.taken_branch - self.other)
        )


@dataclass
class Counts:
    instructions: int = 0
    other: int = 0
    load: int = 0
    store: int = 0
    gpio_store: int = 0
    taken_branch: int = 0
    delay_iterations: int = 0
    tck_rise: int = 0
    tck_fall: int = 0
    swdio_release: int = 0
    # A wire transition while the clock is high. Harmless on SWD and four-wire JTAG; on a 1149.7
    # wire it is an escape edge, which is the whole reason this firmware exists.
    moves_while_high: int = 0
    trace: list[str] = field(default_factory=list)


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


# Acknowledge bits in the order each protocol reads them, then the data phase.
SWD_OK = [1, 0, 0]  # ack bit0 first, DAP_TRANSFER_OK == 1
JTAG_OK = [0, 1, 0]  # JTAG_Transfer reads ack bit1, bit0, bit2
SWD_READ = SWD_OK + [0] * 33  # 32 data bits and parity
SWD_WRITE = SWD_OK
JTAG_READ = JTAG_OK + [0] * 32
JTAG_WRITE = JTAG_OK
# On the two-pin path every clock carries a TDO packet, not just the ones that want TDO, so the
# three clocks that walk the TAP to Shift-DR consume pattern bits of their own.
CJTAG_LEAD = [0, 0, 0]
CJTAG_READ = CJTAG_LEAD + JTAG_READ
CJTAG_WRITE = CJTAG_LEAD + JTAG_WRITE


class Machine:
    def __init__(self, elf: str, target: Target, restart_at: set[int]):
        self.sym = symbols(elf)
        self.target = target
        self.counting = False
        self.restart_at = restart_at
        self.counts = Counts()
        self.md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
        self.md.detail = False
        self._decoded: dict[int, tuple[str, int]] = {}
        self._prev_end: int | None = None

        self.level = 0
        self.dir = 0
        self.mask = 0

        self.uc = Uc(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS)
        for base, size in (FLASH, SRAM, USB_RAM, PERIPH_LOW, PERIPH_HIGH, PPB):
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

    # -- GPIO ----------------------------------------------------------------
    def _gpio_read(self, uc, offset, size, user):
        # B[0][n] is one byte per pin at the start of the block: SWDIO is pin 2, TDO pin 3.
        if offset in (REG_B0 + PIN_SWDIO, REG_B0 + PIN_TDO):
            if self.counting:
                self.counts.trace.append("S" if offset == REG_B0 + PIN_SWDIO else "T")
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

        if not self.counting:
            return
        self.counts.gpio_store += 1

        after_clock = (self.level >> PIN_TCK) & 1
        after_wire = self._driven()
        after_tdi = (self.level >> PIN_TDI) & 1

        # Level changes are recorded before the clock edge in the same store, so that fusing a
        # data write into the falling edge produces the same stream as doing them separately.
        if after_tdi != before_tdi:
            self.counts.trace.append(f"I{after_tdi}")
        if after_wire != before_wire:
            self.counts.trace.append(f"W{after_wire}")
            if after_wire == "z":
                self.counts.swdio_release += 1
            if before_clock and after_clock:
                self.counts.moves_while_high += 1
        if after_clock and not before_clock:
            self.counts.tck_rise += 1
            self.counts.trace.append(f"^{after_wire}{after_tdi}")
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

        if self._prev_end is not None and address != self._prev_end:
            self.counts.taken_branch += 1
            # The pacing loop is two instructions, so a backward branch this short is one of its
            # iterations. Counting them is what lets the delay be separated from everything else
            # without parsing the clock arithmetic out of the source.
            if 0 < self._prev_end - address <= 8:
                self.counts.delay_iterations += 1
        self._prev_end = address + size

        kind, _ = self._classify(address, size)
        c = self.counts
        c.instructions += 1
        if kind == "load":
            c.load += 1
        elif kind == "store":
            c.store += 1
        else:
            c.other += 1

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
        produced = self.call("DAP_ProcessCommand", SCRATCH, SCRATCH + 0x400) & 0xFFFF
        return bytes(self.uc.mem_read(SCRATCH + 0x400, max(produced, 1)))

    def begin(self):
        self.counts = Counts()
        self.counting = True

    def end(self) -> Counts:
        self.counting = False
        return self.counts


def u16(v: int) -> bytes:
    return v.to_bytes(2, "little")


def u32(v: int) -> bytes:
    return v.to_bytes(4, "little")


# DAP command ids used below.
ID_CONNECT, ID_TRANSFER_CONFIG, ID_TRANSFER_BLOCK = 0x02, 0x04, 0x06
ID_SWJ_CLOCK, ID_SWD_CONFIGURE, ID_JTAG_CONFIGURE = 0x11, 0x13, 0x15
ID_CJTAG_CONTROL = 0x83

AP_DRW_READ, AP_DRW_WRITE = 0x0F, 0x0D


# Where to restart the target's bit pattern. The firmware reads the wire in order within one
# transfer, so re-synchronising at the entry of whichever transfer function survived the build is
# enough. Which ones survive depends on inlining, hence the list.
TRANSFER_MARKERS = {
    "swd": ("SWD_Transfer", "SWD_TransferFast", "SWD_TransferSlow"),
    "jtag": ("jtag4_Transfer", "jtag4_TransferFast", "jtag4_TransferSlow"),
    "cjtag": ("cjtag_Transfer", "cjtag_TransferFast", "cjtag_TransferSlow"),
}


def build(elf: str, wire: str, read: bool) -> Machine:
    pattern = {
        ("swd", True): SWD_READ,
        ("swd", False): SWD_WRITE,
        ("jtag", True): JTAG_READ,
        ("jtag", False): JTAG_WRITE,
        ("cjtag", True): CJTAG_READ,
        ("cjtag", False): CJTAG_WRITE,
    }[(wire, read)]
    probe = Machine(elf, Target(pattern), restart_at=set())
    found = {name for name in TRANSFER_MARKERS[wire] if name in probe.sym}
    if not found:
        raise SystemExit(
            f"no transfer entry point survived inlining for {wire}, so the acknowledge bits cannot "
            f"be lined up: looked for {', '.join(TRANSFER_MARKERS[wire])}"
        )
    probe.restart_at = {probe.sym[name] for name in found}
    return probe


def measure(elf: str, wire: str, clock_hz: int, words: int, read: bool) -> tuple[Counts, bytes]:
    probe = build(elf, wire, read)
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

    probe.begin()
    response = probe.command(payload)
    return probe.end(), response


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
        f" {c.taken_branch} taken branches, {c.other} other"
    )
    print(f"    wire moved {c.moves_while_high} times while the clock was high")
    print(
        f"    modelled {cycles} cycles, {bit_cycles:.2f} per clock"
        f"  ->  {achieved / 1e6:.2f} MHz on the wire for {clock_hz / 1e6:.0f} MHz asked"
        f"   ({100 * achieved / clock_hz:.0f}%)"
    )


def dap_constants(path: str) -> tuple[int, int, int]:
    """CPU_CLOCK, IO_PORT_WRITE_CYCLES and DELAY_SLOW_CYCLES, as the image was built with them."""
    text = open(path).read()

    def value(name: str, default: int | None = None) -> int:
        m = re.search(rf"^#define\s+{name}\s+(\d+)U?", text, re.M)
        if m:
            return int(m.group(1))
        if default is None:
            raise SystemExit(f"{name} not found in {path}")
        return default

    return value("CPU_CLOCK"), value("IO_PORT_WRITE_CYCLES"), value("DELAY_SLOW_CYCLES", 3)


def calibrate(elf: str, wire: str, words: int, cpu_hz: int, model: Model, config: str) -> int:
    """Recover the two constants ARM's clock arithmetic assumes, from what the image actually runs.

    DAP.c models a half period as IO_PORT_WRITE_CYCLES + clock_delay * DELAY_SLOW_CYCLES, and picks
    clock_delay from the requested frequency with that model. Measure cycles per bit against delay
    iterations per bit and the line's slope and intercept are those two constants. Where they are
    wrong, every requested clock is wrong by the same amount.
    """
    cpu_clock, io_cycles, slow_cycles = dap_constants(config)
    fast_above = (cpu_clock / 2) / io_cycles
    points = []
    print(f"{elf}, {wire}, calibration sweep")
    print(
        f"built with CPU_CLOCK {cpu_clock / 1e6:g} MHz, IO_PORT_WRITE_CYCLES {io_cycles},"
        f" DELAY_SLOW_CYCLES {slow_cycles}; the delay-free path starts at {fast_above / 1e6:.1f} MHz\n"
    )
    print(f"  {'asked':>8}  {'path':>5}  {'delay/bit':>10}  {'cycles/bit':>11}  {'MHz':>7}")
    for mhz in (0.5, 1, 2, 3, 5, 8, 10, 20, 50):
        counts, _ = measure(elf, wire, int(mhz * 1e6), words, True)
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
    print("  the intercept also carries the per-word bookkeeping, so read it as an upper bound")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", nargs="?", default="build/mculink_cjtag.elf")
    ap.add_argument("--wire", choices=["swd", "jtag", "cjtag"], default="swd")
    ap.add_argument("--words", type=int, default=16, help="words in the measured DAP_TransferBlock")
    ap.add_argument(
        "--clock", type=float, nargs="*", default=[1, 5, 10, 14, 25, 50], help="requested wire clocks, MHz"
    )
    ap.add_argument("--cpu", type=float, default=144.0, help="core clock, MHz")
    ap.add_argument("--store-cycles", type=int, default=1)
    ap.add_argument("--gpio-store-cycles", type=int, default=1)
    ap.add_argument("--load-cycles", type=int, default=2)
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
        "--trace",
        metavar="DIR",
        help="write the wire event stream per scenario, to diff across a change",
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
        gpio_store=args.gpio_store_cycles,
    )
    cpu_hz = int(args.cpu * 1e6)
    if args.calibrate:
        return calibrate(args.elf, args.wire, args.words, cpu_hz, model, args.config)
    print(f"{args.elf}, {args.wire}, {args.words}-word block, core {args.cpu:g} MHz")
    print(
        f"cost model: load {model.load}, store {model.store}, GPIO store {model.gpio_store},"
        f" taken branch {model.taken_branch}, other {model.other}"
    )
    for mhz in args.clock:
        clock_hz = int(mhz * 1e6)
        print(f"\n{mhz:g} MHz requested")
        for label, read in (("read", True), ("write", False)):
            counts, _ = measure(args.elf, args.wire, clock_hz, args.words, read)
            report(label, counts, model, clock_hz, cpu_hz, args.words)
            if args.trace:
                os.makedirs(args.trace, exist_ok=True)
                path = os.path.join(args.trace, f"{args.wire}-{label}-{mhz:g}MHz.trace")
                with open(path, "w") as fh:
                    fh.write("\n".join(counts.trace) + "\n")
                print(f"    wire events written to {path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
