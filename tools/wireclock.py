#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone", "libusb1", "numpy", "pyusb", "pyserial", "rich", "unicorn"]
# ///
"""The SWD clock on the wire against the clock asked for, from a logic analyser, kept in bench/wireclock/.

    uv run tools/wireclock.py run --label gcc --image build/mculink_cjtag.elf
    uv run tools/wireclock.py run --label stock --image <stock .s19>
    uv run tools/wireclock.py run --label clang --image <elf> --clock 1 --clock 10 --clock 50
    uv run tools/wireclock.py analyse <dir kept with --keep>
    uv run tools/wireclock.py compare bench/wireclock/2026-10-20-*.json
    uv run tools/wireclock.py positions bench/wireclock/2026-10-20-stock.json --clock 50
    uv run tools/wireclock.py model build/mculink_cjtag_core1.elf bench/wireclock/2026-10-20-gcc.json

At each clock of tools/cycles.py's table the probe runs the same commands while the analyser
records SWDIO and SWCLK at the target. KINDS lists them: single and block reads and writes of the
AP's TAR, which the AP answers without a bus access so nothing waits, and DPIDR reads eight to a
command and two commands deep, the case stock misreads from 25 MHz asked.

A capture gives two readings. Where every clock pulse is resolved, each command's rising edges are
laid out as the transfers it carries, and every period and both its phases are averaged by
position over all transfers of the same role. The analyser's sampling phase differs from one
transfer to the next, so the average resolves far below its 10 ns sample. Pulses much under 20 ns
are not all resolved at 100 MS/s, and there the second reading takes over: TAR holds 0x55555555,
so SWDIO changes on every data bit, and a bit lasts a whole period rather than one phase.

`model` sets a result for our firmware beside what tools/cycles.py's emulator predicts for the same
commands on CPU 1's image, position by position.

The analyser is driven through Logic 2's local automation endpoint, MCP over HTTP.
"""

import argparse
import datetime
import hashlib
import itertools
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import urllib.request

import numpy as np
from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn, TimeElapsedColumn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import coverage  # noqa: E402
import dap  # noqa: E402
import throughput  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(REPO, "bench", "wireclock")
ANALYSER = "http://127.0.0.1:10530/"

MATRIX = "wire-1"
CLOCKS_MHZ = (1, 2, 3, 5, 8, 10, 12, 14, 16, 20, 25, 30, 40, 50)
SAMPLE_RATE = 100_000_000
PATTERN = 0x5555_5555
AP_TAR_READ = 0x07
AP_TAR_WRITE = 0x05
DP_DPIDR_READ = 0x02

# Each kind: the command, how many to send, and how many in flight. Separated on the wire by
# KIND_GAP of silence, which is how the analysis tells them apart without a trigger.
KINDS = (
    ("transfer ap-read", bytes([dap.DAP_TRANSFER, 0, 1, AP_TAR_READ]), 1000, 1),
    ("transfer ap-write", bytes([dap.DAP_TRANSFER, 0, 1, AP_TAR_WRITE]) + PATTERN.to_bytes(4, "little"), 1000, 1),
    ("block ap-read", bytes([dap.DAP_TRANSFER_BLOCK, 0]) + (32).to_bytes(2, "little") + bytes([AP_TAR_READ]), 200, 1),
    ("block ap-write", bytes([dap.DAP_TRANSFER_BLOCK, 0]) + (32).to_bytes(2, "little") + bytes([AP_TAR_WRITE])
     + PATTERN.to_bytes(4, "little") * 32, 200, 1),
    ("transfer dp-read x8 depth 2", bytes([dap.DAP_TRANSFER, 0, 8]) + bytes([DP_DPIDR_READ]) * 8, 2000, 2),
)
KIND_GAP = 0.03  # seconds of silence between kinds
SEGMENT_GAP = 10e-3  # silence that separates two kinds in the analysis
# Silence that separates two commands at depth 1, which come at least 35 us apart while a capture
# runs. Inside a command the longest pause seen is 16 us.
BURST_GAP = 20e-6
MIN_TRANSFERS = 50  # fewer than this at a position and its average is not reported
STALL = 500e-9  # a period this much over its position's mean: the probe was held up mid-transfer
MIN_RUN = 20  # two-bit SWDIO spans in a row that make a data phase

ACKS = {1: "OK", 2: "WAIT", 4: "FAULT", 7: "none"}
# Periods by the rising edge that starts them: 0-7 the request, 8-10 launch the acknowledge, then
# a read's data from 11 and a write's from 13, after its turnaround at 11 and 12.
READ_DATA = range(11, 43)
WRITE_DATA = range(13, 45)
READ_ROLES = ("ap-read", "rdbuff", "dp-read")


class Analyser:
    """Logic 2's automation tools, called over its local MCP endpoint."""

    def __init__(self, url: str):
        self.url = url
        self.ids = itertools.count(1)

    def call(self, name: str, **arguments) -> dict:
        body = json.dumps({"jsonrpc": "2.0", "id": next(self.ids), "method": "tools/call",
                           "params": {"name": name, "arguments": arguments}}).encode()
        request = urllib.request.Request(self.url, body, {"Content-Type": "application/json",
                                                          "Accept": "application/json, text/event-stream"})
        with urllib.request.urlopen(request, timeout=600) as response:
            reply = json.loads(response.read())
        if "error" in reply:
            raise RuntimeError(f"{name}: {reply['error']}")
        result = reply["result"]
        if result.get("isError"):
            raise RuntimeError(f"{name}: {result['content'][0]['text']}")
        return result.get("structuredContent") or {}

    def capture(self, channels: list[int]) -> int:
        return self.call("start_capture",
                         logicDeviceConfiguration={"digitalSampleRate": SAMPLE_RATE,
                                                   "logicChannels": {"digitalChannels": channels}},
                         captureConfiguration={"manualCaptureMode": {}})["captureId"]

    def finish(self, capture: int, channels: list[int], directory: str) -> None:
        """Stop, export the channels' transitions as Logic 2 binary files into `directory`, close."""
        try:
            self.call("stop_capture", captureId=capture)
            self.call("export_raw_data_binary", captureId=capture, directory=os.path.abspath(directory),
                      analogDownsampleRatio=1, logicChannels={"digitalChannels": channels})
        finally:
            self.call("close_capture", captureId=capture)


def load(path: str) -> tuple[int, np.ndarray]:
    """A Logic 2 digital binary export: the level the channel starts at, and every transition."""
    with open(path, "rb") as f:
        ident, version, kind, initial, begin, end, n = struct.unpack("<8siiIddQ", f.read(44))
        if ident != b"<SALEAE>" or kind != 0:
            raise SystemExit(f"{path} is not a Logic 2 digital export")
        return initial, np.fromfile(f, dtype="<f8", count=n)


# ---- the probe side -------------------------------------------------------------------------


def run_kind(probe: dap.Probe, request: bytes, commands: int, depth: int) -> list[bytes]:
    """Every reply, in order, with nothing stopping at a failure: a failed command is a result."""
    replies, sent = [], 0
    while len(replies) < commands:
        while sent < commands and sent - len(replies) < depth:
            probe.out_ep.write(request, 2000)
            sent += 1
        replies.append(bytes(probe.in_ep.read(probe.in_ep.wMaxPacketSize, 2000)))
    return replies


def host_summary(request: bytes, replies: list[bytes], dpidr: int) -> dict:
    """What the probe reported for each command of a kind, failures by index."""
    reads = request[0] == dap.DAP_TRANSFER and request[3] in (AP_TAR_READ, DP_DPIDR_READ) or (
        request[0] == dap.DAP_TRANSFER_BLOCK and request[4] == AP_TAR_READ)
    words = request[2] if request[0] == dap.DAP_TRANSFER else int.from_bytes(request[2:4], "little")
    expect = PATTERN if AP_TAR_READ in request[3:5] else dpidr
    failures, ok = [], 0
    for i, reply in enumerate(replies):
        count, ack = dap.transfer_counts(request, reply)
        start = 3 if request[0] == dap.DAP_TRANSFER else 4
        data = [int.from_bytes(reply[start + 4 * k : start + 4 * k + 4], "little") for k in range(count)] if reads else []
        if count == words and ack == 1 and all(w == expect for w in data):
            ok += 1
        else:
            failures.append({"command": i, "count": count, "ack": ack, "reply": reply[: start + 4 * count].hex()})
    return {"commands": len(replies), "ok": ok, "failures": failures}


def measure_clock(probe: dap.Probe, mhz: float, ours: bool, analyser: Analyser, channels: list[int],
                  directory: str) -> tuple[list[dict], dap.Probe]:
    """Every kind at one clock, captured. Attaching happens before the capture starts."""
    dpidr = throughput.attach(probe, int(mhz * 1e6), ours)
    reply = probe.command(bytes([dap.DAP_TRANSFER, 0, 1, AP_TAR_WRITE]) + PATTERN.to_bytes(4, "little"))
    if reply[1] != 1 or reply[2] != 1:
        raise RuntimeError(f"TAR write answered count {reply[1]} ack {reply[2]}")
    capture = analyser.capture(channels)
    log = []
    try:
        time.sleep(0.3)
        for name, request, commands, depth in KINDS:
            replies = run_kind(probe, request, commands, depth)
            log.append(dict(name=name, request=request.hex(), depth=depth, host=host_summary(request, replies, dpidr)))
            time.sleep(KIND_GAP)
            if log[-1]["host"]["failures"]:
                # A failure can leave replies behind, and the next kind would read them as its own.
                probe.drain()
        time.sleep(0.1)
    finally:
        analyser.finish(capture, channels, directory)
    return log, probe


# ---- the wire side --------------------------------------------------------------------------


class Wire:
    def __init__(self, directory: str, swdio: int, swclk: int):
        clk0, clk = load(os.path.join(directory, f"digital_{swclk}.bin"))
        self.dio0, self.dio = load(os.path.join(directory, f"digital_{swdio}.bin"))
        # Each line's edges put pulses of one sample on the other where it is low, which this
        # analyser's low threshold registers: on SWDIO ending on a SWCLK edge, on SWCLK just before
        # a rising edge in a write's data. The target sees none of them, since every word written
        # reads back. SWCLK's are highs of under 12 ns, which no clock that decodes produces, and
        # come out here; SWDIO's are stepped around by where levels are read.
        rise = clk[1::2] if clk0 else clk[0::2]
        fall = clk[0::2] if clk0 else clk[1::2]
        first = 1 if clk0 else 0  # the index in `clk` of the first rise
        highs = fall[first:][: len(rise)] - rise[: len(fall) - first]
        glitch = np.nonzero(highs < 12e-9)[0]
        keep = np.ones(len(clk), dtype=bool)
        keep[first + 2 * glitch] = False
        keep[first + 2 * glitch + 1] = False
        clk = clk[keep]
        self.rise = clk[1::2] if clk0 else clk[0::2]
        self.fall = clk[0::2] if clk0 else clk[1::2]

    def level(self, t: np.ndarray) -> np.ndarray:
        """SWDIO half a sample after each time, so a change in the same sample counts."""
        return (self.dio0 ^ (np.searchsorted(self.dio, t + 5e-9, "right") & 1)).astype(np.uint8)

    def segments(self) -> list[np.ndarray]:
        """The rising edges of each kind, split on the silence between kinds."""
        breaks = np.nonzero(np.diff(self.rise) > SEGMENT_GAP)[0]
        return np.split(self.rise, breaks + 1)


def op(request: int) -> str:
    """A request's kind, from its APnDP, RnW and address bits, laid out as a CMSIS-DAP request."""
    if request == 0x0E:
        return "rdbuff"
    return ("ap" if request & 1 else "dp") + ("-read" if request & 2 else "-write")


def label(transfer: dict) -> str:
    """A transfer's kind, and for one cut short after its acknowledge, which acknowledge."""
    name = op(transfer["request"])
    if transfer["length"] == 46:
        return name
    return f"{name}-{'FAULT' if transfer['ack'] == 4 else 'WAIT'}"


def bits_value(bits) -> int:
    return sum(int(b) << i for i, b in enumerate(bits))


def request_bits(request: int) -> np.ndarray:
    """The eight bits a request puts on the wire: start, APnDP, RnW, A2, A3, parity, stop, park."""
    b = [(request >> i) & 1 for i in range(4)]
    return np.array([1, *b, sum(b) & 1, 0, 1], dtype=np.uint8)


def expected_requests(request: bytes) -> list[int]:
    """The transfers a command carries when nothing waits. ARM's DAP_Transfer and its block form,
    which both firmwares follow, finish a posted AP read and check a write with a read of RDBUFF."""
    if request[0] == dap.DAP_TRANSFER_BLOCK:
        out = [request[4] & 0x0F] * int.from_bytes(request[2:4], "little")
    else:
        out, k = [], 3
        for _ in range(request[2]):
            out.append(request[k] & 0x0F)
            k += 1 if request[k] & 2 else 5
    if out[-1] & 3 == 3 or not out[-1] & 2:
        out.append(0x0E)
    return out


def slice_command(bits: np.ndarray, expected: list[int]) -> list[tuple[int, int, int]] | None:
    """Where each transfer of one command starts among its rising edges: (start, length, request).

    The edges hold the expected transfers at 46 each and any WAIT retries at 13, a retry repeating
    the request it waited on, so their count says how many retries there were. They go where the
    request bits read off SWDIO agree best with what the command should carry; a misread bit or
    two costs a point, not the command. None when the count fits no such layout.
    """
    waits, extra = divmod(len(bits) - 46 * len(expected), 13)
    if waits < 0 or extra or waits > 8:
        return None
    wanted = [request_bits(r) for r in expected]

    def agree(at: int, i: int) -> int:
        return int((bits[at : at + 8] == wanted[i]).sum())

    # best[i][w]: the best agreement with i transfers and w retries laid, and how it got there.
    best = {(0, 0): (0, None)}
    for i in range(len(expected) + 1):
        for w in range(waits + 1):
            if (i, w) not in best or i == len(expected):
                continue
            score, _ = best[(i, w)]
            at = 46 * i + 13 * w
            for step, length in (((i + 1, w), 46), ((i, w + 1), 13)):
                if step[1] > waits:
                    continue
                candidate = score + agree(at, i)
                if step not in best or candidate > best[step][0]:
                    best[step] = (candidate, (i, w, length))
    if (len(expected), waits) not in best:
        return None
    layout, state = [], (len(expected), waits)
    while best[state][1] is not None:
        i, w, length = best[state][1]
        layout.append((46 * i + 13 * w, length, expected[i]))
        state = (i, w)
    return layout[::-1]


def slice_kind(wire: Wire, rises: np.ndarray, kind: dict) -> list[list[dict] | None]:
    """Each command of a kind as its transfers, or None for one whose clocks fit no layout.

    Commands are told apart by the silence between them. At two deep the next can follow too
    closely for that, so a run of clocks is shared out between as many commands, in the order
    sent, as its count of edges takes. A command the probe reported failed carried the transfers
    it counted and the one it failed on.
    """
    request = bytes.fromhex(kind["request"])
    full = expected_requests(request)
    failed = {f["command"]: f for f in kind["host"]["failures"]}
    expected = [full if i not in failed or failed[i]["ack"] == 1 else full[: failed[i]["count"] + 1]
                for i in range(kind["host"]["commands"])]
    bits = wire.level(rises)
    after = np.append(bits[1:], 0)  # the target's bit launched at each edge, read after the next
    breaks = np.nonzero(np.diff(rises) > BURST_GAP)[0]
    starts = np.concatenate(([0], breaks + 1))
    ends = np.concatenate((breaks + 1, [len(rises)]))
    out: list[list[dict] | None] = []
    for s, e in zip(starts, ends):
        if len(out) >= len(expected):
            break
        # As many commands as the edges take, allowing each a few retries.
        take, edges = 0, 0
        while len(out) + take < len(expected) and edges + 46 * len(expected[len(out) + take]) <= e - s:
            edges += 46 * len(expected[len(out) + take])
            take += 1
        if take == 0:
            out.append(None)
            continue
        group = [r for c in expected[len(out) : len(out) + take] for r in c]
        layout = slice_command(bits[s:e], group)
        if layout is None:
            out.extend([None] * take)
            continue
        k = 0
        for c in expected[len(out) : len(out) + take]:
            command, placed = [], 0
            while placed < len(c):
                at, length, req = layout[k]
                k += 1
                j = s + at
                t = {"start": j, "length": length, "request": req, "ack": bits_value(after[j + 8 : j + 11])}
                if length == 46:
                    placed += 1
                    data = after[j + 11 : j + 43] if req & 2 else bits[j + 13 : j + 45]
                    t["data"] = bits_value(data)
                command.append(t)
            out.append(command)
    return out + [None] * (len(expected) - len(out))


def phases(wire: Wire, r: np.ndarray) -> tuple[np.ndarray, np.ndarray, np.ndarray] | None:
    """Periods, highs and lows in ns between consecutive rising edges, or None with a pulse lost."""
    idx = np.searchsorted(wire.fall, r[:-1], "right")
    if idx[-1] >= len(wire.fall):
        return None
    f = wire.fall[idx]
    if (f >= r[1:]).any():
        return None
    return (np.diff(r) * 1e9, (f - r[:-1]) * 1e9, (r[1:] - f) * 1e9)


def target_delay(wire: Wire, rises: np.ndarray, transfers: list[dict]) -> list[float]:
    """How long after its launching rising edge each target-driven SWDIO change arrives, in ns."""
    out = []
    for t in transfers:
        if t["ack"] != 1 or not t["request"] & 0b10:
            continue
        r = rises[t["start"] + 8 : t["start"] + 44]
        lo, hi = np.searchsorted(wire.dio, [r[0], r[-1]])
        for change in wire.dio[lo:hi]:
            k = np.searchsorted(r, change, "right") - 1
            out.append((change - r[k]) * 1e9)
    return out


def swdio_runs(wire: Wire, rises: np.ndarray) -> dict:
    """Data bits timed from SWDIO alone, which needs no clock pulse resolved.

    With TAR holding 0x55555555 a data phase changes SWDIO on every bit. Rising and falling edges
    cross this analyser's threshold up to about 20 ns apart on the target's drive, so a single
    spacing says little; the span over two changes, rising to rising or falling to falling, is
    one clean period of two bits. A data phase is a run of those spans within 30% of the command's
    median, and it is timed over the longest even number of bits in it.
    """
    breaks = np.nonzero(np.diff(rises) > BURST_GAP)[0]
    starts = np.concatenate(([0], breaks + 1))
    ends = np.concatenate((breaks, [len(rises) - 1]))
    bits = []
    for s, e in zip(starts, ends):
        lo, hi = np.searchsorted(wire.dio, [rises[s] - 1e-6, rises[e] + 1e-6])
        changes = wire.dio[lo:hi]
        if len(changes) < MIN_RUN + 2:
            continue
        two = changes[2:] - changes[:-2]
        even = np.abs(two - np.median(two)) < 0.3 * np.median(two)
        k = 0
        while k < len(even):
            if not even[k]:
                k += 1
                continue
            m = k
            while m < len(even) and even[m]:
                m += 1
            if m - k >= MIN_RUN:
                span = (m + 1 - k) // 2 * 2
                bits.append((changes[k + span] - changes[k]) / span * 1e9)
            k = m
    if not bits:
        return {"runs": 0}
    return {"runs": len(bits), "bit_ns": float(np.median(bits)), "bit_ns_min": float(np.min(bits)),
            "bit_ns_max": float(np.max(bits))}


def stats(rows: list[np.ndarray]) -> dict:
    a = np.array(rows)
    return {"mean": a.mean(axis=0).round(3).tolist(), "min": a.min(axis=0).round(1).tolist(),
            "max": a.max(axis=0).round(1).tolist()}


def roles(ops: list[str]) -> list[str]:
    """Each transfer's role in its command: its kind, and first, middle, last or only of that kind."""
    out = []
    for i, op in enumerate(ops):
        same = [k for k, o in enumerate(ops) if o == op]
        place = "only" if len(same) == 1 else "first" if i == same[0] else "last" if i == same[-1] else "middle"
        out.append(f"{op}:{place}")
    return out


def analyse_kind(wire: Wire, rises: np.ndarray, kind: dict) -> dict:
    grouped = slice_kind(wire, rises, kind)
    transfers = [t for g in grouped if g for t in g]
    # Acknowledges and data as read off the wire, which at 20 MHz and over on the L1306 is not
    # always what the probe read: see slice_command(). The probe checked every word itself.
    out = {"transfers": len(transfers), "unsliced_commands": sum(g is None for g in grouped), "acks": {}, "data": {}}
    for t in transfers:
        name = ACKS.get(t["ack"], str(t["ack"]))
        out["acks"][name] = out["acks"].get(name, 0) + 1
        if "data" in t:
            out["data"][f"0x{t['data']:08x}"] = out["data"].get(f"0x{t['data']:08x}", 0) + 1
    # Stale reads aside, a kind's data takes a handful of values: keep the commonest few.
    out["data"] = dict(sorted(out["data"].items(), key=lambda kv: -kv[1])[:4])
    out["swdio_runs"] = swdio_runs(wire, rises)
    if not transfers:
        out["resolved"] = False
        return out

    shapes = {}
    for g in filter(None, grouped):
        shape = tuple(label(t) for t in g)
        shapes[shape] = shapes.get(shape, 0) + 1
    usual = max(shapes, key=shapes.get)
    out["commands"] = len(grouped) - out["unsliced_commands"]
    out["usual_shape"] = list(usual) if len(usual) <= 4 else [usual[0], f"... {len(usual)} transfers ...", usual[-1]]
    out["unusual_commands"] = len(grouped) - shapes[usual]

    by_role: dict[str, list] = {}
    lost = 0
    usual_roles = roles(list(usual))
    for g in filter(None, grouped):
        if tuple(label(t) for t in g) != usual:
            continue
        for t, role in zip(g, usual_roles):
            p = phases(wire, rises[t["start"] : t["start"] + t["length"]])
            if p is None:
                lost += 1
                continue
            by_role.setdefault(role, []).append(p)
    out["lost_pulses"] = lost
    # A command with a pulse the analyser lost fits no layout, and is left out. The figures count
    # while most commands are left in: past that the ones left are a chosen few.
    out["complete"] = round(shapes[usual] / len(grouped), 4)
    out["resolved"] = out["complete"] >= 0.5 and lost <= 0.02 * len(transfers)
    stalls: dict[int, int] = {}
    for role, rows in by_role.items():
        if len(rows) < MIN_TRANSFERS:
            continue
        mean = np.array([r[0] for r in rows]).mean(axis=0)
        for r in rows:
            excess = r[0] - mean
            if excess.max() > STALL * 1e9:
                stalls[int(excess.argmax())] = stalls.get(int(excess.argmax()), 0) + 1
    out["stalls_by_position"] = dict(sorted(stalls.items()))
    out["roles"] = {}
    for role, rows in by_role.items():
        if len(rows) < MIN_TRANSFERS:
            continue
        out["roles"][role] = {"transfers": len(rows), "period_ns": stats([r[0] for r in rows]),
                              "high_ns": stats([r[1] for r in rows]), "low_ns": stats([r[2] for r in rows])}

    delays = target_delay(wire, rises, transfers) if not out["unsliced_commands"] else []
    if delays:
        d = np.array(delays)
        out["target_delay_ns"] = {"median": float(np.median(d)), "p5": float(np.percentile(d, 5)),
                                  "p95": float(np.percentile(d, 95)), "changes": len(d)}
    if kind["depth"] > 1:
        out["misreads"] = misreads(wire, rises, grouped, kind, by_role, usual_roles)
    return out


def misreads(wire: Wire, rises: np.ndarray, grouped: list[list[dict]], kind: dict, by_role: dict,
             usual_roles: list[str]) -> list[dict]:
    """For each command the probe reported as failed, what the wire carried for its last transfer."""
    out = []
    mean = {role: np.array([r[0] for r in rows]).mean(axis=0) for role, rows in by_role.items()}
    for failure in kind["host"]["failures"]:
        g = grouped[failure["command"]]
        if g is None:
            out.append({"command": failure["command"], "host_ack": failure["ack"], "unsliced": True})
            continue
        t = g[-1]
        role = usual_roles[len(g) - 1]
        entry = {"command": failure["command"], "host_count": failure["count"], "host_ack": failure["ack"],
                 "wire_transfer": len(g) - 1, "role": role, "wire_ack": ACKS.get(t["ack"], str(t["ack"])),
                 "wire_data": f"0x{t['data']:08x}" if "data" in t else None}
        r = rises[t["start"] : t["start"] + t["length"]]
        p = phases(wire, r)
        if p is not None:
            entry["period_ns"] = p[0].round(1).tolist()
            entry["high_ns"] = p[1].round(1).tolist()
            entry["low_ns"] = p[2].round(1).tolist()
            ref = mean.get(role)
            if ref is not None and len(ref) == len(p[0]):
                entry["stretched"] = [{"position": int(k), "period_ns": round(float(p[0][k]), 1),
                                       "usual_ns": round(float(ref[k]), 1)}
                                      for k in np.nonzero(p[0] - ref > 30)[0]]
                excess = p[0] - ref
                entry["stall_position"] = int(excess.argmax()) if excess.max() > STALL * 1e9 else None
        # When each SWDIO change around the acknowledge came, by the rising edge before it.
        lo, hi = np.searchsorted(wire.dio, [r[8], r[12]])
        changes = []
        for c in wire.dio[lo:hi]:
            k = int(np.searchsorted(r, c, "right") - 1)
            changes.append({"after_rise": k, "ns": round(float((c - r[k]) * 1e9), 1)})
        entry["ack_changes"] = changes
        out.append(entry)
    return out


def summarise(kinds: list[dict]) -> dict:
    """The clock a host gets for data, and the fastest position anywhere, for one requested clock."""
    out = {}
    for direction, data in (("read", READ_DATA), ("write", WRITE_DATA)):
        # The median bit, so the first data bit's decision time and any stop between bits do not
        # pass for the clock.
        rows = [(r["transfers"], np.median(np.array(r["period_ns"]["mean"])[list(data)]))
                for k in kinds if k["wire"].get("resolved") for role, r in k["wire"].get("roles", {}).items()
                if (role.startswith(READ_ROLES) if direction == "read"
                    else role.startswith("ap-write")) and len(r["period_ns"]["mean"]) == 45]
        if rows:
            n = sum(w for w, _ in rows)
            out[f"{direction}_data_ns"] = round(sum(w * v for w, v in rows) / n, 3)
            out[f"{direction}_data_from"] = "swclk"
        else:
            runs = [k["wire"]["swdio_runs"] for k in kinds
                    if k["wire"]["swdio_runs"]["runs"] and ("read" in k["name"]) == (direction == "read")]
            if runs:
                # The kind with the most runs: a block's, where nothing but data bits sits between.
                out[f"{direction}_data_ns"] = round(max(runs, key=lambda r: r["runs"])["bit_ns"], 3)
                out[f"{direction}_data_from"] = "swdio"
    resolved = [k for k in kinds if k["wire"].get("resolved")]
    fastest = None
    for k in resolved:
        for role, r in k["wire"].get("roles", {}).items():
            period = np.array(r["period_ns"]["mean"])
            j = int(period.argmin())
            if fastest is None or period[j] < fastest["period_ns"]:
                fastest = {"kind": k["name"], "role": role, "position": j, "period_ns": round(float(period[j]), 3),
                           "high_ns": r["high_ns"]["mean"][j], "low_ns": r["low_ns"]["mean"][j]}
    # Reads alone as well. Where ours drives SWDIO, the level it just drove moves where this
    # analyser sees SWCLK rise, by up to 10 ns, so a write's data periods alternate long and short in
    # pairs that sum exactly, and its shortest single period is the analyser's, not the probe's.
    reads = [(k, role, r) for k in resolved for role, r in k["wire"]["roles"].items() if role.startswith(READ_ROLES)]
    fastest_read = None
    for k, role, r in reads:
        period = np.array(r["period_ns"]["mean"])
        j = int(period.argmin())
        if fastest_read is None or period[j] < fastest_read["period_ns"]:
            fastest_read = {"kind": k["name"], "role": role, "position": j, "period_ns": round(float(period[j]), 3)}
    if fastest:
        out["fastest"] = fastest
        out["fastest_read"] = fastest_read
        out["shortest_high_ns"] = min(min(r["high_ns"]["mean"]) for k in resolved for r in k["wire"]["roles"].values())
        out["shortest_low_ns"] = min(min(r["low_ns"]["mean"]) for k in resolved for r in k["wire"]["roles"].values())
    return out


def analyse_capture(directory: str, log: list[dict], swdio: int, swclk: int) -> dict:
    wire = Wire(directory, swdio, swclk)
    segments = wire.segments()
    if len(segments) != len(log):
        return {"error": f"{len(segments)} runs of clocks on the wire for {len(log)} kinds"}
    kinds = []
    for rises, kind in zip(segments, log):
        entry = dict(kind)
        entry["wire"] = analyse_kind(wire, rises, kind)
        kinds.append(entry)
    return {"kinds": kinds, "summary": summarise(kinds)}


# ---- commands -------------------------------------------------------------------------------


def provenance(elf: str | None) -> dict:
    record = throughput.provenance(elf)
    measuring = ["tools/wireclock.py", "tools/throughput.py", "tools/dap.py", "tools/coverage.py", "tools/flash.py"]
    record["sources"] = {p: hashlib.sha256(open(os.path.join(REPO, p), "rb").read()).hexdigest() for p in measuring}
    record.pop("probe_rs", None)
    return record


def cmd_run(args) -> int:
    path = args.out or os.path.join(RESULTS, f"{datetime.date.today().isoformat()}-{args.label}.json")
    if os.path.exists(path) and not args.force:
        raise SystemExit(f"{path} exists; pass --force to replace it")
    if args.elf:
        flashed = subprocess.run(["uv", "run", "tools/flash.py", "--cycle", "--serial", args.serial,
                                  "--image", args.elf], cwd=REPO, capture_output=True, text=True)
        if flashed.returncode != 0:
            raise SystemExit(f"flash.py did not bring the image up:\n{flashed.stdout}{flashed.stderr}")
        time.sleep(2)
        attached = [p.serial for p in dap.find()]
        if args.serial not in attached and len(attached) == 1:
            args.serial = attached[0]
    analyser = Analyser(args.analyser)
    devices = analyser.call("get_devices").get("devices", [])
    probe = throughput.open_probe(args.serial)
    probe.wire = "swd"
    firmware = throughput.identify(probe)
    ours = firmware["ours"] is not None
    channels = sorted((args.swdio, args.swclk))
    clocks = tuple(args.clock) if args.clock else CLOCKS_MHZ
    keep = args.keep or tempfile.mkdtemp(prefix="wireclock-")
    console = Console(stderr=True)
    record = {
        "matrix": MATRIX, "label": args.label, "note": args.note,
        "taken": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "serial": args.serial, "chip": args.chip, "firmware": firmware, "provenance": provenance(args.elf),
        "analyser": {"devices": devices, "sample_rate": SAMPLE_RATE, "swdio": args.swdio, "swclk": args.swclk},
        "settings": {"kinds": [{"name": n, "request": r.hex(), "commands": c, "depth": d} for n, r, c, d in KINDS],
                     "pattern": f"0x{PATTERN:08x}"},
        "clocks": [],
    }
    dpidr = throughput.attach(probe, 1_000_000, ours)
    coverage.power_up_and_halt(probe, 0)
    record["target"] = {"dpidr": f"0x{dpidr:08x}"}
    with Progress(TextColumn("{task.description}"), BarColumn(), MofNCompleteColumn(), TimeElapsedColumn(),
                  console=console) as progress:
        task = progress.add_task("clocks", total=len(clocks))
        try:
            for mhz in clocks:
                progress.update(task, description=f"{mhz:g} MHz")
                directory = os.path.join(keep, f"{mhz:g}MHz")
                os.makedirs(directory, exist_ok=True)
                try:
                    log, probe = measure_clock(probe, mhz, ours, analyser, channels, directory)
                except Exception as error:  # the clock fails; the sweep goes on from a fresh attach
                    record["clocks"].append({"clock_mhz": mhz, "error": str(error)})
                    console.print(f"{mhz:g} MHz: [red]{error}[/red]")
                    probe.release()
                    probe = throughput.open_probe(args.serial)
                    progress.advance(task)
                    continue
                with open(os.path.join(directory, "log.json"), "w") as f:
                    json.dump(log, f)
                entry = {"clock_mhz": mhz, **analyse_capture(directory, log, args.swdio, args.swclk)}
                record["clocks"].append(entry)
                console.print(line(entry))
                progress.advance(task)
        finally:
            throughput.attach(probe, 1_000_000, ours)
            probe.release()
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(record, f, indent=1)
        f.write("\n")
    if not args.keep:
        shutil.rmtree(keep)
    print(f"wrote {os.path.relpath(path, REPO)}" + (f"; captures kept in {keep}" if args.keep else ""))
    return 0


def line(entry: dict) -> str:
    if "error" in entry:
        return f"{entry['clock_mhz']:g} MHz: {entry['error']}"
    s = entry["summary"]
    parts = [f"{entry['clock_mhz']:>4g} MHz asked:"]
    for d in ("read", "write"):
        if f"{d}_data_ns" in s:
            parts.append(f"{d} {1000 / s[f'{d}_data_ns']:6.2f} MHz ({s[f'{d}_data_from']})")
    if "fastest" in s:
        f = s["fastest"]
        parts.append(f"fastest {1000 / f['period_ns']:6.2f} MHz at {f['role']} {f['position']}")
    parts.append(f"resolved {sum(bool(k['wire'].get('resolved')) for k in entry['kinds'])}/{len(entry['kinds'])}")
    failures = sum(len(k["host"]["failures"]) for k in entry["kinds"])
    parts.append(f"failed commands {failures}")
    return "  ".join(parts)


def cmd_analyse(args) -> int:
    """Analyse kept captures again: print each clock, or with --result rewrite that file's analysis.

    The probe's side of a result, what it was sent and what it answered, stays as measured. The
    file records the analysing code beside the measuring code, by content.
    """
    record = json.load(open(args.result)) if args.result else None
    # By content as this run loaded it, before anything can change the file under it.
    analysing = {p: hashlib.sha256(open(os.path.join(REPO, p), "rb").read()).hexdigest() for p in ("tools/wireclock.py",)}
    for directory in sorted((d for d in os.listdir(args.dir) if d.endswith("MHz")), key=lambda d: float(d[:-3])):
        log = json.load(open(os.path.join(args.dir, directory, "log.json")))
        entry = {"clock_mhz": float(directory[:-3]),
                 **analyse_capture(os.path.join(args.dir, directory), log, args.swdio, args.swclk)}
        print(line(entry))
        if record is not None:
            record["clocks"] = [entry if c["clock_mhz"] == entry["clock_mhz"] else c for c in record["clocks"]]
        if args.verbose:
            print(json.dumps(entry, indent=1)[:20000])
    if record is not None:
        record["provenance"]["analysed"] = {
            "taken": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
            "sources": analysing,
        }
        with open(args.result, "w") as f:
            json.dump(record, f, indent=1)
            f.write("\n")
        print(f"rewrote {os.path.relpath(args.result, REPO)}")
    return 0


def cmd_compare(args) -> int:
    records = [json.load(open(p)) for p in args.files]
    # A retake measures the same labels again, so a label that repeats carries its date.
    labels = [r["label"] for r in records]
    labels = [f"{r['label']} {r['taken'][:10]}" if labels.count(r["label"]) > 1 else r["label"] for r in records]
    clocks = sorted({c["clock_mhz"] for r in records for c in r["clocks"]})
    for title, key in (("read data bits", "read_data_ns"), ("write data bits", "write_data_ns"),
                       ("fastest period anywhere", "fastest"), ("fastest period in a read", "fastest_read")):
        print(f"{title}, MHz on the wire (s: timed from SWDIO, every other figure from SWCLK)\n")
        print("| asked | " + " | ".join(labels) + " |")
        print("| --- " * (1 + len(labels)) + "|")
        for mhz in clocks:
            cells = []
            for r in records:
                c = next((c for c in r["clocks"] if c["clock_mhz"] == mhz), None)
                s = (c or {}).get("summary", {})
                if key.startswith("fastest"):
                    cells.append(f"{1000 / s[key]['period_ns']:.2f}" if key in s else "--")
                elif key in s:
                    cells.append(f"{1000 / s[key]:.2f}" + (" s" if s[key.replace('_ns', '_from')] == "swdio" else ""))
                else:
                    cells.append("--")
            print(f"| {mhz:g} | " + " | ".join(cells) + " |")
        print()
    return 0


def cmd_positions(args) -> int:
    """Every position's period and phases for one clock of one result, by kind and role."""
    record = json.load(open(args.file))
    c = next(c for c in record["clocks"] if c["clock_mhz"] == args.clock)
    for k in c["kinds"]:
        for role, r in k["wire"].get("roles", {}).items():
            print(f"\n{k['name']}, {role}, {r['transfers']} transfers: position period/high/low ns (min period)")
            cells = [f"{j}:{p:.1f}/{h:.1f}/{lo:.1f} ({m:g})" for j, (p, h, lo, m) in enumerate(
                zip(r["period_ns"]["mean"], r["high_ns"]["mean"], r["low_ns"]["mean"], r["period_ns"]["min"]))]
            for i in range(0, len(cells), 5):
                print("  " + "  ".join(cells[i : i + 5]))
        for m in k["wire"].get("misreads", []):
            print(f"\nmisread: {json.dumps({x: y for x, y in m.items() if x not in ('period_ns', 'high_ns', 'low_ns')})}")
    return 0


CPU_HZ = 150_000_000


def model_clock(core1: str, mhz: float) -> dict:
    """What the emulator puts on the wire for each kind at one clock: periods by role, in cycles.

    Its target never waits, so a model has no WAIT transfers to set beside a measured one.
    """
    import cycles  # noqa: PLC0415  -- the emulator, needed by this command alone

    out = {}
    for name, request, _, _ in KINDS:
        ops = [op(r) for r in expected_requests(request)]
        probe = cycles.build(core1, "swd", "read" in name, cycles.Model(engine=True))
        if "DAP_Setup" in probe.sym:
            probe.call("DAP_Setup")
        probe.command(bytes([cycles.ID_CONNECT, 1]))
        probe.command(bytes([cycles.ID_SWJ_CLOCK]) + cycles.u32(int(mhz * 1e6)))
        probe.command(bytes([cycles.ID_TRANSFER_CONFIG, 0]) + cycles.u16(100) + cycles.u16(0))
        probe.command(bytes([cycles.ID_SWD_CONFIGURE, 0x00]))
        probe.begin()
        probe.command(request)
        counts = probe.end()
        rises, falls = np.array(counts.edge_cycles), np.array(counts.fall_cycles)
        if len(rises) != 46 * len(ops):
            out[name] = {"error": f"{len(rises)} rising edges for {len(ops)} transfers"}
            continue
        by_role: dict[str, list] = {}
        for i, role in enumerate(roles(ops)):
            r = rises[46 * i : 46 * i + 46]
            f = falls[np.searchsorted(falls, r[:-1], "right")]
            by_role.setdefault(role, []).append((np.diff(r), f - r[:-1], r[1:] - f))
        out[name] = {role: {part: np.mean([x[k] for x in rows], axis=0) for k, part in enumerate(("period", "high", "low"))}
                     for role, rows in by_role.items()}
    return out


def cmd_model(args) -> int:
    """A result for our firmware against the emulator, every position of every role resolved."""
    record = json.load(open(args.result))
    ns = 1e9 / CPU_HZ
    print(f"{os.path.relpath(args.result, REPO)} against tools/cycles.py on {args.core1}\n")
    print("| asked | positions | worst period difference, ns | model read / write data, MHz | measured |"
          " model fastest | measured fastest |")
    print("| --- | --- | --- | --- | --- | --- | --- |")
    for clock in record["clocks"]:
        if args.clock and clock["clock_mhz"] not in args.clock:
            continue
        model = model_clock(args.core1, clock["clock_mhz"])
        compared, worst = 0, (0.0, "")
        for kind in clock["kinds"]:
            if not kind["wire"].get("resolved"):
                continue
            for role, measured in kind["wire"]["roles"].items():
                predicted = model.get(kind["name"], {}).get(role)
                if predicted is None or "error" in model[kind["name"]]:
                    continue
                diff = np.array(measured["period_ns"]["mean"]) - predicted["period"] * ns
                compared += len(diff)
                j = int(np.abs(diff).argmax())
                if args.clock:
                    print(f"  {kind['name']} {role}: " + " ".join(f"{d:+.1f}" for d in diff))
                if abs(diff[j]) > abs(worst[0]):
                    worst = (float(diff[j]), f"{kind['name']} {role} {j}")
        data = []
        for direction, positions in (("read", READ_DATA), ("write", WRITE_DATA)):
            values = [np.median(p["period"][list(positions)]) for roles_ in model.values() if "error" not in roles_
                      for role, p in roles_.items()
                      if (role.startswith(READ_ROLES) if direction == "read"
                          else role.startswith("ap-write"))]
            data.append(f"{CPU_HZ / np.median(values) / 1e6:.2f}")
        fastest = min(p["period"].min() for roles_ in model.values() if "error" not in roles_ for p in roles_.values())
        s = clock.get("summary", {})
        measured_data = " / ".join(f"{1000 / s[k]:.2f}" + (" s" if s[k.replace('_ns', '_from')] == "swdio" else "")
                                   if k in s else "--" for k in ("read_data_ns", "write_data_ns"))
        measured_fastest = f"{1000 / s['fastest']['period_ns']:.2f}" if "fastest" in s else "--"
        worst_text = f"{worst[0]:+.2f} at {worst[1]}" if compared else "none resolved"
        print(f"| {clock['clock_mhz']:g} | {compared} | {worst_text} | {' / '.join(data)} | {measured_data} |"
              f" {CPU_HZ / fastest / 1e6:.2f} | {measured_fastest} |")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(required=True)
    run = sub.add_parser("run", help="capture every clock and write one result file")
    run.add_argument("--label", required=True, help="names the result: gcc, clang, stock")
    run.add_argument("--elf", "--image", dest="elf",
                     help="flash this image first, ours or an S-record such as stock; leave out to measure what runs")
    run.add_argument("--serial", default="CJ3E7A91C2")
    run.add_argument("--chip", default="MSPM0L1306")
    run.add_argument("--clock", type=float, action="append", help="MHz asked; repeat for several, default the table")
    run.add_argument("--note", default="")
    run.add_argument("--out", help="result path; default bench/wireclock/<date>-<label>.json")
    run.add_argument("--force", action="store_true", help="replace an existing result file")
    run.add_argument("--keep", help="keep each clock's exported capture and probe log in this directory")
    run.add_argument("--analyser", default=ANALYSER, help="Logic 2's automation endpoint")
    run.set_defaults(run=cmd_run)
    analyse = sub.add_parser("analyse", help="analyse captures kept with --keep")
    for p in (run, analyse):
        p.add_argument("--swdio", type=int, default=6, help="the analyser channel on SWDIO")
        p.add_argument("--swclk", type=int, default=7, help="the analyser channel on SWCLK")
    analyse.add_argument("dir")
    analyse.add_argument("--result", help="rewrite this result file's analysis from the captures")
    analyse.add_argument("--verbose", action="store_true")
    analyse.set_defaults(run=cmd_analyse)
    compare = sub.add_parser("compare", help="set result files side by side as markdown tables")
    compare.add_argument("files", nargs="+")
    compare.set_defaults(run=cmd_compare)
    positions = sub.add_parser("positions", help="every position at one clock of one result")
    positions.add_argument("file")
    positions.add_argument("--clock", type=float, required=True)
    positions.set_defaults(run=cmd_positions)
    model = sub.add_parser("model", help="a result for our firmware against the emulator")
    model.add_argument("core1", help="CPU 1's image, built from the same tree as the one measured")
    model.add_argument("result")
    model.add_argument("--clock", type=float, action="append", help="these clocks only, every position shown")
    model.set_defaults(run=cmd_model)
    args = ap.parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
