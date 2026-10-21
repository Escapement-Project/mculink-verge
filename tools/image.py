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
    load_address,
    sections,
    symbols,
    u16,
    u32,
)
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_R0  # noqa: E402

UUID_ADDRESS = 0x0009FC70
ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
USB_SRAM = (0x40100000, 0x00004000)


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


def shared_offsets(request_slots: int, *fields: str, small_enums: bool = True) -> dict[str, int]:
    """Member offsets of dap_shared_t, and its size as "size", as the firmware's compiler lays it
    out. A host compiler would not: its pointers are a different size. An image's enum size is
    its compiler's choice, so pass the image's own."""
    entries = [f"offsetof(dap_shared_t, {f})" for f in fields] + ["sizeof(dap_shared_t)"]
    source = '#include <stddef.h>\n#include "dap_shared.h"\n'
    source += f"const unsigned layout[] = {{{', '.join(entries)}}};\n"
    with tempfile.TemporaryDirectory() as tmp:
        c_file, obj, raw = (os.path.join(tmp, n) for n in ("layout.c", "layout.o", "layout.bin"))
        open(c_file, "w").write(source)
        subprocess.run(["arm-none-eabi-gcc", "-mthumb", "-mcpu=cortex-m33", f"-DREQUEST_SLOTS={request_slots}U",
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
    0x01: b"\x00",  # HostStatus
    0x02: b"\x01",  # Connect, SWD
    0x03: b"",  # Disconnect
    0x04: b"\x00\x02\x00\x00\x00",  # TransferConfig
    0x05: b"\x00\x00",  # Transfer, no transfers
    0x06: b"\x00\x00\x00\x00",  # TransferBlock, no words
    0x08: b"\x00\x00\x00\x00",  # WriteABORT
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
    # Our REBOOT, asking for the firmware rather than the bootloader. Safe to run here because the
    # command only arms the request and reboot_poll carries it out from the main loop, which
    # nothing in this harness runs. A build that moved the reset into the command would hang here,
    # which is the right thing for this check to do.
    0x85: b"\x00",
    # Our TARGET, reading the supply. The emulator's ADC never finishes calibrating, so this takes
    # the timeout path and answers DAP_ERROR: what is checked is that it answers at all, which is
    # the property that matters on hardware too.
    0x86: b"\x00\x00",
    # Our SAMPLE, no reads. The port is disconnected by this point, so it answers DAP_ERROR;
    # check_sample_probe runs it connected.
    0x87: b"\x00\x00\xff\x00\x00\x00\x00\x00\x00",
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
    """swo_uart_divisors in src/swo_ring.h, by brute force: the rate, its oversampling and its
    error. The C is tested against the same search on the host (tools/swo_ring_test.c)."""
    periods = {o * d: o for o in range(5, 17) for d in range(1, 65537)
               if abs(clock / (o * d) - want) <= want * 0.05}
    period = min(periods, key=lambda p: (abs(clock / p - want), -periods[p]))
    oversampling = max(o for o in range(5, 17) if period % o == 0 and period // o <= 65536)
    return clock // period, oversampling, abs(clock / period - want) / want


def check_swo_baudrate(c: Checker, config: str, clock: int) -> None:
    """SWO_Baudrate clamps every request to SWO_UART_MAX_BAUDRATE and refuses a rate more than 3%
    from any it can make, so a maximum it cannot make fails every request at or above it."""
    m = re.search(r"^#define\s+SWO_UART_MAX_BAUDRATE\s+(\d+)U?", open(config).read(), re.M)
    if not m:
        c.note("SWO_UART_MAX_BAUDRATE not found; the SWO rate not checked")
        return
    want = int(m.group(1))
    actual, oversampling, error = swo_rate(clock, want)
    c.check(
        error <= 0.03,
        f"SWO's maximum of {want} Hz is {error * 100:.2f}% from anything a"
        f" {clock // 1_000_000} MHz Flexcomm can produce, so every request at or above it fails",
    )
    c.check(oversampling >= 8, f"SWO's maximum needs an oversampling ratio of {oversampling}")
    c.note(f"SWO maximum {want / 1e6:g} MHz -> {actual / 1e6:g} MHz, {error * 100:.2f}% out,"
           f" oversampling {oversampling}")


# Where the cycle counter and its two enables live, from the ARMv8-M core definitions.
DCB_DEMCR, DWT_CTRL, DWT_CYCCNT = 0xE000EDFC, 0xE0001000, 0xE0001004
DEMCR_TRCENA, DWT_CYCCNTENA, DWT_NOCYCCNT = 1 << 24, 1 << 0, 1 << 25


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

    One caveat worth keeping honest. CPU 0's clock setup never runs here, so Flexcomm 3 reads as
    0 Hz and SWO_Baudrate answers 0, and capture never starts. The SWO commands answer, which is
    what is being checked, but nothing here says SWO works: that is tools/swo_ring_test.c for the
    arithmetic and the bench for the rest. A build without SWO has to refuse them.
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
    # Anything we do not implement has to say so rather than echo itself back.
    for cmd in (0x07, 0x88, 0x9F):
        reply = probe.command(bytes([cmd, 0, 0, 0]))
        if cmd == 0x07:
            continue  # TransferAbort is handled out of band and answers nothing
        if cmd == 0x88 and code_lab:
            continue  # check_identity verifies the lab's info command
        if not reply or reply[0] != ID_DAP_INVALID:
            c.problems.append(f"command {cmd:#04x} should answer ID_DAP_Invalid")
    c.note(
        f"{answered} of {len(commands)} commands answered, unimplemented ids rejected"
        + (" (SWO with Flexcomm 3 unclocked; see the note above)" if swo else ", SWO not built")
    )


# What a host can make the firmware write, and where it lands. The reply length is chosen by the
# request, not by the firmware: DAP_TransferBlock takes a sixteen-bit word count, DAP_Transfer an
# eight-bit transfer count that can answer with eight bytes each, and DAP_ExecuteCommands runs up
# to 255 sub-commands without checking how much of the packet arrived. None of ARM's command layer
# compares the reply against the buffer.
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
    """Nothing a host can send may write past the response buffer.

    Found by asking the question rather than by reading the code: a five-byte request that says
    65535 words produces a 256 KB reply, which on this part runs off the end of SRAM through the
    stack. It is ARM's reference behaviour, so every probe built from it has it.

    The canary goes past the end of the buffer the image actually contains, taken from the ELF, so
    this measures the shipped size rather than a number repeated here.
    """
    sym = symbols_with_size(elf)
    if "response_copy" not in sym:
        c.note("the response buffer was folded away; its bounds not checked")
        return
    size = sym["response_copy"][1]
    resp = SCRATCH + 0x400
    ceiling = SRAM[0] + SRAM[1] - 0x1000  # clear of the stack, which is at the top
    if resp + size >= ceiling:
        c.problems.append(f"the response buffer is {size} bytes, too large to fence in the harness")
        return

    worst = 0
    for label, payload in OVERFLOW_CASES:
        probe = build(elf, "swd", True, Model())
        if "DAP_Setup" in probe.sym:
            probe.call("DAP_Setup")
        probe.command(bytes([0x02, 1]))  # Connect, SWD
        probe.command(bytes([0x11]) + u32(10_000_000))  # SWJ_Clock
        probe.command(bytes([0x04, 0]) + u16(100) + u16(0))  # TransferConfigure
        probe.command(bytes([0x13, 0x00]))  # SWD_Configure
        probe.uc.mem_write(SCRATCH, payload + bytes(0x400 - len(payload)))
        probe.uc.mem_write(resp, b"\xee" * (ceiling - resp))
        try:
            produced = probe.call("DAP_ExecuteCommand", SCRATCH, resp) & 0xFFFF
        except SystemExit as exc:
            c.problems.append(f"{label} faulted: {exc}")
            continue
        tail = bytes(probe.uc.mem_read(resp + size, ceiling - resp - size))
        past = len(tail.rstrip(b"\xee"))
        if past:
            c.problems.append(f"{label} wrote {past} bytes past the {size}-byte response buffer")
        if produced > size:
            c.problems.append(f"{label} returned {produced} bytes for a {size}-byte buffer")
        worst = max(worst, produced)
    if not c.problems:
        c.note(
            f"the longest reply {len(OVERFLOW_CASES)} oversized requests could produce is {worst}"
            f" bytes, inside the {size}-byte buffer, and only {packet_size} of it is ever sent"
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


def check_request_bounds(c: Checker, elf: str) -> None:
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
        if "DAP_Setup" in probe.sym:
            probe.call("DAP_Setup")
        probe.command(bytes([0x02, 1]))  # Connect, SWD
        probe.uc.mem_write(SCRATCH, payload[:0x400] + bytes(max(0, 0x400 - len(payload))))
        before = bytes(probe.uc.mem_read(base + size, watch))
        resp = SCRATCH + 0x400
        try:
            probe.call("DAP_ExecuteCommand", SCRATCH, resp)
        except SystemExit as exc:
            c.problems.append(f"{label} faulted: {exc}")
            continue
        after = bytes(probe.uc.mem_read(base + size, watch))
        if before != after:
            spoiled = sum(1 for x, y in zip(before, after) if x != y)
            c.problems.append(f"{label} changed {spoiled} bytes past the {size}-byte DAP_Data")
        reply = bytes(probe.uc.mem_read(resp, 2))
        if reply[0] != payload[0] or reply[1] != 0xFF:
            c.problems.append(f"{label} answered {reply.hex()}, wanted the command id then DAP_ERROR")
    if not c.problems:
        c.note(f"{len(REQUEST_CASES)} oversized requests are refused and leave DAP_Data's neighbours alone")


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
    if version != 3:
        c.problems.append(f"vendor 0x82 format version is {version}, wanted 3")
    if reply[9 + length : 10 + length] != bytes([0]):
        c.problems.append(f"vendor 0x82 ends {reply[9 + length :].hex()!r}, wanted the plain board's 00")
    if not flags & 0x01:
        c.problems.append(f"vendor 0x82 flags {flags:#04x} do not claim OScan1, which is the point")
    if flags & 0x02:
        c.problems.append("vendor 0x82 says OScan1 is active before anything turned it on")
    if not build:
        c.problems.append("vendor 0x82 carries no build string, so two builds look identical")

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

    # A mode the firmware does not define has to be refused, not guessed at: getting it wrong
    # costs the probe until someone replugs it.
    refused = probe.command(bytes([0x85, 0x7F]))
    if len(refused) < 2 or refused[0] != 0x85 or refused[1] == 0x00:
        c.problems.append(f"vendor 0x85 accepted an undefined reboot mode: {refused.hex()}")

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
# The boards the image declares for verge (src/board_straps.c). The Pro joins once its support is
# complete.
IMAGE_BOARDS = 1 << 0


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
    """Interfaces, endpoints and functions of PRO.md's one layout: CMSIS-DAP v2 on EP1 and v1 on EP4,
    VCOM on EP3 with its notification at 0x86, VCOM2 on EP5 with its notification at 0x87. A
    function the board lacks or a jumper turns off is left out: the SWD jumper takes both CMSIS-DAP
    interfaces, as stock's does. The functions are bcdDevice's low byte: 1 debug, 2 VCOM, 4 VCOM2."""
    interfaces, endpoints, functions = 0, set(), 0
    if not jumpers & 2:
        interfaces += 2
        endpoints |= {0x01, 0x81, 0x04, 0x84}
        functions |= 1
    if not jumpers & 1:
        interfaces += 2
        endpoints |= {0x03, 0x83, 0x86}
        functions |= 2
    if board == BOARD_PRO:
        interfaces += 2
        endpoints |= {0x05, 0x85, 0x87}
        functions |= 4
    return interfaces, endpoints, functions


def check_straps(c: Checker, elf: str, core1: str, request_slots: int) -> None:
    """CPU 0 reads the board and the jumpers, and everything that follows from them: the USB
    configuration, and the product string USB and DAP_Info both give.

    Each board's straps with every combination of jumpers. The configuration has to be well formed,
    give every function its own endpoints in the one layout, and keep the notification endpoints at
    numbers the controller does not have, declared and never opened, polled every 2^15 microframes.
    """
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
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
        check_device(inner, device, bos=bool(functions & 1))
        c.problems += [f"{state}: {p}" for p in inner.problems]
        (bcd_device,) = struct.unpack_from("<H", device, 12)
        c.check(bcd_device == 0x0100 | functions, f"{state}: bcdDevice {bcd_device:#06x}, wanted {0x0100 | functions:#06x}")
        bos = probe.call("tud_descriptor_bos_cb")
        c.check((bos != 0) == bool(functions & 1), f"{state}: the BOS is {'there' if bos else 'missing'}")

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
        if functions & 1:
            c.check(len(d) > 9 + 5 and d[9 + 2] == 0 and d[9 + 5] == 0xFF,
                    f"{state}: interface 0 is not CMSIS-DAP v2, which MS OS 2.0 names")
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


def check_board(c: Checker, elf: str, request_slots: int) -> None:
    """Only the plain MCU-Link lets the marker near its pins, and an on-board one keeps two-pin cJTAG
    off (PRO.md).

    On a Pro the J8 pins are the analog trace input, a level shifter's direction and two SPI lines,
    so driving them fights other drivers. CPU 1 takes the board from dap_shared, as CPU 0 read it.
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
            c.problems.append(f"{name} board: vendor 0x82 flags {flags:#04x} get two-pin cJTAG wrong")
        if (j8[1] == 0) != plain:
            c.problems.append(f"{name} board: vendor 0x84 answered {j8.hex()}")
        if (enter[1] == 0) != two_pin:
            c.problems.append(f"{name} board: vendor 0x83 mode 2 answered {enter.hex()}")
        if bool(driven & 0x0F) != plain:
            c.problems.append(f"{name} board: J8's pins were {'not ' if plain else ''}driven ({driven:#x})")
    c.note("board: CPU 1 reports each board as CPU 0 read it; J8 on the plain one only, two-pin on the plain one and the Pro")


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
                    r"\s+([0-9a-f]+)?(?: <(\S+)>)?")


def check_sram_code(c: Checker, elf: str, what: str, names: tuple[str, ...], allow: tuple[str, ...] = ()) -> None:
    """Both cores wait for work from SRAM and run each command's own code from there: a core
    fetching from flash slows the other's command code (src/main.c), and CPU 1's wait polls a
    level instead of sleeping (src/core1_main.c). A dropped section attribute, or a call the
    compiler adds back into flash, would undo that and change nothing else checked here.

    `allow` names flash functions a branch may reach anyway, ones that run once rather than per
    command or per wait, matched without the suffix LTO adds to a local symbol's name. A call
    through a register is not checked: on these paths it goes to the generated code in SRAMX."""
    sym = symbols_with_size(elf)
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
            if veneer and veneer.group(1) in sym:
                target = sym[veneer.group(1)][0]
            callee = veneer.group(1) if veneer else (m.group(4) or "")
            if callee.split(".")[0] in allow:
                continue
            c.check(target is not None and target in sram,
                    f"{name} branches out of SRAM at {m.group(1)}: {m.group(2)} {m.group(4) or m.group(3)}")
    if len(c.problems) == before:
        c.note(f"{what} from SRAM: {' and '.join(names)}, and every branch they take")


def check_core1_image(c: Checker, elf: str, core1: str, request_slots: int) -> None:
    """CPU 0 starts CPU 1 from the image it embeds, and both reach the same shared struct.

    Built separately, the two images agree on nothing a linker can check. So this checks what each
    needs of the other: the embedded bytes are the CPU 1 image, the vector table CPU 0 points at
    starts CPU 1 on its own stack, and dap_shared has one address and one layout in both.
    """
    sym0, sym1 = symbols_with_size(elf), symbols_with_size(core1)
    # The linker script defines the stack top, and a linker symbol has no size.
    top = symbols(core1).get("__core1_stack_top")
    missing = [f"{name} (CPU 0)" for name in ("core1_image", "dap_shared") if name not in sym0]
    missing += [f"{name} (CPU 1)" for name in ("core1_vectors", "core1_reset", "dap_shared") if name not in sym1]
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

    c.check("DAP_ExecuteCommand" not in sym0, "CPU 0's image carries the DAP command layer")
    c.check(not any(name.startswith("tud_") for name in sym1), "CPU 1's image carries TinyUSB")
    foreign = foreign_instructions(core1)
    c.check(not foreign, f"CPU 1 has no FPU or DSP extension, and its image uses them: {'; '.join(foreign[:5])}")
    c.note(f"CPU 1's image is embedded at {vectors:#x}, {len(linked)} bytes, and uses no FPU or DSP"
           f" instruction; dap_shared is {size0} bytes at {at0:#x} in both")


DAP_ID_PRODUCT, DAP_ID_SER_NUM = 2, 3


CTIMER3_MR0 = 0x40029018
CTIMER3_MR3 = 0x40029024
ID_DAP_HOST_STATUS = 0x01

# A flash shorter than this is one a person staring at the board can miss, and the whole point
# of the LED is to be seen.
LED_VISIBLE_MS = 50

# How long after the traffic stops the LED has to have stopped blinking. Generous: the check is
# that the blinking ends at all, not what the hold time is.
LED_SETTLE_MS = 800

# What CTIMER3 counts at. A tick is one PWM period, so this is what turns ticks into time.
LED_TIMER_HZ = 1_000_000


def check_led(c: Checker, elf: str, request_slots: int) -> None:
    """CTIMER3 drives the LED as a PWM and its period interrupt decides the level.

    Nothing about the LED is visible from a capture -- P0_5 goes nowhere but the LED -- so the
    policy is checked here, by running the interrupt the way the timer would and reading back the
    match register it writes. MR0 is how long each period the pin is held low, and the LED is lit
    while it is low, so MR0 is the brightness: zero is dark and anything past the period is full.

    Three properties. An idle probe breathes, smoothly and through nearly the whole range. A probe
    whose host says it is connected sits fully lit. And one command is visible: its flash is cut
    out of a blink phase the tick owns rather than started where the command lands, so the length
    depends on the tick it arrives on, and the shortest of those is what a person would see.
    """
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    for name in ("board_clocks_init", "led_init", "CTIMER3_IRQHandler", "dap_shared", "led_ticks"):
        if name not in probe.sym:
            c.note(f"{name} is not a symbol; the LED policy not checked")
            return
    # CPU 1 sets these; check_led_inputs checks that it does.
    fields = shared_offsets(request_slots, "led_activity", "led_connected", small_enums=short_enums(elf))
    seen = probe.sym["dap_shared"] + fields["led_activity"]
    connected = probe.sym["dap_shared"] + fields["led_connected"]

    probe.call("board_clocks_init")
    probe.call("led_init")
    period = int.from_bytes(probe.uc.mem_read(CTIMER3_MR3, 4), "little") + 1
    smooth = period // 50  # the largest step a fade may take in one tick
    tick_ms = period * 1000 / LED_TIMER_HZ
    settle = int(LED_SETTLE_MS / tick_ms)

    def run(n: int) -> list[int]:
        levels = []
        for _ in range(n):
            probe.call("CTIMER3_IRQHandler")
            levels.append(int.from_bytes(probe.uc.mem_read(CTIMER3_MR0, 4), "little"))
        return levels

    def largest_step(levels: list[int]) -> int:
        return max(abs(a - b) for a, b in zip(levels, levels[1:]))

    levels = run(2 * int(10_000 / tick_ms))  # two breaths, if a breath is under ten seconds
    c.check(min(levels) <= smooth, f"an idle probe's LED never goes darker than {min(levels)}/{period}")
    c.check(max(levels) >= period * 9 // 10, f"an idle probe's LED never gets brighter than {max(levels)}/{period}")
    c.check(largest_step(levels) <= smooth, f"an idle probe's fade jumps by {largest_step(levels)}/{period} in one tick")

    probe.uc.mem_write(connected, b"\x01")
    c.check(all(level >= period for level in run(1000)), "the LED is not fully lit after a host said it is connected")
    probe.uc.mem_write(connected, b"\x00")
    c.check(largest_step(run(1000)) <= smooth, "the LED does not go back to breathing after the host disconnected")

    # One command, landing on each tick of a blink period in turn. 128 covers any half-period up
    # to 64 ticks without this check knowing which one the firmware picked. What is measured is
    # the longest unbroken stretch the LED is fully lit, because that is the flash a person sees.
    ticks = probe.sym["led_ticks"]
    worst = settle
    for phase in range(128):
        probe.uc.mem_write(ticks, u32(100_000 + phase))
        probe.uc.mem_write(seen, b"\x01")
        longest = 0
        stretch = 0
        for level in run(settle):
            stretch = stretch + 1 if level >= period else 0
            longest = max(longest, stretch)
        worst = min(worst, longest)
        if largest_step(run(200)) > smooth:
            c.problems.append(f"the LED is still blinking {LED_SETTLE_MS} ms after one command")
            return

    c.check(
        worst * tick_ms >= LED_VISIBLE_MS,
        f"a command landing on the wrong tick flashes the LED for only {worst * tick_ms:.0f} ms",
    )
    c.note(
        f"LED: {LED_TIMER_HZ / period:.0f} Hz PWM, breathes 0 to {max(levels) * 100 // period}% when idle, "
        f"full when connected, one command flashes it for {worst * tick_ms:.0f} ms at worst"
    )


def check_led_inputs(c: Checker, core1: str, request_slots: int) -> None:
    """DAP_HostStatus reaches CPU 0's LED through dap_shared. Activity is set by CPU 1's command
    loop rather than the command layer, so the bench is what shows it."""
    probe = Machine(core1, Target([]), restart_at=set(), model=Model())
    if "dap_shared" not in probe.sym:
        c.problems.append("CPU 1's image has no dap_shared")
        return
    connected = probe.sym["dap_shared"] + shared_offsets(request_slots, "led_connected",
                                                         small_enums=short_enums(core1))["led_connected"]
    probe.command(bytes([ID_DAP_HOST_STATUS, 0, 1]))
    on = probe.uc.mem_read(connected, 1)[0]
    probe.command(bytes([ID_DAP_HOST_STATUS, 0, 0]))
    off = probe.uc.mem_read(connected, 1)[0]
    if c.check((on, off) == (1, 0), f"DAP_HostStatus connected/disconnected left led_connected {on}/{off}"):
        c.note("LED inputs: DAP_HostStatus sets and clears led_connected in dap_shared")


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


def check_clocks(c: Checker, elf: str, request_slots: int) -> None:
    """Run the boot-time clock setup and read the clock each console's UART was set up with.

    This is the fix that would be invisible on the bench: the 96 MHz output of FRO192M is off out
    of reset and the board support never enables it, and FROHFDIV is halted as well, so a Flexcomm
    attached to FRO_HF_DIV gets nothing and a UART with no clock reports no error -- it just never
    reaches a baud rate. Running it against the register model says what the driver would read,
    and src/vcom.c keeps what it read for each console. On the plain board that is VCOM alone; on
    the Pro VCOM2 too, Flexcomm 1.
    """
    board = shared_offsets(request_slots, "board", small_enums=short_enums(elf))["board"]
    for name, kind, consoles in (("plain", 0, 1), ("Pro", 1, 2)):
        probe = Machine(elf, Target([]), restart_at=set(), model=Model())
        for symbol in ("board_clocks_init", "vcom_init", "vcom", "vcom_count"):
            if symbol not in probe.sym:
                c.note(f"{symbol} was inlined away; the console clocks not checked")
                return
        probe.uc.mem_write(probe.sym["dap_shared"] + board, bytes([kind]))
        model_syscon(probe)
        probe.call("board_clocks_init")
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
    args = ap.parse_args()

    sym = symbols_with_size(args.elf)
    missing = [n for n in ("desc_bos",) if n not in sym]
    if missing:
        print(f"  cannot check: {', '.join(missing)} not in the image")
        return 1

    c = Checker()
    ms_os = next((v for k, v in sym.items() if k.startswith("desc_ms_os_20")), None)
    if ms_os:
        check_bos(c, _read_at(args.elf, *sym["desc_bos"]), ms_os[1])
    else:
        c.note("the MS OS 2.0 descriptor was folded away; its length not cross-checked")
    # REQUEST_SLOTS is what the host is told as the packet count, and it sizes dap_shared.
    slots = args.packet_count
    swo = bool(config_value(args.config, "SWO_UART"))
    check_core1_image(c, args.elf, args.core1, slots)
    check_sram_code(c, args.elf, "CPU 0 waits for work", ("wait_for_work", "vcom_ready", "tud_cdc_n_available",
                                                         "tud_cdc_n_write_available", "tu_edpt_stream_write_available"))
    # The HID report callback is left out: it copies with the library's memcpy, in flash, and
    # HID is the transport for hosts that cannot use the bulk one.
    check_sram_code(c, args.elf, "CPU 0 runs a command's USB work",
                    ("serve", "tud_vendor_rx_cb", "tud_vendor_tx_cb", "tud_hid_report_complete_cb"),
                    allow=("__NVIC_SystemReset",))  # the end of a reboot, which clang calls out of line
    check_sram_code(c, args.core1, "CPU 1 waits for work", ("wait_for_mailbox",))
    # Allowed: ARM's processing of every command not routed in src/dap_dispatch.c, regenerating
    # the transfers when an input changed, and the first DAP_Transfer after a line reset or a
    # DAP_JTAG_Configure, which may measure the read latency. The generated JTAG path only where it
    # is built.
    jtag_gen = ("jgen_TransferBlock", "block_whole", "jgen_IR", "jtag_gen_ready", "jtag_gen_learn", "jgen_Transfer")
    if not any(name.split(".")[0] == "jtag_gen_ready" for name in symbols_with_size(args.core1)):
        jtag_gen = ()
    check_sram_code(c, args.core1, "CPU 1 runs a command's dispatch",
                    ("serve", "DAP_ExecuteCommand", "DAP_ProcessCommand", "transfer_block", "SWD_TransferBlock",
                     "SWD_TransferCommand", "memcpy") + jtag_gen,
                    allow=("arm_command", "regenerate", "swd_sweep_connect", "jtag_sweep_connect"))
    check_clocks(c, args.elf, slots)
    check_led(c, args.elf, slots)
    check_led_inputs(c, args.core1, slots)
    check_commands(c, args.core1, args.code_lab, swo)
    check_response_bounds(c, args.core1, args.packet_size)
    check_request_bounds(c, args.core1)
    check_identity(c, args.core1, args.code_lab)
    check_board(c, args.core1, slots)
    check_straps(c, args.elf, args.core1, slots)
    check_unconnected_block(c, args.core1)
    check_image_boards(c, args.elf)
    check_sample_probe(c, args.core1)
    check_timestamp(c, args.core1)
    if swo:
        check_swo_baudrate(c, args.config, 96_000_000)
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
