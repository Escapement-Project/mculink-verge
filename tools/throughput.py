#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["libusb1", "pyusb", "pyserial", "rich"]
# ///
"""SWD throughput of the attached probe, the same cases every time, kept in bench/throughput/.

    uv run tools/throughput.py run --label gcc --image build/mculink_cjtag.elf
    uv run tools/throughput.py run --label stock \
        --image /opt/linkserver/MCU-LINK/probe_firmware/MCU-LINK_CMSIS-DAP_V3_172.s19
    uv run tools/throughput.py run --label now --serial <serial>    # whatever the probe runs
    uv run tools/throughput.py compare bench/throughput/2026-10-20-*.json --clock 50

Two hosts measure each probe. tools/dap.py sends one command over and over and times it: the
commands the emulator models (DAP_TransferBlock and DAP_Transfer on the AP's DRW, reads and writes,
one word to a full packet), the same on DP registers, which no target slows, at depths 1, 2 and 8
and at five clocks. tools/swd-throughput times probe-rs's memory reads and writes end to end.

MATRIX names the case set. A change to the cases changes the name, and compare refuses to set
results from two matrices side by side.

The target is an MSPM0L1306, held out of deep sleep and halted. The AP cases read and write the
first 1 KiB of its SRAM, which the AHB-AP's address wraps within, and the page is put back.
"""

import argparse
import csv
import datetime
import hashlib
import io
import json
import os
import platform
import re
import statistics
import subprocess
import sys
import time

from rich.console import Console
from rich.progress import BarColumn, MofNCompleteColumn, Progress, TextColumn, TimeElapsedColumn

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import coverage  # noqa: E402
import dap  # noqa: E402

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RESULTS = os.path.join(REPO, "bench", "throughput")
SWD_THROUGHPUT = os.path.join(REPO, "tools", "swd-throughput")

MATRIX = "swd-1"
CLOCKS_MHZ = (1, 5, 10, 20, 50)
DEPTHS = (1, 2, 8)
REPEATS = 3
RUN_SECONDS = 0.25
RUN_COMMANDS = (50, 50_000)

# Request byte, and whether the transfer carries a data word. ABORT written with zero sets nothing.
ACCESS = {
    "ap-read": (0x0F, False),
    "ap-write": (0x0D, True),
    "dp-read": (0x02, False),
    "dp-write": (0x00, True),
}
# Up to a full packet each way: a block read's 127 words make a 512-byte reply, a block write's 126
# a 509-byte request, and DAP_Transfer's five bytes a write stop it at 101.
WORDS = {
    ("block", False): (1, 8, 32, 127),
    ("block", True): (1, 8, 32, 126),
    ("transfer", False): (1, 8, 32, 127),
    ("transfer", True): (1, 8, 32, 101),
}

PAGE = 0x2000_0000
PAGE_WORDS = 256
PATTERN = 0xA5A5_5A5A
AP_TAR_WRITE = 0x05
DP_SELECT_WRITE = 0x08
DAP_TRANSFER_CONFIGURE = 0x04
DAP_SWD_CONFIGURE = 0x13
DAP_VENDOR_IDENTIFY = 0x82

le = coverage.le


def cases(quick: bool = False) -> list[dict]:
    """The matrix, or with `quick` a few of its cases for trying the script out."""
    out = []
    for mhz in (10,) if quick else CLOCKS_MHZ:
        for command in ("block", "transfer"):
            for access, (_, write) in ACCESS.items():
                for words in WORDS[(command, write)][1:3] if quick else WORDS[(command, write)]:
                    for depth in (1, 8) if quick else DEPTHS:
                        out.append({"clock_mhz": mhz, "command": command, "access": access,
                                    "words": words, "depth": depth})
    return out


def payload(command: str, access: str, words: int) -> bytes:
    request, write = ACCESS[access]
    data = le(PATTERN if access == "ap-write" else 0) if write else b""
    if command == "block":
        return bytes([dap.DAP_TRANSFER_BLOCK, 0]) + words.to_bytes(2, "little") + bytes([request]) + data * words
    return bytes([dap.DAP_TRANSFER, 0, words]) + (bytes([request]) + data) * words


def reply_words(command: str, reply: bytes, words: int) -> list[int]:
    start = 4 if command == "block" else 3
    return [int.from_bytes(reply[start + 4 * i : start + 4 * i + 4], "little") for i in range(words)]


def open_probe(serial: str) -> dap.Probe:
    for _ in range(50):
        found = dap.find(serial)
        if found:
            found[0].drain()
            return found[0]
        time.sleep(0.2)
    raise SystemExit(f"no probe with serial {serial}")


def identify(probe: dap.Probe) -> dict:
    """What the probe says it is: every DAP_Info field, and our firmware's build if it is ours."""
    info = {}
    for field, (name, kind) in dap.INFO_FIELDS.items():
        try:
            raw = probe.info(field)
        except Exception as error:  # a field the firmware does not answer is a result
            info[name] = f"error: {error}"
            continue
        if kind == "str":
            info[name] = raw.rstrip(b"\x00").decode(errors="replace")
        elif kind in ("byte", "short", "word"):
            info[name] = int.from_bytes(raw, "little") if raw else None
        else:
            info[name] = raw.hex()
    ours = None
    try:
        reply = probe.command(bytes([DAP_VENDOR_IDENTIFY]))
        body = reply[1:]
        if body[:5] == b"CJTAG":
            ours = {"format": body[5], "flags": body[6], "build": body[8 : 8 + body[7]].decode(errors="replace")}
    except RuntimeError:  # stock answers ID_DAP_Invalid, which command() reports as out of step
        pass
    return {"dap_info": info, "ours": ours}


def attach(probe: dap.Probe, hz: int, ours: bool) -> int:
    """Connect at `hz` with a line reset, as a host does when it changes the clock, and power up.

    Our firmware measures the target's read latency on the first transfer after a line reset at a
    clock where its default leaves no margin, so the reset belongs to every clock change.
    """
    if ours:
        probe.command(bytes([dap.DAP_VENDOR_CJTAG_CONTROL, 0]))
    if probe.connect(1) != 1:
        raise RuntimeError("DAP_Connect refused SWD")
    probe.command(bytes([DAP_TRANSFER_CONFIGURE, 0]) + (100).to_bytes(2, "little") + (0).to_bytes(2, "little"))
    probe.command(bytes([DAP_SWD_CONFIGURE, 0x00]))
    probe.swj_clock(hz)
    dpidr = dap.swd_reset(probe)
    probe.command(bytes([dap.DAP_TRANSFER, 0, 2, 0x00]) + le(0x1E) + bytes([0x04]) + le(0x5000_0000))
    coverage.wait_powered(probe, 0)
    if dpidr == coverage.MSPM0_DPIDR:
        probe.command(bytes([dap.DAP_TRANSFER, 0, 3, DP_SELECT_WRITE]) + le(coverage.MSPM0_PWR_AP_SELECT)
                      + bytes([0x01]) + le(coverage.MSPM0_DPREC0_DEBUG) + bytes([DP_SELECT_WRITE]) + le(0))
    else:
        probe.command(bytes([dap.DAP_TRANSFER, 0, 1, DP_SELECT_WRITE]) + le(0))
    return dpidr


def set_tar(probe: dap.Probe, address: int) -> None:
    reply = probe.command(bytes([dap.DAP_TRANSFER, 0, 1, AP_TAR_WRITE]) + le(address))
    if reply[1] != 1 or reply[2] != 1:
        raise RuntimeError(f"TAR write answered count {reply[1]} ack {reply[2]}")


def page_io(probe: dap.Probe, write: list[int] | None = None) -> list[int]:
    """Read the whole SRAM page, or write `write` over it, in block commands from its start."""
    set_tar(probe, PAGE)
    words = []
    left = PAGE_WORDS
    while left:
        n = min(left, 126 if write is not None else 127)
        if write is None:
            reply = probe.command(bytes([dap.DAP_TRANSFER_BLOCK, 0]) + n.to_bytes(2, "little") + bytes([0x0F]))
            words += reply_words("block", reply, n)
        else:
            chunk = write[PAGE_WORDS - left : PAGE_WORDS - left + n]
            reply = probe.command(bytes([dap.DAP_TRANSFER_BLOCK, 0]) + n.to_bytes(2, "little") + bytes([0x0D])
                                  + b"".join(le(w) for w in chunk))
        if int.from_bytes(reply[1:3], "little") != n or reply[3] != 1:
            raise RuntimeError(f"page block answered {reply[:4].hex()}")
        left -= n
    return words


def check_case(probe: dap.Probe, case: dict, request: bytes, dpidr: int, page: list[int]) -> None:
    """One command of the case, with its data checked, before any of it is timed."""
    command, access, words = case["command"], case["access"], case["words"]
    if access.startswith("ap"):
        set_tar(probe, PAGE)
    reply = probe.command(request)
    count, ack = dap.transfer_counts(request, reply)
    if count != words or ack != 1:
        raise RuntimeError(f"answered {count} of {words} words, ack {ack}")
    if access == "ap-read" and reply_words(command, reply, words) != page[:words]:
        raise RuntimeError("SRAM read back different words")
    if access == "dp-read" and set(reply_words(command, reply, words)) != {dpidr}:
        raise RuntimeError("DPIDR read back different words")
    if access == "ap-write":
        set_tar(probe, PAGE)
        back = probe.command(bytes([dap.DAP_TRANSFER_BLOCK, 0]) + words.to_bytes(2, "little") + bytes([0x0F]))
        if reply_words("block", back, words) != [PATTERN] * words:
            raise RuntimeError("SRAM did not hold the words written")


def timed(probe: dap.Probe, serial: str, case: dict, request: bytes, commands: int) -> tuple[dap.Run, dap.Probe]:
    """One timed run. A queued run releases the probe, so the one to go on with comes back too."""
    if case["access"].startswith("ap"):
        set_tar(probe, PAGE)
    if case["depth"] > 2:
        run = dap.queued_blocks(probe, request, commands, case["depth"], case["words"])
        return run, open_probe(serial)
    return dap.pipelined_blocks(probe, request, commands, case["depth"], case["words"]), probe


def custom_host(probe: dap.Probe, serial: str, ours: bool, quick: bool, progress: Progress,
                console: Console) -> tuple[list, dict]:
    todo = cases(quick)
    overall = progress.add_task("dap.py cases", total=len(todo))
    repeats = progress.add_task("case", total=REPEATS)
    limit = min(probe.out_ep.wMaxPacketSize, probe.in_ep.wMaxPacketSize)
    dpidr = attach(probe, 1_000_000, ours)
    coverage.power_up_and_halt(probe, 0)
    page = page_io(probe)
    target = {"dpidr": f"0x{dpidr:08x}"}
    results = []
    clock = None
    try:
        for case in todo:
            if case["clock_mhz"] != clock:
                clock = case["clock_mhz"]
                attach(probe, clock * 1_000_000, ours)
            request = payload(case["command"], case["access"], case["words"])
            label = f"{case['clock_mhz']:>2} MHz {case['command']:<8} {case['access']:<8} {case['words']:>3} words depth {case['depth']}"
            progress.update(repeats, completed=0, description=label)
            entry = dict(case, runs=[], error=None)
            reply_bytes = (4 if case["command"] == "block" else 3) + (0 if ACCESS[case["access"]][1] else 4 * case["words"])
            if len(request) > limit or reply_bytes > limit:
                entry["error"] = f"does not fit a {limit}-byte packet"
            else:
                try:
                    check_case(probe, case, request, dpidr, page)
                    calibration, probe = timed(probe, serial, case, request, 20)
                    if calibration.error:
                        raise RuntimeError(calibration.error)
                    per = calibration.seconds / calibration.done
                    commands = max(RUN_COMMANDS[0], min(RUN_COMMANDS[1], int(RUN_SECONDS / per)))
                    entry["commands_per_run"] = commands
                    for _ in range(REPEATS):
                        run, probe = timed(probe, serial, case, request, commands)
                        entry["runs"].append(vars(run))
                        progress.advance(repeats)
                        if run.error:
                            raise RuntimeError(run.error)
                except Exception as error:  # the case fails; the matrix goes on from a fresh attach
                    entry["error"] = str(error)
                    probe.release()
                    probe = open_probe(serial)
                    attach(probe, clock * 1_000_000, ours)
                if case["access"] == "ap-write":
                    page_io(probe, page)  # the reads that follow check against the page as it was
            if entry["runs"] and entry["error"] is None:
                rates = [r["done"] / r["seconds"] for r in entry["runs"]]
                entry["commands_per_s"] = statistics.median(rates)
                entry["kib_per_s"] = entry["commands_per_s"] * case["words"] * 4 / 1024
                console.print(f"{label}: {entry['commands_per_s']:>8.0f} commands/s {entry['kib_per_s']:>8.1f} KiB/s")
            else:
                console.print(f"{label}: [red]{entry['error']}[/red]")
            results.append(entry)
            progress.advance(overall)
    finally:
        attach(probe, 1_000_000, ours)
        page_io(probe, page)
        probe.release()
    progress.remove_task(repeats)
    return results, target


def probe_rs(serial: str, chip: str, quick: bool, console: Console) -> dict:
    build = subprocess.run(["cargo", "build", "--release", "--quiet", "--manifest-path",
                            os.path.join(SWD_THROUGHPUT, "Cargo.toml")])
    if build.returncode != 0:
        raise SystemExit("tools/swd-throughput did not build")
    csv_path = os.path.join(SWD_THROUGHPUT, "target", "last-sweep.csv")
    binary = os.path.join(SWD_THROUGHPUT, "target", "release", "swd-throughput")
    sweep = ["--speeds", "10000", "--sizes", "64,1024", "--repeats", "3"] if quick else []
    run = subprocess.run([binary, "--probe", f"1fc9:0143:{serial}", "--chip", chip, "--csv", csv_path, *sweep],
                         stdout=subprocess.PIPE, text=True)
    console.print(run.stdout, markup=False, highlight=False)
    rows = []
    if run.returncode == 0:
        for row in csv.DictReader(io.StringIO(open(csv_path).read())):
            rows.append({k: (v if k == "arm" else float(v)) for k, v in row.items()})
    return {"exit": run.returncode, "report": run.stdout, "points": rows}


def provenance(elf: str | None) -> dict:
    def out(*args: str) -> str:
        return subprocess.run(args, cwd=REPO, capture_output=True, text=True).stdout.strip()

    manifest = open(os.path.join(SWD_THROUGHPUT, "Cargo.toml")).read()
    # The code that took the measurement, by content, so a result names its source even when the
    # tree it ran from was not committed.
    measuring = ["tools/throughput.py", "tools/dap.py", "tools/coverage.py", "tools/flash.py",
                 "tools/swd-throughput/Cargo.toml", "tools/swd-throughput/Cargo.lock",
                 "tools/swd-throughput/src/main.rs"]
    record = {
        "repo": out("git", "describe", "--always", "--dirty", "--abbrev=12"),
        "sources": {p: hashlib.sha256(open(os.path.join(REPO, p), "rb").read()).hexdigest() for p in measuring},
        "probe_rs": re.search(r'probe-rs = \{ git = "([^"]+)", rev = "([0-9a-f]+)"', manifest).groups(),
        "host": {"platform": platform.platform(), "python": platform.python_version()},
    }
    try:
        import usb1

        version = usb1.getVersion()
        record["host"]["libusb"] = f"{version.major}.{version.minor}.{version.micro}"
    except Exception:  # the version is a note, not a requirement
        pass
    if elf:
        record["image"] = {
            "path": os.path.relpath(os.path.abspath(elf), REPO),
            "sha256": hashlib.sha256(open(elf, "rb").read()).hexdigest(),
        }
        if not elf.endswith(".s19"):
            record["image"]["compilers"] = sorted({line.split("]", 1)[1].strip() for line in out(
                "arm-none-eabi-readelf", "-p", ".comment", elf).splitlines() if "]" in line})
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
        # Stock and ours give the same unit different serials: follow it when it is the only one.
        attached = [p.serial for p in dap.find()]
        if args.serial not in attached and len(attached) == 1:
            args.serial = attached[0]
    probe = open_probe(args.serial)
    probe.wire = "swd"
    firmware = identify(probe)
    console = Console(stderr=True)
    record = {
        "matrix": f"{MATRIX}-quick" if args.quick else MATRIX, "label": args.label, "note": args.note,
        "taken": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "serial": args.serial, "chip": args.chip, "firmware": firmware, "provenance": provenance(args.elf),
        "settings": {"clocks_mhz": CLOCKS_MHZ, "depths": DEPTHS, "repeats": REPEATS, "run_seconds": RUN_SECONDS},
    }
    with Progress(TextColumn("{task.description}"), BarColumn(), MofNCompleteColumn(), TimeElapsedColumn(),
                  console=console) as progress:
        record["custom_host"], record["target"] = custom_host(probe, args.serial, firmware["ours"] is not None,
                                                              args.quick, progress, console)
    record["probe_rs"] = None if args.no_probe_rs else probe_rs(args.serial, args.chip, args.quick, console)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        json.dump(record, f, indent=1)
        f.write("\n")
    failed = sum(1 for c in record["custom_host"] if c["error"])
    probe_rs_exit = "skipped" if record["probe_rs"] is None else record["probe_rs"]["exit"]
    print(f"wrote {os.path.relpath(path, REPO)}: {len(record['custom_host']) - failed} cases measured, {failed} failed,"
          f" probe-rs {probe_rs_exit}")
    return 0


def cmd_compare(args) -> int:
    records = [json.load(open(p)) for p in args.files]
    matrices = {r["matrix"] for r in records}
    if len(matrices) != 1:
        raise SystemExit(f"results from different matrices: {sorted(matrices)}")
    labels = [r["label"] for r in records]
    metric = "kib_per_s" if args.metric == "kib" else "commands_per_s"
    unit = "KiB/s" if args.metric == "kib" else "commands/s"

    def keep(mhz: float, depth: int | None = None) -> bool:
        return (not args.clock or mhz in args.clock) and (depth is None or not args.depth or depth in args.depth)

    if args.host in ("all", "dap"):
        table = {}
        for i, r in enumerate(records):
            for c in r["custom_host"]:
                if keep(c["clock_mhz"], c["depth"]):
                    key = (c["command"], c["access"], c["depth"], c["clock_mhz"], c["words"])
                    table.setdefault(key, [None] * len(records))[i] = None if c["error"] else c[metric]
        print(f"tools/dap.py, {unit}\n")
        print("| command | access | depth | MHz | words | " + " | ".join(labels) + " |")
        print("| --- " * (5 + len(labels)) + "|")
        for key in sorted(table):
            cells = ["--" if v is None else f"{v:.0f}" if v >= 100 else f"{v:.1f}" for v in table[key]]
            print("| " + " | ".join(str(k) for k in key) + " | " + " | ".join(cells) + " |")
        print()
    if args.host in ("all", "probe-rs"):
        table = {}
        for i, r in enumerate(records):
            for p in (r["probe_rs"] or {"points": []})["points"]:
                mhz = p["speed_khz"] / 1000
                if keep(mhz) and p["arm"] != "failed":  # a failed clock shows as missing
                    key = (p["arm"], int(p["bytes"]), mhz)
                    value = p["median_us"] if p["arm"] == "rt" else p["bytes_per_second"] / 1024
                    table.setdefault(key, [None] * len(records))[i] = value
        print("probe-rs through tools/swd-throughput, KiB/s (rt: us a single-word read)\n")
        print("| arm | bytes | MHz | " + " | ".join(labels) + " |")
        print("| --- " * (3 + len(labels)) + "|")
        for key in sorted(table):
            cells = ["--" if v is None else f"{v:.1f}" for v in table[key]]
            print(f"| {key[0]} | {key[1]} | {key[2]:g} | " + " | ".join(cells) + " |")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(required=True)
    run = sub.add_parser("run", help="measure the attached probe and write one result file")
    run.add_argument("--label", required=True, help="names the result: gcc, clang, stock")
    run.add_argument("--elf", "--image", dest="elf",
                     help="flash this image first, ours or an S-record such as stock; leave out to measure what runs")
    run.add_argument("--serial", default="CJ3E7A91C2")
    run.add_argument("--chip", default="MSPM0L1306")
    run.add_argument("--note", default="", help="free text kept with the result, e.g. a stock image's name")
    run.add_argument("--out", help="result path; default bench/throughput/<date>-<label>.json")
    run.add_argument("--force", action="store_true", help="replace an existing result file")
    run.add_argument("--quick", action="store_true", help="a few cases, to try the script; not comparable")
    run.add_argument("--no-probe-rs", action="store_true", help="leave out the probe-rs half")
    run.set_defaults(run=cmd_run)
    compare = sub.add_parser("compare", help="set result files side by side as markdown tables")
    compare.add_argument("files", nargs="+")
    compare.add_argument("--host", choices=("all", "dap", "probe-rs"), default="all")
    compare.add_argument("--metric", choices=("kib", "commands"), default="kib")
    compare.add_argument("--clock", type=float, action="append", help="MHz asked; repeat for several")
    compare.add_argument("--depth", type=int, action="append")
    compare.set_defaults(run=cmd_compare)
    args = ap.parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
