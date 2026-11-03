# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn", "capstone"]
# ///
"""Check what a built image says about itself, without hardware.

Four things that only show up at the bench, each of which looks like something else when it does.

A malformed descriptor: the device does not appear, nothing reports why, and the first guess is
the cable. The lengths are nested and hand-maintained -- a configuration states a total that has to
match the bytes after it, an interface states how many endpoints follow, the BOS states the size of
a descriptor stored elsewhere -- and nothing in the build checks them against each other.

A clock that is not running: a Flexcomm with no clock reports no error, it simply never reaches a
baud rate, and the console and SWO both look like wiring faults.

A command that does not answer, or that never returns. COMMANDS below is a claim until something
sends all of them.

And the serial number, which comes from a flash region this firmware has never touched.

The firmware is two images: CPU 0's, which owns USB, and CPU 1's, which executes the commands and
is embedded in CPU 0's. Each check runs against the image whose code it is about, and one more
checks that the two agree on everything they share.
"""

import argparse
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from cycles import (  # noqa: E402
    ORDINARY_GPIO,
    REG_B0,
    RETURN_MAGIC,
    SCRATCH,
    SRAM,
    Machine,
    Model,
    Target,
    build,
    dap_setup,
    load_address,
    sections,
    symbols,
    u16,
    u32,
)
from unicorn import UC_HOOK_CODE  # noqa: E402
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0  # noqa: E402

UUID_ADDRESS = 0x0009FC70
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
USB_SRAM = (0x40100000, 0x00004000)
SRAM3 = (0x20030000, 0x00010000)


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


def symbols_with_size(elf: str) -> dict[str, tuple[int, int]]:
    out = {}
    for line in subprocess.run(
        ["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True, check=True
    ).stdout.splitlines():
        parts = line.split()
        if len(parts) == 4:
            entry = (int(parts[0], 16), int(parts[1], 16))
            out[parts[3]] = entry
            out.setdefault(base_name(parts[3]), entry)
    return out


def config_value(config: str, name: str) -> int | None:
    m = re.search(rf"^#define\s+{name}\s+(\d+)U?", open(config).read(), re.M)
    return int(m.group(1)) if m else None


def short_enums(elf: str) -> bool:
    """Whether the image was built with enums as small as their values, as arm-none-eabi-gcc
    builds them, rather than an int, as clang does."""
    attributes = subprocess.run(["arm-none-eabi-readelf", "-A", elf], capture_output=True, text=True,
                                check=True).stdout
    return "Tag_ABI_enum_size: small" in attributes


def shared_fields() -> list[str]:
    """Every member dap_shared.h declares in dap_shared_t, with a member whose type is another
    struct from the same header named field by field, as `outer.inner`."""
    text = open(os.path.join(ROOT, "src", "dap_shared.h")).read()
    text = re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S)
    structs = {name: body for body, name in re.findall(r"typedef struct \{(.*?)\} (\w+);", text, re.S)}

    def fields(body: str) -> list[str]:
        out = []
        for declaration in re.findall(r"([^;{}]+);", body):
            m = re.search(r"(\w+)\s*(?:\[[^;]*\])?\s*$", declaration.strip())
            if not m:
                continue
            kind = next((t for t in structs if re.search(rf"\b{t}\b", declaration)), None)
            out += [f"{m.group(1)}.{inner}" for inner in fields(structs[kind])] if kind else [m.group(1)]
        return out

    return fields(structs["dap_shared_t"])


def shared_offsets(request_slots: int, *fields: str, small_enums: bool = True,
                   struct_type: str = "dap_shared_t", timing_diagnostics: bool = False) -> dict[str, int]:
    """Member offsets of dap_shared_t, and its size as "size", as the firmware's compiler lays it
    out. A host compiler would not: its pointers are a different size. An image's enum size is
    its compiler's choice, so pass the image's own."""
    entries = [f"offsetof({struct_type}, {f})" for f in fields] + [f"sizeof({struct_type})"]
    source = '#include <stddef.h>\n#include "dap_shared.h"\n'
    source += f"const unsigned layout[] = {{{', '.join(entries)}}};\n"
    with tempfile.TemporaryDirectory() as tmp:
        c_file, obj, raw = (os.path.join(tmp, n) for n in ("layout.c", "layout.o", "layout.bin"))
        open(c_file, "w").write(source)
        subprocess.run(["arm-none-eabi-gcc", "-std=gnu23", "-mthumb", "-mcpu=cortex-m33", f"-DREQUEST_SLOTS={request_slots}U",
                        *(["-DDAP_TIMING_DIAGNOSTICS"] if timing_diagnostics else []),
                        "-fshort-enums" if small_enums else "-fno-short-enums",
                        f"-I{os.path.join(ROOT, 'src')}", "-c", c_file, "-o", obj], check=True)
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", "-j", ".rodata", obj, raw], check=True)
        values = struct.unpack(f"<{len(entries)}I", open(raw, "rb").read()[: 4 * len(entries)])
    return dict(zip([*fields, "size"], values))


def binary(elf: str, *sections: str) -> bytes:
    """What `objcopy -O binary` makes of the image, or of the sections named."""
    with tempfile.NamedTemporaryFile(suffix=".bin") as raw:
        only = [arg for name in sections for arg in ("-j", name)]
        subprocess.run(["arm-none-eabi-objcopy", "-O", "binary", *only, elf, raw.name], check=True)
        return open(raw.name, "rb").read()


class Checker:
    def __init__(self) -> None:
        self.problems: list[str] = []
        self.notes: list[str] = []

    def check(self, ok: object, message: str) -> bool:
        """Record a problem when the condition fails, and hand the result back so a note can
        describe what was actually found rather than what was hoped for."""
        if not ok:
            self.problems.append(message)
        return bool(ok)

    def note(self, message: str) -> None:
        self.notes.append(message)


def check_device(c: Checker, d: bytes, bos: bool = True) -> None:
    if not c.check(len(d) == 18, f"device descriptor is {len(d)} bytes, wanted 18"):
        return
    c.check(d[0] == 18, f"device descriptor bLength is {d[0]}, wanted 18")
    c.check(d[1] == 0x01, f"device descriptor bDescriptorType is {d[1]:#x}, wanted 0x01")
    (bcd_usb,) = struct.unpack_from("<H", d, 2)
    if bos:
        c.check(bcd_usb >= 0x0210, f"bcdUSB is {bcd_usb:#06x}; the BOS descriptor needs 0x0210 or more")
    else:
        c.check(bcd_usb < 0x0201, f"bcdUSB is {bcd_usb:#06x} with no BOS, which 2.01 and later must have")
    c.check(d[7] in (8, 16, 32, 64), f"bMaxPacketSize0 is {d[7]}, not a legal control size")
    vid, pid = struct.unpack_from("<HH", d, 8)
    c.check(d[17] == 1, f"bNumConfigurations is {d[17]}, wanted 1")
    for name, index in (("iManufacturer", d[14]), ("iProduct", d[15]), ("iSerialNumber", d[16])):
        c.check(index != 0, f"{name} is 0, so the host has no string to read")
    c.note(f"device {vid:#06x}:{pid:#06x}, USB {bcd_usb >> 8:x}.{(bcd_usb >> 4) & 0xF:x}, EP0 {d[7]}")


def at_high_speed(probe: Machine) -> None:
    """Make tud_speed_get answer high speed. TinyUSB says full until a bus reset says otherwise, and
    the configuration is built for the speed it says (usb_checks.py covers full speed)."""
    entry = probe.sym["tud_speed_get"]

    def answer(uc, at, size, data):
        uc.reg_write(UC_ARM_REG_R0, 2)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    probe.uc.hook_add(UC_HOOK_CODE, answer, begin=entry, end=entry)
    probe.uc.ctl_flush_tb()


def check_configuration(c: Checker, d: bytes) -> dict[int, str]:
    if not c.check(len(d) >= 9, f"configuration descriptor is {len(d)} bytes"):
        return {}
    c.check(d[0] == 9 and d[1] == 0x02, "configuration descriptor does not start with a 9-byte type 2")
    (total,) = struct.unpack_from("<H", d, 2)
    c.check(total == len(d), f"wTotalLength says {total}, the descriptor is {len(d)} bytes")

    interfaces, endpoints, strings = set(), {}, {}
    expect_endpoints = 0
    i = 0
    while i < len(d):
        length, kind = d[i], d[i + 1]
        if not c.check(length >= 2, f"a descriptor at offset {i} claims length {length}"):
            return strings
        if kind == 0x04:  # interface
            c.check(
                expect_endpoints == 0,
                f"interface {d[i + 2]} starts with {expect_endpoints} endpoints still owed",
            )
            interfaces.add(d[i + 2])
            expect_endpoints = d[i + 4]
            if d[i + 8]:
                strings[d[i + 8]] = f"interface {d[i + 2]}"
        elif kind == 0x05:  # endpoint
            addr, attr = d[i + 2], d[i + 3]
            (mps,) = struct.unpack_from("<H", d, i + 4)
            c.check(addr not in endpoints, f"endpoint {addr:#04x} is used twice")
            endpoints[addr] = (attr, mps)
            if attr & 3 == 2:  # bulk
                c.check(mps == 512, f"bulk endpoint {addr:#04x} is {mps} bytes; high speed wants 512")
            expect_endpoints -= 1
        i += length

    c.check(expect_endpoints == 0, f"the last interface is owed {expect_endpoints} more endpoints")
    c.check(
        d[4] == len(interfaces),
        f"bNumInterfaces says {d[4]}, {len(interfaces)} interface descriptors are present",
    )
    c.check(
        interfaces == set(range(len(interfaces))),
        f"interface numbers are {sorted(interfaces)}, which is not 0..n",
    )
    c.note(f"{len(interfaces)} interfaces, {len(endpoints)} endpoints, {total} bytes")
    return strings


def check_bos(c: Checker, d: bytes, ms_os_len: int) -> None:
    if not c.check(len(d) >= 5, f"BOS descriptor is {len(d)} bytes"):
        return
    c.check(d[0] == 5 and d[1] == 0x0F, "BOS descriptor does not start with a 5-byte type 15")
    (total,) = struct.unpack_from("<H", d, 2)
    c.check(total == len(d), f"BOS wTotalLength says {total}, the descriptor is {len(d)} bytes")
    i, found = 5, 0
    while i < len(d):
        if d[i + 1] == 0x10 and d[i + 2] == 0x05:  # platform capability
            (declared,) = struct.unpack_from("<H", d, i + 24)
            c.check(
                declared == ms_os_len,
                f"the BOS says the MS OS 2.0 descriptor is {declared} bytes, it is {ms_os_len}",
            )
            found += 1
        i += d[i]
    c.check(found == 1, f"{found} Microsoft platform capabilities in the BOS, wanted 1")


def check_serial(c: Checker, elf: str, uuid: bytes, expect_prefix: str, uniform: bool = False) -> None:
    """Run GetSerialNum against a programmed unique id and read back what it produced."""
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    if "GetSerialNum" not in probe.sym:
        c.note("GetSerialNum was inlined away; serial not checked")
        return
    # The unique id lives in the protected flash region, which the image is already mapped over.
    probe.uc.mem_write(UUID_ADDRESS, uuid)
    probe.call("GetSerialNum")
    addr = probe.uc.reg_read(UC_ARM_REG_R0)
    c.check(addr != 0, "GetSerialNum returned NULL")
    if addr == 0:
        return
    c.check(SRAM[0] <= addr < SRAM[0] + SRAM[1], f"the serial lives at {addr:#x}, outside RAM")
    raw = bytes(probe.uc.mem_read(addr, 32))
    text = raw.split(b"\x00")[0].decode("ascii", "replace")
    c.check(text.startswith(expect_prefix), f"serial {text!r} does not start with {expect_prefix!r}")
    c.check(1 <= len(text) <= 30, f"serial {text!r} is {len(text)} characters")
    if uniform:
        # An unprogrammed or unreadable region reads back uniform, and hashing that would give
        # every unit the same serial. Saying so is the firmware's job.
        c.check("NOUUID" in text, f"a uniform unique id produced {text!r}, which looks like a hash")
    else:
        c.check("NOUUID" not in text, f"a programmed unique id produced {text!r}")
    c.note(f"{'uniform' if uniform else 'programmed'} unique id -> {text!r}")


# Every standard command the firmware answers, with operands chosen to be harmless: no pin waits, no
# resets that go anywhere. What is being checked is that the firmware answers at all.
#
# DAP_Delay is asked for zero on purpose. ARM's delay takes its argument as a count and decrements
# before testing, so zero counts down from 2^32: about ninety seconds at 144 MHz with the probe
# answering nothing. src/dap_dispatch.c answers it directly, and this is what checks that.
DAP_COMMANDS = {
    0x00: b"\xf0",  # Info, vendor name
    0x01: b"\x00\x00",  # HostStatus
    0x02: b"\x01",  # Connect, SWD
    0x03: b"",  # Disconnect
    0x04: b"\x00\x02\x00\x00\x00",  # TransferConfig
    0x05: b"\x00\x00",  # Transfer, no transfers
    0x06: b"\x00\x00\x00\x00",  # TransferBlock, no words
    0x08: b"\x00\x00\x00\x00\x00",  # WriteABORT
    0x09: b"\x00\x00",  # Delay, none
    0x0A: b"",  # ResetTarget
    0x10: b"\x00\x00\x00\x00\x00\x00",  # SWJ_Pins, no wait
    0x11: b"\x40\x42\x0f\x00",  # SWJ_Clock, 1 MHz
    0x12: b"\x08\xa5",  # SWJ_Sequence
    0x13: b"\x00",  # SWD_Configure
    0x14: b"\x01\x08\xa5",  # JTAG_Sequence
    0x15: b"\x01\x04",  # JTAG_Configure
    0x16: b"\x00",  # JTAG_IDCODE
    0x17: b"\x01",  # SWO_Transport
    0x18: b"\x01",  # SWO_Mode
    0x19: b"\x00\xc2\x01\x00",  # SWO_Baudrate
    0x1A: b"\x00",  # SWO_Control
    0x1B: b"",  # SWO_Status
    0x1C: b"\x00\x01",  # SWO_Data
    0x1D: b"\x01\x08\xa5",  # SWD_Sequence
    0x1E: b"\x00\x01",  # SWO_ExtendedStatus
    0x7F: b"\x01\x00\xf0",  # ExecuteCommands, one Info
    0x82: b"",  # our CJTAG_INFO
    0x83: b"\x00",  # our CJTAG_CONTROL, off
    0x84: b"\x00\x00",  # our J8, taking no pins, which hands all four back to the marker
    # Our TARGET, reading the supply. The emulator's ADC never finishes calibrating, so this takes
    # the timeout path and answers DAP_ERROR: what is checked is that it answers at all, which is
    # the property that matters on hardware too.
    0x86: b"\x00\x00",
    # Our SAMPLE, no reads. The port is disconnected by this point, so it answers DAP_ERROR;
    # check_sample_probe runs it connected.
    0x87: b"\x00\x00\xff\x00\x00\x00\x00\x00\x00",
    0x8C: b"\xff",  # our STAGE, asking only
    0x8E: b"\x00\x00\x00",  # our DIO, only reading
}
ID_DAP_INVALID = 0xFF
SWO_COMMANDS = (0x17, 0x18, 0x19, 0x1A, 0x1B, 0x1C, 0x1E)


def nxp_baudrate(src: int, want: int) -> tuple[int, int, bool]:
    """NXP's USART_SetBaudRate search, transcribed: oversampling 15 down to 4, 3% allowed error.
    The console's driver; SWO has its own search (swo_rate)."""
    best_diff, best_osr, best_brg = 1 << 32, 0xF, (1 << 32) - 1
    allowed = (want // 100) * 3
    for osr in range(0xF, 3, -1):
        if osr <= 8 and best_diff <= allowed:
            break
        brg = (((src * 10) // ((osr + 1) * want)) - 5) // 10
        if brg > 0xFFFF:
            continue
        actual = src // ((osr + 1) * (brg + 1))
        diff = abs(actual - want)
        if diff < best_diff:
            best_diff, best_osr, best_brg = diff, osr, brg
    actual = src // ((best_osr + 1) * (best_brg + 1))
    return actual, best_osr, best_diff <= allowed


def swo_rate(clock: int, want: int) -> tuple[int, int, float]:
    """swo_uart_divisors in src/swo_ring.h: the rate, its oversampling and its error. For each
    oversampling only the two dividers either side of the ideal can be closest. The C is tested
    against an exhaustive search on the host (tools/swo_ring_test.c)."""
    periods = {}
    for o in range(5, 17):
        below = clock // (o * want)
        for d in (below, below + 1):
            if 1 <= d <= 65536:
                periods.setdefault(o * d, o)
    period = min(periods, key=lambda p: (abs(clock - want * p) / p, -max(o for o in range(5, 17) if p % o == 0)))
    oversampling = max(o for o in range(5, 17) if period % o == 0 and period // o <= 65536)
    return clock // period, oversampling, abs(clock - want * period) / period / want


FRG_LIMIT = 100_000_000  # UM11126 4.5.49
UART_LIMIT = 10_000_000  # LPC55S6x data sheet 7.26.3.4, asynchronous


def swo_clock(fro: int, main: int, want: int) -> tuple[int, int, int, float]:
    """swo_uart_clock in src/swo_ring.h: the function clock, the rate, its oversampling and its
    error. The first source within 0.5% of the rate, in order: the 96 MHz clock, the main clock
    through the fractional divider within its limit, then above it; with none that close, the
    closest of the three."""
    def best(clocks: list[int]) -> tuple[int, int, int, float] | None:
        found = [(f, *swo_rate(f, want)) for f in clocks]
        found = [t for t in found if t[3] <= 0.03]
        return min(found, key=lambda t: t[3]) if found else None

    outputs = [main * 256 // (256 + mult) for mult in range(256)]
    sources = [best([fro]), best([f for f in outputs if f <= FRG_LIMIT]), best([f for f in outputs if f > FRG_LIMIT])]
    close = next((t for t in sources if t and t[3] <= 0.005), None)
    found = close or min((t for t in sources if t), key=lambda t: t[3], default=None)
    return found if found else (0, 0, 0, 1.0)


def check_swo_baudrate(c: Checker, config: str, fro: int, main: int) -> None:
    """SWO_Baudrate clamps every request to SWO_UART_MAX_BAUDRATE and refuses a rate more than 3%
    from any it can make, so a maximum it cannot make fails every request at or above it. Which of
    NXP's limits the maximum passes is a note: README.md's "Past NXP's limits" is where a user
    reads it, and a change here belongs there too."""
    m = re.search(r"^#define\s+SWO_UART_MAX_BAUDRATE\s+(\d+)U?", open(config).read(), re.M)
    if not m:
        c.note("SWO_UART_MAX_BAUDRATE not found; the SWO rate not checked")
        return
    want = int(m.group(1))
    clock, actual, oversampling, error = swo_clock(fro, main, want)
    c.check(
        error <= 0.03,
        f"SWO's maximum of {want} Hz is {error * 100:.2f}% from anything the Flexcomm's clocks can"
        " produce, so every request at or above it fails",
    )
    past = ([f"the UART's {UART_LIMIT // 1_000_000} Mbit/s"] if want > UART_LIMIT else []) + (
        [f"the fractional divider's {FRG_LIMIT // 1_000_000} MHz"] if clock > FRG_LIMIT else [])
    c.note(f"SWO maximum {want / 1e6:g} MHz -> {actual / 1e6:g} MHz from {clock / 1e6:g} MHz,"
           f" {error * 100:.2f}% out, oversampling {oversampling}"
           + (f"; past {' and '.join(past)}" if past else ""))


# Where the cycle counter and its two enables live, from the ARMv8-M core definitions.
DCB_DEMCR, DWT_CTRL, DWT_CYCCNT = 0xE000EDFC, 0xE0001000, 0xE0001004
DEMCR_TRCENA, DWT_CYCCNTENA, DWT_NOCYCCNT = 1 << 24, 1 << 0, 1 << 25


def check_connect_on_capture_board(c: Checker, elf: str, slots: int) -> None:
    """A host connecting at 25 MHz to a target that answers at 9, which the default latency's build
    reads right. Elsewhere nothing is measured. On a board that can capture, a capture starting
    later rebuilds for its stall a build that does not serve 9, so the connection has to measure.
    JTAG connections measure there anyway (tools/jtag_latency_test.c, test_stall_connect)."""
    if "swd_sweep_last" not in symbols(elf):
        return
    from cycles import ID_SWJ_SEQUENCE, ID_TRANSFER, ID_VENDOR_SWD_GEN, timed_dpidr_target
    from energy_checks import INTEGRATION_FIELDS, attach_integration

    offsets = shared_offsets(slots, "energy_available", "energy_integration", small_enums=short_enums(elf))
    layout = shared_offsets(slots, *INTEGRATION_FIELDS, small_enums=short_enums(elf),
                            struct_type="struct energy_integration")
    for available in (0, 1):
        probe = timed_dpidr_target(elf, Model(), 9, 25_000_000)
        probe.uc.mem_write(probe.sym["dap_shared"] + offsets["energy_available"], bytes([available]))
        # A capture that is not running, so the measurement's hold has nothing to stop.
        attach_integration(probe, offsets, layout)
        probe.begin()
        probe.command(bytes([ID_SWJ_SEQUENCE, 54]) + bytes([0xFF] * 6 + [0x07]))  # 51 high, 3 low
        reply = probe.command(bytes([ID_TRANSFER, 0, 1, 0x02]))
        probe.end()
        state = probe.command(bytes([ID_VENDOR_SWD_GEN, 0, 0]))
        measurements = int.from_bytes(state[42:46], "little")
        board = "a board that can capture" if available else "a board without capture"
        c.check(reply[1:3] == bytes([1, 1]) and measurements == available and state[2] == (9 if available else 7),
                f"connecting at 25 MHz to latency 9 on {board}: DPIDR read answered {reply[1:7].hex()}, "
                f"latency {state[2]} after {measurements} measurements")
    c.note("a connection on a board that can capture measures a target the default latency reads right")


def check_timestamp(c: Checker, elf: str) -> None:
    """DWT is powered down until TRCENA, and the counter does not run until CYCCNTENA.

    Without both, TIMESTAMP_GET reads zero forever: DAP_SWJ_Pins with a wait spins on a deadline
    that never arrives, and a transfer asked for a timestamp reports nothing.
    """
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    if "board_timestamp_init" not in probe.sym:
        c.note("board_timestamp_init was inlined away; the cycle counter not checked")
        return
    probe.call("board_timestamp_init")
    demcr = int.from_bytes(probe.uc.mem_read(DCB_DEMCR, 4), "little")
    ctrl = int.from_bytes(probe.uc.mem_read(DWT_CTRL, 4), "little")
    trcena = c.check(demcr & DEMCR_TRCENA, "TRCENA is clear, so DWT stays powered down")
    cyccnt = c.check(ctrl & DWT_CYCCNTENA, "CYCCNTENA is clear, so the cycle counter never runs")
    c.note(
        "cycle counter: TRCENA "
        + ("set" if trcena else "CLEAR")
        + ", CYCCNTENA "
        + ("set" if cyccnt else "CLEAR")
    )


def check_commands(c: Checker, elf: str, code_lab: bool, swo: bool) -> None:
    """Every command in COMMANDS has to answer. The claim is that none is a downgrade.

    One caveat worth keeping honest. CPU 0's clock setup never runs here, so the 96 MHz clock reads
    as 0 Hz, SWO_Baudrate makes its rate from the main clock instead, and capture never starts.
    The SWO commands answer, which is what is being checked, but nothing here says SWO works: that
    is tools/swo_ring_test.c for the arithmetic and the bench for the rest. A build without SWO has
    to refuse them.
    """
    probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())
    answered = 0
    commands = {cmd: ops for cmd, ops in DAP_COMMANDS.items() if swo or cmd not in SWO_COMMANDS}
    for cmd in SWO_COMMANDS if not swo else ():
        reply = probe.command(bytes([cmd]) + DAP_COMMANDS[cmd])
        if not reply or reply[0] != ID_DAP_INVALID:
            c.problems.append(f"command {cmd:#04x} should answer ID_DAP_Invalid in a build without SWO")
    for cmd, operands in commands.items():
        reply = probe.command(bytes([cmd]) + operands)
        if not reply:
            c.problems.append(f"command {cmd:#04x} returned nothing")
        elif reply[0] == ID_DAP_INVALID:
            c.problems.append(f"command {cmd:#04x} answered ID_DAP_Invalid")
        elif reply[0] != cmd:
            c.problems.append(f"command {cmd:#04x} echoed {reply[0]:#04x}")
        else:
            answered += 1
    # Anything we do not implement has to say so rather than echo itself back. 0x85 was a reboot;
    # the EP0 request (src/reboot_usb.h) is the only one now, so a stray 0x85 must not reset.
    for cmd in (0x07, 0x85, 0x88, 0x9F):
        reply = probe.command(bytes([cmd, 0, 0, 0]))
        if cmd == 0x07:
            continue  # TransferAbort is handled out of band and answers nothing
        if cmd == 0x88 and code_lab:
            continue  # check_identity verifies the lab's info command
        if not reply or reply[0] != ID_DAP_INVALID:
            c.problems.append(f"command {cmd:#04x} should answer ID_DAP_Invalid")
    c.note(
        f"{answered} of {len(commands)} commands answered, unimplemented ids rejected"
        + (" (SWO answering, capture not started)" if swo else ", SWO not built")
    )


# Counts that would exceed a packet in ARM's unbounded command layer. Sized dispatch must reject
# or shorten them before a handler writes its reply.
OVERFLOW_CASES = [
    ("TransferBlock, 128 words", bytes([0x06, 0]) + u16(128) + bytes([0x0F])),
    ("TransferBlock, 4000 words", bytes([0x06, 0]) + u16(4000) + bytes([0x0F])),
    ("TransferBlock, 65535 words", bytes([0x06, 0]) + u16(65535) + bytes([0x0F])),
    ("Transfer, 255 reads", bytes([0x05, 0, 255]) + bytes([0x0F]) * 255),
    ("Transfer, 255 timestamped reads", bytes([0x05, 0, 255]) + bytes([0x8F]) * 255),
    ("ExecuteCommands, 255 x DAP_Info", bytes([0x7F, 255]) + bytes([0x00, 0xFF]) * 255),
    (
        "ExecuteCommands, 255 x Transfer",
        bytes([0x7F, 255]) + (bytes([0x05, 0, 255]) + bytes([0x0F]) * 255) * 2,
    ),
]


def check_response_bounds(c: Checker, elf: str, packet_size: int) -> None:
    """Fence the reply at the transport limit and check the linked private buffer's size."""
    sym = symbols_with_size(elf)
    if not c.check("response_copy" in sym, "private response buffer missing from bounds checks"):
        return
    size = sym["response_copy"][1]
    c.check(size == packet_size, f"private response buffer is {size} bytes, wanted {packet_size}")
    resp = SCRATCH + 0x400
    ceiling = SRAM[0] + SRAM[1] - 0x1000  # clear of the stack, which is at the top
    if resp + size >= ceiling:
        c.problems.append(f"the response buffer is {size} bytes, too large to fence in the harness")
        return

    worst = 0
    for label, payload in OVERFLOW_CASES:
        probe = build(elf, "swd", True, Model())
        dap_setup(probe)
        probe.command(bytes([0x02, 1]))  # Connect, SWD
        probe.command(bytes([0x11]) + u32(10_000_000))  # SWJ_Clock
        probe.command(bytes([0x04, 0]) + u16(100) + u16(0))  # TransferConfigure
        probe.command(bytes([0x13, 0x00]))  # SWD_Configure
        probe.uc.mem_write(SCRATCH, payload + bytes(0x400 - len(payload)))
        probe.uc.mem_write(resp, b"\xee" * (ceiling - resp))
        try:
            produced = probe.call("DAP_ExecuteCommandSized", SCRATCH, min(len(payload), packet_size), packet_size,
                                  resp) & 0xFFFF
        except SystemExit as exc:
            c.problems.append(f"{label} faulted: {exc}")
            continue
        tail = bytes(probe.uc.mem_read(resp + packet_size, ceiling - resp - packet_size))
        past = len(tail.rstrip(b"\xee"))
        if past:
            c.problems.append(f"{label} wrote {past} bytes past the {packet_size}-byte reply capacity")
        if produced > packet_size:
            c.problems.append(f"{label} returned {produced} bytes for a {packet_size}-byte reply capacity")
        worst = max(worst, produced)
    if not c.problems:
        c.note(
            f"the longest reply {len(OVERFLOW_CASES)} oversized requests could produce is {worst}"
            f" bytes, inside the {size}-byte buffer"
        )


# A request can drive a write or a read past a buffer as well as a reply past one, and these are
# the commands where ARM's handler takes a count from the request and never compares it with
# anything. `DAP_ExecuteCommand` is handed a pointer and no length, so no handler can know how
# much of the packet actually arrived.
REQUEST_CASES = [
    # 255 devices into eight-element ir_length, ir_before and ir_after arrays.
    ("JTAG_Configure, 255 devices", bytes([0x15, 255]) + bytes([4]) * 255),
    # 255 sequences of 64 clocks: nine request bytes each, out of a 512-byte packet.
    ("SWD_Sequence, 255 x 64 clocks", bytes([0x1D, 255]) + bytes([0x00] + [0xAA] * 8) * 255),
    ("JTAG_Sequence, 255 x 64 clocks", bytes([0x14, 255]) + bytes([0x00] + [0xAA] * 8) * 255),
]


def check_request_bounds(c: Checker, elf: str, packet_size: int) -> None:
    """Nothing a host can send may write past DAP_Data or read past the request buffer.

    A different class from the reply check above: these counts drive a write into a fixed array,
    or a walk forward through the request. A JTAG chain of 255 devices is a write past three
    eight-element arrays, and a 255-sequence list walks about 2 KB forward through a 512-byte
    request buffer and clocks whatever is there onto the wire.

    **The refusal is what this check rests on, not the memory snapshot.** Unbuilt, the array write
    is undefined once the index reaches 8 and GCC uses that to bound the loop, so the snapshot
    stays clean even on an image with no guard at all. It is kept because another compiler, or
    another optimisation level, need not make the same choice.
    """
    sym = symbols_with_size(elf)
    if "DAP_Data" not in sym:
        c.note("DAP_Data was folded away; its bounds not checked")
        return
    base, size = sym["DAP_Data"]
    watch = 0x800

    for label, payload in REQUEST_CASES:
        probe = build(elf, "swd", True, Model())
        dap_setup(probe)
        probe.command(bytes([0x02, 1]))  # Connect, SWD
        probe.uc.mem_write(SCRATCH, payload[:0x400] + bytes(max(0, 0x400 - len(payload))))
        before = bytes(probe.uc.mem_read(base + size, watch))
        resp = SCRATCH + 0x400
        try:
            probe.call("DAP_ExecuteCommandSized", SCRATCH, min(len(payload), packet_size), packet_size, resp)
        except SystemExit as exc:
            c.problems.append(f"{label} faulted: {exc}")
            continue
        after = bytes(probe.uc.mem_read(base + size, watch))
        expected = bytearray(before)
        if payload[0] == 0x15 and "jtag_gen_command" in sym:
            # Rejected JTAG configurations still update the dispatch command marker.
            marker = sym["jtag_gen_command"][0] - base - size
            if 0 <= marker < watch:
                expected[marker] = payload[0]
        if expected != after:
            spoiled = sum(1 for x, y in zip(expected, after) if x != y)
            c.problems.append(f"{label} changed {spoiled} bytes past the {size}-byte DAP_Data")
        reply = bytes(probe.uc.mem_read(resp, 2))
        if reply[0] != ID_DAP_INVALID and (reply[0] != payload[0] or reply[1] != 0xFF):
            c.problems.append(f"{label} answered {reply.hex()}, wanted the command id then DAP_ERROR")
    if not c.problems:
        c.note(f"{len(REQUEST_CASES)} oversized requests refused; neighbouring writes limited to the command marker")


def check_bounded_commands(c: Checker, elf: str, slots: int) -> None:
    sym = symbols_with_size(elf)
    stage = shared_offsets(slots, "response", "response[1]", small_enums=short_enums(elf),
                           struct_type="dap_stage_t")
    stride = stage["response[1]"] - stage["response"]
    buffers = [sym["response_copy"][0]] + [sym["dap_stage"][0] + stage["response"] + stride * i
                                         for i in range(slots)]
    for capacity, response in ((capacity, buffer) for capacity in (64, 512) for buffer in buffers):
        probe = build(elf, "swd", True, Model())
        dap_setup(probe)
        probe.command(bytes([2, 1]))
        probe.command(bytes([0x11]) + u32(10_000_000))
        state, state_size = sym["DAP_Data"]

        def execute(payload: bytes, received: int | None = None, *, legacy: bool = False) -> bytes:
            probe.uc.mem_write(SCRATCH, payload + bytes(1024 - len(payload)))
            probe.uc.mem_write(response, bytes([0xee]) * capacity)
            guard = bytes(probe.uc.mem_read(response + capacity, 32))
            if legacy:
                result = probe.call("DAP_ExecuteCommand", SCRATCH, response)
            else:
                result = probe.call("DAP_ExecuteCommandSized", SCRATCH,
                                    len(payload) if received is None else received, capacity, response)
            c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "bounded command did not return")
            produced = result & 0xffff
            c.check(produced <= capacity, "bounded command exceeded transport capacity")
            c.check(bytes(probe.uc.mem_read(response + capacity, 32)) == guard,
                    "bounded command changed reply guard")
            return bytes(probe.uc.mem_read(response, min(produced, capacity)))

        for stale_hz in (1_000_000, 5_000_000):
            before = bytes(probe.uc.mem_read(state, state_size))
            c.check(execute(bytes([0x11]) + u32(stale_hz), 1) == b"\xff", "short clock was accepted")
            c.check(bytes(probe.uc.mem_read(state, state_size)) == before, "short clock changed DAP state")
            for symbol in ("swd_gen_clock_hz", "jtag_gen_clock_hz"):
                if symbol in probe.sym:
                    c.check(bytes(probe.uc.mem_read(probe.sym[symbol], 4)) == u32(10_000_000),
                            "short clock changed generator clock")
        for payload in (bytes([6, 0, 1, 0, 0]), bytes([0x1d, 1, 8])):
            before = bytes(probe.uc.mem_read(state, state_size))
            c.check(execute(payload) == b"\xff", "truncated transfer/sequence accepted")
            c.check(bytes(probe.uc.mem_read(state, state_size)) == before, "truncated command changed state")
        reply = execute(bytes([6, 0, 0xff, 0xff, 2]))
        c.check(len(reply) == capacity and int.from_bytes(reply[1:3], "little") == (capacity - 4) // 4,
                "block read did not match transport ceiling")
        if capacity == 512 and "DAP_ExecuteCommand" in probe.sym:
            c.check(execute(bytes([6, 0, 0xff, 0xff, 2]), legacy=True) == reply,
                    "legacy entry point bypassed the packet ceiling")
        if capacity == 64:
            c.check(execute(bytes([0x1d, 8]) + bytes([0x80]) * 8) == b"\xff",
                    "oversized input sequence accepted")
        for prefix in (b"", bytes([2, 1]), bytes([2, 1, 2, 1])):
            payload = bytes([0x7f, len(prefix) // 2 + 2]) + prefix + bytes([0x11, 2, 2])
            reply = execute(payload)
            c.check(reply == bytes([0x7f, len(prefix) // 2]) + bytes([2, 1]) * (len(prefix) // 2),
                    "malformed batch lost prior replies or counted rejected command")
            c.check(bytes(probe.uc.mem_read(state, 1)) == b"\x01", "rejected batch tail changed port")
            c.check(execute(bytes([2, 1])) == bytes([2, 1]), "command after malformed batch failed")
        if capacity == 64:
            payload = bytes([0x7f, 17]) + bytes([0, 0xff]) * 14 + bytes([0, 0xfe, 2, 1, 2, 2])
            reply = execute(payload)
            c.check(len(reply) == 63 and reply[:2] == bytes([0x7f, 16]), "full batch reply framing")
            c.check(bytes(probe.uc.mem_read(state, 1)) == b"\x01", "overflowing Connect executed")
        c.check(execute(bytes([0x7f, 1])) == bytes([0x7f, 0]), "empty batch payload accepted")
        disconnects = (capacity - 6) // 2
        payload = bytes([0x7f, disconnects + 2]) + bytes([3]) * disconnects + bytes([0, 0xff, 2, 1])
        reply = execute(payload)
        c.check(reply == bytes([0x7f, disconnects + 1]) + bytes([3, 0]) * disconnects + bytes([0, 2]) + u16(capacity),
                "exact-capacity batch reply or executed count changed")
        c.check(bytes(probe.uc.mem_read(state, 1)) == b"\x00", "command after full batch reply executed")
        c.check(execute(bytes([2, 1])) == bytes([2, 1]), "short reply after full batch failed")
    c.note("HID/bulk boundaries, malformed commands and full batches in private and every staged reply buffer")
    if "DAP_ExecuteCommand" not in sym:
        c.note("legacy DAP_ExecuteCommand is unlinked; its wrapper is not exercised")


def check_jtag_chain(c: Checker, elf: str) -> None:
    probe = build(elf, "jtag", True, Model())
    dap_setup(probe)
    probe.command(bytes([2, 2]))
    probe.command(bytes([0x11]) + u32(10_000_000))
    state = symbols_with_size(elf)["DAP_Data"]
    for valid in (bytes([0x15, 1, 4]), bytes([0x15, 8]) + bytes([255]) * 8):
        c.check(probe.command(valid) == bytes([0x15, 0]), "valid JTAG chain rejected")
        before = bytes(probe.uc.mem_read(*state))
        for malformed in (bytes([0x15, 1, 0]), bytes([0x15, 3, 4, 4, 0])):
            c.check(probe.command(malformed) == bytes([0x15, 0xff]), "zero-length IR accepted")
            c.check(bytes(probe.uc.mem_read(*state)) == before, "rejected chain changed prior TAP configuration")
        # This command selects IR; the former zero-length countdown prevented it from returning.
        reply = probe.command(bytes([5, 0, 1, 2]))
        c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and reply[0] == 5,
                "transfer after rejected chain did not return")
    c.check(probe.command(bytes([0x15, 0])) == bytes([0x15, 0]), "empty chain reset rejected")
    c.note("zero-length JTAG IRs rejected without changing the prior chain, including long-IR chains")


def check_energy_descriptors(c: Checker, elf: str, slots: int) -> None:
    sym = symbols_with_size(elf)
    if not c.check("capture_ring_init" in sym, "energy ring initialization missing"):
        return
    from energy_checks import INTEGRATION_AT, INTEGRATION_FIELDS

    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    probe.sym["capture_ring_init"] = sym["capture_ring_init"][0]
    offsets = shared_offsets(slots, "dma_table", "energy_integration", small_enums=short_enums(elf))
    layout = shared_offsets(slots, *INTEGRATION_FIELDS, small_enums=short_enums(elf),
                            struct_type="struct energy_integration")
    probe.uc.mem_write(sym["dap_shared"][0] + offsets["energy_integration"], struct.pack("<I", INTEGRATION_AT))
    entry = sym["dap_shared"][0] + offsets["dma_table"] + 21 * 16
    ring, size = INTEGRATION_AT + layout["ring"], layout["size"] - layout["ring"]
    descriptors, descriptor_bytes = INTEGRATION_AT + layout["descriptors"], layout["ring"] - layout["descriptors"]
    c.check(size == 4096 and descriptor_bytes == 32, "energy ring/descriptor dimensions changed")
    for words in (512, 64, 16):
        for restart in range(2):
            probe.uc.mem_write(descriptors, bytes([0xee]) * descriptor_bytes)
            probe.uc.mem_write(entry, bytes([0xee]) * 16)
            probe.call("capture_ring_init", words)
            c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "energy ring init did not return")
            published = struct.unpack("<I", probe.uc.mem_read(INTEGRATION_AT + layout["segment_words"], 4))[0]
            c.check(published == words, f"{words}-word segments published as {published}")
            at = entry
            destinations = []
            for i in range(5):
                config, source, destination, link = struct.unpack("<4I", probe.uc.mem_read(at, 16))
                destinations.append((destination + 4 - ring) // (words * 4) - 1)
                c.check(source == struct.unpack("<I", probe.uc.mem_read(descriptors + 4, 4))[0],
                        "energy descriptor changed ADC source")
                c.check(destination == ring + (i % 2 + 1) * words * 4 - 4,
                        f"{words}-word segments: energy destination/stride mismatch")
                if i != 0:
                    c.check((config >> 16) & 0x3ff == words - 1 and (config >> 8) & 3 == 2,
                            f"{words}-word segments: energy descriptor count/word width mismatch")
                    # The half it fills raises its own flag: SETINTA (bit 4) the first, SETINTB (bit 5) the second.
                    c.check((config >> 4) & 3 == (1 if i % 2 == 0 else 2),
                            f"{words}-word segments: step {i} fills half {i % 2} and raises flags {(config >> 4) & 3}")
                c.check(link in (descriptors, descriptors + 16) and link % 16 == 0,
                        f"energy descriptor link {link:#x} outside the ring or off 16 bytes")
                at = link
            c.check(destinations == [0, 1, 0, 1, 0], f"{words}-word energy startup/restart {restart}: {destinations}")
    c.note("actual energy descriptor links alternate segments from the first transfer, including restart, at"
           " 512-, 64- and 16-word segments, each half raising its own flag")


def check_allocator_trap(c: Checker, elf: str) -> None:
    if os.environ.get("MCULINK_CHECK_ALLOCATIONS") != "1":
        return
    from cycles import AllocationAttempt
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    if not probe.allocator_entries:
        c.note(f"{elf}: no allocator entry linked")
        return
    for address, name in probe.allocator_entries.items():
        try:
            probe.call(name, 0, 16, timeout_insns=100)
        except AllocationAttempt:
            continue
        c.check(False, f"allocator trap missed {name} at {address:#x}")
    c.note(f"{elf}: allocator trap falsified at {len(probe.allocator_entries)} linked entries; active for all image checks")


def check_stream_storage(c: Checker, elf0: str, elf1: str) -> None:
    sym0, sym1 = symbols_with_size(elf0), symbols(elf1)
    at, size = sym0["energy_stream_slots"]
    c.check((at, size) == (0x20040000, 8192), f"stream slots at {at:#x}, {size} bytes")
    c.check(sections(elf0)[".stream_slots"][:2] == (8192, at), "stream slots are not in their reserved section")
    c.check(sym1.get("energy_stream_slots") == at, "cores disagree on singleton stream storage")
    c.check(sym1["__data_start__"] == 0x20014000, "CPU1 data does not follow its stack")
    c.check(sym1["__bss_end__"] <= 0x2003E000, "CPU1 data overlaps staged commands")
    c.note("16 stream message slots in SRAM4; CPU1 stack retains 16 KiB in SRAM1")


def check_energy_control(c: Checker, elf0: str, elf1: str, slots: int) -> None:
    from energy_checks import INTEGRATION_FIELDS, check_calibration, check_control, check_hold, check_samples

    fields = ("energy_control_request", "energy_control_ack", "energy_segments", "swo_segments",
              "energy_dma_error", "energy_integration", "energy_available", "energy_accumulator_pj",
              "energy_accumulator_pairs", "energy_accumulator_fraction_q32", "energy_dma_sequence",
              "energy_range_retry_pending", "energy_range_retry_enabled")
    offsets = [shared_offsets(slots, *fields, small_enums=short_enums(elf)) for elf in (elf0, elf1)]
    # CPU 0 owns the struct and CPU 1 writes into it, so both compilers' layouts have to agree.
    layouts = [shared_offsets(slots, *INTEGRATION_FIELDS, small_enums=short_enums(elf),
                              struct_type="struct energy_integration") for elf in (elf0, elf1)]
    if not c.check(layouts[0] == layouts[1], f"struct energy_integration differs: CPU 0 {layouts[0]}, CPU 1 {layouts[1]}"):
        return
    symbols0, symbols1 = symbols_with_size(elf0), symbols_with_size(elf1)
    # DMA0 reads a reload descriptor only on 16 bytes, and writes the ring a word at a time.
    at = symbols0["energy_integration"][0]
    c.check((at + layouts[0]["descriptors"]) % 16 == 0 and (at + layouts[0]["ring"]) % 4 == 0,
            f"energy descriptors at {at + layouts[0]['descriptors']:#x}, ring at {at + layouts[0]['ring']:#x}")
    check_control(c, elf0, elf1, symbols0, symbols1, *offsets, layouts[0])
    check_samples(c, elf0, elf1, symbols0, symbols1, offsets[1], layouts[0], model_syscon)
    check_calibration(c, elf1, symbols1, offsets[1], model_syscon)
    check_hold(c, elf1, symbols1, offsets[1], layouts[0])


def check_console_recovery(c: Checker, elf: str, slots: int) -> None:
    from vcom_checks import check_dma, check_line, check_ring_flags

    offsets = shared_offsets(slots, "dma_table", "swo_segments", "swo_lost", small_enums=short_enums(elf))
    check_dma(c, elf, symbols_with_size(elf), offsets)
    check_line(c, elf, symbols_with_size(elf))
    check_ring_flags(c, elf, symbols_with_size(elf), offsets)


def check_hid_padding(c: Checker, elf: str, slots: int) -> None:
    from energy_checks import machine

    sym = symbols_with_size(elf)
    if not c.check("dap_task" in sym, "CPU1 command task missing from HID checks"):
        return
    names = ("hid_request_len", "hid_request_seq", "hid_request_stage", "hid_request_head",
             "hid_request", "hid_response_tail", "hid_response_len")
    fields = shared_offsets(slots, *names, small_enums=short_enums(elf))
    stage = shared_offsets(slots, "hid_request", "hid_response", "hid_response[1]",
                           small_enums=short_enums(elf), struct_type="dap_stage_t")
    stride = stage["hid_response[1]"] - stage["hid_response"]
    own, own_size = sym["response_copy"]
    for staged in (False, True):
        probe = machine(elf, sym)
        shared = probe.sym["dap_shared"]
        staging = probe.sym["dap_stage"]
        # Each round reuses every reply slot. A one-byte Info retains a stale ff selector.
        for payload, prefix in ((b"\x00\x02", None), (b"\x7e", b"\xff"),
                                (b"\x00", b"\xff"), (b"\x00\xff", b"\x00\x02\x40\x00")):
            for slot in range(slots):
                probe.uc.mem_write(shared + fields["hid_request_head"], bytes([slot]))
                probe.uc.mem_write(shared + fields["hid_response_tail"], bytes([slot]))
                probe.uc.mem_write(shared + fields["hid_request_len"] + slot * 2, u16(len(payload)))
                probe.uc.mem_write(shared + fields["hid_request_seq"] + slot * 4, u32(1))
                probe.uc.mem_write(shared + fields["hid_request_stage"] + slot, bytes([slot + 1 if staged else 0]))
                probe.uc.mem_write(shared + fields["hid_response_len"] + slot * 2, u16(0))
                request = staging + stage["hid_request"] + 512 * slot if staged else shared + fields["hid_request"] + 64 * slot
                probe.uc.mem_write(request, b"\xa5\xff" + b"\xa5" * 62)
                probe.uc.mem_write(request, payload)
                response = staging + stage["hid_response"] + stride * slot
                if prefix is None:
                    probe.uc.mem_write(response, b"\xa5" * 64)
                probe.uc.mem_write(own, b"\x5a" * own_size)
                probe.call("dap_task")
                c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "HID command task did not return")
                reply = bytes(probe.uc.mem_read(response, 64))
                if prefix is None:
                    c.check(reply[0] == 0 and reply[1] > 4, "long HID fixture did not produce a string")
                    meaningful = 2 + reply[1]
                else:
                    meaningful = len(prefix)
                    c.check(reply[:meaningful] == prefix, "HID payload or packet-size override changed")
                c.check(reply[meaningful:] == bytes(64 - meaningful), "HID reply leaked its previous tail")
                c.check(bytes(probe.uc.mem_read(shared + fields["hid_response_len"] + slot * 2, 2)) == u16(64),
                        "HID reply did not publish a full report")
                c.check(bytes(probe.uc.mem_read(own, 64)) == reply,
                        "HID reply did not run in CPU 1's own packet-long buffer")
    c.note("HID long/short/malformed Info replies, zero padding and packet size across both staging modes and all slots")


def check_usb_transport(c: Checker, elf0: str, elf1: str, slots: int, code_lab: bool = False) -> None:
    from usb_checks import (check_aux, check_aux_stream, check_completion_chains, check_descriptors, check_full_speed,
                            check_interfaces_apart, check_rechirp, check_transport, check_unconfigured)
    from usb_idle_checks import check_idle, check_idle_rearm, check_idle_reply, check_idle_serviced
    from usb_lifecycle import check_sequences

    fields = ("transfer_abort", "request", "request_tail", "rx_armed_slot", "next_request_seq", "first_live_seq",
              "response_len", "response_tail", "response_seq", "response_head", "hid_response_len",
              "hid_response_tail", "hid_response_seq", "hid_response_head", "board", "jumpers", "bulk_packet_size", "energy_range_retry_pending", "energy_range_retry_enabled")
    offsets = shared_offsets(slots, *fields, small_enums=short_enums(elf0))
    check_idle(c, elf0, symbols_with_size(elf0), code_lab)
    check_idle_rearm(c, elf0, symbols_with_size(elf0), offsets)
    check_idle_reply(c, elf0, symbols_with_size(elf0), offsets)
    check_idle_serviced(c, elf0, symbols_with_size(elf0))
    check_unconfigured(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets, slots)
    check_transport(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets, slots)
    check_interfaces_apart(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets, slots)
    check_completion_chains(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets)
    check_full_speed(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets)
    check_descriptors(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets)
    check_rechirp(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets)
    check_sequences(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets, slots)
    check_aux(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets)
    stream_shared = shared_offsets(slots, "energy_integration", "energy_available", "energy_dma_error",
                                   "energy_control_ack", small_enums=short_enums(elf0))
    stream_layout = shared_offsets(slots, "running", "setting", "full_scale_mv", "previous_low", "previous_known",
                                   "previous_na", "switch_weight_q16", "rearm_between_pairs", "low_range",
                                   "high_range", "decoder", "stream.written", "stream.sent", "stream.length",
                                   "stream.active", "stream.next_sequence", "stream.period",
                                   "stream.clock_hz", "stream.timeline_pending", "capture_id", "capture_id_sequence", small_enums=short_enums(elf0),
                                   struct_type="struct energy_integration")
    check_aux_stream(c, elf0, elf1, symbols_with_size(elf0), symbols_with_size(elf1), offsets | stream_shared,
                     stream_layout)
    from dio_edges_checks import check_edge_rest, check_edge_spi
    edge_shared = shared_offsets(slots, "energy_control_ack", "dio_spi_turns", small_enums=short_enums(elf0))
    edge_layout = shared_offsets(slots, "stream", "stream.active", "stream.edge_mask", "stream.timeline_pending",
                                 "stream.edge_begins", "stream.edge_entries_written", "stream.edge_entries_read",
                                 "stream.edge_entries", small_enums=short_enums(elf0),
                                 struct_type="struct energy_integration")
    check_edge_rest(c, elf0, symbols_with_size(elf0), edge_shared, edge_layout)
    check_edge_spi(c, elf0, symbols_with_size(elf0), edge_shared, edge_layout)


def written_before_branch(elf: str, function: str, register: str) -> int | None:
    """The value `function` moves into a special register with MSR before it first branches, or
    None if it branches first or loads the value some other way than a literal or immediate."""
    listing = subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", elf],
                             capture_output=True, text=True, check=True).stdout
    words, body, inside = {}, [], False
    for line in listing.splitlines():
        header = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if header:
            inside = header.group(1) == function
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+(\S+)\s*([^@]*?)\s*(?:@ \(([0-9a-f]+) .*)?$", line)
        if not m:
            continue
        if m.group(2) == ".word":
            words[int(m.group(1), 16)] = int(m.group(3), 16)
        elif inside:
            body.append((m.group(2), [op.strip() for op in m.group(3).split(",")], m.group(4)))
    held: dict[str, int | None] = {}
    for mnemonic, operands, literal in body:
        base = mnemonic.split(".")[0]
        if base == "msr" and operands[0] == register:
            return held.get(operands[1])
        if re.match(r"^(b|bl|blx|bx|cbn?z)$", base) or re.match(r"^b(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)$", base):
            return None
        if base.startswith(("str", "cmp", "cmn", "tst", "teq", "msr", "push", "it", "cps", "dsb", "isb", "dmb")):
            continue
        if base == "ldr" and literal is not None:
            held[operands[0]] = words.get(int(literal, 16))
        elif base in ("mov", "movs", "movw") and operands[1].startswith("#"):
            held[operands[0]] = int(operands[1][1:], 0)
        else:
            held[operands[0]] = None
    return None


def check_stacks(c: Checker, elf0: str, elf1: str) -> None:
    cpu0, cpu1 = symbols(elf0), symbols(elf1)
    c.check(cpu0["__StackTop"] == 0x20010000 and cpu0["__StackLimit"] == 0x2000f000,
            "CPU0 does not reserve its configured 4 KiB stack")
    c.check(cpu0["__HeapLimit"] == cpu0["__HeapBase"], "CPU0 unexpectedly reserves a heap")
    c.check(cpu0["__HeapLimit"] <= cpu0["__StackLimit"], "CPU0 data/heap overlaps its reserved stack")
    c.check(cpu1.get("__core1_stack_bottom") == 0x20010000 and cpu1["__core1_stack_top"] == 0x20014000,
            "CPU1 does not reserve its configured 16 KiB stack")
    c.note("linked stack reservations: CPU0 4 KiB, CPU1 16 KiB, no CPU0 data/heap overlap")
    # CPU 0's limit comes from NXP's startup file, which a TinyUSB update replaces.
    limits = ((elf0, "Reset_Handler", "MSPLIM", cpu0["__StackLimit"], "CPU0's reset"),
              (elf1, "core1_reset", "MSPLIM", cpu1["__core1_stack_bottom"], "CPU1's reset"),
              # After an overflow MSP sits at the limit, and a push in the handler locks the core up.
              (elf1, "core1_default_handler", "MSP", cpu1["__core1_stack_top"], "CPU1's fault handler"))
    found = [(what, register, want, written_before_branch(elf, function, register))
             for elf, function, register, want, what in limits]
    for what, register, want, got in found:
        c.check(got == want, f"{what} does not set {register} to {want:#x} before it branches"
                             f" ({'nothing found' if got is None else f'sets {got:#x}'})")
    if all(got == want for _, _, want, got in found):
        c.note("stack limits: CPU0 and CPU1 set MSPLIM to their stacks' bottoms at reset;"
               " CPU1's fault handler restarts its stack")


def check_identity(c: Checker, elf: str, code_lab: bool) -> None:
    """A host has to be able to recognise this firmware and tell two builds of it apart.

    Three things carry it, and they have to agree. The vendor command answers a magic, a
    capability bitmap and the build string, for a host that knows us. DAP_Info's product firmware
    version answers the same build string, for one that does not -- pyOCD and OpenOCD can read it
    without knowing anything about this image. And the protocol version stays ARM's, because it
    describes CMSIS-DAP rather than us.
    """
    probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())

    reply = probe.command(bytes([0x82]))
    if len(reply) < 9 or reply[0] != 0x82:
        c.problems.append(f"vendor 0x82 answered {reply.hex()}, which is not an identity")
        return
    magic, version, flags, length = reply[1:6], reply[6], reply[7], reply[8]
    build = reply[9 : 9 + length].decode("ascii", "replace")
    if magic != b"CJTAG":
        c.problems.append(f"vendor 0x82 magic is {magic!r}, wanted b'CJTAG'")
    if version != 6:
        c.problems.append(f"vendor 0x82 format version is {version}, wanted 6")
    # The plain board, then the scan formats it frames: OScan1, 2, 5 and 6 (SCNFMT 9, 10, 13, 14).
    formats = ((1 << 9) | (1 << 10) | (1 << 13) | (1 << 14)).to_bytes(4, "little")
    if reply[9 + length : 18 + length] != bytes([0]) + formats + bytes(4):
        c.problems.append(
            f"vendor 0x82 ends {reply[9 + length :].hex()!r}, wanted the plain board's 00 {formats.hex()} 00000000"
        )
    if not flags & 0x01:
        c.problems.append(f"vendor 0x82 flags {flags:#04x} do not claim OScan1, which is the point")
    if flags & 0x02:
        c.problems.append("vendor 0x82 says OScan1 is active before anything turned it on")
    if not build:
        c.problems.append("vendor 0x82 carries no build string, so two builds look identical")

    # 0x83 mode 4 limits the scan formats offered: OScan1 alone, then every one again. A limited
    # format is refused by mode 3, and 0x82 reports the limit.
    def offered() -> bytes:
        reply = probe.command(bytes([0x82]))
        return reply[10 + reply[8] : 14 + reply[8]]
    limit = probe.command(bytes([0x83, 4]) + (1 << 9).to_bytes(4, "little"))
    refused = probe.command(bytes([0x83, 3, 10, 1]))
    limited = offered()
    probe.command(bytes([0x83, 4]) + bytes([0xFF] * 4))
    restored = offered()
    probe.command(bytes([0x83, 0]))
    if limit[:2] != bytes([0x83, 0]) or refused[:2] != bytes([0x83, 0xFF]):
        c.problems.append(f"vendor 0x83 mode 4 answered {limit.hex()}, then mode 3 OScan2 {refused.hex()}")
    if limited != (1 << 9).to_bytes(4, "little") or restored != formats:
        c.problems.append(f"vendor 0x82 offered {limited.hex()} limited and {restored.hex()} restored")

    # DAP_Info 0x09, the product firmware version. ARM leaves it a stub returning nothing.
    info = probe.command(bytes([0x00, 0x09]))
    reported = info[2 : 2 + info[1]].rstrip(b"\x00").decode("ascii", "replace") if len(info) > 1 else ""
    if not reported:
        c.problems.append("DAP_Info 0x09 is empty, so a host that does not know us cannot tell builds apart")
    elif reported != build:
        c.problems.append(f"DAP_Info 0x09 says {reported!r} and vendor 0x82 says {build!r}")

    # The protocol version is ARM's and must not be repurposed as ours.
    proto = probe.command(bytes([0x00, 0x04]))
    proto_str = proto[2 : 2 + proto[1]].rstrip(b"\x00").decode("ascii", "replace") if len(proto) > 1 else ""
    if proto_str == build:
        c.problems.append("DAP_Info 0x04 carries the build string; it is the CMSIS-DAP version")

    def info_str(sub: int) -> str:
        reply = probe.command(bytes([0x00, sub]))
        if len(reply) < 2:
            return ""
        return reply[2 : 2 + reply[1]].rstrip(b"\x00").decode("ascii", "replace")

    vendor, product = info_str(0x01), info_str(0x02)
    # The specification stipulates that the product string contain "CMSIS-DAP", and for a v1 HID
    # probe it is the only thing OpenOCD matches on -- see cmsis_dap_usb_hid.c, which quotes the
    # spec. probe-rs matches the interface string instead, so a rename would break other tools
    # first and look like a host bug.
    if "CMSIS-DAP" not in product:
        c.problems.append(f"product string {product!r} has no 'CMSIS-DAP'; the spec requires it")
    # NXP built the hardware, not this firmware. The USB vendor id stays theirs because hosts bind
    # on it; the string must not.
    if "NXP" in vendor:
        c.problems.append(f"vendor string {vendor!r} claims NXP, who did not write this")
    if not vendor:
        c.problems.append("vendor string is empty")
    c.note(f"strings: vendor {vendor!r}, product {product!r}")

    lab = probe.command(bytes([0x88, 0]))
    if code_lab:
        if len(lab) != 19 or lab[0] != 0x88 or lab[1] != 0 or lab[2] != 2:
            c.problems.append(f"vendor 0x88 lab info answered {lab.hex()}, wanted two arenas")
        else:
            c.note("vendor 0x88 code lab: two arenas reported")
    elif not lab or lab[0] != ID_DAP_INVALID:
        c.problems.append("vendor 0x88 should answer ID_DAP_Invalid until something implements it")

    # J8 hands the marker's pins to the host. Taking all four and giving them back has to work,
    # because that is the sequence that identifies which pad an analyser is clipped to.
    for mask, levels in ((0x0F, 0x0F), (0x0F, 0x00), (0x00, 0x00)):
        reply = probe.command(bytes([0x84, mask, levels]))
        if len(reply) < 3 or reply[0] != 0x84 or reply[1] != 0x00:
            c.problems.append(f"vendor 0x84 {mask:#04x}/{levels:#04x} answered {reply.hex()}")

    c.note(f"identity: vendor 0x82 magic CJTAG v{version} flags {flags:#04x}, build {build!r}")


# The straps CPU 0 reads (src/board_straps.c), as byte registers of the ordinary GPIO: each reads
# 1 with its pull-up, 0 when the board or a jumper pulls it low.
STRAPS = {"HW_VER_2": (1, 30), "HW_VER_3": (1, 4), "HW_VER_4": (1, 5), "HW_VER_6": (0, 18), "HW_VER_7": (0, 27)}
# What each board's straps read, and its BOARD_KIND_*: high and high on the plain board, whose
# package leaves both unbonded; HW_VER_3 high and HW_VER_4 low on a Pro; both low on an MCU-Link
# built into an evaluation board.
BOARDS = (("plain", {}, 0), ("Pro", {"HW_VER_4": 0}, 1), ("on-board", {"HW_VER_3": 0, "HW_VER_4": 0}, 2))
# The jumpers, by the strap each pulls low, and its BOARD_JUMPER_* bit.
JUMPERS = (("VCOM off", "HW_VER_6", 1), ("SWD off", "HW_VER_7", 2), ("USB-SIO off", "HW_VER_2", 4))
BOARD_PRO = 1
# The boards the image declares for verge (src/board_straps.c): the plain MCU-Link and the Pro.
IMAGE_BOARDS = (1 << 0) | (1 << BOARD_PRO)


def board_states():
    """Every board with every combination of jumpers: its name, the strap levels, the board and
    the jumper bits CPU 0 should read from them."""
    for name, board_straps, board in BOARDS:
        for fitted in range(1 << len(JUMPERS)):
            levels = {strap: 1 for strap in STRAPS} | board_straps
            jumpers, labels = 0, []
            for i, (label, strap, bit) in enumerate(JUMPERS):
                if fitted & (1 << i):
                    levels[strap] = 0
                    jumpers |= bit
                    labels.append(label)
            yield f"{name}{', ' if labels else ''}{', '.join(labels)}", levels, board, jumpers


def set_straps(probe: Machine, levels: dict[str, int]) -> None:
    for strap, (port, pin) in STRAPS.items():
        probe.uc.mem_write(ORDINARY_GPIO + REG_B0 + 32 * port + pin, bytes([levels[strap]]))


def usb_string(probe: Machine, index: int) -> str:
    at = probe.call("tud_descriptor_string_cb", index, 0x0409)
    length = probe.uc.mem_read(at, 1)[0]
    return bytes(probe.uc.mem_read(at + 2, length - 2)).decode("utf-16-le")


def expected_layout(board: int, jumpers: int) -> tuple[int, set[int], int]:
    """Interfaces, endpoints and functions of the one layout: CMSIS-DAP v2 on EP1 and v1 on EP3,
    VCOM on EP2 with its notification at 0x86, VCOM2 on EP4 with its notification at 0x87, and Verge
    Aux on EP5, last. A function the board lacks or a jumper turns off is left out: the SWD jumper
    takes both CMSIS-DAP interfaces, as stock's does, and leaves Verge Aux. The functions are
    bcdDevice's low byte: 1 debug, 2 VCOM, 4 VCOM2, 8 Verge Aux."""
    interfaces, endpoints, functions = 0, set(), 0
    if not jumpers & 2:
        interfaces += 2
        endpoints |= {0x01, 0x81, 0x03, 0x83}
        functions |= 1
    if not jumpers & 1:
        interfaces += 2
        endpoints |= {0x02, 0x82, 0x86}
        functions |= 2
    if board == BOARD_PRO:
        interfaces += 3
        endpoints |= {0x04, 0x84, 0x87, 0x05, 0x85}
        functions |= 4 | 8
    return interfaces, endpoints, functions


# The vendor interfaces Windows binds WinUSB to: bcdDevice's function bit, the interface string, the GUID.
WINUSB_FUNCTIONS = ((1, "CMSIS-DAP v2", "{CDB3B5AD-293B-4663-AA36-1AAE46463776}"),
                    (8, "Verge Aux", "{A297EA18-39D8-41BE-B2E8-884CEC8FE819}"))


def ms_os_bindings(c: Checker, d: bytes) -> list[tuple[int, str]]:
    """The MS OS 2.0 set's function subsets as (first interface, DeviceInterfaceGUID), with every
    nested length checked against where its bytes end."""
    if not c.check(len(d) >= 18 and d[2] == 0 and struct.unpack_from("<H", d, 8)[0] == len(d),
                   f"MS OS 2.0 set header does not give its own {len(d)}-byte length"):
        return []
    c.check(d[12] == 1 and struct.unpack_from("<H", d, 16)[0] == len(d) - 10,
            "MS OS 2.0 configuration subset does not cover the rest of the set")
    found, i = [], 18
    while i + 8 <= len(d):
        length, kind, first, subset = d[i], d[i + 2], d[i + 4], struct.unpack_from("<H", d, i + 6)[0]
        if not c.check(length == 8 and kind == 2 and 8 <= subset <= len(d) - i,
                       f"MS OS 2.0 function subset at {i} is malformed"):
            return found
        guid, j = None, i + 8
        while j + 4 <= i + subset:
            (feature, ftype) = struct.unpack_from("<HH", d, j)
            if not c.check(feature >= 4, f"MS OS 2.0 feature at {j} claims {feature} bytes"):
                return found
            if ftype == 3:
                c.check(d[j + 4 : j + 12] == b"WINUSB\0\0", "MS OS 2.0 compatible id is not WINUSB")
            if ftype == 4:
                name_len = struct.unpack_from("<H", d, j + 6)[0]
                data_len = struct.unpack_from("<H", d, j + 8 + name_len)[0]
                c.check(feature == 10 + name_len + data_len, "MS OS 2.0 registry property length disagrees")
                name = d[j + 8 : j + 8 + name_len].decode("utf-16-le").rstrip("\0")
                if name == "DeviceInterfaceGUIDs":
                    guid = d[j + 10 + name_len : j + 10 + name_len + data_len].decode("utf-16-le").rstrip("\0")
            j += feature
        c.check(j == i + subset, f"MS OS 2.0 function subset at {i} says {subset} bytes, its features take {j - i}")
        found.append((first, guid))
        i += subset
    c.check(i == len(d), f"MS OS 2.0 set has {len(d) - i} bytes after its last function subset")
    return found


def check_straps(c: Checker, elf: str, core1: str, request_slots: int) -> None:
    """CPU 0 reads the board and the jumpers, and everything that follows from them: the USB
    configuration, and the product string USB and DAP_Info both give.

    Each board's straps with every combination of jumpers. The configuration has to be well formed,
    give every function its own endpoints in the one layout, and keep the notification endpoints at
    numbers the controller does not have, declared and never opened, polled every 2^15 microframes.
    """
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    at_high_speed(probe)
    ms_os_size = next((size for name, (_, size) in symbols_with_size(elf).items() if name.split(".")[0] == "desc_ms_os_20"), 0)
    for name in ("board_read_straps", "tud_descriptor_configuration_cb", "tud_descriptor_string_cb"):
        if name not in probe.sym:
            c.problems.append(f"no {name} symbol, so the board's USB cannot be run")
            return
    shared0 = shared_offsets(request_slots, "board", "jumpers", small_enums=short_enums(elf))
    shared1 = shared_offsets(request_slots, "board", "jumpers", small_enums=short_enums(core1))
    at = probe.sym["dap_shared"]
    products = {}
    layouts = set()
    for state, levels, board, jumpers in board_states():
        set_straps(probe, levels)
        probe.call("board_read_straps")
        read = probe.uc.mem_read(at + shared0["board"], 1)[0], probe.uc.mem_read(at + shared0["jumpers"], 1)[0]
        if not c.check(read == (board, jumpers), f"{state}: CPU 0 read board {read[0]}, jumpers {read[1]:#x}"):
            continue

        interfaces, endpoints, functions = expected_layout(board, jumpers)
        device = bytes(probe.uc.mem_read(probe.call("tud_descriptor_device_cb"), 18))
        inner = Checker()
        winusb = [f for f in WINUSB_FUNCTIONS if functions & f[0]]
        check_device(inner, device, bos=bool(winusb))
        c.problems += [f"{state}: {p}" for p in inner.problems]
        (bcd_device,) = struct.unpack_from("<H", device, 12)
        c.check(bcd_device == 0x0100 | functions, f"{state}: bcdDevice {bcd_device:#06x}, wanted {0x0100 | functions:#06x}")
        probe.call("ms_os_20_init")
        bos = probe.call("tud_descriptor_bos_cb")
        c.check((bos != 0) == bool(winusb), f"{state}: the BOS is {'there' if bos else 'missing'}")
        # The whole buffer, so a set whose header claims more than it holds shows.
        ms_os = bytes(probe.uc.mem_read(probe.sym["desc_ms_os_20"], ms_os_size))
        ms_os = ms_os[: min(len(ms_os), struct.unpack_from("<H", ms_os, 8)[0])]
        if bos:
            inner = Checker()
            check_bos(inner, bytes(probe.uc.mem_read(bos, struct.unpack("<H", probe.uc.mem_read(bos + 2, 2))[0])), len(ms_os))
            c.problems += [f"{state}: {p}" for p in inner.problems]
        inner = Checker()
        bindings = ms_os_bindings(inner, ms_os)
        c.problems += [f"{state}: {p}" for p in inner.problems]

        config = probe.call("tud_descriptor_configuration_cb", 0)
        (total,) = struct.unpack("<H", probe.uc.mem_read(config + 2, 2))
        d = bytes(probe.uc.mem_read(config, total))
        inner = Checker()
        check_configuration(inner, d)
        c.problems += [f"{state}: {p}" for p in inner.problems]
        found, notifications = set(), {}
        i = 0
        while i + 1 < len(d) and d[i] >= 2:
            if d[i + 1] == 0x05:
                found.add(d[i + 2])
                if d[i + 3] & 3 == 3:
                    notifications[d[i + 2]] = d[i + 6]
            i += d[i]
        c.check(d[4] == interfaces, f"{state}: {d[4]} interfaces, the layout has {interfaces}")
        c.check(found == endpoints, f"{state}: endpoints {sorted(map(hex, found))}, the layout has {sorted(map(hex, endpoints))}")
        real = {e & 0x7F for e in found} - {e & 0x7F for e in notifications if e in (0x86, 0x87)}
        c.check(max(real, default=0) <= 5, f"{state}: endpoint {max(real, default=0)} carries data, and USB1 stops at EP5")
        # Each MS OS 2.0 binding names a vendor interface by number, and that number has to be the one
        # carrying the function's string, whatever the jumpers left before it.
        vendor, i = {}, 0
        while i + 1 < len(d) and d[i] >= 2:
            if d[i + 1] == 0x04 and d[i + 5] == 0xFF and d[i + 8]:
                vendor[d[i + 2]] = usb_string(probe, d[i + 8])
            i += d[i]
        c.check(len(bindings) == len(winusb), f"{state}: MS OS 2.0 binds {len(bindings)} interfaces, wanted {len(winusb)}")
        for (first, guid), (_, name, want) in zip(bindings, winusb):
            c.check(vendor.get(first) == name and guid == want,
                    f"{state}: MS OS 2.0 binds {guid} to interface {first} ({vendor.get(first)!r}), wanted {want} on {name!r}")
        if functions & 1:
            c.check(vendor.get(0) == "CMSIS-DAP v2", f"{state}: interface 0 is not CMSIS-DAP v2")
        for address, interval in notifications.items():
            if address in (0x86, 0x87):
                c.check(interval == 16, f"{state}: notification {address:#04x} is polled at interval {interval}")
        layouts.add((d[4], len(d), bcd_device))
        products.setdefault(board, usb_string(probe, 2))

    for name, _, board in BOARDS:
        cpu1 = Machine(core1, Target([0] * 64), restart_at=set(), model=Model())
        cpu1.uc.mem_write(cpu1.sym["dap_shared"] + shared1["board"], bytes([board]))
        cpu1.call("board_debug_pins_init")
        reply = cpu1.command(bytes([0x00, DAP_ID_PRODUCT]))
        info = bytes(reply[2 : 2 + reply[1]]).rstrip(b"\x00").decode("ascii", "replace")
        c.check(info == products.get(board), f"{name} board: DAP_Info names {info!r}, USB names {products.get(board)!r}")
        c.check("CMSIS-DAP" in info, f"{name} board: the product {info!r} lacks CMSIS-DAP, which v1 hosts match on")
    c.check("Pro" in products.get(BOARD_PRO, ""), f"the Pro's product {products.get(BOARD_PRO)!r} does not name it")
    c.note(f"board: {len(list(board_states()))} strap states read; configurations of "
           f"{', '.join(f'{n} interfaces in {b} bytes (bcdDevice {v:#06x})' for n, b, v in sorted(layouts))}")
    c.note(f"board: products {', '.join(repr(p) for _, p in sorted(products.items()))}, the same on USB and DAP_Info")


def check_image_boards(c: Checker, elf: str) -> None:
    """The record verge reads to know which boards the image runs on: exactly
    one in the whole image, CPU 1's embedded one included, right after the vector table."""
    flash = binary(elf)
    base = load_address(elf)
    found = [m.start() for m in re.finditer(b"VERGEIMG", flash)]
    if not c.check(len(found) == 1, f"the image has {len(found)} VERGEIMG records, wanted one"):
        return
    at = found[0]
    vectors = sections(elf)[".interrupts"]
    c.check(at % 4 == 0 and base + at < 0x200, f"the record is at {base + at:#x}, not just after the vector table")
    fmt, length, zero, boards = struct.unpack_from("<BBHI", flash, at + 8)
    c.check((fmt, length, zero) == (1, 16, 0), f"the record reads format {fmt}, length {length}, {zero:#x}")
    c.check(boards == IMAGE_BOARDS, f"the image declares boards {boards:#x}, wanted {IMAGE_BOARDS:#x}")
    c.note(f"image: declares boards {boards:#x} for verge, at {base + at:#x} in {vectors[0]:#x} bytes of vectors")


# ARM's transfer paths, which only the reference build (both generators off) may carry.
ARM_TRANSFER_PATHS = re.compile(r"(arm_SWD_Transfer|arm_jtag4_Transfer|jtag4_[A-Za-z]+|cjtag[ft]?_[A-Z][A-Za-z]*|cJTAG_Frame(Slow|Fast)"
                                r"|JTAG_TransferBlock(Fast|Slow)?|SWD_Transfer(Block|Command)?(Fast|Slow))")


def check_core1_placement(c: Checker, elf: str) -> None:
    """A product image keeps everything CPU 1 touches inside a wire window off SRAM3: its RAM code
    and all its data but `.cold_bss`, which only a wire or configuration switch reaches. The ARM
    reference is a test build and may spill."""
    sym = symbols_with_size(elf)
    if "swd_gen_clock_hz" not in sym:
        c.note("ARM reference: CPU 1 placement not held to SRAM1 and SRAM2")
        return
    # SRAM3 is port 5, where CPU 0 copies dap_stage while CPU 1's wire windows run (BUSES.md).
    linked = symbols(elf)
    start, end = linked["__data_start__"], linked["__cold_bss_start__"]
    c.check(end <= SRAM3[0], f"CPU 1's RAM code and data run to {end:#x}, into SRAM3 from {SRAM3[0]:#x}")
    if end <= SRAM3[0]:
        c.note(f"CPU 1's RAM code and hot data {start:#x}..{end:#x}, {SRAM3[0] - end} bytes below SRAM3")


def check_generation_refusals(c: Checker, elf: str) -> None:
    """A product image runs every transfer on generated code, with no ARM path behind it. A clock
    the generators cannot serve is refused and the clock kept; and where nothing could be
    generated anyway, a command fails without a pin moving, rather than answering OK over a wire
    it never drove."""
    sym = symbols_with_size(elf)
    if "swd_gen_clock_hz" not in sym:
        arm = sorted(n for n in sym if ARM_TRANSFER_PATHS.fullmatch(n.split(".")[0]))
        c.check(bool(arm), "the ARM reference carries no ARM transfer path")
        c.note("ARM reference: carries ARM's transfer paths, and is not held to the generation refusals")
        return
    arm = sorted(n for n in sym if ARM_TRANSFER_PATHS.fullmatch(n.split(".")[0]))
    c.check(not arm, f"a product image carries ARM transfer paths: {', '.join(arm[:6])}")
    for wire, port, clock in (("swd", 1, "swd_gen_clock_hz"), ("jtag", 2, "jtag_gen_clock_hz")):
        probe = build(elf, wire, True, Model())
        dap_setup(probe)
        probe.command(bytes([2, port]))
        probe.command(bytes([0x11]) + u32(1_000_000))
        if wire == "jtag":
            probe.command(bytes([0x15, 1, 4]))
        at = probe.sym[clock]

        def quiet(request: bytes, expect: bytes, what: str) -> None:
            still = (probe.level, probe.dir)
            probe.begin()
            reply = probe.command(request)
            counts = probe.end()
            c.check(reply == expect, f"{wire} {what}: answered {reply.hex()}, expected {expect.hex()}")
            c.check(counts.gpio_store == 0 and counts.tck_rise == 0 and (probe.level, probe.dir) == still,
                    f"{wire} {what}: moved the wire ({counts.gpio_store} pin stores, {counts.tck_rise} clocks)")

        # 8 Hz is the fastest clock whose period no generator takes; 9 Hz the slowest that has code.
        quiet(bytes([0x11]) + u32(8), bytes([0x11, 0xff]), "an 8 Hz clock")
        c.check(bytes(probe.uc.mem_read(at, 4)) == u32(1_000_000), f"{wire}: a refused clock changed the clock")
        builds, failures = (probe.sym[f"{wire}_gen_{name}"] for name in ("builds", "failures"))
        before = [bytes(probe.uc.mem_read(a, 4)) for a in (builds, failures)]
        c.check(probe.command(bytes([0x11]) + u32(9)) == bytes([0x11, 0]), f"{wire}: a 9 Hz clock was refused")
        after = [bytes(probe.uc.mem_read(a, 4)) for a in (builds, failures)]
        c.check(after[0] != before[0] and after[1] == before[1], f"{wire}: a 9 Hz clock generated no code")
        c.check(probe.command(bytes([0x11]) + u32(1_000_000)) == bytes([0x11, 0]), f"{wire}: 1 MHz refused")
        # No request reaches a clock without code now, so set one behind the command's back; a
        # DAP_TransferConfigure, as ARM's defaults, makes the generators look at their inputs again.
        probe.uc.mem_write(at, u32(1))
        c.check(probe.command(bytes([4, 0, 100, 0, 0, 0])) == bytes([4, 0]), f"{wire}: TransferConfigure failed")
        if wire == "swd":
            quiet(bytes([5, 0, 1, 0x02]), bytes([5, 0, 8]), "transfer with nothing generated")
            # After a line reset a DPIDR read goes through the connect sweep, which reads a failed
            # first read as a reason to reset the line again.
            probe.command(bytes([0x12, 64]) + b"\xff" * 7 + b"\x00")
            quiet(bytes([5, 0, 1, 0x02]), bytes([5, 0, 8]), "DPIDR read after a line reset with nothing generated")
            quiet(bytes([6, 0, 1, 0, 0x02]), bytes([6, 0, 0, 8]), "block with nothing generated")
            quiet(bytes([8, 0]) + u32(0x1E), bytes([8, 0xff]), "WriteABORT with nothing generated")
        else:
            quiet(bytes([0x14, 1, 0x88, 0xff]), bytes([0x14, 0xff]), "sequence with nothing generated")
            quiet(bytes([0x16, 0]), bytes([0x16, 0xff]), "IDCODE with nothing generated")
            quiet(bytes([5, 0, 1, 0x02]), bytes([5, 0, 8]), "transfer with nothing generated")
            quiet(bytes([6, 0, 1, 0, 0x02]), bytes([6, 0, 0, 8]), "block with nothing generated")
            quiet(bytes([8, 0]) + u32(0x1E), bytes([8, 0xff]), "WriteABORT with nothing generated")
        probe.command(bytes([0x11]) + u32(1_000_000))
        probe.begin()
        reply = probe.command(bytes([5, 0, 1, 0x02]))
        counts = probe.end()
        # This harness's TAP model cannot answer a generated JTAG read in time, so the JTAG side
        # shows only that the wire runs again.
        recovered = reply[:3] == bytes([5, 1, 1]) if wire == "swd" else counts.tck_rise > 0
        c.check(recovered, f"{wire}: a transfer after a refusal did not run: {reply[:8].hex()}")
    c.note("product image: no ARM transfer path; 8 Hz refused and 9 Hz served; with nothing generated,"
           " SWD and JTAG transfers, blocks, WriteABORT, JTAG sequences and IDCODE fail with no pin moved")


def check_unconnected_block(c: Checker, elf: str) -> None:
    """A transfer block with no port connected moves no pin and answers no transfers, as ARM's
    does. An oversized read used to be clamped and run on the JTAG path whatever the port; with a
    chain configured, which needs no port, that clocked it."""
    probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())
    probe.call("board_debug_pins_init")
    probe.command(bytes([0x11]) + (1_000_000).to_bytes(4, "little"))  # DAP_SWJ_Clock, so delays end
    probe.command(bytes([0x15, 1, 4]))  # DAP_JTAG_Configure: one device, a 4-bit IR
    still = (probe.level, probe.dir)
    reply = probe.command(bytes([0x06, 0, 200, 0, 0x02]))  # a 200-word read of DP register 0
    c.check(reply[:4] == bytes([0x06, 0, 0, 0]), f"a block with no port connected answered {reply[:8].hex()}")
    c.check((probe.level, probe.dir) == still,
            f"a block with no port connected moved the debug port: levels {probe.level:#x}, directions {probe.dir:#x}")
    c.note("a transfer block with no port connected answers none and moves no pin")


# P0_14 drives the target's ISP pin, debug connector pin 7, through an open-drain buffer.
ISP_CTRL_PIN = 14


def check_board(c: Checker, elf: str, request_slots: int) -> None:
    """Only the plain MCU-Link lets the marker near its pins, and an on-board one keeps cJTAG
    off (BOARDS.md).

    On a Pro the J8 pins are the analog trace input, a level shifter's direction and two SPI lines,
    so driving them fights other drivers. CPU 1 takes the board from dap_shared, as CPU 0 read it.

    The plain board and the Pro release the target's ISP pin: P0_14 driven high, as P0_13's I2C
    cell is set up to drive, since a low one boots an attached NXP part into its bootloader.
    """
    j8_dir = ORDINARY_GPIO + 0x2380 + 4  # DIRSET[1]
    j8_set = ORDINARY_GPIO + 0x2200 + 4  # SET[1]
    offset = shared_offsets(request_slots, "board", small_enums=short_enums(elf))["board"]
    for name, _, board in BOARDS:
        probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())
        if "board_debug_pins_init" not in probe.sym:
            c.problems.append("no board_debug_pins_init symbol, so the board's pin setup cannot be run")
            return
        probe.uc.mem_write(probe.sym["dap_shared"] + offset, bytes([board]))
        probe.call("board_debug_pins_init")
        isp = ((probe.level >> ISP_CTRL_PIN) & 1, (probe.dir >> ISP_CTRL_PIN) & 1)
        iocon = [int.from_bytes(probe.uc.mem_read(IOCON + 4 * pin, 4), "little") for pin in (13, ISP_CTRL_PIN)]
        if board == 2:
            c.check(isp == (0, 0) and iocon[1] == 0, f"{name} board: P0_14, whose net is unknown there, was set up")
        else:
            c.check(isp == (1, 1) and iocon[1] == iocon[0],
                    f"{name} board: the target's ISP pin is not released: P0_14 level {isp[0]}, output {isp[1]}, "
                    f"IOCON {iocon[1]:#x} against P0_13's {iocon[0]:#x}")
        reply = probe.command(bytes([0x82]))
        flags, length = reply[7], reply[8]
        reported = reply[9 + length] if len(reply) > 9 + length else None
        j8 = probe.command(bytes([0x84, 0x0F, 0x0F]))
        enter = probe.command(bytes([0x83, 2]))
        probe.command(bytes([0x83, 0]))
        driven = int.from_bytes(probe.uc.mem_read(j8_dir, 4), "little") | int.from_bytes(
            probe.uc.mem_read(j8_set, 4), "little"
        )
        plain = board == 0
        two_pin = board != 2
        if reported != board:
            c.problems.append(f"{name} board: vendor 0x82 reports board {reported}, wanted {board}")
        if bool(flags & 0x11) != two_pin:
            c.problems.append(f"{name} board: vendor 0x82 flags {flags:#04x} get cJTAG wrong")
        if (j8[1] == 0) != plain:
            c.problems.append(f"{name} board: vendor 0x84 answered {j8.hex()}")
        if (enter[1] == 0) != two_pin:
            c.problems.append(f"{name} board: vendor 0x83 mode 2 answered {enter.hex()}")
        if bool(driven & 0x0F) != plain:
            c.problems.append(f"{name} board: J8's pins were {'not ' if plain else ''}driven ({driven:#x})")
    c.note("board: CPU 1 reports each board as CPU 0 read it; J8 on the plain one only, cJTAG on the plain one and the Pro, "
           "the target's ISP pin released on both")


# The Pro's DIO pins (src/dio.c): DIO0 to DIO3, then IO_DIR1 and IO_DIR2, as (port, pin), and the
# direction pin each DIO pin's translator takes.
DIO_PINS = ((0, 20), (0, 26), (1, 2), (1, 3), (1, 1), (1, 25))
DIO_DIR_OF = (4, 4, 4, 5)
CAP_DIO = 1 << 6
CAP_TARGET_POWER = 1 << 7
EXT_CAP_DIO_AUX = 1 << 6
EXT_CAP_MARKS = 1 << 7
EXT_CAP_CAPTURE_ID = 1 << 8
# The ordinary GPIO's port-wide registers, which the hook below sees: SET, CLR, NOT, DIRSET, DIRCLR,
# DIRNOT, one word per port each.
GPIO_PORT_REGS = {0x2200: "SET", 0x2280: "CLR", 0x2300: "NOT", 0x2380: "DIRSET", 0x2400: "DIRCLR", 0x2480: "DIRNOT"}


def watch_gpio_ports(probe, store) -> None:
    """Calls store(address, value) for each store to the ordinary GPIO's port registers. The page
    becomes MMIO over its own bytes rather than taking a write hook: under a write hook unicorn
    2.1.4 ran an instruction at a 1 KiB boundary after an IT block's conditional store as if it
    were still conditional, and dio_set's GPIO stores are all conditional."""
    page = ORDINARY_GPIO + 0x2000
    memory = bytearray(probe.uc.mem_read(page, 0x1000))
    probe.uc.mem_unmap(page, 0x1000)

    def read(uc, offset, size, user) -> int:
        return int.from_bytes(memory[offset:offset + size], "little")

    def write(uc, offset, size, value, user) -> None:
        memory[offset:offset + size] = value.to_bytes(size, "little")
        if min(GPIO_PORT_REGS) - 0x2000 <= offset < max(GPIO_PORT_REGS) + 0x80 - 0x2000:
            store(page + offset, value)

    probe.uc.mmio_map(page, 0x1000, read, None, write, None)


class DioPins:
    """Every store to the ordinary GPIO's port registers, replayed onto the DIO pins: each pin's
    translator input (A) and header (B) level after each store. A is driven by the probe's pin while
    it is an output, by the translator from the header while its direction pin is low, and otherwise
    keeps its level; the header follows A while the direction pin is high and is the target's ("z")
    otherwise. A direction pin is high only as an output with its latch set: the pull-down holds it
    low as an input."""

    def __init__(self, probe) -> None:
        self.latch = [0, 0]
        self.dir = [0, 0]
        self.stores: list[tuple[str, int, int]] = []
        self.a = ["z"] * 4
        self.header = ["z"] * 4
        self.moves = [0] * 4
        self.fights: list[str] = []
        watch_gpio_ports(probe, self._store)

    def bit(self, words: list[int], index: int) -> int:
        port, pin = DIO_PINS[index]
        return (words[port] >> pin) & 1

    def _store(self, address: int, value: int) -> None:
        offset = address - ORDINARY_GPIO
        reg, port = GPIO_PORT_REGS.get(offset & ~0x7F, "?"), (offset & 0x7F) // 4
        self.stores.append((reg, port, value))
        if port > 1 or reg not in ("SET", "CLR", "DIRSET", "DIRCLR"):
            self.fights.append(f"{reg}[{port}] {value:#x}")
            return
        words = self.latch if reg in ("SET", "CLR") else self.dir
        words[port] = words[port] | value if reg in ("SET", "DIRSET") else words[port] & ~value
        for n in range(4):
            toward_header = self.bit(self.dir, DIO_DIR_OF[n]) and self.bit(self.latch, DIO_DIR_OF[n])
            driving = self.bit(self.dir, n)
            if driving and not toward_header:
                self.fights.append(f"DIO{n}: the probe and the translator both drive A after {reg}[{port}]")
            self.a[n] = str(self.bit(self.latch, n)) if driving else "z" if not toward_header else self.a[n]
            header = self.a[n] if toward_header else "z"
            if header != self.header[n]:
                self.moves[n] += 1
                self.header[n] = header

    def step(self) -> None:
        self.stores.clear()
        self.moves = [0] * 4
        self.fights.clear()


def vendor_boards(elf: str, request_slots: int) -> dict:
    """The plain board as a machine starts, and the Pro with its DIO pins set up."""
    board = shared_offsets(request_slots, "board", small_enums=short_enums(elf))["board"]

    def pro(probe) -> None:
        probe.uc.mem_write(probe.sym["dap_shared"] + board, bytes([BOARD_PRO]))
        probe.call("board_debug_pins_init")
        if "dio_init" in probe.sym:
            probe.call("dio_init")

    return {"plain": lambda probe: None, "Pro": pro}


def check_dio(c: Checker, elf: str, request_slots: int) -> None:
    """Vendor 0x8E drives the Pro's DIO pins and nothing else's: refused, and no pin stored to, on
    the other boards and with J15 fitted. On the Pro it turns a group round whole or not at all,
    the probe and a translator never drive the same line, each header moves at most once a
    command, and every reply carries the state and the groups."""
    if "dio_init" not in symbols(elf):
        c.problems.append("no dio_init symbol, so the DIO pins' setup cannot be run")
        return
    offsets = shared_offsets(request_slots, "board", "jumpers", "led_sio", small_enums=short_enums(elf))
    for name, board, jumpers in (("plain", 0, 0), ("on-board", 2, 0), ("Pro", BOARD_PRO, 0), ("Pro with J15", BOARD_PRO, 4)):
        probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())
        shared = probe.sym["dap_shared"]
        probe.uc.mem_write(shared + offsets["board"], bytes([board]))
        probe.uc.mem_write(shared + offsets["jumpers"], bytes([jumpers]))
        probe.call("board_debug_pins_init")
        pins = DioPins(probe)
        probe.call("dio_init")
        iocon = [int.from_bytes(probe.uc.mem_read(IOCON + 4 * (32 * port + pin), 4), "little") for port, pin in DIO_PINS]
        info = probe.command(bytes([0x82]))
        flags = info[7]
        extended = int.from_bytes(info[14 + info[8]:18 + info[8]], "little")

        def dio(mask: int, outputs: int, levels: int) -> tuple[bool, list[int]]:
            pins.step()
            reply = probe.command(bytes([0x8E, mask, outputs, levels]))
            return reply[1] == 0, [reply[2], reply[3], reply[5], *reply[6:]]

        if name != "Pro":
            # PIO1_1 to PIO1_3 are J8's on the plain board, which the marker takes.
            mine = [v for (port, pin), v in zip(DIO_PINS, iocon) if board != 0 or port != 1 or pin > 3]
            c.check(not any(mine) and not pins.stores,
                    f"{name} board: the DIO pins were set up: IOCON {[hex(v) for v in iocon]}, stores {pins.stores}")
            c.check(not flags & CAP_DIO, f"{name} board: vendor 0x82 claims the DIO pins")
            c.check(not extended & EXT_CAP_DIO_AUX, f"{name} board: vendor 0x82 claims the DIO pins on Verge Aux")
            c.check(not extended & EXT_CAP_MARKS, f"{name} board: vendor 0x82 claims markers without energy capture")
            c.check(not extended & EXT_CAP_CAPTURE_ID, f"{name} board: vendor 0x82 claims capture identity")
            ok, state = dio(0x0F, 0x0F, 0x0F)
            c.check(not ok and state == [0, 0, 0, 0] and not pins.stores,
                    f"{name} board: vendor 0x8E answered {ok}, {state}, stores {pins.stores}")
            continue
        c.check(all(v & 0x13F == 0x100 for v in iocon), f"{name}: the DIO pins are not plain digital GPIO: "
                f"IOCON {[hex(v) for v in iocon]}")
        c.check(not pins.fights and pins.dir[1] & 0x2000002 == 0x2000002 and not pins.latch[1] & 0x2000002
                and all(not pins.bit(pins.dir, n) for n in range(4)),
                f"{name}: dio_init left the directions {pins.dir}, latches {pins.latch}: {pins.fights}")
        c.check(bool(flags & CAP_DIO), f"{name}: vendor 0x82 flags {flags:#04x} lack the DIO pins")
        c.check(bool(extended & EXT_CAP_DIO_AUX), f"{name}: vendor 0x82 extended {extended:#x} lacks DIO on Verge Aux")
        # (mask, outputs, levels), whether it is taken, and outputs, latch and the LED after it.
        steps = (
            ((0x00, 0x00, 0x00), True, 0x00, 0x00, 0),
            ((0x01, 0x01, 0x01), False, 0x00, 0x00, 0),  # DIO0 alone splits its group
            ((0x10, 0x10, 0x10), False, 0x00, 0x00, 0),  # no DIO4
            ((0x07, 0x07, 0x05), True, 0x07, 0x05, 1),
            ((0x01, 0x00, 0x00), False, 0x07, 0x05, 1),  # DIO0 back alone splits it too
            ((0x08, 0x08, 0x08), True, 0x0F, 0x0D, 1),
            ((0x02, 0x02, 0x02), True, 0x0F, 0x0F, 1),  # a level alone, the group staying out
            ((0x0F, 0x07, 0x02), True, 0x07, 0x02, 1),  # DIO3 in while the others change level
            ((0x0F, 0x08, 0x08), True, 0x08, 0x08, 1),  # both groups turned at once
            ((0x0F, 0x00, 0x00), True, 0x00, 0x00, 0),
        )
        for request, taken, outputs, latch, lit in steps:
            ok, state = dio(*request)
            sio = probe.uc.mem_read(shared + offsets["led_sio"], 1)[0]
            want = [outputs, latch, 0x0F, 2, 0x07, 0x08]
            c.check(ok == taken and state == want and sio == lit,
                    f"{name}: vendor 0x8E {bytes(request).hex()} answered {ok}, {[hex(v) for v in state]}, SIO LED {sio}; "
                    f"wanted {taken}, {[hex(v) for v in want]}, {lit}")
            headers = ["z" if not outputs >> n & 1 else str(latch >> n & 1) for n in range(4)]
            c.check(not pins.fights and max(pins.moves) <= 1 and pins.header == headers and (taken or not pins.stores),
                    f"{name}: vendor 0x8E {bytes(request).hex()} left headers {pins.header} (wanted {headers}), "
                    f"moved them {pins.moves} times, {pins.fights or 'no fights'}, stores {pins.stores}")
    c.note("DIO: vendor 0x8E on the Pro only, J15 honoured, groups turned whole without a fight or a double move, "
           "state and groups in every reply")


def check_target_power(c: Checker, elf: str, request_slots: int) -> None:
    """The Pro's switched supply defaults off; other boards leave P0_8 alone."""
    if "target_power_init" not in symbols(elf):
        c.problems.append("no target_power_init symbol, so the Pro's supply setup cannot be run")
        return

    offsets = shared_offsets(request_slots, "board", small_enums=short_enums(elf))["board"]
    mask = 1 << 8
    for name, board in (("plain", 0), ("on-board", 2), ("Pro", BOARD_PRO)):
        probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())
        probe.uc.mem_write(probe.sym["dap_shared"] + offsets, bytes([board]))
        probe.call("board_debug_pins_init")
        stores = []

        def record(address: int, value: int) -> None:
            offset = address - ORDINARY_GPIO
            reg = GPIO_PORT_REGS.get(offset & ~0x7F, "?")
            stores.append((reg, (offset & 0x7F) // 4, value))

        watch_gpio_ports(probe, record)
        probe.call("target_power_init")
        if board == BOARD_PRO:
            pin = int.from_bytes(probe.uc.mem_read(IOCON + 4 * 8, 4), "little")
            want_pin = 0x100  # GPIO function, no pull, digital input buffer.
            c.check(stores == [("CLR", 0, mask), ("DIRSET", 0, mask)]
                    and pin == want_pin,
                    f"{name}: supply init stores {stores}, P0_8 mode {pin:#x}")
            info = probe.command(bytes([0x82]))
            c.check(bool(info[7] & CAP_TARGET_POWER), f"{name}: vendor 0x82 lacks target supply capability")
            steps = ((2, 0, 0, None), (1, 0, 1, ("SET", 0, mask)),
                     (1, 0, 1, ("SET", 0, mask)), (2, 0, 1, None),
                     (0, 0, 0, ("CLR", 0, mask)), (3, 0xFF, 0, None))
            for mode, status, state, store in steps:
                before = len(stores)
                reply = probe.command(bytes([0x8F, mode]))
                c.check(reply == bytes([0x8F, status, state]),
                        f"{name}: vendor 0x8F mode {mode} answered {reply.hex()}")
                added = stores[before:]
                want = [] if store is None else [store]
                c.check(added == want,
                        f"{name}: vendor 0x8F mode {mode} wrote {added}, wanted {want}")
        else:
            c.check(not stores, f"{name}: setup touched the absent supply: {stores}")
            info = probe.command(bytes([0x82]))
            c.check(not info[7] & CAP_TARGET_POWER, f"{name}: vendor 0x82 claims target supply")
            reply = probe.command(bytes([0x8F, 1]))
            c.check(reply == bytes([0x8F, 0xFF, 0]) and not stores,
                    f"{name}: vendor 0x8F answered {reply.hex()}, stores {stores}")

    c.note("target supply: Pro-only P0_8 control starts disabled; unsupported boards leave it untouched")


def check_sample_probe(c: Checker, elf: str) -> None:
    """Vendor 0x87 frames a DPIDR read the way a target expects, in both shapes and every position.

    The emulator's target hands out bits in order and has no wire delay, so both sample positions
    read the same word. What this checks is the framing and the counting, and that the command
    refuses operands it cannot honour rather than running with them.
    """
    word = 0x0BE12477
    parity = bin(word).count("1") & 1
    probe = Machine(elf, Target([1, 0, 0] + [(word >> i) & 1 for i in range(32)] + [parity]),
                    restart_at=set(), model=Model())
    probe.resync_on_release = True

    def run(low: int, fill: int, at: int, count: int, expected: int) -> bytes:
        return probe.command(bytes([0x87, low, fill, at]) + u16(count) + u32(expected))

    refused = run(0, 4, 0xFF, 1, word)
    if len(refused) < 2 or refused[1] == 0x00:
        c.problems.append(f"vendor 0x87 ran with no SWD port connected: {refused.hex()}")

    probe.command(bytes([0x02, 0x01]))  # Connect, SWD
    for at in (0xFF, 0, 4, 0x80, 0x84):
        reply = run(0, 4, at, 3, word)
        good, bad_ack, bad_word = (int.from_bytes(reply[i:i + 2], "little") for i in (2, 4, 6))
        if len(reply) != 17 or reply[:2] != b"\x87\x00" or (good, bad_ack, bad_word) != (3, 0, 0):
            c.problems.append(f"vendor 0x87 at {at:#04x} answered {reply.hex()}, wanted three good reads")
    wrong = run(0, 4, 0xFF, 2, word ^ 1)
    if int.from_bytes(wrong[6:8], "little") != 2 or int.from_bytes(wrong[9:13], "little") != word:
        c.problems.append(f"vendor 0x87 did not report a mismatched word: {wrong.hex()}")
    for fill, at in ((33, 0), (4, 5), (4, 0x85)):
        reply = run(0, fill, at, 1, word)
        if len(reply) < 2 or reply[1] == 0x00:
            c.problems.append(f"vendor 0x87 accepted fill {fill} with the sample at {at}")
    c.note("sample probe: DPIDR framed and counted on both sides of the edge, bad operands refused")


# What DAP_Info tells a host about the probe. probe-rs sizes its buffers from the packet size and
# decides what it may ask for from the capabilities, so a wrong answer here breaks every transfer
# rather than one.
CAPABILITY_BITS = {
    0: "SWD",
    1: "JTAG",
    2: "SWO UART",
    3: "SWO Manchester",
    4: "atomic commands",
    5: "timestamps",
    6: "SWO streaming",
    7: "UART port",
}


def check_info(c: Checker, elf: str, packet_size: int, packet_count: int, swo: bool) -> None:
    probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())

    def info(sub: int) -> bytes:
        reply = probe.command(bytes([0x00, sub]))
        return reply[2 : 2 + reply[1]] if len(reply) > 1 else b""

    size = int.from_bytes(info(0xFF), "little")
    count = int.from_bytes(info(0xFE), "little")
    c.check(size == packet_size, f"DAP_Info reports a {size}-byte packet, wanted {packet_size}")
    c.check(count == packet_count, f"DAP_Info reports {count} packets, wanted {packet_count}")

    caps = info(0xF0)
    bits = caps[0] if caps else 0
    named = [n for b, n in CAPABILITY_BITS.items() if bits & (1 << b)]
    for wanted in ("SWD", "JTAG", "atomic commands", "timestamps") + (("SWO UART",) if swo else ()):
        c.check(wanted in named, f"DAP_Info does not claim {wanted}, which COMMANDS relies on")
    for unwanted in ("UART port",) + (() if swo else ("SWO UART",)):
        c.check(unwanted not in named, f"DAP_Info claims {unwanted}, which is not implemented")

    product = info(DAP_ID_PRODUCT).rstrip(b"\x00").decode("ascii", "replace")
    serial = info(DAP_ID_SER_NUM).rstrip(b"\x00").decode("ascii", "replace")
    c.check(bool(serial), "DAP_Info has no serial number, so two probes cannot be told apart")
    c.check(bool(product), "DAP_Info has no product name, so a host cannot say what it found")
    c.note(f"DAP_Info: {size}-byte packets x{count}, {', '.join(named)}")
    c.note(f"DAP_Info: product {product!r}, serial {serial!r}")


# Instructions CPU 1 cannot execute: it has no FPU and no DSP extension (LPC55S6x data sheet,
# section 7.4). The DSP list is the Armv7E-M one; SMULL, SMLAL, SSAT and the like are base
# Mainline and stay allowed.
CONDITIONS = {"eq", "ne", "cs", "hs", "cc", "lo", "mi", "pl", "vs", "vc", "hi", "ls", "ge", "lt", "gt", "le", "al"}
COPROCESSOR = re.compile(r"^(mcrr?2?|mrrc2?|mrc2?|cdp2?|ldc2?l?|stc2?l?)$")
DSP = re.compile(
    r"^([su]add(8|16)|[su]sub(8|16)|[su]asx|[su]sax|q(add|sub)(8|16)?|qasx|qsax|qdadd|qdsub"
    r"|[su]h(add|sub)(8|16)|[su]h(asx|sax)|uq(add|sub)(8|16)|uq(asx|sax)|usada?8|sel|pkh(bt|tb)"
    r"|[su]sat16|[su]xta[bh]|[su]xtab16|[su]xtb16|smla(bb|bt|tb|tt|d|dx|wb|wt)|smlal(bb|bt|tb|tt|d|dx)"
    r"|smlsl?dx?|smml[as]r?|smmulr?|smuadx?|smul(bb|bt|tb|tt|wb|wt)|smusdx?)$"
)


def foreign_instructions(elf: str) -> list[str]:
    """Every FPU, coprocessor or DSP instruction in the image, by where it is.

    From objdump rather than capstone over each function's bytes: a function's size takes in its
    literal pool, and its constants decode as instructions. The ELF's mapping symbols say which
    bytes are data, and objdump reads them.
    """
    found = []
    function = "?"
    listing = subprocess.run(["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", elf],
                             capture_output=True, text=True, check=True).stdout
    for line in listing.splitlines():
        header = re.match(r"^[0-9a-f]+ <(.+)>:$", line)
        if header:
            function = header.group(1)
            continue
        m = re.match(r"^\s*([0-9a-f]+):\s+([a-z][a-z0-9.]*)", line)
        if not m:
            continue
        name = m.group(2).split(".")[0]
        bare = name[:-2] if name[-2:] in CONDITIONS and len(name) > 3 else name
        if name.startswith("v") or COPROCESSOR.match(bare) or DSP.match(name) or DSP.match(bare):
            found.append(f"{m.group(2)} at {m.group(1)} in {function}")
    return found


# A branch in an objdump listing: address, mnemonic, target, and the symbol objdump names it by.
# objdump indents an address only when it has fewer than eight digits.
BRANCH = re.compile(r"^\s*([0-9a-f]+):\s+(bl|blx|b(?:eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le)?(?:\.[nw])?)"
                    r"\s+([0-9a-f]+)(?=\s|$)(?: <(\S+)>)?")


def check_sram_code(c: Checker, elf: str, what: str, names: tuple[str, ...], allow: tuple[str, ...] = ()) -> None:
    """Both cores wait for work from SRAM and run each command's own code from there: a core
    fetching from flash slows the other's command code (src/main.c), and CPU 1's wait polls a
    level instead of sleeping (src/core1_main.c). A dropped section attribute, or a call the
    compiler adds back into flash, would undo that and change nothing else checked here.

    `allow` names flash functions a branch may reach anyway, ones that run once rather than per
    command or per wait, matched without the suffix LTO adds to a local symbol's name. A call
    through a register has no static target and is not checked here."""
    sym = symbols_with_size(elf)
    # A veneer's target can be a symbol with no size, as the C library's assembly helpers are.
    every = symbols(elf)
    sram = range(0x2000_0000, 0x3000_0000)
    before = len(c.problems)
    for name in names:
        # A local function can carry the suffix LTO gives it.
        found = name if name in sym else next((k for k in sym if k.split(".")[0] == name), None)
        if not c.check(found is not None, f"{name} is not in the image ({what})"):
            continue
        at, size = sym[found]
        if not c.check(at in sram, f"{name} is at {at:#x}, not in SRAM"):
            continue
        listing = subprocess.run(
            ["arm-none-eabi-objdump", "-d", "--no-show-raw-insn", f"--start-address={at:#x}",
             f"--stop-address={at + size:#x}", elf], capture_output=True, text=True, check=True).stdout
        for line in listing.splitlines():
            m = BRANCH.match(line)
            if not m or (m.group(2) == "blx" and not m.group(3)):
                continue
            target = int(m.group(3), 16) if m.group(3) else None
            # A call out of range gets a linker veneer, placed next to the caller in SRAM. What
            # matters is where the veneer goes, so take the function it is named for.
            veneer = re.fullmatch(r"__(.+)_veneer", m.group(4) or "")
            if veneer and veneer.group(1) in every:
                target = every[veneer.group(1)] & ~1
            callee = veneer.group(1) if veneer else (m.group(4) or "")
            if callee.split(".")[0] in allow:
                continue
            c.check(target is not None and target in sram,
                    f"{name} branches out of SRAM at {m.group(1)}: {m.group(2)} {m.group(4) or m.group(3)}")
    if len(c.problems) == before:
        c.note(f"{what} from SRAM: {' and '.join(names)}, and every branch they take")


def check_core1_image(c: Checker, elf: str, core1: str, request_slots: int,
                      timing_diagnostics: bool = False) -> None:
    """CPU 0 starts CPU 1 from the image it embeds, and both reach the same shared struct.

    Built separately, the two images agree on nothing a linker can check. So this checks what each
    needs of the other: the embedded bytes are the CPU 1 image, the vector table CPU 0 points at
    starts CPU 1 on its own stack, and dap_shared has one address and one layout in both.
    """
    sym0, sym1 = symbols_with_size(elf), symbols_with_size(core1)
    # The linker script defines the stack top, and a linker symbol has no size.
    top = symbols(core1).get("__core1_stack_top")
    missing = [f"{name} (CPU 0)" for name in ("core1_image", "dap_shared", "dap_stage") if name not in sym0]
    missing += [f"{name} (CPU 1)" for name in ("core1_vectors", "core1_reset", "dap_shared", "dap_stage")
                if name not in sym1]
    missing += [] if top is not None else ["__core1_stack_top (CPU 1)"]
    if missing:
        c.problems.append(f"the two images are missing {', '.join(missing)}")
        return

    image_at, vectors = sym0["core1_image"][0], sym1["core1_vectors"][0]
    c.check(image_at == vectors, f"CPU 0 embeds CPU 1's image at {image_at:#x}; CPU 1 is linked at {vectors:#x}")
    c.check(vectors % 512 == 0, f"CPU 1's vector table at {vectors:#x} is not 512-byte aligned")
    linked = binary(core1)
    c.check(binary(elf, ".core1_code")[: len(linked)] == linked,
            "the image CPU 0 embeds is not the CPU 1 image built alongside it")

    table = struct.unpack("<87I", linked[: 87 * 4])
    c.check(table[0] == top, f"CPU 1 starts with its stack at {table[0]:#x}, wanted {top:#x}")
    c.check(table[1] == sym1["core1_reset"][0] | 1, "CPU 1's reset vector does not point at core1_reset")
    c.check(len(set(table[2:])) == 1, "CPU 1's vector table names a handler; nothing should interrupt it")

    (at0, size0), (at1, size1) = sym0["dap_shared"], sym1["dap_shared"]
    c.check((at0, size0) == (at1, size1),
            f"dap_shared is {size0} bytes at {at0:#x} on CPU 0 and {size1} bytes at {at1:#x} on CPU 1")
    # Equal sizes can hide moved members, so compare where each image's compiler puts every one.
    fields = shared_fields()
    layout0 = shared_offsets(request_slots, *fields, small_enums=short_enums(elf))
    layout1 = shared_offsets(request_slots, *fields, small_enums=short_enums(core1))
    moved = [f for f in fields if layout0[f] != layout1[f]]
    c.check(not moved, f"CPU 0 and CPU 1 lay dap_shared out differently: {', '.join(moved)}")
    c.check(size0 == layout0["size"], f"dap_shared is {size0} bytes in the images and {layout0['size']} in dap_shared.h")
    c.check(USB_SRAM[0] <= at0 and at0 + size0 <= USB_SRAM[0] + USB_SRAM[1],
            f"dap_shared at {at0:#x} is outside USB SRAM, the only memory the USB DMA reaches")

    (stage0, stage_size0), (stage1, stage_size1) = sym0["dap_stage"], sym1["dap_stage"]
    c.check((stage0, stage_size0) == (stage1, stage_size1),
            f"dap_stage is {stage_size0} bytes at {stage0:#x} on CPU 0 and {stage_size1} bytes at {stage1:#x} on CPU 1")
    stage_fields = ("request", "hid_request", "response", "response[1]", "hid_response", "hid_response[1]")
    stages = [shared_offsets(request_slots, *stage_fields, small_enums=short_enums(image),
                             struct_type="dap_stage_t", timing_diagnostics=timing_diagnostics)
              for image in (elf, core1)]
    c.check(stages[0] == stages[1] and stages[0]["size"] == stage_size0,
            "staged buffer layouts differ between cores or from dap_shared.h")
    c.check(stages[0]["response[1]"] - stages[0]["response"] == 512,
            "staged reply slots are not packet-sized")
    c.check(stages[0]["hid_response[1]"] - stages[0]["hid_response"] == 64, "HID reply rows are not report-sized")
    c.check(stage0 == SRAM3[0] + SRAM3[1] - 8192, "dap_stage does not start in SRAM3's top 8 KiB")
    # The top of SRAM3, where dap_shared.h and BUSES.md put it, past CPU 1's data.
    bss_end = symbols(core1).get("__bss_end__", top)
    c.check(SRAM3[0] <= stage0 and stage0 + stage_size0 <= SRAM3[0] + SRAM3[1],
            f"dap_stage at {stage0:#x} is outside SRAM3")
    c.check(stage0 >= bss_end, f"dap_stage at {stage0:#x} overlaps CPU 1's data, which ends at {bss_end:#x}")

    c.check("DAP_ExecuteCommand" not in sym0, "CPU 0's image carries the DAP command layer")
    c.check(not any(name.startswith("tud_") for name in sym1), "CPU 1's image carries TinyUSB")
    foreign = foreign_instructions(core1)
    c.check(not foreign, f"CPU 1 has no FPU or DSP extension, and its image uses them: {'; '.join(foreign[:5])}")
    c.note(f"CPU 1's image is embedded at {vectors:#x}, {len(linked)} bytes, and uses no FPU or DSP"
           f" instruction; dap_shared is {size0} bytes at {at0:#x} in both, dap_stage {stage_size0} at {stage0:#x}")


DAP_ID_PRODUCT, DAP_ID_SER_NUM = 2, 3


# Each timer's registers, by offset; an LED's level is in a match shadow register, which the timer
# copies into the match register as it wraps.
CTIMER1, CTIMER3 = 0x40009000, 0x40029000
CTIMER_TCR, CTIMER_PR, CTIMER_MCR, CTIMER_MR0, CTIMER_MR3, CTIMER_PWMC, CTIMER_MSR0, CTIMER_MSR3 = (
    0x04, 0x0C, 0x14, 0x18, 0x24, 0x74, 0x78, 0x84)
CTIMER_TCR_CEN = 1
# SYSCON's clock select for CTimer 1 and 3, and the clock gate bit for CTimer 1 in AHBCLKCTRL1.
CTIMER1_CLKSEL, CTIMER3_CLKSEL, AHBCLKCTRL1, AHBCLKCTRL1_TIMER1 = 0x40000270, 0x40000278, 0x40000204, 1 << 27
IOCON = 0x40001000
CTIMER_MCR_MR0R, CTIMER_MCR_MR0RL, CTIMER_MCR_MR3RL, CTIMER_PWMC_PWMEN3 = 1 << 1, 1 << 24, 1 << 27, 1 << 3
GPIO_SET, GPIO_CLR, GPIO_DIRSET = 0x2200, 0x2280, 0x2380
ID_DAP_HOST_STATUS = 0x01

# A flash shorter than this is one a person staring at the board can miss, and the whole point
# of the LED is to be seen.
LED_VISIBLE_MS = 50

# How long after the traffic stops the LED has to be back to what it showed. Generous: the check
# is that the flicker ends at all, not how long it lasts.
LED_SETTLE_MS = 800

# What the LED timers count at. A tick is one PWM period, so this is what turns ticks into time.
LED_TIMER_HZ = 1_000_000

# The largest step in L* a fade may take in one tick, out of 100.
LED_SMOOTH_LSTAR = 1.0

# How far an inverted LED has to be from what it would have shown, in L*.
LED_FLICKER_LSTAR = 40

# The Pro's LEDs on port 1 (src/led.c, BOARDS.md), and FUNC's ramp in ms (512 ticks).
PRO_LED_USB, PRO_LED_SWO, PRO_LED_VCOM, PRO_LED_FUNC, PRO_LED_SIO, PRO_LED_NRG = 12, 13, 15, 16, 17, 18
PRO_SWITCHED = (PRO_LED_USB, PRO_LED_SWO, PRO_LED_VCOM, PRO_LED_SIO, PRO_LED_NRG)
LED_RAMP_MS = (900, 1200)
LED_WIRE_NONE, LED_WIRE_SWD, LED_WIRE_JTAG, LED_WIRE_CJTAG = 0, 1, 2, 3
# Written into the port 1 pin registers before the LEDs start, to show which the firmware writes.
LED_PIN_SENTINEL = 0xAA


def lightness(level: int, period: int) -> float:
    """CIE 1976 L*, 0 to 100, of a PWM level: how bright the LED looks rather than its duty."""
    y = min(level, period) / period
    return 116 * y ** (1 / 3) - 16 if y > 216 / 24389 else y * 24389 / 27


def largest_step(values: list[float]) -> float:
    return max(abs(a - b) for a, b in zip(values, values[1:]))


def stretches(flags: list[bool]) -> list[int]:
    """The lengths of each unbroken run of True, except one the list ends inside."""
    runs, run = [], 0
    for flag in flags:
        if flag:
            run += 1
        elif run:
            runs.append(run)
            run = 0
    return runs


class LedTick:
    """CPU 0's LED tick on the emulator: the interrupt run the way the timer would, the levels it
    leaves read back."""

    def __init__(self, elf: str, request_slots: int, board: int = 0, jumpers: int = 0):
        self.probe = Machine(elf, Target([]), restart_at=set(), model=Model())
        self.ok = all(name in self.probe.sym for name in
                      ("board_clocks_init", "led_init", "CTIMER3_IRQHandler", "dap_shared", "led_ticks"))
        if not self.ok:
            return
        fields = shared_offsets(request_slots, "led_activity", "led_connected", "led_wire", "led_swo", "led_sio",
                                "board", "jumpers", small_enums=short_enums(elf))
        self.shared = {name: self.probe.sym["dap_shared"] + offset for name, offset in fields.items()}
        self.write("board", board)
        self.write("jumpers", jumpers)
        for pin in range(PRO_LED_USB, PRO_LED_NRG + 1):
            self.probe.uc.mem_write(ORDINARY_GPIO + REG_B0 + 32 + pin, bytes([LED_PIN_SENTINEL]))
        model_syscon(self.probe)
        self.probe.call("board_clocks_init")
        self.probe.call("led_init")
        self.period = self.read32(CTIMER3 + CTIMER_MR3) + 1
        self.tick_ms = self.period * 1000 / LED_TIMER_HZ

    def write(self, field: str, value: int) -> None:
        self.probe.uc.mem_write(self.shared[field], bytes([value]))

    def read32(self, address: int) -> int:
        return int.from_bytes(self.probe.uc.mem_read(address, 4), "little")

    def pin(self, pin: int) -> int:
        return self.probe.uc.mem_read(ORDINARY_GPIO + REG_B0 + 32 + pin, 1)[0]

    def ticks(self, value: int) -> None:
        self.probe.uc.mem_write(self.probe.sym["led_ticks"], u32(value))

    def run(self, n: int, read=None, every: int = 0, flag: str = "led_activity", burst: int = 1,
            until: int | None = None) -> list:
        """n ticks, each giving read(); by default the status LED's L*. With `every`, `flag` is
        set before the first `burst` of every that-many ticks, up to tick `until`."""
        read = read or (lambda: lightness(self.read32(CTIMER3 + CTIMER_MSR0), self.period))
        out = []
        for i in range(n):
            if every and i % every < burst and (until is None or i < until):
                if flag in self.shared:
                    self.write(flag, 1)
                else:
                    self.probe.uc.mem_write(self.probe.sym[flag], b"\x01")
            self.probe.call("CTIMER3_IRQHandler")
            out.append(read())
        return out


def check_led(c: Checker, elf: str, request_slots: int) -> None:
    """CTIMER3 drives the status LED as a PWM and its period interrupt decides the level.

    Nothing about the LED is visible from a capture -- P0_5 goes nowhere but the LED -- so the
    policy is checked here, by running the interrupt the way the timer would and reading back the
    level it writes, into MR0's shadow so the timer takes it as it wraps. MR0 is how long each
    period the pin is held low, and the LED is lit while it is low, so MR0 is the brightness: zero
    is dark and anything past the period is full. Each level is judged by its L*, how bright it
    looks.

    An idle probe breathes through the whole range in steps too small to see, and spends as long
    below half lightness as above it, which is what an even fade to the eye means. A probe whose
    host says it is connected sits fully lit. A command inverts the LED, at least LED_FLICKER_LSTAR
    from what it showed, for long enough to see, and once per burst; steady traffic keeps doing so.
    """
    led = LedTick(elf, request_slots)
    if not led.ok:
        c.problems.append("an LED symbol is missing, so the LED policy cannot be run")
        return
    c.check(led.read32(CTIMER3 + CTIMER_MCR) & CTIMER_MCR_MR0RL,
            "CTIMER3 does not take the status LED's level from MR0's shadow as it wraps")
    settle = int(LED_SETTLE_MS / led.tick_ms)

    breath = led.run(2 * int(10_000 / led.tick_ms))  # two breaths, if a breath is under ten seconds
    below = sum(1 for v in breath if v < 50) / len(breath)
    c.check(min(breath) <= 1, f"an idle probe's LED never goes darker than L* {min(breath):.1f}")
    c.check(max(breath) >= 99, f"an idle probe's LED never gets brighter than L* {max(breath):.1f}")
    c.check(largest_step(breath) <= LED_SMOOTH_LSTAR,
            f"an idle probe's fade jumps by L* {largest_step(breath):.1f} in one tick")
    c.check(0.4 <= below <= 0.6, f"an idle probe's LED spends {below:.0%} of a breath below half lightness")

    led.write("led_connected", 1)
    c.check(all(v >= 100 for v in led.run(1000)), "the LED is not fully lit after a host said it is connected")
    led.write("led_connected", 0)
    c.check(largest_step(led.run(1000)) <= LED_SMOOTH_LSTAR, "the LED does not go back to breathing after the host disconnected")

    # One command, landing at points through a breath and on a connected probe, against the same
    # ticks with no command.
    worst = settle
    for connected in (0, 1):
        led.write("led_connected", connected)
        for start in range(0, 4096, 64):
            led.run(settle)
            led.ticks(100_000 + start)
            quiet = led.run(settle)
            led.ticks(100_000 + start)
            busy = led.run(settle, every=settle)
            inverted = stretches([abs(a - b) >= LED_FLICKER_LSTAR for a, b in zip(quiet, busy)])
            if not c.check(len(inverted) == 1, f"one command inverted the LED {len(inverted)} times"):
                return
            worst = min(worst, inverted[0])
            c.check(quiet[-50:] == busy[-50:], f"the LED is still flickering {LED_SETTLE_MS} ms after one command")
    led.write("led_connected", 0)
    c.check(worst * led.tick_ms >= LED_VISIBLE_MS,
            f"a command flickers the LED for only {worst * led.tick_ms:.0f} ms")

    # Bursts of commands lasting 40 ms, 300 ms apart, flicker once each; a steady stream keeps on
    # flickering, at a rate the eye still separates.
    ticks = int(3000 / led.tick_ms)
    every = int(300 / led.tick_ms)
    for spacing_ms, want in ((300, (ticks - 1) // every + 1), (0, 15)):
        led.run(settle)
        led.ticks(100_000)
        quiet = led.run(ticks + settle)
        led.ticks(100_000)
        busy = led.run(ticks + settle, every=every if spacing_ms else 1, burst=int(40 / led.tick_ms), until=ticks)
        inverted = stretches([abs(a - b) >= LED_FLICKER_LSTAR for a, b in zip(quiet, busy)])
        what = "bursts of commands every 300 ms" if spacing_ms else "a steady stream of commands"
        enough = len(inverted) == want if spacing_ms else len(inverted) >= want
        c.check(enough and min(inverted, default=0) * led.tick_ms >= LED_VISIBLE_MS,
                f"{what} flickered the LED {len(inverted)} times in 3 s, wanted {want}, the shortest "
                f"{min(inverted, default=0) * led.tick_ms:.0f} ms")
    starts = [i for i in range(1, len(busy)) if abs(quiet[i] - busy[i]) >= LED_FLICKER_LSTAR
              and abs(quiet[i - 1] - busy[i - 1]) < LED_FLICKER_LSTAR]
    steady = 1000 * (len(starts) - 1) / ((starts[-1] - starts[0]) * led.tick_ms)
    c.note(f"LED: {LED_TIMER_HZ / led.period:.0f} Hz PWM, breathes L* 0 to 100 when idle, {below:.0%} of it below "
           f"half, full when connected; a command flickers it for {worst * led.tick_ms:.0f} ms, steady traffic "
           f"{steady:.1f} times a second")


def check_pro_leds(c: Checker, elf: str, request_slots: int) -> None:
    """The Pro's six other LEDs (BOARDS.md), and that no other board touches their pins.

    FUNC is CTIMER1's match 3 as a PWM, its level written to the shadow register that the timer
    takes as it wraps. Each wire has to show its own shape: SWD a rising ramp, cJTAG a falling one,
    JTAG a triangle, all at one rate and in steps too small to see. The others are switched
    GPIO, lit by a low pin.
    """
    plain = LedTick(elf, request_slots)
    if not plain.ok:
        c.problems.append("an LED symbol is missing, so the Pro's LEDs cannot be run")
        return
    pro_pins = [IOCON + 4 * (32 + pin) for pin in range(PRO_LED_USB, PRO_LED_NRG + 1)]
    plain.run(200, every=50)
    plain.run(200, every=50, flag="led_swo")
    touched = [hex(a) for a in pro_pins if plain.read32(a)]
    touched += [f"GPIO {name}" for name, at in (("SET", GPIO_SET), ("CLR", GPIO_CLR), ("DIRSET", GPIO_DIRSET))
                if plain.read32(ORDINARY_GPIO + at + 4) & (0x7F << PRO_LED_USB)]
    touched += [f"B[1][{pin}]" for pin in range(PRO_LED_USB, PRO_LED_NRG + 1) if plain.pin(pin) != LED_PIN_SENTINEL]
    touched += ["CTIMER1"] if plain.read32(CTIMER1 + CTIMER_PWMC) or plain.read32(CTIMER1 + CTIMER_MCR) else []
    c.check(not touched, f"the plain board's LEDs touched the Pro's LED pins: {', '.join(touched)}")

    led = LedTick(elf, request_slots, board=BOARD_PRO)
    funcs = {pin: led.read32(IOCON + 4 * (32 + pin)) & 0xF for pin in range(PRO_LED_USB, PRO_LED_NRG + 1)}
    want = {pin: 3 if pin == PRO_LED_FUNC else 0 for pin in PRO_SWITCHED + (PRO_LED_FUNC,)}
    c.check(all(funcs[pin] == f for pin, f in want.items()), f"the Pro's LED pins have functions {funcs}")
    c.check(funcs[14] == 0 and led.read32(IOCON + 4 * (32 + 14)) == 0, "PIO1_14, between the LEDs, was touched")
    switched = sum(1 << pin for pin in PRO_SWITCHED)
    driven = led.read32(ORDINARY_GPIO + GPIO_DIRSET + 4) & (0x7F << PRO_LED_USB)
    dark = led.read32(ORDINARY_GPIO + GPIO_SET + 4) & (0x7F << PRO_LED_USB)
    c.check(driven == switched and dark == switched,
            f"the Pro's switched LEDs: outputs {driven:#x}, set dark {dark:#x}, wanted {switched:#x}")
    timer = {name: led.read32(CTIMER1 + offset) for name, offset in
             (("TCR", CTIMER_TCR), ("PR", CTIMER_PR), ("MCR", CTIMER_MCR), ("MR0", CTIMER_MR0), ("PWMC", CTIMER_PWMC))}
    c.check(timer["MR0"] + 1 == led.period and timer["PR"] == led.read32(CTIMER3 + CTIMER_PR)
            and timer["MCR"] == CTIMER_MCR_MR0R | CTIMER_MCR_MR3RL and timer["PWMC"] == CTIMER_PWMC_PWMEN3
            and timer["TCR"] & CTIMER_TCR_CEN,
            f"CTIMER1 {', '.join(f'{k} {v:#x}' for k, v in timer.items())}: wanted the status LED's period and "
            f"prescale, reset on MR0, MR3 from its shadow, PWM on match 3, counting")
    c.check(led.read32(CTIMER1_CLKSEL) == led.read32(CTIMER3_CLKSEL) and led.read32(AHBCLKCTRL1) & AHBCLKCTRL1_TIMER1,
            f"CTIMER1 is not clocked as CTIMER3 is: select {led.read32(CTIMER1_CLKSEL):#x} against "
            f"{led.read32(CTIMER3_CLKSEL):#x}, AHBCLKCTRL1 {led.read32(AHBCLKCTRL1):#x}")

    def func() -> float:
        return lightness(led.read32(CTIMER1 + CTIMER_MSR3), led.period)

    ticks = 2048
    shapes = {}
    for wire in (LED_WIRE_NONE, LED_WIRE_SWD, LED_WIRE_CJTAG, LED_WIRE_JTAG):
        led.write("led_wire", wire)
        led.ticks(0)
        shapes[wire] = led.run(ticks, func)
    c.check(max(shapes[LED_WIRE_NONE]) == 0, "FUNC is not dark with no port connected")
    for wire, name, jump in ((LED_WIRE_SWD, "SWD", -1), (LED_WIRE_CJTAG, "cJTAG", 1)):
        levels = shapes[wire]
        steps = [b - a for a, b in zip(levels, levels[1:])]
        wraps = [i for i, s in enumerate(steps) if s * jump > 50]
        against = [s for s in steps if 0 < s * jump <= 50]
        smooth = max(-s * jump for s in steps if s * jump <= 50)
        spacing = [(b - a) * led.tick_ms for a, b in zip(wraps, wraps[1:])]
        c.check(len(wraps) >= 3 and all(LED_RAMP_MS[0] <= s <= LED_RAMP_MS[1] for s in spacing) and not against,
                f"FUNC for {name} is not a {'rising' if jump < 0 else 'falling'} ramp every "
                f"{LED_RAMP_MS[0]} to {LED_RAMP_MS[1]} ms: it starts again at ticks {wraps}, "
                f"{len(against)} steps the other way")
        c.check(smooth <= LED_SMOOTH_LSTAR and min(levels) <= 1 and max(levels) >= 99,
                f"FUNC for {name} ramps L* {min(levels):.0f} to {max(levels):.0f} in steps up to {smooth:.1f}")
    jtag = shapes[LED_WIRE_JTAG]
    # The ticks it arrives at each end, which alternate: a side is the time between two.
    ends = sorted([i for i in range(1, ticks) if jtag[i] <= 1 < jtag[i - 1]]
                  + [i for i in range(1, ticks) if jtag[i] >= 99 > jtag[i - 1]])
    halves = [(b - a) * led.tick_ms for a, b in zip(ends, ends[1:])]
    c.check(largest_step(jtag) <= LED_SMOOTH_LSTAR and min(jtag) <= 1 and max(jtag) >= 99
            and len(halves) >= 2 and all(LED_RAMP_MS[0] <= h <= LED_RAMP_MS[1] for h in halves),
            f"FUNC for JTAG is not a triangle of {LED_RAMP_MS[0]} to {LED_RAMP_MS[1]} ms a side: "
            f"it reaches an end at ticks {ends[:6]}, in steps up to {largest_step(jtag):.1f}")
    led.write("jumpers", 2)
    c.check(all(v >= 100 for v in led.run(500, func)), "FUNC is not solid with the SWD jumper fitted")
    led.write("jumpers", 0)

    def pin_lit(pin: int):
        return lambda: led.pin(pin) == 0

    settle = int(LED_SETTLE_MS / led.tick_ms)
    for pin, flag, name in ((PRO_LED_SWO, "led_swo", "SWO"), (PRO_LED_VCOM, "led_vcom_seen", "VCOM")):
        if flag not in led.shared and flag not in led.probe.sym:
            c.problems.append(f"no {flag}, so the {name} LED cannot be run")
            continue
        led.run(settle)
        quiet = led.run(settle, pin_lit(pin))
        busy = led.run(settle, pin_lit(pin), every=settle, flag=flag)
        lit = stretches(busy)
        c.check(not any(quiet) and len(lit) == 1 and lit[0] * led.tick_ms >= LED_VISIBLE_MS,
                f"the {name} LED: lit {sum(quiet)} ticks when quiet, {lit} after one burst")

    # TinyUSB's own answers, found by setting each byte of its device state in turn.
    usb = pin_lit(PRO_LED_USB)
    if "_usbd_dev" in led.probe.sym and "tud_mounted" in led.probe.sym and "tud_suspended" in led.probe.sym:
        state = led.probe.sym["_usbd_dev"]
        size = symbols_with_size(elf)["_usbd_dev"][1]

        def flag(answer: str) -> int | None:
            for offset in range(size):
                was = bytes(led.probe.uc.mem_read(state + offset, 1))
                led.probe.uc.mem_write(state + offset, b"\x01")
                found = led.probe.call(answer) & 0xFF
                led.probe.uc.mem_write(state + offset, was)
                if found:
                    return offset
            return None

        mounted_at, suspended_at = flag("tud_mounted"), flag("tud_suspended")
        if c.check(None not in (mounted_at, suspended_at), "TinyUSB's mounted or suspended state was not found"):
            unmounted = led.run(5, usb)
            led.probe.uc.mem_write(state + mounted_at, b"\x01")
            mounted = led.run(5, usb)
            led.probe.uc.mem_write(state + suspended_at, b"\x01")
            suspended = led.run(5, usb)
            c.check(not any(unmounted) and all(mounted) and not any(suspended),
                    f"the USB LED: lit {unmounted} unconfigured, {mounted} configured, {suspended} suspended")
    else:
        c.problems.append("the image lacks _usbd_dev, tud_mounted or tud_suspended, so the USB LED cannot be run")
    c.check(led.pin(PRO_LED_NRG) == LED_PIN_SENTINEL, "the tick drove NRG's PIO1_18 after leaving it dark")
    sio = []
    for driving in (0, 1, 0):
        led.write("led_sio", driving)
        sio.append(all(led.run(5, pin_lit(PRO_LED_SIO))))
    c.check(sio == [False, True, False], f"the SIO LED is not lit exactly while the host drives a DIO pin: {sio}")
    c.note(f"Pro LEDs: FUNC ramps in {512 * led.tick_ms:.0f} ms (SWD up, cJTAG down, JTAG both), solid "
           f"with the SWD jumper, dark with no port; SWO and VCOM flicker, USB lit while configured")


def check_led_inputs(c: Checker, core1: str, request_slots: int) -> None:
    """DAP_HostStatus and the wire in use reach CPU 0's LEDs through dap_shared. Activity is set by
    CPU 1's command loop rather than the command layer, and SWO's by a capture, so the bench is
    what shows those."""
    probe = Machine(core1, Target([0] * 64), restart_at=set(), model=Model())
    if "dap_shared" not in probe.sym:
        c.problems.append("CPU 1's image has no dap_shared")
        return
    fields = shared_offsets(request_slots, "led_connected", "led_wire", small_enums=short_enums(core1))
    connected = probe.sym["dap_shared"] + fields["led_connected"]
    wire = probe.sym["dap_shared"] + fields["led_wire"]
    probe.call("board_debug_pins_init")
    probe.command(bytes([ID_DAP_HOST_STATUS, 0, 1]))
    on = probe.uc.mem_read(connected, 1)[0]
    probe.command(bytes([ID_DAP_HOST_STATUS, 0, 0]))
    off = probe.uc.mem_read(connected, 1)[0]
    c.check((on, off) == (1, 0), f"DAP_HostStatus connected/disconnected left led_connected {on}/{off}")

    steps = (("DAP_Connect SWD", bytes([0x02, 1]), LED_WIRE_SWD), ("DAP_Connect JTAG", bytes([0x02, 2]), LED_WIRE_JTAG),
             ("vendor 0x83 mode 2", bytes([0x83, 2]), LED_WIRE_CJTAG), ("vendor 0x83 mode 0", bytes([0x83, 0]), LED_WIRE_JTAG),
             ("DAP_Disconnect", bytes([0x03]), LED_WIRE_NONE))
    for name, request, want in steps:
        probe.command(request)
        got = probe.uc.mem_read(wire, 1)[0]
        c.check(got == want, f"{name} left led_wire {got}, wanted {want}")
    c.note("LED inputs: DAP_HostStatus sets led_connected, and DAP_Connect, DAP_Disconnect and vendor 0x83 set "
           "led_wire, in dap_shared")


SYSCON = 0x40000000
# SYSCON's SET and CLR registers change the register they name, PRESETCTRLn at 0x100 and
# AHBCLKCTRLn at 0x200, rather than hold what was written (UM11126 chapter 4). A driver that resets
# a peripheral waits for PRESETCTRL to show it, so in plain memory it waits for ever.
SET_CLEAR = {0x120 + 4 * n: 0x100 + 4 * n for n in range(3)} | {0x220 + 4 * n: 0x200 + 4 * n for n in range(3)}


def model_syscon(probe: Machine) -> None:
    """Give SYSCON's page SET and CLR behaviour, for code that resets or clocks a peripheral."""
    page = bytearray(probe.uc.mem_read(SYSCON, 0x1000))
    probe.uc.mem_unmap(SYSCON, 0x1000)

    def read(uc, offset, size, data):
        return int.from_bytes(page[offset : offset + size], "little")

    def write(uc, offset, size, value, data):
        for set_at, target in SET_CLEAR.items():
            if offset in (set_at, set_at + 0x20):
                word = int.from_bytes(page[target : target + 4], "little")
                word = word | value if offset == set_at else word & ~value
                page[target : target + 4] = word.to_bytes(4, "little")
                return
        page[offset : offset + size] = value.to_bytes(size, "little")

    probe.uc.mmio_map(SYSCON, 0x1000, read, None, write, None)


# DMA0 and the consoles' Flexcomms. A descriptor may hold either alias of a FIFO.
DMA0_BASE, USART_BASES = 0x4008_2000, (0x4008_6000, 0x4008_7000)
SECURE_ALIAS = 0x1000_0000
DMA_XFERCFG_RX = 0x1 | 0x2 | 0x4 | 0x10 | (1 << 14) | (1023 << 16)  # CFGVALID RELOAD SWTRIG SETINTA DSTINC 1024
USART_FIFOCFG, USART_FIFOWR, USART_FIFORD = 0xE00, 0xE20, 0xE30
FIFOCFG_DMATX, FIFOCFG_DMARX = 1 << 12, 1 << 13
VCOM_RX_RING_BYTES = 4096


def check_console_dma(c: Checker, probe, name: str, count: int, table: int) -> None:
    """After dma0_init and vcom_init, DMA0 is on with its table in dap_shared, and each console's
    receive ring runs into its own buffer, round a circle of descriptors, and its send channel
    points at its UART, both with DMA requests on. USART_Init leaves the requests off and
    USART_Deinit turns them off, so a console that loses them receives nothing and reports no
    error. Console i is Flexcomm i on both boards with no jumper fitted. The channel enables are
    write-only, so the model cannot say whether they took."""
    def word(at: int) -> int:
        return int.from_bytes(probe.uc.mem_read(at, 4), "little")

    def same(a: int, b: int) -> bool:
        return (a & ~SECURE_ALIAS) == (b & ~SECURE_ALIAS)

    # The model maps the non-secure alias, which is the one the image's registers are at.
    dma = DMA0_BASE
    before = len(c.problems)
    c.check(word(dma) & 1, f"{name} board: DMA0 is not enabled")
    c.check(same(word(dma + 0x8), table), f"{name} board: DMA0's SRAMBASE is {word(dma + 0x8):#x}, not dap_shared.dma_table")
    for i in range(count):
        usart = USART_BASES[i]
        rx, tx = 4 + 2 * i, 5 + 2 * i
        entry_rx, entry_tx = table + 16 * rx, table + 16 * tx
        ring = probe.sym["vcom_rx_ring"] + VCOM_RX_RING_BYTES * i
        c.check(same(word(entry_rx + 4), usart + USART_FIFORD) and word(entry_rx + 8) == ring + 1023,
                f"{name} board: console {i}'s receive entry does not run from its FIFO into its ring")
        link, ends = word(entry_rx + 12), []
        segments = VCOM_RX_RING_BYTES // 1024
        for k in range(1, segments + 1):
            ends.append(word(link + 8))
            # Descriptor k raises INTA (bit 4) when k is even and INTB (bit 5) when odd (src/dma_ring.h).
            flag = 0x10 if k % segments % 2 == 0 else 0x20
            c.check(word(link) == (DMA_XFERCFG_RX & ~(0x4 | 0x10)) | flag and same(word(link + 4), usart + USART_FIFORD),
                    f"{name} board: console {i}'s ring descriptor at {link:#x} is not a reloading segment"
                    f" raising {'INTA' if flag == 0x10 else 'INTB'}")
            link = word(link + 12)
        c.check(link == word(entry_rx + 12) and ends == [ring + 1024 * (k + 2) - 1 for k in range(len(ends) - 1)] + [ring + 1023],
                f"{name} board: console {i}'s ring descriptors do not fill its ring in order and come round")
        c.check(word(dma + 0x400 + 0x10 * rx + 8) == DMA_XFERCFG_RX,
                f"{name} board: console {i}'s receive channel was not started as a ring of 1024-byte segments")
        c.check(same(word(entry_tx + 8), usart + USART_FIFOWR),
                f"{name} board: console {i}'s send entry does not write its FIFO")
        for channel in (rx, tx):
            c.check(word(dma + 0x400 + 0x10 * channel) & 1,
                    f"{name} board: DMA0 channel {channel} does not take requests from console {i}")
        fifocfg = word(usart + USART_FIFOCFG)
        c.check((fifocfg & (FIFOCFG_DMATX | FIFOCFG_DMARX)) == FIFOCFG_DMATX | FIFOCFG_DMARX,
                f"{name} board: console {i}'s UART has its DMA requests off, so DMA0 moves nothing")
    if len(c.problems) == before:
        c.note(f"after boot, {name} board: {count} console{'s' if count > 1 else ''} on DMA0, receive rings running")


def check_clocks(c: Checker, elf: str, request_slots: int) -> None:
    """Run the boot-time clock setup and read the clock each console's UART was set up with.

    This is the fix that would be invisible on the bench: the 96 MHz output of FRO192M is off out
    of reset and the board support never enables it, and FROHFDIV is halted as well, so a Flexcomm
    attached to FRO_HF_DIV gets nothing and a UART with no clock reports no error -- it just never
    reaches a baud rate. Running it against the register model says what the driver would read,
    and src/vcom.c keeps what it read for each console. On the plain board that is VCOM alone; on
    the Pro VCOM2 too, Flexcomm 1.
    """
    offsets = shared_offsets(request_slots, "board", "dma_table", small_enums=short_enums(elf))
    board = offsets["board"]
    for name, kind, consoles in (("plain", 0, 1), ("Pro", 1, 2)):
        probe = Machine(elf, Target([]), restart_at=set(), model=Model())
        for symbol in ("board_clocks_init", "dma0_init", "vcom_init", "vcom", "vcom_count", "vcom_rx_ring"):
            if symbol not in probe.sym:
                c.note(f"{symbol} was inlined away; the console clocks not checked")
                return
        probe.uc.mem_write(probe.sym["dap_shared"] + board, bytes([kind]))
        for port in range(2):
            probe.uc.mem_write(0x40086ff8 + port * 4096, u32(0x10))  # USART present
        model_syscon(probe)
        probe.call("board_clocks_init")
        probe.call("dma0_init", timeout_insns=100_000)
        probe.call("vcom_init", timeout_insns=1_000_000)
        if not c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC,
                       f"{name} board: vcom_init did not return; a driver waits on a register the model lacks"):
            continue
        count = int.from_bytes(probe.uc.mem_read(probe.sym["vcom_count"], 4), "little")
        if not c.check(count == consoles, f"{name} board: {count} consoles started, wanted {consoles}"):
            continue
        # SWO's Flexcomm is CPU 1's and attaches to the same FRO_HF_DIV, so this is its check too.
        clocks = [int.from_bytes(probe.uc.mem_read(probe.sym["vcom"] + 8 * i + 4, 4), "little") for i in range(count)]
        for i, clock in enumerate(clocks):
            c.check(
                clock == 96_000_000,
                f"{name} board: console {i} has {clock} Hz; its fractional divider is not back to one",
            )
        c.note(f"after boot, {name} board: console Flexcomms at {', '.join(f'{k / 1e6:g}' for k in clocks)} MHz, "
               "from the FRO_HF_DIV SWO uses as well")
        check_console_dma(c, probe, name, count, probe.sym["dap_shared"] + offsets["dma_table"])

    # What a host can actually ask a console for. The driver accepts anything within 3%, which
    # leaves a link no room for the other end's error, so check the rates against a tighter bound.
    console = 96_000_000
    worst = max(
        (abs(nxp_baudrate(console, r)[0] - r) / r, r) for r in (9600, 115200, 921600, 1_000_000, 3_000_000)
    )
    c.check(
        worst[0] < 0.01,
        f"the console is {worst[0] * 100:.2f}% out at {worst[1]} baud, which leaves a link nothing",
    )
    c.note(f"console baud error at worst {worst[0] * 100:.2f}%, at {worst[1]}")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", nargs="?", default="build/mculink_cjtag.elf", help="CPU 0's image")
    ap.add_argument("--core1", default="build/mculink_cjtag_core1.elf", help="CPU 1's image")
    ap.add_argument("--prefix", default="CJ", help="what the serial should start with")
    ap.add_argument("--config", default="src/DAP_config.h")
    ap.add_argument("--packet-size", type=int, default=512)
    # Two, not ARM's eight: TinyUSB's vendor class is a byte stream and a buffer of more than one
    # packet hands two commands to one read. src/tusb_config.h has the measurement.
    ap.add_argument("--packet-count", type=int, default=2)
    ap.add_argument("--code-lab", action="store_true", help="check the optional vendor 0x88 code lab")
    ap.add_argument("--timing-diagnostics", action="store_true", help="include timing counters in the staged layout")
    ap.add_argument("--core-clock-mhz", type=int, default=150, help="the main clock, src/core_clock.h")
    ap.add_argument("--stack-report", help="record executed-call SP minima as JSON; not a worst-case bound")
    args = ap.parse_args()

    observer = None
    if args.stack_report:
        from stack_usage import StackObserver
        observer = StackObserver()
        observer.install()

    c = Checker()
    check_allocator_trap(c, args.elf)
    check_allocator_trap(c, args.core1)
    # REQUEST_SLOTS is what the host is told as the packet count, and it sizes dap_shared.
    slots = args.packet_count
    swo = bool(config_value(args.config, "SWO_UART"))
    check_core1_image(c, args.elf, args.core1, slots, args.timing_diagnostics)
    check_stacks(c, args.elf, args.core1)
    check_sram_code(c, args.elf, "CPU 0 waits for work", ("wait_for_work", "energy_control_task", "vcom_ready", "tud_cdc_n_available",
                                                         "tud_cdc_n_write_available", "tu_edpt_stream_write_available"))
    check_sram_code(c, args.elf, "CPU 0 integrates energy", ("DMA0_IRQHandler", "energy_integrate_segment"))
    check_sram_code(c, args.elf, "CPU 0 runs a command's USB work",
                    ("serve", "tud_vendor_rx_cb", "tud_vendor_tx_cb", "tud_hid_set_report_cb",
                     "tud_hid_report_complete_cb", "memcpy", "aux_task", "aux_has_work"),
                    # The end of a reboot, which clang calls out of line, and a console taking a new line
                    # coding, which a host sends when it opens the port.
                    allow=("__NVIC_SystemReset", "vcom_configure"))
    check_sram_code(c, args.core1, "CPU 1 waits for work", ("wait_for_mailbox",))
    # Allowed: ARM's processing of every command not routed in src/dap_dispatch.c, regenerating
    # the transfers when an input changed, and the first DAP_Transfer after a line reset or a
    # DAP_JTAG_Configure, which may measure the read latency. The generated JTAG path only where it
    # is built.
    jtag_gen = ("jgen_TransferBlock", "block_whole", "jgen_IR", "jtag_gen_ready", "jtag_gen_ready_transfers",
                "jtag_gen_learn", "jgen_Transfer")
    if not any(name.split(".")[0] == "jtag_gen_ready" for name in symbols_with_size(args.core1)):
        jtag_gen = ()
    check_sram_code(c, args.core1, "CPU 1 runs a command's dispatch",
                    ("serve", "DAP_ExecuteCommandSized", "execute_one", "DAP_ProcessCommand", "transfer_block", "SWD_TransferBlock",
                     "SWD_TransferCommand", "JTAG_TransferCommand", "memcpy") + jtag_gen,
                    allow=("arm_command", "arm_DAP_ProcessCommand", "regenerate", "swd_sweep_connect", "jtag_sweep_connect"))
    check_clocks(c, args.elf, slots)
    check_led(c, args.elf, slots)
    check_pro_leds(c, args.elf, slots)
    check_led_inputs(c, args.core1, slots)
    check_commands(c, args.core1, args.code_lab, swo)
    check_response_bounds(c, args.core1, args.packet_size)
    check_request_bounds(c, args.core1, args.packet_size)
    check_bounded_commands(c, args.core1, slots)
    from dispatch_fuzz import campaign, check_vendor_plans
    for seed in (23, 24301):
        for capacity in (64, 512):
            campaign(c, args.core1, symbols_with_size(args.core1), seed=seed, steps=128, capacity=capacity)
    check_vendor_plans(c, args.core1, symbols_with_size(args.core1), vendor_boards(args.core1, slots))
    check_jtag_chain(c, args.core1)
    check_energy_descriptors(c, args.core1, slots)
    check_stream_storage(c, args.elf, args.core1)
    check_energy_control(c, args.elf, args.core1, slots)
    from energy_identity_checks import check_rng
    check_rng(c, args.core1, symbols_with_size(args.core1), slots)
    check_console_recovery(c, args.elf, slots)
    check_hid_padding(c, args.core1, slots)
    check_usb_transport(c, args.elf, args.core1, slots, args.code_lab)
    check_identity(c, args.core1, args.code_lab)
    check_board(c, args.core1, slots)
    check_dio(c, args.core1, slots)
    check_target_power(c, args.core1, slots)
    check_straps(c, args.elf, args.core1, slots)
    check_unconnected_block(c, args.core1)
    check_generation_refusals(c, args.core1)
    check_core1_placement(c, args.core1)
    check_image_boards(c, args.elf)
    check_sample_probe(c, args.core1)
    check_timestamp(c, args.core1)
    check_connect_on_capture_board(c, args.core1, slots)
    if swo:
        check_swo_baudrate(c, args.config, 96_000_000, args.core_clock_mhz * 1_000_000)
        from swo_checks import check_copy, check_restart
        offsets = shared_offsets(slots, "swo_ring", "swo_dma", "swo_segments", "swo_lost",
                                 small_enums=short_enums(args.core1))
        check_copy(c, args.core1, offsets["swo_ring"])
        check_restart(c, args.core1, offsets)
    check_info(c, args.core1, args.packet_size, args.packet_count, swo)
    for image in (args.elf, args.core1):
        check_serial(c, image, bytes(range(1, 17)), args.prefix)
        check_serial(c, image, b"\xff" * 16, args.prefix, uniform=True)

    print(f"{args.elf} with {args.core1}")
    for n in c.notes:
        print(f"  {n}")
    for p in c.problems:
        print(f"  FAIL  {p}")
    if not c.problems:
        print("  descriptors and serial are self-consistent")
    if observer is not None:
        observer.finish(args.stack_report, not c.problems)
    return 1 if c.problems else 0


def _read_at(elf: str, addr: int, size: int, section: str = ".text") -> bytes:
    """The bytes a symbol occupies, read out of the image.

    Three things to get right. `objdump -s` dumps every section that covers the range and the
    debug sections overlap, so take only the one the descriptors live in. Each line ends with an
    ASCII rendering that can itself look like hex, so cap the groups per line at four. And a line
    starts at a sixteen-byte boundary whatever address was asked for, so place the bytes by the
    address they are printed with rather than by where they land in the output.
    """
    out = subprocess.run(
        [
            "arm-none-eabi-objdump",
            "-s",
            "-j",
            section,
            # Widened to whole dump lines: objdump truncates the last one at the stop address,
            # and the bytes are placed by address afterwards anyway.
            f"--start-address={addr & ~0xF}",
            f"--stop-address={(addr + size + 0xF) & ~0xF}",
            elf,
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    at: dict[int, int] = {}
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-f]+) ((?:[0-9a-f]{2,8} ?){1,4})", line)
        if not m:
            continue
        where = int(m.group(1), 16)
        for group in m.group(2).split():
            if len(group) % 2:
                continue
            for byte in bytes.fromhex(group):
                at[where] = byte
                where += 1
    return bytes(at[a] for a in range(addr, addr + size) if a in at)


if __name__ == "__main__":
    sys.exit(main())
