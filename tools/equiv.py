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

`--ignore-samples` leaves the loads of SWDIO and TDO out of the comparison. Generated code samples
a bit up to two clocks after the edge that launched it, so its loads fall among different edges
than ARM's; the edges, the levels and the answer still have to match.
"""

import argparse
import concurrent.futures
import itertools
import os
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from cycles import (  # noqa: E402
    ID_JTAG_SEQUENCE,
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
    TWO_PIN,
    Target,
    cjtag_enter,
    u16,
    u32,
)

AP_DRW_READ, AP_DRW_WRITE = 0x0F, 0x0D
# Off, so that a generated image's JTAG wire is ARM's exactly even when the target answers WAIT.
ID_JTAG_PACING = 0x8B
# Mode 2 pins the latency in use, so a first transfer that fails does not make a generated image
# measure it with IDCODE reads (src/jtag_sweep.h), which ARM's path never sends.
ID_JTAG_GEN, JTAG_GEN_PIN = 0x8A, 2

# Set by --ignore-samples and --clock.
IGNORE_SAMPLES = False
CLOCK_HZ = 10_000_000


def wire(trace: list[str]) -> list[str]:
    """The trace with TMS and TDI changes put in one order between clock edges, where a TAP cannot
    see which came first."""
    events = [event for event in trace if event not in ("S", "T")] if IGNORE_SAMPLES else list(trace)
    i = 0
    while i < len(events):
        j = i
        while j < len(events) and events[j][0] in "WI":
            j += 1
        events[i:j] = sorted(events[i:j], key=lambda event: event[0] == "I")
        i = j + 1
    return events

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
    probe = Machine(elf, SwdTarget(ack, word, bad_parity), restart_at=set(), model=Model(), timed=False)
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
    probe = Machine(elf, Target([]), restart_at=set(), model=Model(), timed=False)
    probe.tap = JtagTap(dr, 35)
    probe.command(bytes([ID_CONNECT, 2]))
    probe.command(bytes([ID_JTAG_PACING, 0]))
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
    elf: str, ack: int, read: bool, idle: int, word: int, fast: bool, words: int = 2, wire_name: str = "cjtag"
) -> tuple[list[str], bytes]:
    """The same comparison again, through OScan1 framing.

    Three TMSC packets carry one JTAG clock, so the TAP underneath is the same one the four-wire
    comparison drives; only the way TMS and TDO reach it differs. The model finds the framing from
    the wire -- a frame releases the line exactly once, before its third packet -- rather than from
    anything in the firmware, so it stays in step with either compiled copy.
    """
    dr = ((ack >> 1) & 1) | ((ack & 1) << 1) | (((ack >> 2) & 1) << 2) | (word << 3)
    probe = Machine(elf, Target([]), restart_at=set(), model=Model(), timed=False)
    probe.command(bytes([ID_CONNECT, 2]))
    probe.command(bytes([ID_JTAG_PACING, 0]))
    probe.command(bytes([ID_SWJ_CLOCK]) + u32(50_000_000 if fast else 10_000_000))
    probe.command(bytes([ID_TRANSFER_CONFIG, idle]) + u16(2) + u16(0))
    probe.command(bytes([ID_JTAG_CONFIGURE, 1, 4]))
    probe.command(cjtag_enter(wire_name, True))

    # The activation sequence ends in a TAP reset on a real target; start the model where the
    # firmware's first transfer expects to find it.
    probe.cjtag_tap = JtagTap(dr, 35)
    probe.cjtag_oscan2 = wire_name == "cjtag2"
    probe.cjtag_oscan5 = wire_name == "cjtag5"

    payload = bytes([ID_TRANSFER_BLOCK, 0]) + u16(words) + bytes([AP_DRW_READ if read else AP_DRW_WRITE])
    if not read:
        payload += u32(word) * words
    probe.begin()
    reply = probe.command(payload)
    return wire(probe.end().trace), reply


def run_jtag_commands(
    elf: str,
    wire_name: str,
    ack: int,
    word: int,
    fast: bool,
    idle: int,
    commands: list[bytes],
    chain: tuple[int, ...] = (4,),
    index: int = 0,
) -> tuple[list[str], bytes]:
    """Any commands on four wires or through OScan1 framing, against the TAP run_jtag and
    run_cjtag use: the replies, joined. The TAP is at chain position `index`; the TAPs nearer TDO
    are in BYPASS during a DR scan, one zero each ahead of its DR."""
    dr = (((ack >> 1) & 1) | ((ack & 1) << 1) | (((ack >> 2) & 1) << 2) | (word << 3)) << index
    two_pin = wire_name in TWO_PIN
    probe = Machine(elf, Target([]), restart_at=set(), model=Model(), timed=False)
    probe.command(bytes([ID_CONNECT, 2]))
    probe.command(bytes([ID_JTAG_PACING, 0]))
    probe.command(bytes([ID_JTAG_GEN, 0, 0, JTAG_GEN_PIN]))
    # On four wires the generated path samples a short clock's TDO after the next falling edge,
    # which an order-only TAP cannot hold for it; tools/jtagcheck.py times those. 12.5 MHz is the
    # fastest clock that samples within its own.
    probe.command(bytes([ID_SWJ_CLOCK]) + u32((50_000_000 if two_pin else 12_500_000) if fast else 10_000_000))
    probe.command(bytes([ID_TRANSFER_CONFIG, idle]) + u16(2) + u16(2))
    probe.command(bytes([ID_JTAG_CONFIGURE, len(chain)]) + bytes(chain))
    if two_pin:
        probe.command(cjtag_enter(wire_name, True))
        probe.cjtag_tap = JtagTap(dr, 35 + index)
        probe.cjtag_oscan2 = wire_name == "cjtag2"
        probe.cjtag_oscan5 = wire_name == "cjtag5"
    else:
        probe.tap = JtagTap(dr, 35 + index)
    probe.begin()
    replies = b"".join(probe.command(c) for c in commands)
    return wire(probe.end().trace), replies


def jtag_sequence(*seqs: tuple[int, bool, bool, int]) -> bytes:
    """DAP_JTAG_Sequence from (clocks, tms, capture, tdi) entries; 64 clocks encode as 0."""
    body = b""
    for clocks, tms, capture, tdi in seqs:
        body += bytes([(clocks & 0x3F) | (0x40 if tms else 0) | (0x80 if capture else 0)])
        body += tdi.to_bytes((clocks + 7) // 8, "little")
    return bytes([ID_JTAG_SEQUENCE, len(seqs)]) + body


def cjtag_sequence_cases(word: int) -> dict[str, list[bytes]]:
    """Sequences of every length class, and the transfers whose first frames carry the TDI a
    sequence left behind: ARM sets none before the request bits."""
    mask = lambda n: (1 << n) - 1  # noqa: E731
    pattern = 0xF0E1_D2C3_B4A5_9687
    read = bytes([ID_TRANSFER, 0, 2, AP_DRW_READ, AP_DRW_READ])
    write = bytes([ID_TRANSFER, 0, 1, AP_DRW_WRITE]) + u32(word)
    return {
        "probe-rs dr scan": [
            jtag_sequence((1, True, False, 1), (2, False, False, 3), (34, False, True, pattern & mask(34)),
                          (1, True, True, 1), (1, True, False, 1), (1, False, False, 1))
        ],
        "lengths": [
            jtag_sequence(*[(n, n % 2 == 0, True, pattern & mask(n)) for n in (1, 7, 8, 9, 31, 32, 33, 63)]),
            jtag_sequence((64, False, True, pattern), (64, True, False, pattern ^ mask(64))),
        ],
        "stale tdi 0 then read": [jtag_sequence((5, False, False, 0)), read],
        "stale tdi 1 then write": [jtag_sequence((5, False, False, 0x1F)), write],
        "stale tdi 0 then idcode": [jtag_sequence((3, False, False, 0)), bytes([0x16, 0])],
        "write abort": [jtag_sequence((2, False, False, 0)), bytes([0x08, 0]) + u32(word)],
        # A cJTAG command part: Exit2-DR into Shift-DR for one bit, whose TMS 1 leaves it again.
        # In OScan5 that one Shift frame is also the EOT escape.
        "exit2 one-bit shift": [
            jtag_sequence(*[(1, tms, False, 1) for tms in (True, False, True, False, True, False)],
                          (1, True, True, 0), (1, True, False, 1), (1, False, False, 1))
        ],
    }


def run_transfer(
    elf: str, ack: str, kind: str, turnaround: int, idle: int, word: int
) -> tuple[list[str], bytes]:
    """DAP_Transfer, the command that reaches one register at a time.

    Covers what a block never does: match values, match masks and timestamps, each of which puts
    the firmware on a path of its own. Anyone rewriting DAP_SWD_Transfer needs these to pass as
    well as the block cases; the command is ARM's most intricate and the gate only sees its wire.
    """
    probe = Machine(elf, SwdTarget(ack, word, False), restart_at=set(), model=Model(), timed=False)
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
# would show, and neither appeared anywhere in this file. A full packet is 127 words of reply, but
# 126 of request: a write's five header bytes take the 127th.
BLOCK_SIZES = (1, 3, 63, 127)
WRITE_BLOCK_MAX = 126


def size_cases(driver, **fixed) -> list[tuple]:
    """A read and a write block at every size in BLOCK_SIZES, writes capped at WRITE_BLOCK_MAX, as
    cases of `driver` with the arguments in `fixed`."""
    cases = []
    for read in (True, False):
        for words in BLOCK_SIZES:
            words = words if read else min(words, WRITE_BLOCK_MAX)
            cases.append((f"{'read' if read else 'write'} {words}-word block", driver, (),
                          {**fixed, "read": read, "words": words}))
    return cases


def set_globals(ignore_samples: bool, clock_hz: int) -> None:
    global IGNORE_SAMPLES, CLOCK_HZ
    IGNORE_SAMPLES = ignore_samples
    CLOCK_HZ = clock_hz


def compare_case(job: tuple) -> tuple[str, str | None, str]:
    """Runs one case on both images: its name, what differed (None if nothing) and the verbose line."""
    reference, candidate, (name, runner, args, kwargs) = job
    a = runner(reference, *args, **kwargs)
    b = runner(candidate, *args, **kwargs)
    if a[0] != b[0]:
        first = next((i for i, (x, y) in enumerate(zip(a[0], b[0])) if x != y), min(len(a[0]), len(b[0])))
        return name, (f"  WIRE   {name}\n         diverges at event {first} of {len(a[0])}/{len(b[0])}:"
                      f" {a[0][first : first + 6]} against {b[0][first : first + 6]}"), ""
    if a[1] != b[1]:
        return name, f"  REPLY  {name}\n         {a[1].hex()} against {b[1].hex()}", ""
    return name, None, f"  ok     {name}  ({len(a[0])} events, {len(a[1])}B reply)"


def jtag_command_cases(command: str) -> list[tuple]:
    # Two TAPs as the CC13xx's ICEPick puts them, the DAP at either end, so the IR and DR scans
    # cross bypass bits on both sides.
    chains = ((4,), (4, 6))
    cases = []
    for ack, word, fast, idle, chain in itertools.product(
        (0, 1, 2, 4, 7), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF), (False, True), (0, 3), chains
    ):
        if command == "sequence":
            for label, commands in cjtag_sequence_cases(word).items():
                cases.append((f"{label} ack={ack} word=0x{word:08x} {'fast' if fast else 'paced'} idle={idle} chain={chain}",
                              (ack, word, fast, idle, commands, chain)))
        else:
            for index in range(len(chain)):
                for kind, payload in (
                    ("read", bytes([ID_TRANSFER, index, 3]) + bytes([AP_DRW_READ]) * 3),
                    ("write", bytes([ID_TRANSFER, index, 2]) + (bytes([AP_DRW_WRITE]) + u32(word)) * 2),
                    ("dp", bytes([ID_TRANSFER, index, 2, 0x02, 0x06])),
                    ("match", bytes([ID_TRANSFER, index, 2, 0x20]) + u32(0xFFFF0000) + bytes([0x1F]) + u32(word)),
                    ("timestamped", bytes([ID_TRANSFER, index, 2, AP_DRW_READ | 0x80, AP_DRW_READ])),
                    ("block", bytes([ID_TRANSFER_BLOCK, index]) + u16(3) + bytes([AP_DRW_READ])),
                ):
                    cases.append((f"{kind} index={index} ack={ack} word=0x{word:08x} {'fast' if fast else 'paced'} idle={idle} chain={chain}",
                                  (ack, word, fast, idle, [payload], chain, index)))
    return cases


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("reference")
    ap.add_argument("candidate")
    ap.add_argument("--wire", choices=["swd", "jtag", "cjtag", "cjtag2", "cjtag5"], default="swd")
    ap.add_argument("--command", choices=["block", "transfer", "sequence"], default="block")
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--ignore-samples", action="store_true", help="compare without the SWDIO loads")
    ap.add_argument("--clock", type=float, default=10.0, help="SWD clock in MHz for the SWD cases")
    ap.add_argument("--jobs", type=int, default=os.cpu_count(), help="processes to run the cases in")
    args = ap.parse_args()
    set_globals(args.ignore_samples, int(args.clock * 1e6))

    # Each case: (name, runner, args, kwargs); the runner takes the image first.
    if args.wire in ("cjtag", "cjtag2", "cjtag5", "jtag") and args.command != "block":
        cases = [(name, run_jtag_commands, (args.wire, *case), {}) for name, case in jtag_command_cases(args.command)]
        summary = f"{'two-pin' if args.wire in TWO_PIN else 'four-wire'} {args.command} cases"
    elif args.command == "transfer":
        kinds = ("read", "write", "match-hit", "match-miss", "timestamped", "address-sweep")
        cases = [
            (f"ack={ack} {kind} turnaround={turnaround} idle={idle} word=0x{word:08x}", run_transfer,
             (ack, kind, turnaround, idle, word), {})
            for ack, kind, turnaround, idle, word in itertools.product(
                ACKS, kinds, (1, 2), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF))
        ]
        summary = "DAP_Transfer cases"
    elif args.wire in TWO_PIN:
        cases = [
            (f"ack={ack} {'read' if read else 'write'} idle={idle} word=0x{word:08x} {'delay-free' if fast else 'paced'}",
             run_cjtag, (ack, read, idle, word, fast), {"wire_name": args.wire})
            for ack, read, idle, word, fast in itertools.product(
                (0, 1, 2, 4, 7), (True, False), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF), (False, True))
        ]
        cases += size_cases(run_cjtag, ack=2, idle=0, word=0xA5A5_5A5A, fast=False, wire_name=args.wire)
        summary = "two-pin cases"
    elif args.wire == "jtag":
        # A chain of one never reaches the bypass branches, which is where a rewritten data phase
        # is least likely to be noticed. (2, 0) puts a bypass after the data, (2, 1) before it.
        chains = ((1, 0), (2, 0), (2, 1))
        cases = [
            (f"ack={ack} {'read' if read else 'write'} idle={idle} word=0x{word:08x} chain={count}/{index}",
             run_jtag, (ack, read, idle, word, count, index), {})
            for ack, read, idle, word, (count, index) in itertools.product(
                (0, 1, 2, 4, 7), (True, False), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF), chains)
        ]
        cases += size_cases(run_jtag, ack=2, idle=0, word=0xA5A5_5A5A, count=1, index=0)
        summary = "JTAG cases"
    else:
        cases = [
            (f"ack={ack} {'read' if read else 'write'} turnaround={turnaround} data_phase={int(data_phase)}"
             f" idle={idle} word=0x{word:08x}{' bad-parity' if bad_parity else ''}",
             run, (ack, read, turnaround, data_phase, idle, word, bad_parity), {})
            for ack, read, turnaround, data_phase, idle, word in itertools.product(
                ACKS, (True, False), (1, 2, 3, 4), (True, False), (0, 3), (0x00000000, 0xA5A5_5A5A, 0xFFFFFFFF))
            for bad_parity in ((False, True) if (ack == "ok" and read) else (False,))
        ]
        cases += size_cases(run, ack="ok", turnaround=1, data_phase=False, idle=0, word=0xA5A5_5A5A,
                            bad_parity=False)
        summary = "cases"

    jobs = [(args.reference, args.candidate, case) for case in cases]
    with concurrent.futures.ProcessPoolExecutor(
        max_workers=args.jobs, initializer=set_globals, initargs=(IGNORE_SAMPLES, CLOCK_HZ)
    ) as pool:
        failures = 0
        for _, failure, ok in pool.map(compare_case, jobs, chunksize=4):
            if failure is not None:
                print(failure)
                failures += 1
            elif args.verbose:
                print(ok)
    print(f"\n{len(cases) - failures}/{len(cases)} {summary} identical on the wire and in the reply")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
