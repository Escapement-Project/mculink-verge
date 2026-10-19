# /// script
# requires-python = ">=3.11"
# dependencies = ["numpy"]
# ///
"""Check an ITM counter stream from itm_counter.c, from the wire or from the probe.

    uv run itm_check.py line <digital_N.bin> [--baud HZ]   decode a Saleae binary export as 8N1
    uv run itm_check.py bytes <file>                        check bytes the probe captured

Every packet should be 0x03 and the next counter value, little-endian. Reports the bit time it
measured on the line, framing errors, and every break in the counter: how many words went missing
and where. The first packet is found by locking on to three consecutive counters, since a capture
can start mid-packet.
"""
import argparse
import struct
import sys

import numpy as np

LOCK = 3


def read_export(path):
    with open(path, "rb") as f:
        _ident, _version, _kind, initial, _begin, _end, n = struct.unpack("<8siiIddQ", f.read(44))
        t = np.fromfile(f, dtype="<f8", count=n)
    return initial, t


def bit_time(t):
    """The mean of the shortest edge-to-edge intervals, which are single bits. Biased by the
    sample period; `frame_bit_time` refines it once the frames are known."""
    d = np.diff(t)
    shortest = np.min(d)
    return float(np.mean(d[d < 1.5 * shortest]))


def decode_8n1(initial, t, bit):
    """Bytes, and the index of every byte whose stop bit was low. The line idles high."""
    def level(at):
        # Transitions before `at`, counted from the initial level.
        return (initial + int(np.searchsorted(t, at, side="right"))) % 2

    falling = t[(initial + 1 + np.arange(len(t))) % 2 == 0]
    out = bytearray()
    framing = []
    starts = []
    i = 0
    while i < len(falling):
        start = falling[i]
        value = 0
        for b in range(8):
            value |= level(start + (1.5 + b) * bit) << b
        if level(start + 9.5 * bit) == 0:
            framing.append(len(out))
        out.append(value)
        starts.append(start)
        i = int(np.searchsorted(falling, start + 9.5 * bit, side="left"))
    return bytes(out), framing, np.array(starts)


def frame_bit_time(starts, bit):
    """Bit time from back-to-back frames, 10 bits start to start, which a sample period cannot
    bias over a long run. None when the stream never runs two frames back to back."""
    d = np.diff(starts)
    back_to_back = d[d < 10.5 * bit]
    return float(np.sum(back_to_back) / (10 * len(back_to_back))) if len(back_to_back) else None


def word_at(data, i):
    return int.from_bytes(data[i + 1:i + 5], "little")


def locked_at(data, i):
    if i + 5 * LOCK > len(data):
        return False
    first = word_at(data, i)
    return all(data[i + 5 * k] == 0x03 and word_at(data, i + 5 * k) == (first + k) & 0xFFFFFFFF
               for k in range(LOCK))


def check(data):
    i = next((j for j in range(min(len(data), 64)) if locked_at(data, j)), None)
    if i is None:
        print(f"{len(data)} bytes: no counter stream found")
        return 1
    lead = i
    first = word_at(data, i)
    expect = first
    words = 0
    breaks = []
    while i + 5 <= len(data):
        if data[i] == 0x03 and word_at(data, i) == expect:
            words += 1
            expect = (expect + 1) & 0xFFFFFFFF
            i += 5
            continue
        j = next((k for k in range(i + 1, len(data)) if locked_at(data, k)), None)
        if j is None:
            break
        got = word_at(data, j)
        breaks.append((i, j - i, (got - expect) & 0xFFFFFFFF))
        expect = got
        i = j
    tail = len(data) - i
    print(f"{len(data)} bytes: {words} words in order from {first:#x}, {lead} leading and {tail} "
          f"trailing bytes outside a packet, {len(breaks)} breaks")
    for at, skipped, missing in breaks[:20]:
        print(f"  byte {at}: {skipped} bytes skipped to resync, {missing} words missing")
    return 1 if breaks else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    line = sub.add_parser("line")
    line.add_argument("export")
    line.add_argument("--baud", type=float, help="decode at this rate rather than the measured one")
    raw = sub.add_parser("bytes")
    raw.add_argument("file")
    args = ap.parse_args()

    if args.cmd == "bytes":
        return check(open(args.file, "rb").read())
    initial, t = read_export(args.export)
    if len(t) < 20:
        print(f"{len(t)} transitions: no stream")
        return 1
    measured = bit_time(t)
    print(f"{len(t)} transitions over {(t[-1] - t[0]) * 1e3:.2f} ms; bit time {measured * 1e9:.1f} ns, "
          f"{1 / measured / 1e6:.4f} Mbaud")
    bit = 1 / args.baud if args.baud else measured
    data, framing, starts = decode_8n1(initial, t, bit)
    refined = frame_bit_time(starts, bit)
    if refined:
        print(f"back-to-back frames: bit time {refined * 1e9:.2f} ns, {1 / refined / 1e6:.4f} Mbaud")
    print(f"{len(framing)} framing errors, at bytes {framing[:20]} of {len(data)}")
    return check(data)


if __name__ == "__main__":
    sys.exit(main())
