# /// script
# requires-python = ">=3.11"
# dependencies = ["libusb1", "pyusb"]
# ///
"""Check dap.py's reading of replies: vendor 0x82 against the layout docs/host-interface.md gives
each format, and the three ways a reply can be wrong. Check that `dap.py cjtag` exits 1 when an
IDCODE it reads is not the one expected."""

import contextlib
import io
import sys
from unittest import mock

import dap
from dap import DAP_VENDOR_JTAG_FORMAT, Probe, UnknownCommand, parse_identify


def reply(format: int, flags: int, build: bytes, tail: bytes) -> bytes:
    return b"CJTAG" + bytes([format, flags, len(build)]) + build + tail


def u32(value: int) -> bytes:
    return value.to_bytes(4, "little")


# Format 9 on the standalone board: the board, a zero where formats 4 to 6 had vendor 0x83's scan
# formats, then the extended capabilities.
assert parse_identify(reply(9, 0x6C, b"v0.3.0", bytes([0]) + u32(0) + u32(0x440))) == {
    "format": 9, "flags": 0x6C, "build": "v0.3.0", "board": 0, "scan_formats": None, "extended": 0x440}

# Format 6 carried the scan formats, OScan1 and OScan2 here, and already the extended capabilities.
found = parse_identify(reply(6, 0x2D, b"abc1234", bytes([1]) + u32((1 << 9) | (1 << 10)) + u32(1)))
assert (found["board"], found["scan_formats"], found["extended"]) == (1, [9, 10], 1), found

# Format 5 has no extended field, so the bytes after the scan formats are not read as one.
found = parse_identify(reply(5, 0x01, b"abc1234", bytes([0]) + u32(1 << 9) + u32(0xFFFF_FFFF)))
assert (found["scan_formats"], found["extended"]) == ([9], None), found

# Format 2 has no board. A v1 HID reply is padded to 64 bytes, and the padding is no board.
found = parse_identify(reply(2, 0x01, b"14d05d4", bytes(64 - 16)))
assert (found["board"], found["scan_formats"], found["extended"]) == (None, None, None), found

# NXP's V3.174 answers 82 00 01; after the ID that is no magic.
assert parse_identify(bytes([0x00, 0x01])) is None
assert parse_identify(b"CJTAG") is None


def answer(request: bytes, reply: bytes) -> bytes:
    endpoint = mock.Mock()
    endpoint.read.return_value = reply
    return Probe(None, None, endpoint, endpoint, "S", "", "").command(request)


def raised(request: bytes, reply: bytes) -> Exception | None:
    try:
        answer(request, reply)
    except Exception as error:
        return error
    return None


# 0xFF is CMSIS-DAP's reply to a command the firmware does not have, not a reply out of step.
error = raised(bytes([0x89, 0, 0]), bytes([0xFF]))
assert isinstance(error, UnknownCommand) and str(error).startswith("0x89:"), repr(error)
error = raised(bytes([0x89, 0, 0]), b"")
assert type(error) is RuntimeError and "no reply" in str(error), repr(error)
error = raised(bytes([0x89, 0, 0]), bytes([0x05, 0]))
assert type(error) is RuntimeError and "out of step" in str(error), repr(error)
assert answer(bytes([0x89, 0, 0]), bytes([0x89, 0])) == bytes([0x89, 0])


def cjtag(idcodes: list[int], *flags: str) -> tuple[int, mock.Mock, str]:
    """`dap.py cjtag --no-activate` through main(), against a probe whose reads return `idcodes`."""
    probe = mock.Mock(serial="S", vendor="V", product="P")
    probe.connect.return_value = 2
    probe.swj_clock.return_value = 0
    probe.command.return_value = bytes([0, 0])
    # The 31 bits before Exit1-DR, then the last bit, as the command's sequence captures them.
    probe.jtag_sequence.side_effect = [[idcode & 0x7FFF_FFFF, idcode >> 31] for idcode in idcodes]
    argv = ["dap.py", "cjtag", "--no-activate", "--repeat", str(len(idcodes)), *flags]
    out = io.StringIO()
    with mock.patch.object(sys, "argv", argv), mock.patch.object(dap, "one", return_value=probe), \
            contextlib.redirect_stdout(out):
        status = dap.main()
    return status, probe, out.getvalue()


status, probe, out = cjtag([0x3BB4102F, 0x3BB4102F])
assert status == 0, out
status, probe, out = cjtag([0x3BB4102F, 0xFFFF_FFFF])
assert status == 1, out
assert "IDCODE 0xffffffff (expected 0x3bb4102f)" in out, out
# A wrong read still turns framing off and releases the probe.
last = probe.command.call_args
assert last.args[0] == bytes([DAP_VENDOR_JTAG_FORMAT, 0]), last
assert probe.release.called
# A wrong read followed by a right one still fails.
status, _, out = cjtag([0xFFFF_FFFF, 0x3BB4102F])
assert status == 1, out
status, _, out = cjtag([0x9234_5677], "--expect", "0x92345677")
assert status == 0, out
status, _, out = cjtag([0x9234_5677], "--expect", "0x92345676")
assert status == 1, out

print("dap.py reads vendor 0x82 at formats 2, 5, 6 and 8 as docs/host-interface.md lays them out, "
      "tells an unknown command from a missing or out-of-step reply, and `cjtag` exits 1 on a "
      "wrong IDCODE")
