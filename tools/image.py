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
"""

import argparse
import re
import struct
import subprocess
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from cycles import SRAM, Machine, Model, Target  # noqa: E402
from unicorn.arm_const import UC_ARM_REG_R0  # noqa: E402

UUID_ADDRESS = 0x0009FC70


def symbols_with_size(elf: str) -> dict[str, tuple[int, int]]:
    out = {}
    for line in subprocess.run(
        ["arm-none-eabi-nm", "-S", elf], capture_output=True, text=True, check=True
    ).stdout.splitlines():
        parts = line.split()
        if len(parts) == 4:
            out[parts[3]] = (int(parts[0], 16), int(parts[1], 16))
    return out


class Checker:
    def __init__(self) -> None:
        self.problems: list[str] = []
        self.notes: list[str] = []

    def check(self, ok: bool, message: str) -> bool:
        if not ok:
            self.problems.append(message)
        return ok

    def note(self, message: str) -> None:
        self.notes.append(message)


def check_device(c: Checker, d: bytes) -> None:
    c.check(len(d) == 18, f"device descriptor is {len(d)} bytes, wanted 18")
    c.check(d[0] == 18, f"device descriptor bLength is {d[0]}, wanted 18")
    c.check(d[1] == 0x01, f"device descriptor bDescriptorType is {d[1]:#x}, wanted 0x01")
    (bcd_usb,) = struct.unpack_from("<H", d, 2)
    c.check(bcd_usb >= 0x0210, f"bcdUSB is {bcd_usb:#06x}; the BOS descriptor needs 0x0210 or more")
    c.check(d[7] in (8, 16, 32, 64), f"bMaxPacketSize0 is {d[7]}, not a legal control size")
    vid, pid = struct.unpack_from("<HH", d, 8)
    c.check(d[17] == 1, f"bNumConfigurations is {d[17]}, wanted 1")
    for name, index in (("iManufacturer", d[14]), ("iProduct", d[15]), ("iSerialNumber", d[16])):
        c.check(index != 0, f"{name} is 0, so the host has no string to read")
    c.note(f"device {vid:#06x}:{pid:#06x}, USB {bcd_usb >> 8:x}.{(bcd_usb >> 4) & 0xF:x}, EP0 {d[7]}")


def check_configuration(c: Checker, d: bytes) -> dict[int, str]:
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
}
ID_DAP_INVALID = 0xFF


def check_commands(c: Checker, elf: str) -> None:
    """Every command in COMMANDS has to answer. The claim is that none is a downgrade.

    One caveat worth keeping honest. This runs before the clock setup, so the four SWO commands
    reach a Flexcomm with no clock and NXP's driver returns early rather than configuring
    anything. They answer, which is what is being checked, but nothing here says SWO works. With
    a real clock the driver waits on a status bit the register model never sets, so the emulator
    cannot take that path at all: SWO is a bench item.
    """
    probe = Machine(elf, Target([0] * 64), restart_at=set(), model=Model())
    answered = 0
    for cmd, operands in DAP_COMMANDS.items():
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
    for cmd in (0x07, 0x84, 0x9F):
        reply = probe.command(bytes([cmd, 0, 0, 0]))
        if cmd == 0x07:
            continue  # TransferAbort is handled out of band and answers nothing
        if not reply or reply[0] != ID_DAP_INVALID:
            c.problems.append(f"command {cmd:#04x} should answer ID_DAP_Invalid")
    c.note(
        f"{answered} of {len(DAP_COMMANDS)} commands answered, unimplemented ids rejected"
        " (the SWO four with the clock still off; see the note above)"
    )


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


def check_info(c: Checker, elf: str, packet_size: int, packet_count: int) -> None:
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
    for wanted in ("SWD", "JTAG", "SWO UART", "atomic commands", "timestamps"):
        c.check(wanted in named, f"DAP_Info does not claim {wanted}, which COMMANDS relies on")
    for unwanted in ("UART port",):
        c.check(unwanted not in named, f"DAP_Info claims {unwanted}, which is not implemented")

    product = info(DAP_ID_PRODUCT).rstrip(b"\x00").decode("ascii", "replace")
    serial = info(DAP_ID_SER_NUM).rstrip(b"\x00").decode("ascii", "replace")
    c.check(bool(serial), "DAP_Info has no serial number, so two probes cannot be told apart")
    c.check(bool(product), "DAP_Info has no product name, so a host cannot say what it found")
    c.note(f"DAP_Info: {size}-byte packets x{count}, {', '.join(named)}")
    c.note(f"DAP_Info: product {product!r}, serial {serial!r}")


DAP_ID_PRODUCT, DAP_ID_SER_NUM = 2, 3


def check_clocks(c: Checker, elf: str) -> None:
    """Run the boot-time clock setup and ask the clock driver what came out.

    This is the fix that would be invisible on the bench: the 96 MHz output of FRO192M is off out
    of reset and the board support never enables it, and FROHFDIV is halted as well, so a Flexcomm
    attached to FRO_HF_DIV gets nothing and a UART with no clock reports no error -- it just never
    reaches a baud rate. Running it against the register model says what the driver would read.
    """
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    for name in ("board_clocks_init", "board_swo_pin_init", "vcom_init", "USART3_GetFreq"):
        if name not in probe.sym:
            c.note(f"{name} was inlined away; the clock setup not checked")
            return

    probe.call("board_clocks_init")
    probe.call("board_swo_pin_init")
    swo = probe.call("USART3_GetFreq")
    c.check(swo != 0, "SWO's Flexcomm has no clock, so it would never reach a baud rate")
    c.check(
        swo == 96_000_000,
        f"SWO's Flexcomm reads {swo} Hz, wanted the 96 MHz FRO_HF through a divider of one",
    )

    probe.call("vcom_init")
    console = probe.call("CLOCK_GetFlexCommClkFreq", 0) if "CLOCK_GetFlexCommClkFreq" in probe.sym else 0
    c.check(console != 0, "the console Flexcomm has no clock")
    # Only SWO's figure is exact. The console's Flexcomm runs through a fractional divider the
    # board support programmes before any of this, and the register model starts it at zero, so
    # on hardware the console reads lower than it does here. Non-zero is the claim worth making.
    c.note(f"after boot: SWO Flexcomm {swo / 1e6:g} MHz, console Flexcomm non-zero")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("elf", nargs="?", default="build/mculink_cjtag.elf")
    ap.add_argument("--prefix", default="CJ", help="what the serial should start with")
    ap.add_argument("--packet-size", type=int, default=512)
    ap.add_argument("--packet-count", type=int, default=8)
    args = ap.parse_args()

    sym = symbols_with_size(args.elf)
    missing = [n for n in ("desc_device", "desc_configuration", "desc_bos") if n not in sym]
    if missing:
        print(f"  cannot check: {', '.join(missing)} not in the image")
        return 1

    c = Checker()
    check_device(c, _read_at(args.elf, *sym["desc_device"]))
    check_configuration(c, _read_at(args.elf, *sym["desc_configuration"]))
    ms_os = next((v for k, v in sym.items() if k.startswith("desc_ms_os_20")), None)
    if ms_os:
        check_bos(c, _read_at(args.elf, *sym["desc_bos"]), ms_os[1])
    else:
        c.note("the MS OS 2.0 descriptor was folded away; its length not cross-checked")
    check_commands(c, args.elf)
    check_clocks(c, args.elf)
    check_info(c, args.elf, args.packet_size, args.packet_count)
    check_serial(c, args.elf, bytes(range(1, 17)), args.prefix)
    check_serial(c, args.elf, b"\xff" * 16, args.prefix, uniform=True)

    print(args.elf)
    for n in c.notes:
        print(f"  {n}")
    for p in c.problems:
        print(f"  FAIL  {p}")
    if not c.problems:
        print("  descriptors and serial are self-consistent")
    return 1 if c.problems else 0


def _read_at(elf: str, addr: int, size: int) -> bytes:
    """The bytes a symbol occupies, read out of the image.

    Two things to get right. `objdump -s` dumps every section that covers the range, and the debug
    sections overlap, so take only the one the descriptors live in. And each line ends with an
    ASCII rendering that can itself look like hex, so cap the groups taken per line at four.
    """
    out = subprocess.run(
        [
            "arm-none-eabi-objdump",
            "-s",
            "-j",
            ".text",
            f"--start-address={addr}",
            f"--stop-address={addr + size}",
            elf,
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    data = bytearray()
    for line in out.splitlines():
        m = re.match(r"^\s*[0-9a-f]+ ((?:[0-9a-f]{2,8} ?){1,4})", line)
        if not m:
            continue
        for group in m.group(1).split():
            if len(group) % 2 == 0:
                data += bytes.fromhex(group)
    return bytes(data[:size])


if __name__ == "__main__":
    sys.exit(main())
