#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = []
# ///
"""Measure the real clock period out of an analyser capture of `DAP_SWJ_Sequence` bursts.

`tools/dap.py sweep` drives one group of bursts per requested rate with a silent gap between the
groups, so the capture has all the structure needed to read it without a trigger: a *burst* is one
command's 256 clocks, and a *group* is every burst sent at one requested rate.

    uv run tools/dap.py --serial S sweep --rates 50000000,10000000,5000000
    uv run tools/clockscope.py <dir>/digital.csv --channel 2

The period is taken across a whole burst rather than between two edges. One edge pair at 100 MS/s
quantises to 10 ns, which is a third of a period at the top of the range; 255 periods of the same
burst brings that to 0.04 ns and the answer stops depending on the sample clock.

With `--cpu-hz`, each period is also reported in core cycles, which is the number the cost model in
tools/cycles.py predicts and the only form the two can be compared in.
"""

import argparse
import statistics
import sys
from pathlib import Path

BURST_GAP = 20e-6  # a USB round trip between two commands, far longer than any bit period
GROUP_GAP = 50e-3  # the silent gap tools/dap.py sweep leaves between two requested rates


def read(path: Path, channel: int) -> list[tuple[float, int]]:
    """Every transition on one channel, as (time, new level)."""
    with path.open() as handle:
        header = handle.readline().rstrip("\n").split(",")
        try:
            column = header.index(f"Channel {channel}")
        except ValueError:
            sys.exit(f"{path} has no 'Channel {channel}'; it has {header[1:]}")
        edges = []
        previous = None
        for line in handle:
            if not line.strip():
                continue
            fields = line.rstrip("\n").split(",")
            level = int(fields[column])
            if level != previous:
                edges.append((float(fields[0]), level))
                previous = level
    return edges


def split(edges: list[tuple[float, int]], gap: float) -> list[list[tuple[float, int]]]:
    groups: list[list[tuple[float, int]]] = []
    for edge in edges:
        if groups and edge[0] - groups[-1][-1][0] <= gap:
            groups[-1].append(edge)
        else:
            groups.append([edge])
    return groups


def measure(burst: list[tuple[float, int]]) -> dict | None:
    """Period, high time and low time of one burst, averaged over the whole burst."""
    rising = [time for time, level in burst if level == 1]
    falling = [time for time, level in burst if level == 0]
    if len(rising) < 2:
        return None
    period = (rising[-1] - rising[0]) / (len(rising) - 1)
    # Pair each rising edge with the fall that follows it, so the high time is unambiguous.
    highs, lows = [], []
    for index, start in enumerate(rising[:-1]):
        after = [time for time in falling if time > start]
        if after and after[0] < rising[index + 1]:
            highs.append(after[0] - start)
            lows.append(rising[index + 1] - after[0])
    return {
        "clocks": len(rising),
        "period": period,
        "high": statistics.fmean(highs) if highs else float("nan"),
        "low": statistics.fmean(lows) if lows else float("nan"),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("csv", type=Path, help="digital.csv from the analyser's raw export")
    parser.add_argument("--channel", type=int, required=True, help="the SWCLK channel")
    parser.add_argument("--rates", help="comma separated Hz, in the order they were driven")
    parser.add_argument("--cpu-hz", type=float, help="core clock, to report periods in cycles")
    args = parser.parse_args()

    edges = read(args.csv, args.channel)
    if not edges:
        sys.exit(f"channel {args.channel} never moved in {args.csv}")
    groups = split(edges, GROUP_GAP)
    asked = [int(rate) for rate in args.rates.split(",")] if args.rates else []

    print(f"channel {args.channel}: {len(edges)} edges in {len(groups)} groups")
    for index, group in enumerate(groups):
        bursts = [measure(burst) for burst in split(group, BURST_GAP)]
        bursts = [burst for burst in bursts if burst]
        if not bursts:
            continue
        periods = [burst["period"] for burst in bursts]
        period = statistics.fmean(periods)
        spread = statistics.pstdev(periods)
        clocks = sum(burst["clocks"] for burst in bursts)
        label = f"{asked[index] / 1e6:.2f} MHz asked" if index < len(asked) else f"group {index}"
        print(f"\n{label}: {len(bursts)} bursts, {clocks} clocks, starts at {group[0][0]:.3f} s")
        print(f"  period   {period * 1e9:8.2f} ns +/- {spread * 1e9:.2f}  ->  {1 / period / 1e6:7.3f} MHz")
        print(
            f"  high     {statistics.fmean(b['high'] for b in bursts) * 1e9:8.2f} ns"
            f"   low {statistics.fmean(b['low'] for b in bursts) * 1e9:8.2f} ns"
        )
        if args.cpu_hz:
            cycles = period * args.cpu_hz
            print(f"  cycles   {cycles:8.3f} at {args.cpu_hz / 1e6:.0f} MHz core")
        if index < len(asked):
            print(f"  achieved {1 / period / asked[index] * 100:8.1f}% of what was asked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
