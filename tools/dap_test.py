# /// script
# requires-python = ">=3.11"
# dependencies = ["libusb1", "pyusb"]
# ///
"""Check dap.py's reading of replies: vendor 0x82 against the layout docs/host-interface.md gives
each format, and the three ways a reply can be wrong."""

from unittest import mock

from dap import Probe, UnknownCommand, parse_identify


def reply(format: int, flags: int, build: bytes, tail: bytes) -> bytes:
    return b"CJTAG" + bytes([format, flags, len(build)]) + build + tail


def u32(value: int) -> bytes:
    return value.to_bytes(4, "little")


# Format 8 on the standalone board: the board, a zero where formats 4 to 6 had vendor 0x83's scan
# formats, then the extended capabilities.
assert parse_identify(reply(8, 0x6C, b"v0.3.0", bytes([0]) + u32(0) + u32(0x440))) == {
    "format": 8, "flags": 0x6C, "build": "v0.3.0", "board": 0, "scan_formats": None, "extended": 0x440}

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

print("dap.py reads vendor 0x82 at formats 2, 5, 6 and 8 as docs/host-interface.md lays them out, "
      "and tells an unknown command from a missing or out-of-step reply")
