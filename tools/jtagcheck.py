# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn", "capstone"]
# ///
"""Hold the JTAG and cJTAG (OScan1) paths to the wire's timing, on the timing engine.

For each clock asked for, run what a host sends against a target that answers by time
(tools/cycles.py's TimedCjtagTarget or TimedJtagTarget, with the TDO windows measured on a
CC1312R1): a DR scan and an IR scan as DAP_JTAG_Sequence commands, the way probe-rs reaches a debug
port, then DAP_Transfer and DAP_TransferBlock reads and writes. Check that every reply carries
what the target shifted out, that the target's registers got what was shifted in, that every TDO
sample landed in its window, and that no clock or packet is shorter than the clock asked for. With
cJTAG, also that the wire never moved with TCKC high, stayed driven into a TDO packet, or
entered Test-Logic-Reset.

Reports, per clock: the cycles a JTAG clock takes in the DR scan, the least TDO margin, and what
went wrong.

`--stall` builds for another master holding a load that many cycles (src/gen.h, gen_stall), as
energy capture's DMA does, and takes it off the late margin of every sample placed after the edge
that ends its window.

    uv run tools/jtagcheck.py build-gen/mculink_cjtag_core1.elf
    uv run tools/jtagcheck.py build/mculink_cjtag_core1.elf --wire jtag --mhz 1 10 25 37.5
"""

import argparse
import concurrent.futures
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cycles as cy  # noqa: E402
from host_resources import available_cpus  # noqa: E402

WORD = 0xA5A5_5A5A
WRITE_WORD = 0x3C3C_C3C3
DPACC, APACC = 0xA, 0xB
AP_DRW_READ, AP_DRW_WRITE = 0x0F, 0x0D
JTAG_SEQUENCE_TMS, JTAG_SEQUENCE_TDO = 0x40, 0x80
ID_VENDOR_JTAG_GEN = 0x8A


def sequence(bits: list[int], tms: int, capture: bool) -> bytes:
    """One DAP_JTAG_Sequence entry: info byte, then TDI least significant bit first."""
    info = (len(bits) & 0x3F) | (JTAG_SEQUENCE_TMS if tms else 0) | (JTAG_SEQUENCE_TDO if capture else 0)
    data = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        data[i // 8] |= b << (i % 8)
    return bytes([info]) + bytes(data)


def bits_of(value: int, n: int) -> list[int]:
    return [(value >> i) & 1 for i in range(n)]


def dr_scan(tdi: int) -> tuple[bytes, list[int]]:
    """A 35-bit DR scan from Run-Test/Idle back to it, as probe-rs lowers a DPACC access. Returns
    the command and the lengths of its captured sequences."""
    bits = bits_of(tdi, 35)
    seqs = [
        sequence([1], 1, False),  # Select-DR
        sequence([1, 1], 0, False),  # Capture-DR, Shift-DR
        sequence(bits[:34], 0, True),
        sequence(bits[34:], 1, True),  # the last bit and Exit1-DR
        sequence([1], 1, False),  # Update-DR
        sequence([1], 0, False),  # Run-Test/Idle
    ]
    return bytes([cy.ID_JTAG_SEQUENCE, len(seqs)]) + b"".join(seqs), [34, 1]


def ir_scan(ir: int) -> bytes:
    bits = bits_of(ir, 4)
    seqs = [
        sequence([1, 1], 1, False),  # Select-DR, Select-IR
        sequence([1, 1], 0, False),  # Capture-IR, Shift-IR
        sequence(bits[:3], 0, False),
        sequence(bits[3:], 1, False),
        sequence([1], 1, False),
        sequence([1], 0, False),
    ]
    return bytes([cy.ID_JTAG_SEQUENCE, len(seqs)]) + b"".join(seqs)


def unpack(reply: bytes, lengths: list[int]) -> int:
    """The captured TDO bits of a DAP_JTAG_Sequence reply, in order."""
    value, at, shift = 0, 2, 0
    for n in lengths:
        nbytes = (n + 7) // 8
        chunk = int.from_bytes(reply[at : at + nbytes], "little") & ((1 << n) - 1)
        value |= chunk << shift
        at, shift = at + nbytes, shift + n
    return value


def held_late(r: cy.Read, stall: int) -> int | None:
    """A sample's late margin once a load held `stall` cycles has taken its share."""
    return None if r.late is None else r.late - (stall if r.after_next else 0)


def run(elf: str, hz: int, latency: int, cpu_hz: int, gen_latency: int = 0, wire: str = "cjtag",
        stall: int = 0) -> dict:
    dr = cy.JTAG_DR_OK | (WORD << 3)
    model = cy.Model(engine=True)
    probe = cy.Machine(elf, cy.Target([]), restart_at=set(), model=model, record_trace=False, record_stats=False)
    if "DAP_Setup" in probe.sym:
        probe.call("DAP_Setup")
    # An image from before the stall only has its margins taken.
    if stall and "gen_stall" in probe.sym:
        probe.uc.mem_write(probe.sym["gen_stall"], stall.to_bytes(4, "little"))
    probe.command(bytes([cy.ID_CONNECT, 2]))
    probe.command(bytes([cy.ID_SWJ_CLOCK]) + cy.u32(hz))
    probe.command(bytes([cy.ID_TRANSFER_CONFIG, 0]) + cy.u16(100) + cy.u16(0))
    probe.command(bytes([cy.ID_JTAG_CONFIGURE, 1, 4]))
    two_pin = wire in cy.TWO_PIN
    if two_pin:
        # Framing on without an activation: the target starts in Run-Test/Idle, framed, which is
        # where a host leaves it after driving the activation itself.
        probe.command(cy.cjtag_enter(wire))
    if gen_latency:
        probe.command(bytes([ID_VENDOR_JTAG_GEN, gen_latency if two_pin else 0, 0 if two_pin else gen_latency, 0]))
    tap = cy.RecordingTap(dr, 35)
    # An escape edge a host's clock period after the last: the standard asks for the target's
    # shortest TCKC period, which a host running at that clock or slower meets.
    target = (cy.TimedCjtagTarget(tap, latency=latency, oscan2=wire == "cjtag2", oscan5=wire == "cjtag5",
                                  oscan6=wire == "cjtag6", escape_gap=-(-cpu_hz // hz),
                                  quarter=-(-max(-(-cpu_hz // hz), 3) // 4)) if two_pin
              else cy.TimedJtagTarget(tap, latency=latency))
    probe.target = target

    problems: list[str] = []
    probe.begin()
    scan, lengths = dr_scan(0x5_1234_5678)
    first_rise = len(probe.counts.edge_cycles)
    reply = probe.command(scan)
    scan_rises = probe.counts.edge_cycles[first_rise:]
    if reply[:2] != bytes([cy.ID_JTAG_SEQUENCE, 0]):
        problems.append(f"DR scan answered {reply[:2].hex()}")
    elif (got := unpack(reply, lengths)) != dr:
        problems.append(f"DR scan read {got:09x}, the target shifted out {dr:09x}")
    if tap.updates[-1:] != [("dr", 0x5_1234_5678)]:
        problems.append(f"DR scan wrote {tap.updates[-1:]}, wanted 0x512345678")

    reply = probe.command(ir_scan(APACC))
    if tap.updates[-1:] != [("ir", APACC)]:
        problems.append(f"IR scan wrote {tap.updates[-1:]}, wanted IR 0x{APACC:x}")

    # DAP_Transfer: an AP read is posted, so two of them return the word once; then a write.
    reply = probe.command(bytes([cy.ID_TRANSFER, 0, 3, AP_DRW_READ, AP_DRW_READ, AP_DRW_WRITE]) + cy.u32(WRITE_WORD))
    want = bytes([cy.ID_TRANSFER, 3, 1]) + cy.u32(WORD) * 2
    if reply != want:
        problems.append(f"DAP_Transfer replied {reply.hex()}, wanted {want.hex()}")
    writes = [v for kind, v in tap.updates if kind == "dr" and (v & 0b111) == ((AP_DRW_WRITE >> 1) & 0b111)]
    if not writes or (writes[-1] >> 3) != WRITE_WORD:
        problems.append(f"DAP_Transfer write left {writes[-1:]} in the DR")

    reply = probe.command(bytes([cy.ID_TRANSFER_BLOCK, 0]) + cy.u16(4) + bytes([AP_DRW_READ]))
    want = bytes([cy.ID_TRANSFER_BLOCK]) + cy.u16(4) + bytes([1]) + cy.u32(WORD) * 4
    if reply != want:
        problems.append(f"block read replied {reply[:12].hex()}..., wanted {want[:12].hex()}...")
    reply = probe.command(bytes([cy.ID_TRANSFER_BLOCK, 0]) + cy.u16(4) + bytes([AP_DRW_WRITE]) + cy.u32(WRITE_WORD) * 4)
    if reply != bytes([cy.ID_TRANSFER_BLOCK]) + cy.u16(4) + bytes([1]):
        problems.append(f"block write replied {reply.hex()}")
    counts = probe.end()

    problems += target.problems[:4]
    if len(target.problems) > 4:
        problems.append(f"... {len(target.problems) - 4} more from the target")
    if two_pin and counts.moves_while_high != target.escape_edges:
        problems.append(f"{counts.moves_while_high - target.escape_edges} TMSC moves with TCKC high outside an "
                        "EOT escape")
    outside = [r for r in target.reads if r.early < 0 or (r.late is not None and r.late < 0)]
    if outside:
        r = outside[0]
        problems.append(f"{len(outside)} of {len(target.reads)} TDO samples outside the window (first: early {r.early}, late {r.late})")
    held = [r for r in target.reads if (late := held_late(r, stall)) is not None and late < 0]
    if stall and held:
        problems.append(f"{len(held)} of {len(target.reads)} TDO samples outside the window when held {stall}")
    elif stall and any(late is not None and late < 1 for late in (held_late(r, stall) for r in target.reads)):
        problems.append(f"no cycle to spare when held {stall}")

    # No packet shorter than the clock asked for, and neither phase shorter than the generator's
    # own split of it.
    period = max(-(-cpu_hz // hz), 3)
    low = 2 if period == 3 else period // 2
    high = period - low
    rises, falls = counts.edge_cycles, counts.fall_cycles
    for name, seq in (("rise-to-rise", rises), ("fall-to-fall", falls)):
        gaps = [b - a for a, b in zip(seq, seq[1:])]
        if gaps and min(gaps) < period:
            problems.append(f"{name} {min(gaps)} cycles, period {period}")
    min_low, min_high = cy.minimum_clock_phases(rises, falls)
    if min_low is not None and min_low < low:
        problems.append(f"low phase {min_low} cycles, wanted {low}")
    if min_high is not None and min_high < high:
        problems.append(f"high phase {min_high} cycles, wanted {high}")

    per = 3 if two_pin else 1
    frames = sorted(b - a for a, b in zip(scan_rises, scan_rises[per:]))
    margins = [m for r in target.reads for m in (r.early, held_late(r, stall)) if m is not None]
    return {
        "period": period,
        "frame": frames[len(frames) // 2] if frames else 0,
        "margin": min(margins) if margins else None,
        "problems": problems,
    }


ID_JTAG_FORMAT, ID_JTAG_ESCAPE = 0x95, 0x96
RAW_BITS = [1, 0, 1, 1, 0, 0, 1, 0, 1, 1, 1, 0, 0, 0, 1, 0]
# CJTAG_ESCAPE_GAP_MAX (src/cjtag.c): 50 us in delay-loop passes of three cycles.
ESCAPE_CAP_US = 50


def swj_sequence(bits: list[int]) -> bytes:
    data = bytearray((len(bits) + 7) // 8)
    for i, b in enumerate(bits):
        data[i // 8] |= b << (i % 8)
    return bytes([cy.ID_SWJ_SEQUENCE, len(bits)]) + bytes(data)


def run_raw(elf: str, hz: int, cpu_hz: int) -> dict:
    """Raw output with an OScan format selected, as the cJTAG profile's timing rules ask: raw
    DAP_SWJ_Sequence sends one TCKC period a bit, its TMSC changes only while TCKC is low and a
    quarter period after the fall before; DAP_JTAG_Escape makes exactly its edges while TCKC is high,
    each, and the fall, at least a period (capped at 50 us) after the one before, and no more than
    the cap where the period is longer; every command leaves TCKC low."""
    model = cy.Model(engine=True)
    probe = cy.Machine(elf, cy.Target([]), restart_at=set(), model=model, record_trace=False, record_stats=False)
    if "DAP_Setup" in probe.sym:
        probe.call("DAP_Setup")
    probe.command(bytes([cy.ID_CONNECT, 2]))
    probe.command(bytes([cy.ID_SWJ_CLOCK]) + cy.u32(hz))
    probe.command(bytes([ID_JTAG_FORMAT, 1]))
    period = max(-(-cpu_hz // hz), 3)
    quarter = -(-period // 4)
    cap = cpu_hz * ESCAPE_CAP_US // 1_000_000
    gap = min(period, cap)
    problems: list[str] = []
    events: list[tuple[int, int, str]] = []
    probe.pin_events = events
    probe.begin()
    # Each escape from the level the one before did not leave, so every one moves TMSC first.
    steps = [("sequence", swj_sequence(RAW_BITS), RAW_BITS), ("escape 6", bytes([ID_JTAG_ESCAPE, 6, 1]), (6, 1)),
             ("escape 7", bytes([ID_JTAG_ESCAPE, 7, 0]), (7, 0)), ("escape 10", bytes([ID_JTAG_ESCAPE, 10, 0]), (10, 0)),
             ("sequence after escapes", swj_sequence([0, 1, 1, 0]), [0, 1, 1, 0])]
    spans = []
    for name, request, what in steps:
        start = len(events)
        reply = probe.command(request)
        if reply[:2] != bytes([request[0], 0]):
            problems.append(f"{name} answered {reply[:2].hex()}")
        spans.append((name, what, start, len(events)))
    probe.end()

    # The level each event leaves, and the last fall before it, over the whole stream: the quarter
    # period holds across commands too.
    tck, wire, last_fall = 0, "1", None
    before: list[tuple[int, str, int | None]] = []
    for cycle, t, w in events:
        before.append((tck, wire, last_fall))
        if w != wire and w != "z" and t == 0 and last_fall is not None and cycle - last_fall < quarter:
            problems.append(f"TMSC to {w} at {cycle}, {cycle - last_fall} cycles after a fall, under a quarter "
                            f"period ({quarter})")
        if tck == 1 and t == 0:
            last_fall = cycle
        tck, wire = t, w
    for name, what, start, end in spans:
        span = events[start:end]
        if not span:
            problems.append(f"{name} moved nothing")
            continue
        if span[-1][1] != 0:
            problems.append(f"{name} left TCKC high")
        rises = [i for i, (c, t, w) in enumerate(span) if t == 1 and (before[start + i][0] == 0)]
        if name.startswith("sequence"):
            if len(rises) != len(what):
                problems.append(f"{name}: {len(rises)} TCKC periods for {len(what)} bits")
            sent = [int(span[i][2]) for i in rises]
            if sent != what[: len(sent)]:
                problems.append(f"{name}: TMSC at the rises {sent}, sent {what}")
            gaps = [span[b][0] - span[a][0] for a, b in zip(rises, rises[1:])]
            if gaps and min(gaps) < period:
                problems.append(f"{name}: rise to rise {min(gaps)} cycles, period {period}")
            high = [c for i, (c, t, w) in enumerate(span) if t == 1 and before[start + i][1] != w]
            if high:
                problems.append(f"{name}: TMSC moved with TCKC high at {high[0]}")
            continue
        edges, level = what
        if len(rises) != 1:
            problems.append(f"{name}: {len(rises)} TCKC rises, wanted 1")
            continue
        rise = rises[0]
        fall = next((i for i in range(rise + 1, len(span)) if span[i][1] == 0), None)
        if fall is None:
            problems.append(f"{name}: no fall")
            continue
        moves = [span[i][0] for i in range(rise + 1, fall) if span[i][2] != "z"]
        if len(moves) != edges:
            problems.append(f"{name}: {len(moves)} TMSC edges with TCKC high, wanted {edges}")
        marks = [span[rise][0], *moves, span[fall][0]]
        spacing = [b - a for a, b in zip(marks, marks[1:])]
        if min(spacing) < gap:
            problems.append(f"{name}: edges {min(spacing)} cycles apart, at least {gap} wanted")
        if period > cap and max(spacing) > cap + 16:
            problems.append(f"{name}: edges {max(spacing)} cycles apart, past the {cap}-cycle cap")
        level_before = before[start + rise][1]
        level_after = span[fall][2]
        if level_before != str(level):
            problems.append(f"{name}: TMSC {level_before} at the rise, asked {level}")
        if level_after != str(level ^ (edges & 1)):
            problems.append(f"{name}: TMSC {level_after} after {edges} edges from {level}")
    return {"period": period, "frame": 0, "margin": None, "problems": problems}


def run_point(args: tuple[str, int, int, int, int, str, int]) -> dict:
    if args[5] == "raw":
        return run_raw(args[0], args[1], args[3])
    return run(*args)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf", nargs="*", default=["build-gen/mculink_cjtag_core1.elf"], help="CPU 1 image(s)")
    ap.add_argument("--wire", nargs="+", choices=["cjtag", "cjtag2", "cjtag5", "cjtag6", "jtag", "raw"],
                    default=["cjtag"], help="raw: DAP_SWJ_Sequence and DAP_JTAG_Escape with OScan1 selected")
    ap.add_argument("--mhz", type=float, nargs="*")
    ap.add_argument("--latency", type=int, default=8, help="cycles from the TDO packet's fall to valid TDO")
    ap.add_argument("--gen-latency", type=int, default=0, help="the latency to generate for, with vendor 0x8A; 0 leaves the image's")
    ap.add_argument("--cpu", type=int, default=150)
    ap.add_argument("--stall", type=int, default=0, help="cycles another master can hold a TDO sample's load")
    ap.add_argument("--jobs", type=int, default=min(4, available_cpus()),
                    help="independent clock simulations to run at once")
    args = ap.parse_args()
    if args.jobs < 1:
        ap.error("--jobs must be at least 1")
    cpu_hz = args.cpu * 1_000_000
    mhz = args.mhz or [-(-cpu_hz // p) / 1e6 for p in list(range(3, 41)) + [75, 150, 1500]]
    # An escape's cap only acts where a period passes 50 us.
    raw_mhz = [m for m in mhz] + ([] if args.mhz else [0.01])

    points = [
        (elf, int(m * 1e6 + 0.5), args.latency, cpu_hz, args.gen_latency, wire, args.stall)
        for elf in args.elf
        for wire in args.wire
        for m in (raw_mhz if wire == "raw" else mhz)
    ]
    if args.jobs == 1 or len(points) == 1:
        results = list(map(run_point, points))
    else:
        with concurrent.futures.ProcessPoolExecutor(max_workers=min(args.jobs, len(points))) as pool:
            results = list(pool.map(run_point, points))

    failures = 0
    print(f"{'image':>16} {'wire':>7} {'MHz':>8} {'P':>5} {'frame':>6} {'margin':>7}  problems")
    for point, r in zip(points, results):
        elf, hz, _, _, _, wire, _ = point
        failures += bool(r["problems"])
        margin = "-" if r["margin"] is None else str(r["margin"])
        print(f"{os.path.basename(os.path.dirname(elf)):>16} {wire:>7} {hz / 1e6:8.3f} {r['period']:5d} "
              f"{r['frame']:6d} {margin:>7}  " + ("; ".join(r["problems"]) or "ok"))
    print(f"\n{len(points) - failures}/{len(points)} clock points clean")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
