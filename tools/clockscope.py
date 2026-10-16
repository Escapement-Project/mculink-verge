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

`--histogram` reads a real transfer instead of a clock burst. A transfer has no silent gaps to split
on and several periods in it at once -- the data bits, the per-word bookkeeping, the gap between
commands -- so it is read as a distribution rather than an average. The busiest cluster is the data
bit, which is the one the cost model predicts.
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


def runs(intervals: list[float], tolerance: float, least: int) -> list[tuple[int, float]]:
    """Maximal runs of consecutive clocks at one period, as (length, mean period).

    A transfer does not have one period. A read bit and a write bit cost different numbers of I/O
    operations, and the bookkeeping between words costs more again, so the capture holds several
    periods interleaved. Averaging a whole run recovers each one far more precisely than the
    analyser's sample period: the probe's clock and the analyser's are independent, so the
    quantisation error cancels over the run rather than accumulating.
    """
    out = []
    current: list[float] = []
    for interval in intervals:
        if current and abs(interval - current[0]) > tolerance:
            if len(current) >= least:
                out.append((len(current), statistics.fmean(current)))
            current = []
        current.append(interval)
    if len(current) >= least:
        out.append((len(current), statistics.fmean(current)))
    return out


def gather(found: list[tuple[int, float]], bin_ns: float) -> list[dict]:
    """Group run periods that agree to within a bin, busiest first."""
    bins: dict[int, list[tuple[int, float]]] = {}
    for length, period in found:
        bins.setdefault(round(period * 1e9 / bin_ns), []).append((length, period))
    out = []
    for key, members in bins.items():
        clocks = sum(length for length, _ in members)
        weighted = sum(length * period for length, period in members) / clocks
        out.append({"clocks": clocks, "runs": len(members), "period": weighted})
    out.sort(key=lambda item: item["clocks"], reverse=True)
    return out


def histogram(args) -> int:
    edges = read(args.csv, args.channel)
    if not edges:
        sys.exit(f"channel {args.channel} never moved in {args.csv}")
    groups = split(edges, args.group_gap)
    asked = [int(rate) for rate in args.rates.split(",")] if args.rates else []
    print(f"channel {args.channel}: {len(edges)} edges in {len(groups)} groups")
    shown = 0
    for group in groups:
        rising = [time for time, level in group if level == 1]
        if len(rising) < 1000:
            continue
        intervals = [b - a for a, b in zip(rising, rising[1:])]
        found = runs(intervals, args.tolerance * 1e-9, args.least)
        if not found:
            continue
        clusters = gather(found, args.bin_ns)
        total = sum(item["clocks"] for item in clusters)
        label = f"{asked[shown] / 1e6:.2f} MHz asked" if shown < len(asked) else f"group {shown}"
        span = group[-1][0] - group[0][0]
        print(f"\n{label}: {len(rising)} clocks over {span * 1e3:.1f} ms, starts at {group[0][0]:.3f} s")
        print(f"  {total} of them in runs of {args.least} or more at one period")
        for item in clusters[:5]:
            share = item["clocks"] / total * 100
            line = (
                f"  {item['period'] * 1e9:9.3f} ns  {item['clocks']:>8} clocks in"
                f" {item['runs']:>6} runs  {share:5.1f}%  -> {1 / item['period'] / 1e6:7.3f} MHz"
            )
            if args.cpu_hz:
                line += f"  {item['period'] * args.cpu_hz:7.3f} cycles"
            print(line)
        if shown < len(asked):
            best = clusters[0]
            print(f"  busiest run period is {1 / best['period'] / asked[shown] * 100:.1f}% of what was asked")
        shown += 1
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("csv", type=Path, help="digital.csv from the analyser's raw export")
    parser.add_argument("--channel", type=int, required=True, help="the SWCLK channel")
    parser.add_argument("--rates", help="comma separated Hz, in the order they were driven")
    parser.add_argument("--cpu-hz", type=float, help="core clock, to report periods in cycles")
    parser.add_argument("--histogram", action="store_true", help="read a transfer, not a burst")
    parser.add_argument("--group-gap", type=float, default=GROUP_GAP, help="seconds between runs")
    parser.add_argument("--tolerance", type=float, default=15.0, help="ns a run may vary by")
    parser.add_argument("--least", type=int, default=24, help="shortest run worth averaging")
    parser.add_argument("--bin-ns", type=float, default=0.5, help="how close two runs must agree")
    args = parser.parse_args()

    if args.histogram:
        return histogram(args)

    edges = read(args.csv, args.channel)
    if not edges:
        sys.exit(f"channel {args.channel} never moved in {args.csv}")
    groups = split(edges, args.group_gap)
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
