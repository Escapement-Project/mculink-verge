# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn", "capstone"]
# ///
"""Hold the generated SWD paths to their own schedule, on the timing engine.

For each clock asked for, configure the image, then run a block read and a block write against a
target that answers by time (tools/cycles.py's TimedSwdTarget, serving what the timing puts on
the pin). Check that no clock and no clock phase is shorter than the period, that every sample
read the bit it was placed for, and that the replies carry what the target sent. Report the
smallest sample margin, and the cycles a transfer takes in a running block against its floor: 46
clocks, those the probe drives at the write period and the rest at the read period.
Clock points run in parallel across up to four workers by default; pass `--jobs 1` to run them
serially.

    uv run tools/gencheck.py build-gen/mculink_cjtag_core1.elf
    uv run tools/gencheck.py build-gen/mculink_cjtag_core1.elf --mhz 37.5 30 25 --latency 7
"""

import argparse
import concurrent.futures
import os
import sys
from host_resources import available_cpus

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import cycles as cy  # noqa: E402

WORD = 0xA5A5_5A5A
AP_DRW_READ, AP_DRW_WRITE = 0x0F, 0x0D


def pattern() -> list[int]:
    """OK, then the word and its parity. A write reads only the acknowledge, and the RDBUFF read
    that checks a write block gets the rest."""
    data = [(WORD >> i) & 1 for i in range(32)]
    return [1, 0, 0] + data + [sum(data) & 1]


def run(elf: str, hz: int, read: bool, words: int, latency: int, target_latency: int, cpu_hz: int):
    model = cy.Model(engine=True, latency=latency)
    target = cy.TimedSwdTarget(pattern(), target_latency, serve="wire", idle=1)
    probe = cy.Machine(elf, target, restart_at=set(), model=model, record_trace=False, record_stats=False)
    probe.resync_on_release = True
    if "DAP_Setup" in probe.sym:
        probe.call("DAP_Setup")
    probe.command(bytes([cy.ID_CONNECT, 1]))
    probe.command(bytes([cy.ID_SWJ_CLOCK]) + cy.u32(hz))
    probe.command(bytes([cy.ID_TRANSFER_CONFIG, 0]) + cy.u16(100) + cy.u16(0))
    probe.command(bytes([cy.ID_SWD_CONFIGURE, 0x00]))
    payload = bytes([cy.ID_TRANSFER_BLOCK, 0]) + cy.u16(words) + bytes([AP_DRW_READ if read else AP_DRW_WRITE])
    if not read:
        payload += cy.u32(WORD) * words
    probe.begin()
    reply = probe.command(payload)
    counts = probe.end()

    problems = []
    want = bytes([cy.ID_TRANSFER_BLOCK]) + cy.u16(words) + bytes([1])
    if read:
        want += cy.u32(WORD) * words
    if reply != want:
        problems.append(f"reply {reply[:12].hex()}..., wanted {want[:12].hex()}...")

    # The clock asked for is a ceiling on every period; the probe's own clocks run at it down to
    # three cycles and the target's down to four.
    period = max(-(-cpu_hz // hz), 3)
    read_period = max(period, 4)
    low = min(read_period // 2, 2 if period == 3 else period // 2)
    high = min(read_period - read_period // 2, period - (2 if period == 3 else period // 2))
    rises, falls = counts.edge_cycles, counts.fall_cycles
    shortest = {}
    for name, seq in (("rise-to-rise", rises), ("fall-to-fall", falls)):
        gaps = [b - a for a, b in zip(seq, seq[1:])]
        shortest[name] = min(gaps) if gaps else None
        if gaps and min(gaps) < period:
            problems.append(f"{name} {min(gaps)} cycles, period {period}")
    min_low, min_high = cy.minimum_clock_phases(rises, falls)
    if min_low is not None and min_low < low:
        problems.append(f"low phase {min_low} cycles, wanted {low}")
    if min_high is not None and min_high < high:
        problems.append(f"high phase {min_high} cycles, wanted {high}")
    margins = [r.margin for r in counts.reads if r.margin is not None]
    wrong = [r for r in counts.reads if r.got != r.wanted]
    if wrong:
        problems.append(f"{len(wrong)} of {len(counts.reads)} samples read a neighbouring bit")
    # A transfer is 46 rising edges at turnaround 1, read or write: the median distance between
    # edges that far apart is the transfer period in a running block, overheads between commands
    # left out.
    per = sorted(b - a for a, b in zip(rises, rises[46:]))
    # Packet bits at the write period, the rest at the read period; a write's data too.
    driven = 8 + (0 if read else 33)
    return {
        "period": period,
        "floor": driven * period + (46 - driven) * read_period,
        "margin": min(margins) if margins else None,
        "cycles": per[len(per) // 2] if per else 0,
        "problems": problems,
    }


def run_point(args: tuple[str, int, int, int, int, int]) -> tuple[dict, dict]:
    elf, hz, words, latency, target_latency, cpu_hz = args
    read = run(elf, hz, True, words, latency, target_latency, cpu_hz)
    write = run(elf, hz, False, words, latency, target_latency, cpu_hz)
    return read, write


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("elf", nargs="?", default="build-gen/mculink_cjtag_core1.elf", help="CPU 1's image")
    ap.add_argument("--mhz", type=float, nargs="*")
    ap.add_argument("--words", type=int, default=16)
    ap.add_argument("--latency", type=int, default=7, help="the latency the image generates for")
    ap.add_argument("--target-latency", type=int, help="the latency the modelled target has (default: --latency)")
    ap.add_argument("--cpu", type=int, default=150)
    ap.add_argument("--jobs", type=int, default=min(4, available_cpus()),
                    help="independent clock points to simulate in parallel")
    args = ap.parse_args()
    if args.jobs < 1:
        ap.error("--jobs must be at least 1")
    cpu_hz = args.cpu * 1_000_000
    # The lowest clock that gives each period, so every period is tried once.
    mhz = args.mhz or [-(-cpu_hz // p) / 1e6 for p in list(range(3, 41)) + [75, 150, 1500]]
    target_latency = args.target_latency if args.target_latency is not None else args.latency
    points = [
        (args.elf, int(m * 1e6 + 0.5), args.words, args.latency, target_latency, cpu_hz)
        for m in mhz
    ]
    if args.jobs == 1 or len(points) == 1:
        results = list(map(run_point, points))
    else:
        with concurrent.futures.ProcessPoolExecutor(max_workers=min(args.jobs, len(points))) as pool:
            results = list(pool.map(run_point, points))

    failures = 0
    print(f"{'MHz':>8} {'P':>5} {'read':>7} {'floor':>7} {'write':>7} {'floor':>7} {'margin':>7}  problems")
    for m, (r, w) in zip(mhz, results):
        hz = int(m * 1e6 + 0.5)
        problems = r["problems"] + [f"write: {p}" for p in w["problems"]]
        failures += bool(problems)
        margin = min(x for x in (r["margin"], w["margin"]) if x is not None)
        print(
            f"{hz / 1e6:8.3f} {r['period']:5d} {r['cycles']:7d} {r['floor']:7d} {w['cycles']:7d} {w['floor']:7d} {margin:7d}  "
            + ("; ".join(problems) if problems else "ok")
        )
    print(f"\n{len(mhz) - failures}/{len(mhz)} clocks clean")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
