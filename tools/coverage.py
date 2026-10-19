#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["libusb1", "pyusb", "pyserial"]
# ///
"""Exercise the DAP commands a throughput run never sends, on a real target.

Each run attaches by hand (nRESET, then the wire's own bring-up), halts the core, and checks what
comes back against what was sent. Targets: a CC13xx over `jtag` or `cjtag`, an MSPM0L1306 over
`swd`.

    uv run tools/coverage.py exec --wire jtag      DAP_ExecuteCommands, block data at every offset
    uv run tools/coverage.py abort --wire cjtag    DAP_TransferAbort cuts a running transfer short
    uv run tools/coverage.py hid --wire swd        the same packets through the HID interface
    uv run tools/coverage.py pipeline              HID reports queued up to the advertised count
    uv run tools/coverage.py rate --wire jtag      block reads at a fixed number of commands queued
    uv run tools/coverage.py mark --wire jtag      VTOR to 0x2000, for telling a reset happened
    uv run tools/coverage.py console               the CDC console, with J7's TX and RX jumpered
"""

import argparse
import os
import select
import sys
import threading
import time

import serial
from serial.tools import list_ports

import dap

CSW = 0x2300_0052
DHCSR = 0xE000_EDF0
VTOR = 0xE000_ED08
ICEPICK_IDCODE = 0x3BB4_102F
MSPM0_DPIDR = 0x6BA0_2477
DAP_TRANSFER_ABORT = 0x07
DAP_TRANSFER_CONFIGURE = 0x04
DAP_VENDOR_JTAG_GEN = 0x8A
ACK_OK_MISMATCH = 0x11

# Where each target's SRAM test goes: base, stride per packet, block words over bulk.
LAYOUT = {
    "jtag": (0x2000_2000, 0x400, 116),
    "cjtag": (0x2000_2000, 0x400, 116),
    "swd": (0x2000_0000, 0x100, 64),  # an MSPM0L1306 has 4 KiB
}
ROM = {"jtag": 0x1000_0000, "cjtag": 0x1000_0000, "swd": 0x0000_0000}


def le(v: int) -> bytes:
    return v.to_bytes(4, "little")


def connect(probe: dap.Probe, wire: str) -> int:
    """Attach over `wire`, power the debug domain up and halt the core. Returns the DAP's index."""
    probe.wire = wire
    return attach_swd(probe) if wire == "swd" else attach_icepick(probe, wire == "cjtag")


def power_up_and_halt(probe: dap.Probe, index: int) -> None:
    for _ in range(100):
        reply = probe.command(bytes([dap.DAP_TRANSFER, index, 1, 0x06]))
        if int.from_bytes(reply[3:7], "little") & 0xA000_0000 == 0xA000_0000:
            break
    else:
        raise RuntimeError("the DAP did not power up")
    steps = bytes([0x08]) + le(0) + bytes([0x01]) + le(CSW)
    steps += bytes([0x05]) + le(DHCSR) + bytes([0x0D]) + le(0xA05F_0003) + bytes([0x05]) + le(DHCSR) + bytes([0x0F])
    reply = probe.command(bytes([dap.DAP_TRANSFER, index, 6]) + steps)
    dhcsr = int.from_bytes(reply[3:7], "little")
    if reply[1] != 6 or reply[2] != 1 or not dhcsr & (1 << 17):
        raise RuntimeError(f"halt: count {reply[1]} ack {reply[2]} DHCSR 0x{dhcsr:08x}")


def attach_swd(probe: dap.Probe) -> int:
    probe.command(bytes([dap.DAP_VENDOR_CJTAG_CONTROL, 0]))
    if probe.connect(1) != 1:
        raise RuntimeError("DAP_Connect refused SWD")
    probe.swj_clock(1_000_000)
    dpidr = dap.swd_reset(probe)
    if dpidr != MSPM0_DPIDR:
        raise RuntimeError(f"DPIDR 0x{dpidr:08x}, not an MSPM0")
    probe.command(bytes([dap.DAP_TRANSFER, 0, 2, 0x00]) + le(0x1E) + bytes([0x04]) + le(0x5000_0000))
    power_up_and_halt(probe, 0)
    return 0


def attach_icepick(probe: dap.Probe, two_pin: bool) -> int:
    """nRESET, the wire's bring-up as probe-rs does it, then the ICEPick routed to the Cortex-M DAP."""
    probe.command(bytes([dap.DAP_VENDOR_CJTAG_CONTROL, 0]))
    probe.connect(2)
    probe.swj_clock(1_000_000)
    probe.command(bytes([dap.DAP_SWJ_PINS, 0x00, 0x80]) + bytes(4))
    time.sleep(0.02)
    probe.command(bytes([dap.DAP_SWJ_PINS, 0x80, 0x80]) + bytes(4))
    time.sleep(0.05)
    probe.command(bytes([dap.DAP_JTAG_CONFIGURE, 1, 6]))
    if two_pin:
        # probe-rs's activate_oscan1: a reset escape and wake clocks, then TI's steps after their
        # Test-Logic-Reset, which would drop OScan1 again. Vendor 0x83 01 does not bring a CC13xx up.
        wake = [("escape", 10), ("raw", "1" * 20), ("raw", "0"), ("pause", 200)]
        dap.run_ti_activation(probe, wake + dap.TI_OSCAN1[1:])
    else:
        # The part leaves reset in cJTAG mode. probe-rs's ctag_to_jtag switches it to four pins:
        # two zero-bit scans, then DR scans of 1, 2 and 9 bits.
        probe.jtag_sequence([(51, 1, False, (1 << 51) - 1), (1, 0, False, 1)])
        walk = dap.TapWalk().scan(True, 0x3F, 6).idle(1)
        walk._clock("1010110")
        walk._clock("1010110")
        walk.scan(False, 1, 1).scan(False, 3, 2).scan(False, 0x1FF, 9).scan(True, 0x3F, 6).scan(True, 0x04, 6).idle(1)
        dap.run_walk(probe, walk)
    idcode = dap.run_walk(probe, dap.TapWalk().scan(True, 0x04, 6).scan(False, 0, 32).idle(1))[-1]
    if idcode != ICEPICK_IDCODE:
        raise RuntimeError(f"ICEPick IDCODE 0x{idcode:08x}")
    # The routing is icepick_read_cpuid's in dap.py.
    walk = dap.TapWalk().scan(True, 0x07, 6).scan(False, 0x89, 8).idle(1)
    walk.scan(True, 0x02, 6).scan(False, 0x8100_0080, 32).scan(True, 0x02, 6).scan(False, 0, 32).idle(1)
    dap.run_walk(probe, walk)
    sd_tap0 = (1 << 31) | (0x20 << 24) | (1 << 20) | (1 << 8) | (1 << 6) | (1 << 3)
    walk = dap.TapWalk().scan(True, 0x02, 6).scan(False, sd_tap0, 32, via_idle=False)
    walk.scan(True, 0x3F, 6, via_idle=False).idle(10)
    dap.run_walk(probe, walk)
    pattern = 0x0123456789ABCDEF_FEDCBA98
    for _ in range(200):
        if dap.run_walk(probe, dap.TapWalk().scan(False, pattern, 96).idle(1))[0] != (1 << 96) - 1:
            break
        time.sleep(0.005)
    for order, index in (((4, 6), 0), ((6, 4), 1)):
        probe.command(bytes([dap.DAP_JTAG_CONFIGURE, 2, *order]))
        reply = probe.command(bytes([dap.DAP_TRANSFER, index, 2, 0x04]) + le(0x5000_0000) + bytes([0x06]))
        if reply[1] == 2 and reply[2] == 1:
            break
    else:
        raise RuntimeError("the DAP did not answer in either chain order")
    power_up_and_halt(probe, index)
    return index


def finish(probe: dap.Probe) -> None:
    """End as probe-rs's power_down does: framed TMS-high clocks to Test-Logic-Reset, which takes a
    two-pin target out of OScan1, then framing off. A later four-wire attach needs that."""
    if probe.wire != "swd":
        probe.jtag_sequence([(8, 1, False, 0xFF)])
    probe.command(bytes([dap.DAP_VENDOR_CJTAG_CONTROL, 0]))


def exec_offsets(probe: dap.Probe, index: int, words: int, limit: int, base: int, stride: int) -> int:
    """One DAP_ExecuteCommands per (HostStatus count, DAP_Info count): a TAR write, a block write,
    a TAR write and a block read back. HostStatus moves the request by three bytes and the reply by
    two, DAP_Info the request by two and the reply by three, so the sixteen packets put the block
    data at every offset modulo four on both sides. Returns the failing packets."""
    failures = 0
    for k in range(4):
        for m in range(4):
            address = base + stride * (4 * k + m)
            data = os.urandom(4 * words)
            pkt = bytearray([dap.DAP_EXECUTE_COMMANDS, k + m + 4])
            pkt += bytes([dap.DAP_HOST_STATUS, 0, 0]) * k + bytes([dap.DAP_INFO, 0xFE]) * m
            pkt += bytes([dap.DAP_TRANSFER, index, 1, 0x05]) + le(address)
            request_at = len(pkt) + 5
            pkt += bytes([dap.DAP_TRANSFER_BLOCK, index, words & 0xFF, words >> 8, 0x0D]) + data
            pkt += bytes([dap.DAP_TRANSFER, index, 1, 0x05]) + le(address)
            pkt += bytes([dap.DAP_TRANSFER_BLOCK, index, words & 0xFF, words >> 8, 0x0F])
            assert len(pkt) <= limit, len(pkt)
            reply = probe.command(bytes(pkt))
            at, problems = 2, []
            if reply[1] != k + m + 4:
                problems.append(f"count {reply[1]}")
            for _ in range(k):
                if reply[at : at + 2] != bytes([dap.DAP_HOST_STATUS, 0]):
                    problems.append(f"HostStatus {reply[at:at + 2].hex()}")
                at += 2
            for _ in range(m):
                if reply[at : at + 2] != bytes([dap.DAP_INFO, 1]):
                    problems.append(f"Info {reply[at:at + 3].hex()}")
                at += 3
            block = bytes([dap.DAP_TRANSFER_BLOCK, words & 0xFF, words >> 8, 1])
            for part in (bytes([dap.DAP_TRANSFER, 1, 1]), block, bytes([dap.DAP_TRANSFER, 1, 1]), block):
                if reply[at : at + len(part)] != part:
                    problems.append(f"expected {part.hex()} got {reply[at:at + len(part)].hex()}")
                at += len(part)
            reply_at = at
            if reply[at : at + 4 * words] != data:
                problems.append("read-back differs inside the batch")
            separate, ack = dap.block_read(probe, index, address, words)
            if separate != data or ack != 1:
                problems.append(f"separate read differs (ack {ack})")
            failures += bool(problems)
            print(f"    HostStatus x{k} Info x{m}: request data at {request_at} (mod 4 = {request_at % 4}), "
                  f"reply data at {reply_at} (mod 4 = {reply_at % 4}): {'; '.join(problems) or 'ok'}")
    return failures


def transfer_configure(probe: dap.Probe, wait_retry: int, match_retry: int) -> None:
    request = bytes([DAP_TRANSFER_CONFIGURE, 0]) + wait_retry.to_bytes(2, "little") + match_retry.to_bytes(2, "little")
    if probe.command(request)[1] != 0:
        raise RuntimeError("DAP_TransferConfigure refused")


def match_read(index: int) -> bytes:
    """Read DP CTRL/STAT until it equals zero, which it never will with the debug domain up."""
    return bytes([dap.DAP_TRANSFER, index, 2, 0x20]) + le(0xFFFF_FFFF) + bytes([0x16]) + le(0)


def cmd_exec(args) -> int:
    probe = dap.one(args)
    failures = 0
    for hz in args.hz:
        index = connect(probe, args.wire)
        probe.swj_clock(hz)
        print(f"{args.wire} at {hz / 1e6:g} MHz, DAP at chain index {index}:")
        base, stride, words = LAYOUT[args.wire]
        failures += exec_offsets(probe, index, args.words or words, 512, base, stride)
        if args.wire == "swd":
            continue
        # The odd offsets matter in the generated transfers, so say whether they ran.
        gen = probe.command(bytes([DAP_VENDOR_JTAG_GEN, 0, 0, 0]))
        built = int.from_bytes(gen[6:10], "little")
        print(f"    generator: ok {gen[5] & 1}, transfers {gen[5] >> 1 & 1}, looped {gen[5] >> 2 & 1}, "
              f"built for {built / 1e6:g} MHz")
        failures += gen[5] & 3 != 3
    print(f"{failures} failing packets; CPU 1: {probe.core1_status()}")
    finish(probe)
    probe.release()
    return failures != 0


def cmd_abort(args) -> int:
    """DAP_TransferAbort reaches CPU 1 through the flag pointer it publishes to CPU 0, on either
    transport, and gets no reply of its own."""
    probe = dap.one(args)
    index = connect(probe, args.wire)
    probe.swj_clock(args.hz)
    transfer_configure(probe, 100, 0xFFFF)
    if args.hid:
        hid = HidProbe(probe.serial)
        send, receive = hid.send, lambda timeout: hid.receive(timeout)
    else:
        def send(request: bytes) -> None:
            probe.out_ep.write(request, 1000)

        def receive(timeout: float) -> bytes | None:
            try:
                return bytes(probe.in_ep.read(probe.in_ep.wMaxPacketSize, int(timeout * 1000)))
            except dap.usb.core.USBTimeoutError:
                return None

    def timed(started: float) -> tuple[bytes, float]:
        reply = receive(30)
        if reply is None:
            raise TimeoutError("no reply within 30 s")
        return reply, time.perf_counter() - started

    cut_short = bytes([dap.DAP_TRANSFER, 1, ACK_OK_MISMATCH])
    print(f"  over {'HID' if args.hid else 'bulk'}:")

    started = time.perf_counter()
    send(match_read(index))
    reply, full = timed(started)
    print(f"  match read, not aborted: {full:.3f} s, reply {reply[:3].hex()}")

    started = time.perf_counter()
    send(match_read(index))
    time.sleep(args.after)
    send(bytes([DAP_TRANSFER_ABORT]))
    reply, aborted = timed(started)
    print(f"  aborted {args.after * 1e3:.0f} ms in: {aborted:.3f} s, reply {reply[:3].hex()}")
    ok = reply[:3] == cut_short and aborted < full / 2

    send(bytes([DAP_TRANSFER_ABORT]))
    stray = 0
    while receive(0.2) is not None:
        stray += 1
    send(bytes([dap.DAP_INFO, 0x01]))
    vendor = receive(2)
    print(f"  abort with nothing running: {stray} stray replies, then DAP_Info answers {vendor[:2].hex() if vendor else None}")
    ok &= stray == 0 and vendor is not None and vendor[0] == dap.DAP_INFO

    # The flag is cleared when a transfer starts, so an abort reaches only the one running.
    started = time.perf_counter()
    send(match_read(index))
    send(match_read(index))
    time.sleep(args.after)
    send(bytes([DAP_TRANSFER_ABORT]))
    first, t1 = timed(started)
    second, t2 = timed(started)
    print(f"  two queued, abort {args.after * 1e3:.0f} ms in: first {t1:.3f} s {first[:3].hex()}, "
          f"second {t2:.3f} s {second[:3].hex()}")

    if args.hid:
        hid.release()
    transfer_configure(probe, 100, 0)
    reply = probe.command(bytes([dap.DAP_TRANSFER, index, 1, 0x06]))
    print(f"  CTRL/STAT afterwards: count {reply[1]} ack {reply[2]} 0x{int.from_bytes(reply[3:7], 'little'):08x}")
    ok &= reply[1] == 1 and reply[2] == 1
    print(f"  CPU 1: {probe.core1_status()}")
    finish(probe)
    probe.release()
    print("ok" if ok else "FAILED")
    return not ok


def cmd_mark(args) -> int:
    """Leave the core halted with VTOR at 0x2000. A reset puts it back; an attach does not."""
    probe = dap.one(args)
    index = connect(probe, args.wire)
    write = bytes([0x05]) + le(VTOR) + bytes([0x0D]) + le(0x2000)
    read = bytes([0x05]) + le(VTOR) + bytes([0x0F])
    reply = probe.command(bytes([dap.DAP_TRANSFER, index, 4]) + write + read)
    vtor = int.from_bytes(reply[3:7], "little")
    print(f"  VTOR 0x{vtor:08x} (count {reply[1]} ack {reply[2]})")
    finish(probe)
    probe.release()
    return reply[1] != 4 or vtor != 0x2000


def cmd_rate(args) -> int:
    """Block reads of DP CTRL/STAT with exactly `depth` commands outstanding: what the probe does
    with a host that queues, without the host's own gaps in the figure."""
    probe = dap.one(args)
    index = connect(probe, args.wire)
    probe.swj_clock(args.hz)
    words = args.words
    request = bytes([dap.DAP_TRANSFER_BLOCK, index, words & 0xFF, words >> 8, 0x06])
    started = time.perf_counter()
    for _ in range(args.rtt):
        probe.command(bytes([dap.DAP_INFO, 0xFE]))
    rtt = (time.perf_counter() - started) / args.rtt
    print(f"  {args.wire} at {args.hz / 1e6:g} MHz: DAP_Info round trip {rtt * 1e6:.1f} us")
    bad = 0
    for depth in args.depth:
        sent = received = 0
        started = time.perf_counter()
        while received < args.blocks:
            while sent < args.blocks and sent - received < depth:
                probe.out_ep.write(request, 1000)
                sent += 1
            reply = bytes(probe.in_ep.read(probe.in_ep.wMaxPacketSize, 2000))
            received += 1
            bad += reply[:4] != bytes([dap.DAP_TRANSFER_BLOCK, words & 0xFF, words >> 8, 1])
        elapsed = time.perf_counter() - started
        print(f"    depth {depth}: {args.blocks * words * 4 / elapsed / 1024:.0f} KiB/s, "
              f"{elapsed / args.blocks * 1e6:.1f} us a block")
    print(f"  {bad} bad replies; CPU 1: {probe.core1_status() if args.core1 else 'not asked'}")
    finish(probe)
    probe.release()
    return bad != 0


class HidProbe(dap.Probe):
    """The same probe through its CMSIS-DAP v1 interface, one 64-byte report per command."""

    def __init__(self, serial: str):
        for name in sorted(os.listdir("/sys/class/hidraw")):
            with open(f"/sys/class/hidraw/{name}/device/uevent") as uevent:
                text = uevent.read()
            if "00001FC9:00000143" in text and f"HID_UNIQ={serial}" in text:
                self.path = f"/dev/{name}"
                self.fd = os.open(self.path, os.O_RDWR)
                break
        else:
            raise RuntimeError(f"no hidraw node for {serial}")
        self.serial = serial

    def send(self, request: bytes) -> None:
        assert len(request) <= 64, len(request)
        os.write(self.fd, b"\0" + request.ljust(64, b"\0"))

    def receive(self, timeout: float) -> bytes | None:
        if not select.select([self.fd], [], [], timeout)[0]:
            return None
        return os.read(self.fd, 64)

    def command(self, request: bytes, timeout: int = 2000) -> bytes:
        self.send(request)
        reply = self.receive(timeout / 1000)
        if reply is None:
            raise TimeoutError(f"no HID reply to 0x{request[0]:02x}")
        if reply[0] != request[0]:
            raise RuntimeError(f"asked for 0x{request[0]:02x}, got 0x{reply[0]:02x}")
        return reply

    def release(self) -> None:
        os.close(self.fd)


def cmd_hid(args) -> int:
    bulk = dap.one(args)
    hid = HidProbe(bulk.serial)
    print(f"  HID node {hid.path}")
    size = int.from_bytes(hid.command(bytes([dap.DAP_INFO, 0xFF]))[2:4], "little")
    count = hid.command(bytes([dap.DAP_INFO, 0xFE]))[2]
    print(f"  HID DAP_Info: packet size {size}, packet count {count}")
    ok = size == 64
    index = connect(bulk, args.wire)
    base, stride, _ = LAYOUT[args.wire]
    failures = 0
    for hz in args.hz:
        hid.swj_clock(hz)
        print(f"  {args.wire} at {hz / 1e6:g} MHz over HID:")
        failures += exec_offsets(hid, index, 5, 64, base, stride)
        data, ack = dap.block_read(hid, index, ROM[args.wire], 14)
        reference, ack_bulk = dap.block_read(bulk, index, ROM[args.wire], 14)
        print(f"    14-word read: HID ack {ack}, bulk ack {ack_bulk}, {'same' if data == reference else 'DIFFERENT'}")
        failures += data != reference or ack != 1
    # Both transports in flight at once, asking the one question whose answer differs between
    # them: each reply has to come back on the transport that asked.
    wrong = 0
    for _ in range(args.mixed):
        bulk.out_ep.write(bytes([dap.DAP_INFO, 0xFF]), 1000)
        hid.send(bytes([dap.DAP_INFO, 0xFF]))
        b = bytes(bulk.in_ep.read(bulk.in_ep.wMaxPacketSize, 2000))
        h = hid.receive(2)
        if h is None:
            raise TimeoutError("no HID reply with bulk in flight")
        wrong += int.from_bytes(b[2:4], "little") != 512 or int.from_bytes(h[2:4], "little") != 64
    print(f"  {args.mixed} bulk + HID pairs in flight together: {wrong} answered on the wrong transport")
    print(f"  CPU 1: {bulk.core1_status()}")
    finish(bulk)
    hid.release()
    bulk.release()
    ok &= failures == 0 and wrong == 0
    print("ok" if ok else "FAILED")
    return not ok


def cmd_pipeline(args) -> int:
    """Queue HID reports up to the packet count DAP_Info advertises, and count the replies."""
    bulk = dap.one(args)
    hid = HidProbe(bulk.serial)
    bulk.release()
    advertised = hid.command(bytes([dap.DAP_INFO, 0xFE]))[2]
    lost = 0
    for depth in range(1, advertised + 1):
        # DAP_Info returns at once; a DAP_Delay holds CPU 1 while the next reports arrive.
        for name, request in (("DAP_Info", bytes([dap.DAP_INFO, 0xFE])),
                              (f"DAP_Delay {args.delay} us", bytes([dap.DAP_DELAY]) + args.delay.to_bytes(2, "little"))):
            got = 0
            for _ in range(args.trials):
                for _ in range(depth):
                    hid.send(request)
                while hid.receive(0.5) is not None:
                    got += 1
            lost += args.trials * depth - got
            print(f"  depth {depth}, {name}: {got}/{args.trials * depth} replies")
    hid.release()
    print("ok" if lost == 0 else f"FAILED: {lost} replies lost at or below the advertised count {advertised}")
    return lost != 0


def cmd_console(args) -> int:
    """Send random bytes out of the console and read them back through a jumper across J7's TX
    and RX pins. Each rate sends one second's worth."""
    probe = dap.one(args)
    serial_number = probe.serial
    probe.release()
    port = next((p.device for p in list_ports.comports() if p.serial_number == serial_number), None)
    if port is None:
        raise SystemExit(f"no CDC port for {serial_number}")
    failed = 0
    for baud in args.baud:
        size = baud // 10
        data = os.urandom(size)
        got = bytearray()
        with serial.Serial(port, baud, timeout=0.5) as s:
            s.reset_input_buffer()

            def reader() -> None:
                quiet_since = time.perf_counter()
                while len(got) < size and time.perf_counter() - quiet_since < 2.0:
                    chunk = s.read(4096)
                    if chunk:
                        got.extend(chunk)
                        quiet_since = time.perf_counter()

            thread = threading.Thread(target=reader)
            thread.start()
            s.write(data)
            thread.join()
        same = bytes(got) == data
        failed += not same
        print(f"  {port} at {baud} baud: {len(got)} of {size} bytes back, {'identical' if same else 'DIFFERENT'}")
    return failed != 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", help="pick one probe when several are attached")
    sub = parser.add_subparsers(dest="command", required=True)
    rates = lambda v: [int(float(x)) for x in v.split(",")]  # noqa: E731
    wires = ("jtag", "cjtag", "swd")

    p = sub.add_parser("exec", help="DAP_ExecuteCommands with block data at every offset")
    p.add_argument("--wire", choices=wires, default="jtag")
    p.add_argument("--hz", type=rates, default=[10_000_000, 50_000_000], help="comma-separated, e.g. 10e6,50e6")
    p.add_argument("--words", type=int, default=0, help="block words; 0 for the target's default")
    p.set_defaults(run=cmd_exec)

    p = sub.add_parser("abort", help="DAP_TransferAbort against a long match-retry transfer")
    p.add_argument("--wire", choices=wires, default="jtag")
    p.add_argument("--hz", type=int, default=1_000_000)
    p.add_argument("--after", type=float, default=0.2, help="seconds before the abort")
    p.add_argument("--hid", action="store_true", help="send the transfers and the abort over HID")
    p.set_defaults(run=cmd_abort)

    p = sub.add_parser("hid", help="the exec packets and a read through HID, then both transports at once")
    p.add_argument("--wire", choices=wires, default="jtag")
    p.add_argument("--hz", type=rates, default=[10_000_000, 50_000_000])
    p.add_argument("--mixed", type=int, default=200, help="bulk + HID pairs sent together")
    p.set_defaults(run=cmd_hid)

    p = sub.add_parser("pipeline", help="HID reports queued up to the advertised packet count")
    p.add_argument("--delay", type=int, default=2000, help="DAP_Delay microseconds, never 0")
    p.add_argument("--trials", type=int, default=20)
    p.set_defaults(run=cmd_pipeline)

    p = sub.add_parser("rate", help="block reads with a fixed number of commands outstanding")
    p.add_argument("--wire", choices=wires, default="jtag")
    p.add_argument("--hz", type=int, default=50_000_000)
    p.add_argument("--words", type=int, default=126)
    p.add_argument("--blocks", type=int, default=2000)
    p.add_argument("--rtt", type=int, default=2000, help="DAP_Info round trips to average")
    p.add_argument("--depth", type=lambda v: [int(x) for x in v.split(",")], default=[1, 2, 3])
    p.add_argument("--no-core1", dest="core1", action="store_false", help="for an image without CPU 1's status")
    p.set_defaults(run=cmd_rate)

    p = sub.add_parser("console", help="CDC console loopback through a jumper on J7")
    p.add_argument("--baud", type=lambda v: [int(float(x)) for x in v.split(",")],
                   default=[9600, 115200, 921600, 2_000_000])
    p.set_defaults(run=cmd_console)

    p = sub.add_parser("mark", help="halt with VTOR at 0x2000, to tell a reset happened")
    p.add_argument("--wire", choices=wires, default="jtag")
    p.set_defaults(run=cmd_mark)

    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    sys.exit(main())
