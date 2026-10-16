# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn", "capstone"]
# ///
"""Compare two builds of the firmware on the wire, across the cases a transfer can take.

The wire traces in tools/baseline cover one case: a target that acknowledges OK and shifts out
zeros. That is enough to catch a pin-layer mistake and not enough to let anyone rewrite a transfer
function, because the acknowledge, retry and back-off paths are where the hard-to-see mistakes
live.

This drives both builds over the same matrix -- every acknowledge a target can give, every
turnaround length, with and without the data phase, with and without idle cycles -- and requires
that they produce the same wire events and the same answer. Byte for byte, case for case.

`--ignore-samples` leaves the loads of SWDIO out of the comparison. Generated code samples a bit
up to two clocks after the edge that launched it, so its loads fall among different edges than
ARM's; the edges, the levels and the answer still have to match.
"""

import argparse
import itertools
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from cycles import (  # noqa: E402
    ID_CJTAG_CONTROL,
    ID_TRANSFER,
    ID_CONNECT,
    ID_JTAG_CONFIGURE,
    JtagTap,
    ID_SWD_CONFIGURE,
    ID_SWJ_CLOCK,
    ID_TRANSFER_BLOCK,
    ID_TRANSFER_CONFIG,
    Machine,
    Model,
    Target,
    u16,
    u32,
)

AP_DRW_READ, AP_DRW_WRITE = 0x0F, 0x0D

# Set by --ignore-samples and --clock.
IGNORE_SAMPLES = False
CLOCK_HZ = 10_000_000


def wire(trace: list[str]) -> list[str]:
    return [event for event in trace if event != "S"] if IGNORE_SAMPLES else trace

# The acknowledge, in the order SW_READ_BIT reads it: bit 0 first.
ACKS = {
    "ok": (1, [1, 0, 0]),
    "wait": (2, [0, 1, 0]),
    "fault": (4, [0, 0, 1]),
    "none": (0, [0, 0, 0]),
    "bogus": (7, [1, 1, 1]),
}


class SwdTarget(Target):
    """A SWD target with a chosen acknowledge, data word and parity.

    The probe reads the wire in order within one transfer and lets go of it exactly once, at the
    turnaround, so the release is where the bit list restarts. That marker is on the wire rather
    than in the symbol table, which is what makes it survive whatever the optimiser does.
    """

    def __init__(self, ack: str, word: int, bad_parity: bool):
        bits = list(ACKS[ack][1])
        if ACKS[ack][0] == 1:
            data = [(word >> i) & 1 for i in range(32)]
            parity = sum(data) & 1
            bits += data + [parity ^ (1 if bad_parity else 0)]
        super().__init__(bits)


def run(
    elf: str,
    ack: str,
    read: bool,
    turnaround: int,
    data_phase: bool,
    idle: int,
    word: int,
    bad_parity: bool,
    words: int = 2,
) -> tuple[list[str], bytes]:
    probe = Machine(elf, SwdTarget(ack, word, bad_parity), restart_at=set(), model=Model())
    probe.resync_on_release = True
    probe.command(bytes([ID_CONNECT, 1]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(CLOCK_HZ))
    probe.command(bytes([ID_TRANSFER_CONFIG, idle]) + u16(2) + u16(0))
    probe.command(bytes([ID_SWD_CONFIGURE, (turnaround - 1) | (0x04 if data_phase else 0)]))

    payload = bytes([ID_TRANSFER_BLOCK, 0]) + u16(words) + bytes([AP_DRW_READ if read else AP_DRW_WRITE])
    if not read:
        payload += u32(word) * words
    probe.begin()
    reply = probe.command(payload)
    return wire(probe.end().trace), reply


def run_jtag(
    elf: str, ack: int, read: bool, idle: int, word: int, count: int = 1, index: int = 0, words: int = 2
) -> tuple[list[str], bytes]:
    """The same comparison on four wires, with a TAP that walks the state machine.

    A flat bit list cannot check a JTAG transfer: which bit the probe reads depends on where the
    TAP is, so a wrong walk and a wrong bit look the same. The model here tracks the state machine
    from TMS, which means it fails a transfer that takes the wrong route as well as one that reads
    the wrong bit.
    """
    # The acknowledge leaves the TAP in the order JTAG_Transfer reassembles it: first bit out is
    # ack[1], then ack[0], then ack[2]. Then the data word.
    dr = ((ack >> 1) & 1) | ((ack & 1) << 1) | (((ack >> 2) & 1) << 2) | (word << 3)
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    probe.tap = JtagTap(dr, 35)
    probe.command(bytes([ID_CONNECT, 2]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(10_000_000))
    probe.command(bytes([ID_TRANSFER_CONFIG, idle]) + u16(2) + u16(0))
    probe.command(bytes([ID_JTAG_CONFIGURE, count]) + bytes([4] * count))

    payload = bytes([ID_TRANSFER_BLOCK, index]) + u16(words) + bytes([AP_DRW_READ if read else AP_DRW_WRITE])
    if not read:
        payload += u32(word) * words
    probe.begin()
    reply = probe.command(payload)
    return wire(probe.end().trace), reply


def run_cjtag(
    elf: str, ack: int, read: bool, idle: int, word: int, fast: bool, words: int = 2
) -> tuple[list[str], bytes]:
    """The same comparison again, through OScan1 framing.

    Three TMSC packets carry one JTAG clock, so the TAP underneath is the same one the four-wire
    comparison drives; only the way TMS and TDO reach it differs. The model finds the framing from
    the wire -- a frame releases the line exactly once, before its third packet -- rather than from
    anything in the firmware, so it stays in step with either compiled copy.
    """
    dr = ((ack >> 1) & 1) | ((ack & 1) << 1) | (((ack >> 2) & 1) << 2) | (word << 3)
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    probe.command(bytes([ID_CONNECT, 2]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(50_000_000 if fast else 10_000_000))
    probe.command(bytes([ID_TRANSFER_CONFIG, idle]) + u16(2) + u16(0))
    probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
    probe.command(bytes([ID_CJTAG_CONTROL, 1]))

    # The activation sequence ends in a TAP reset on a real target; start the model where the
    # firmware's first transfer expects to find it.
    probe.cjtag_tap = JtagTap(dr, 35)

    payload = bytes([ID_TRANSFER_BLOCK, 0]) + u16(words) + bytes([AP_DRW_READ if read else AP_DRW_WRITE])
    if not read:
        payload += u32(word) * words
    probe.begin()
    reply = probe.command(payload)
    return wire(probe.end().trace), reply


def run_transfer(
    elf: str, ack: str, kind: str, turnaround: int, idle: int, word: int
) -> tuple[list[str], bytes]:
    """DAP_Transfer, the command that reaches one register at a time.

    Covers what a block never does: match values, match masks and timestamps, each of which puts
    the firmware on a path of its own. Anyone rewriting DAP_SWD_Transfer needs these to pass as
    well as the block cases; the command is ARM's most intricate and the gate only sees its wire.
    """
    probe = Machine(elf, SwdTarget(ack, word, False), restart_at=set(), model=Model())
    probe.resync_on_release = True
    probe.command(bytes([ID_CONNECT, 1]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(CLOCK_HZ))
    # A match retry budget of two, so a mismatching read exercises the retry path and then gives up.
    probe.command(bytes([ID_TRANSFER_CONFIG, idle]) + u16(2) + u16(2))
    probe.command(bytes([ID_SWD_CONFIGURE, turnaround - 1]))

    if kind == "read":
        payload = bytes([ID_TRANSFER, 0, 3]) + bytes([AP_DRW_READ]) * 3
    elif kind == "write":
        payload = bytes([ID_TRANSFER, 0, 2]) + (bytes([AP_DRW_WRITE]) + u32(word)) * 2
    elif kind == "match-hit":
        # Mask everything out, then a value that must therefore match whatever comes back.
        payload = (
            bytes([ID_TRANSFER, 0, 2])
            + bytes([0x20])
            + u32(0x00000000)
            + bytes([AP_DRW_READ | 0x10])
            + u32(0x00000000)
        )
    elif kind == "match-miss":
        payload = (
            bytes([ID_TRANSFER, 0, 2])
            + bytes([0x20])
            + u32(0xFFFFFFFF)
            + bytes([AP_DRW_READ | 0x10])
            + u32(0xDEADBEEF)
        )
    elif kind == "address-sweep":
        # Every combination of APnDP, RnW, A2 and A3 in one command. The packet parity is taken
        # over exactly those four bits, and two address patterns appear anywhere else in this
        # file, so without this a wrong parity passes for fourteen of the sixteen. Only the
        # acknowledge-OK cases get through all of them; the rest stop at the first transfer, which
        # is what they are for.
        body = b""
        for rq in range(16):
            body += bytes([rq])
            if (rq & 0x02) == 0:  # a write carries its data
                body += u32(word)
        payload = bytes([ID_TRANSFER, 0, 16]) + body
    else:  # timestamped
        payload = bytes([ID_TRANSFER, 0, 2]) + bytes([AP_DRW_READ | 0x80, AP_DRW_READ])

    probe.begin()
    reply = probe.command(payload)
    return wire(probe.end().trace), reply


# Every case above sends a two-word block. The loops that changed most recently derive the reply's
# word count from the response pointer, so one word and a full packet are where an off-by-one
# would show, and neither appeared anywhere in this file.
BLOCK_SIZES = (1, 3, 63, 127)


def sweep_sizes(reference: str, candidate: str, driver, verbose: bool) -> tuple[int, int]:
    total = 0
    failures = 0
    for read in (True, False):
        for words in BLOCK_SIZES:
            total += 1
            name = f"{'read' if read else 'write'} {words}-word block"
            a = driver(reference, read, words)
            b = driver(candidate, read, words)
            if a[0] != b[0]:
                first = next(
                    (i for i, (x, y) in enumerate(zip(a[0], b[0])) if x != y),
                    min(len(a[0]), len(b[0])),
                )
                print(f"  WIRE   {name}")
                print(
                    f"         diverges at event {first} of {len(a[0])}/{len(b[0])}:"
                    f" {a[0][first : first + 6]} against {b[0][first : first + 6]}"
                )
                failures += 1
            elif a[1] != b[1]:
                print(f"  REPLY  {name}\n         {a[1].hex()} against {b[1].hex()}")
                failures += 1
            elif verbose:
                print(f"  ok     {name}  ({len(a[0])} events, {len(a[1])}B reply)")
    return total, failures


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--wire", choices=["swd", "jtag", "cjtag"], default="swd")
    ap.add_argument("--command", choices=["block", "transfer"], default="block")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--ignore-samples", action="store_true", help="compare without the SWDIO loads")
    ap.add_argument("--clock", type=float, default=10.0, help="SWD clock in MHz for the SWD cases")
    args = ap.parse_args()
    global IGNORE_SAMPLES, CLOCK_HZ
    IGNORE_SAMPLES = args.ignore_samples
    CLOCK_HZ = int(args.clock * 1e6)

    if args.command == "transfer":
        failures = 0
        total = 0
        kinds = ("read", "write", "match-hit", "match-miss", "timestamped", "address-sweep")
        cases = itertools.product(ACKS, kinds, (1, 2), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF))
        for ack, kind, turnaround, idle, word in cases:
            total += 1
            name = f"ack={ack} {kind} turnaround={turnaround} idle={idle} word=0x{word:08x}"
            a = run_transfer(args.reference, ack, kind, turnaround, idle, word)
            b = run_transfer(args.candidate, ack, kind, turnaround, idle, word)
            if a[0] != b[0]:
                first = next(
                    (i for i, (x, y) in enumerate(zip(a[0], b[0])) if x != y),
                    min(len(a[0]), len(b[0])),
                )
                print(f"  WIRE   {name}")
                print(
                    f"         diverges at event {first} of {len(a[0])}/{len(b[0])}:"
                    f" {a[0][first : first + 6]} against {b[0][first : first + 6]}"
                )
                failures += 1
            elif a[1] != b[1]:
                print(f"  REPLY  {name}\n         {a[1].hex()} against {b[1].hex()}")
                failures += 1
            elif args.verbose:
                print(f"  ok     {name}  ({len(a[0])} events, {len(a[1])}B reply)")
        print(f"\n{total - failures}/{total} DAP_Transfer cases identical on the wire and in the reply")
        return 1 if failures else 0

    if args.wire == "cjtag":
        failures = 0
        total = 0
        cases = itertools.product(
            (0, 1, 2, 4, 7), (True, False), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF), (False, True)
        )
        for ack, read, idle, word, fast in cases:
            total += 1
            name = (
                f"ack={ack} {'read' if read else 'write'} idle={idle} word=0x{word:08x}"
                f" {'delay-free' if fast else 'paced'}"
            )
            a = run_cjtag(args.reference, ack, read, idle, word, fast)
            b = run_cjtag(args.candidate, ack, read, idle, word, fast)
            if a[0] != b[0]:
                first = next(
                    (i for i, (x, y) in enumerate(zip(a[0], b[0])) if x != y),
                    min(len(a[0]), len(b[0])),
                )
                print(f"  WIRE   {name}")
                print(
                    f"         diverges at event {first} of {len(a[0])}/{len(b[0])}:"
                    f" {a[0][first : first + 6]} against {b[0][first : first + 6]}"
                )
                failures += 1
            elif a[1] != b[1]:
                print(f"  REPLY  {name}\n         {a[1].hex()} against {b[1].hex()}")
                failures += 1
            elif args.verbose:
                print(f"  ok     {name}  ({len(a[0])} events, {len(a[1])}B reply)")
        extra, bad = sweep_sizes(
            args.reference,
            args.candidate,
            lambda elf, read, words: run_cjtag(elf, 2, read, 0, 0xA5A5_5A5A, False, words),
            args.verbose,
        )
        total += extra
        failures += bad
        print(f"\n{total - failures}/{total} two-pin cases identical on the wire and in the reply")
        return 1 if failures else 0

    if args.wire == "jtag":
        failures = 0
        total = 0
        # A chain of one never reaches the bypass branches, which is where a rewritten data phase
        # is least likely to be noticed. (2, 0) puts a bypass after the data, (2, 1) before it.
        chains = ((1, 0), (2, 0), (2, 1))
        cases = itertools.product(
            (0, 1, 2, 4, 7), (True, False), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF), chains
        )
        for ack, read, idle, word, (count, index) in cases:
            total += 1
            name = (
                f"ack={ack} {'read' if read else 'write'} idle={idle} word=0x{word:08x} chain={count}/{index}"
            )
            a = run_jtag(args.reference, ack, read, idle, word, count, index)
            b = run_jtag(args.candidate, ack, read, idle, word, count, index)
            if a[0] != b[0]:
                first = next(
                    (i for i, (x, y) in enumerate(zip(a[0], b[0])) if x != y),
                    min(len(a[0]), len(b[0])),
                )
                print(f"  WIRE   {name}")
                print(
                    f"         diverges at event {first} of {len(a[0])}/{len(b[0])}:"
                    f" {a[0][first : first + 6]} against {b[0][first : first + 6]}"
                )
                failures += 1
            elif a[1] != b[1]:
                print(f"  REPLY  {name}\n         {a[1].hex()} against {b[1].hex()}")
                failures += 1
            elif args.verbose:
                print(f"  ok     {name}  ({len(a[0])} events, {len(a[1])}B reply)")
        extra, bad = sweep_sizes(
            args.reference,
            args.candidate,
            lambda elf, read, words: run_jtag(elf, 2, read, 0, 0xA5A5_5A5A, 1, 0, words),
            args.verbose,
        )
        total += extra
        failures += bad
        print(f"\n{total - failures}/{total} JTAG cases identical on the wire and in the reply")
        return 1 if failures else 0

    cases = itertools.product(
        ACKS, (True, False), (1, 2, 3, 4), (True, False), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF)
    )
    failures = 0
    total = 0
    for ack, read, turnaround, data_phase, idle, word in cases:
        for bad_parity in (False, True) if (ack == "ok" and read) else (False,):
            total += 1
            name = (
                f"ack={ack} {'read' if read else 'write'} turnaround={turnaround}"
                f" data_phase={int(data_phase)} idle={idle} word=0x{word:08x}"
                f"{' bad-parity' if bad_parity else ''}"
            )
            a = run(args.reference, ack, read, turnaround, data_phase, idle, word, bad_parity)
            b = run(args.candidate, ack, read, turnaround, data_phase, idle, word, bad_parity)
            if a[0] != b[0]:
                first = next(
                    (i for i, (x, y) in enumerate(zip(a[0], b[0])) if x != y), min(len(a[0]), len(b[0]))
                )
                print(f"  WIRE   {name}")
                print(
                    f"         diverges at event {first} of {len(a[0])}/{len(b[0])}:"
                    f" {a[0][first : first + 6]} against {b[0][first : first + 6]}"
                )
                failures += 1
            elif a[1] != b[1]:
                print(f"  REPLY  {name}")
                print(f"         {a[1].hex()} against {b[1].hex()}")
                failures += 1
            elif args.verbose:
                print(f"  ok     {name}  ({len(a[0])} events, {len(a[1])} byte reply)")

    extra, bad = sweep_sizes(
        args.reference,
        args.candidate,
        lambda elf, read, words: run(elf, "ok", read, 1, False, 0, 0xA5A5_5A5A, False, words),
        args.verbose,
    )
    total += extra
    failures += bad
    print(f"\n{total - failures}/{total} cases identical on the wire and in the reply")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
