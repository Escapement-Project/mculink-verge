#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyusb"]
# ///
"""Talk to a CMSIS-DAP v2 probe over its bulk endpoints, without probe-rs in the way.

The bench needs three things probe-rs cannot do: read every `DAP_Info` field including the ones it
ignores, send vendor commands, and emit a bare clock burst with nothing wrapped around it. The
third is what the clock calibration measures -- `DAP_SWJ_Sequence` is the only command on the wire
whose cycles-per-bit is the quantity ARM's clock arithmetic is about.

    uv run tools/dap.py list
    uv run tools/dap.py info --serial QK7RM2VTX8D41
    uv run tools/dap.py identify
    uv run tools/dap.py clock --hz 10000000 --bursts 200
    uv run tools/dap.py sample --fill 24

Never send `DAP_TransferAbort` and then read: it stalls the bulk endpoint and every later command
times out. Nothing here sends it.
"""

import argparse
import sys
import time

import usb.core
import usb.util

# Stock MCU-Link calls its bulk interface "MCU-LINK (r0FB) CMSIS-DAP V3.172" and ours calls it
# "CMSIS-DAP v2", so match on the part of the name the spec actually stipulates.
CMSIS_DAP = "cmsis-dap"

DAP_INFO = 0x00
DAP_CONNECT = 0x02
DAP_DISCONNECT = 0x03
DAP_HOST_STATUS = 0x01
DAP_TRANSFER = 0x05
DAP_TRANSFER_BLOCK = 0x06
DAP_SWJ_PINS = 0x10
DAP_SWJ_CLOCK = 0x11
DAP_SWJ_SEQUENCE = 0x12
DAP_VENDOR_J8 = 0x84
DAP_VENDOR_REBOOT = 0x85
DAP_VENDOR_TARGET = 0x86
DAP_VENDOR_SAMPLE = 0x87
DAP_VENDOR_SWD_GEN = 0x89

INFO_FIELDS = {
    0x01: ("vendor", "str"),
    0x02: ("product", "str"),
    0x03: ("serial", "str"),
    0x04: ("protocol version", "str"),
    0x05: ("target device vendor", "str"),
    0x06: ("target device name", "str"),
    0x07: ("target board vendor", "str"),
    0x08: ("target board name", "str"),
    0x09: ("firmware version", "str"),
    0xF0: ("capabilities", "bytes"),
    0xF1: ("test domain timer", "word"),
    0xFB: ("UART rx buffer", "word"),
    0xFC: ("UART tx buffer", "word"),
    0xFD: ("SWO trace buffer", "word"),
    0xFE: ("max packet count", "byte"),
    0xFF: ("max packet size", "short"),
}

CAPABILITIES = [
    "SWD",
    "JTAG",
    "SWO UART",
    "SWO Manchester",
    "atomic commands",
    "test domain timer",
    "SWO streaming trace",
    "UART via USB COM",
]


class Probe:
    """One CMSIS-DAP v2 probe, held open on its bulk pair."""

    def __init__(self, device, interface, out_ep, in_ep, serial, product, vendor):
        self.device = device
        self.interface = interface
        self.out_ep = out_ep
        self.in_ep = in_ep
        self.serial = serial
        self.product = product
        self.vendor = vendor

    def drain(self) -> int:
        """Throw away replies queued by a run that stopped early.

        Nothing in the protocol resynchronises a bulk endpoint, so a reply nobody read is handed
        to whatever asks next and every answer after it is off by one. Pipelined runs make this
        routine rather than rare.
        """
        count = 0
        while True:
            try:
                self.in_ep.read(self.in_ep.wMaxPacketSize, 50)
                count += 1
            except usb.core.USBError:
                return count

    def command(self, request: bytes, timeout: int = 2000) -> bytes:
        self.out_ep.write(request, timeout)
        reply = bytes(self.in_ep.read(self.in_ep.wMaxPacketSize, timeout))
        if not reply or reply[0] != request[0]:
            raise RuntimeError(
                f"asked for 0x{request[0]:02x}, got 0x{reply[0]:02x} -- the probe is out of step"
            )
        return reply

    def info(self, field: int) -> bytes:
        reply = self.command(bytes([DAP_INFO, field]))
        return reply[2 : 2 + reply[1]]

    def connect(self, port: int = 1) -> int:
        return self.command(bytes([DAP_CONNECT, port]))[1]

    def disconnect(self) -> int:
        return self.command(bytes([DAP_DISCONNECT]))[1]

    def swj_clock(self, hz: int) -> int:
        return self.command(bytes([DAP_SWJ_CLOCK]) + hz.to_bytes(4, "little"))[1]

    def swj_sequence(self, bits: int, data: bytes) -> int:
        # A count of 256 is encoded as 0; anything above needs several commands.
        count = 0 if bits == 256 else bits
        return self.command(bytes([DAP_SWJ_SEQUENCE, count]) + data)[1]

    def release(self) -> None:
        usb.util.dispose_resources(self.device)


def find(serial: str | None = None, vid: int = 0x1FC9, pid: int = 0x0143) -> list[Probe]:
    """Every attached probe that offers a CMSIS-DAP v2 bulk interface."""
    probes = []
    for device in usb.core.find(find_all=True, idVendor=vid, idProduct=pid):
        try:
            device_serial = usb.util.get_string(device, device.iSerialNumber) or ""
            product = usb.util.get_string(device, device.iProduct) or ""
            manufacturer = usb.util.get_string(device, device.iManufacturer) or ""
        except usb.core.USBError as error:
            print(f"  cannot read strings from {device}: {error}", file=sys.stderr)
            continue
        if serial is not None and device_serial != serial:
            continue
        for configuration in device:
            for interface in configuration:
                name = ""
                if interface.iInterface:
                    try:
                        name = usb.util.get_string(device, interface.iInterface) or ""
                    except usb.core.USBError:
                        name = ""
                if CMSIS_DAP not in name.lower() or interface.bInterfaceClass != 0xFF:
                    continue
                out_ep = in_ep = None
                for endpoint in interface:
                    if usb.util.endpoint_type(endpoint.bmAttributes) != usb.util.ENDPOINT_TYPE_BULK:
                        continue
                    if usb.util.endpoint_direction(endpoint.bEndpointAddress) == usb.util.ENDPOINT_OUT:
                        out_ep = out_ep or endpoint
                    else:
                        in_ep = in_ep or endpoint
                if out_ep is None or in_ep is None:
                    continue
                if device.is_kernel_driver_active(interface.bInterfaceNumber):
                    device.detach_kernel_driver(interface.bInterfaceNumber)
                probes.append(
                    Probe(device, interface, out_ep, in_ep, device_serial, product, manufacturer)
                )
                break
    return probes


def one(args) -> Probe:
    probes = find(args.serial)
    if not probes:
        sys.exit("no CMSIS-DAP v2 probe found" + (f" with serial {args.serial}" if args.serial else ""))
    if len(probes) > 1:
        names = ", ".join(probe.serial for probe in probes)
        sys.exit(f"{len(probes)} probes attached ({names}); pass --serial")
    # A run that stopped early leaves replies queued, and nothing in the protocol resynchronises a
    # bulk endpoint: the next thing to ask gets them as its own answers, off by one for ever.
    drained = probes[0].drain()
    if drained:
        print(f"  drained {drained} replies left by a run that stopped early")
    return probes[0]


def cmd_list(args) -> int:
    probes = find(args.serial)
    if not probes:
        print("no CMSIS-DAP v2 probe found")
        return 1
    for probe in probes:
        port = f"{probe.device.bus}-{probe.device.address}"
        print(f"{probe.serial:<16} {probe.vendor} / {probe.product}   (bus {port})")
    return 0


def cmd_info(args) -> int:
    probe = one(args)
    print(f"{probe.serial}  {probe.vendor} / {probe.product}")
    for field, (name, kind) in INFO_FIELDS.items():
        try:
            raw = probe.info(field)
        except Exception as error:
            print(f"  0x{field:02x} {name:<22} -- {error}")
            continue
        if not raw:
            print(f"  0x{field:02x} {name:<22} (empty)")
        elif kind == "str":
            print(f"  0x{field:02x} {name:<22} {raw.rstrip(b"\x00").decode(errors='replace')!r}")
        elif kind == "byte":
            print(f"  0x{field:02x} {name:<22} {raw[0]}")
        elif kind == "short":
            print(f"  0x{field:02x} {name:<22} {int.from_bytes(raw[:2], 'little')}")
        elif kind == "word":
            print(f"  0x{field:02x} {name:<22} {int.from_bytes(raw[:4], 'little')}")
        else:
            flags = int.from_bytes(raw, "little")
            have = [n for i, n in enumerate(CAPABILITIES) if flags & (1 << i)]
            print(f"  0x{field:02x} {name:<22} 0x{flags:0{2*len(raw)}x}: {', '.join(have)}")
    probe.release()
    return 0


def cmd_identify(args) -> int:
    probe = one(args)
    print(f"{probe.serial}  {probe.vendor} / {probe.product}")
    try:
        reply = probe.command(bytes([0x82]))
    except Exception as error:
        print(f"  vendor 0x82 failed: {error}")
        probe.release()
        return 1
    body = reply[1:]
    if body[:5] != b"CJTAG":
        print(f"  vendor 0x82 answered {reply[:8].hex()} -- not our firmware")
        probe.release()
        return 1
    version, flags, length = body[5], body[6], body[7]
    build = body[8 : 8 + length].decode(errors="replace")
    supported = "yes" if flags & 1 else "no"
    active = "yes" if flags & 2 else "no"
    print(f"  magic CJTAG, format {version}, flags 0x{flags:02x}")
    print(f"  OScan1 supported {supported}, active now {active}")
    print(f"  build {build!r}")
    probe.release()
    return 0


def cmd_vendor(args) -> int:
    probe = one(args)
    payload = bytes.fromhex(args.payload) if args.payload else b""
    try:
        reply = probe.command(bytes([args.id]) + payload)
        print(f"0x{args.id:02x} -> {reply.hex()}")
        printable = bytes(c for c in reply[1:] if 32 <= c < 127)
        if len(printable) > 3:
            print(f"  text: {printable.decode()!r}")
    except Exception as error:
        print(f"0x{args.id:02x} failed: {error}")
        probe.release()
        return 1
    probe.release()
    return 0


def cmd_clock(args) -> int:
    """Emit a long, uninterrupted run of SWCLK edges at one requested rate.

    `DAP_SWJ_Sequence` clocks `count` bits with SWDIO following the payload and nothing else on the
    wire: no packet, no turnaround, no acknowledge. The period measured here is the per-bit cost
    the clock arithmetic in `Set_Clock_Delay` is trying to hit, so it is what the cost model in
    tools/cycles.py has to agree with.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.vendor} / {probe.product}")
    status = probe.connect(1)
    print(f"  DAP_Connect -> {status} ({'SWD' if status == 1 else 'JTAG' if status == 2 else 'failed'})")
    status = probe.swj_clock(args.hz)
    print(f"  DAP_SWJ_Clock {args.hz} Hz -> {'OK' if status == 0 else f'status {status}'}")
    # SWDIO is held low through the burst, so only SWCLK moves and the analyser sees one signal.
    payload = bytes(32)
    started = time.perf_counter()
    for _ in range(args.bursts):
        probe.swj_sequence(256, payload)
    elapsed = time.perf_counter() - started
    bits = args.bursts * 256
    print(f"  {args.bursts} bursts x 256 bits = {bits} clocks in {elapsed * 1e3:.1f} ms")
    print(f"  including USB, that is {bits / elapsed / 1e6:.3f} Mbit/s end to end")
    probe.release()
    return 0


def cmd_sweep(args) -> int:
    """Drive one bare clock burst per requested rate, separated by a silent gap.

    The gap is what lets the analyser tell the runs apart without a trigger: nothing else on the
    bench drives SWCLK, so a run is a dense group of edges and the rate it was asked for is the
    position of that group in the sequence.
    """
    probe = one(args)
    rates = [int(rate) for rate in args.rates.split(",")]
    print(f"{probe.serial}  {probe.vendor} / {probe.product}")
    status = probe.connect(1)
    print(f"  DAP_Connect -> {status}")
    payload = bytes(32)
    time.sleep(args.gap)
    for rate in rates:
        status = probe.swj_clock(rate)
        started = time.perf_counter()
        for _ in range(args.bursts):
            probe.swj_sequence(256, payload)
        elapsed = time.perf_counter() - started
        clocks = args.bursts * 256
        print(
            f"  {rate / 1e6:>6.2f} MHz asked: {clocks} clocks,"
            f" status {status}, {elapsed * 1e3:7.2f} ms end to end"
        )
        time.sleep(args.gap)
    probe.release()
    return 0


def cmd_blink(args) -> int:
    """Flash one unit's LED, so two identical probes on a bench can be told apart by eye.

    `DAP_HostStatus` drives the connect and running LEDs, and every CMSIS-DAP probe answers it, so
    this works on stock as well as on ours. On ours the commands themselves are what blink it, at
    the firmware's own rate: any traffic does, and these happen to be the cheapest.
    """
    probe = one(args)
    print(f"blinking {probe.serial} ({probe.product}) for {args.seconds:.0f} s")
    deadline = time.perf_counter() + args.seconds
    state = 0
    while time.perf_counter() < deadline:
        state ^= 1
        for kind in (0, 1):
            probe.command(bytes([DAP_HOST_STATUS, kind, state]))
        time.sleep(0.2)
    for kind in (0, 1):
        probe.command(bytes([DAP_HOST_STATUS, kind, 0]))
    probe.release()
    return 0


def cmd_led(args) -> int:
    """Walk our LED through its three states, saying which is due, so each can be checked by eye.

    Breathing when unused, blinking while commands arrive, lit while a host has said it is
    connected. The commands sent to make it blink are the running-LED form of `DAP_HostStatus`,
    which our firmware ignores, so they change nothing but the traffic.
    """
    probe = one(args)

    def phase(what: str) -> None:
        print(f"{what}, for {args.seconds:.0f} s", flush=True)

    phase("breathing: nothing is being sent")
    time.sleep(args.seconds)

    phase("blinking: commands are arriving")
    deadline = time.perf_counter() + args.seconds
    while time.perf_counter() < deadline:
        probe.command(bytes([DAP_HOST_STATUS, 1, 0]))

    phase("lit: the host says it is connected")
    probe.command(bytes([DAP_HOST_STATUS, 0, 1]))
    time.sleep(args.seconds)
    probe.command(bytes([DAP_HOST_STATUS, 0, 0]))
    print("back to breathing")
    probe.release()
    return 0


def read_target(probe, mode: int, channel: int = 0) -> tuple[int, int, int]:
    """One vendor 0x86 reading: the ADC code, millivolts and J6 pin 9's detect bit."""
    reply = probe.command(bytes([DAP_VENDOR_TARGET, mode, channel]))
    if len(reply) < 7 or reply[0] != DAP_VENDOR_TARGET:
        raise RuntimeError(f"vendor 0x86 answered {reply.hex()}; is this our firmware?")
    if reply[1] != 0:
        raise RuntimeError("the probe's ADC did not answer")
    return int.from_bytes(reply[2:4], "little"), int.from_bytes(reply[4:6], "little"), reply[6]


def cmd_target(args) -> int:
    """The target's supply as J6 pin 1 presents it, and whether anything grounds pin 9.

    An empty connector reads about 3.3 V, because the probe holds pin 1 up through 33K, so the
    voltage alone does not say a target is there: pin 9 does, when the target header grounds it.
    A target that is connected but unpowered reads well below its supply but not zero, wherever
    the dead board pulls the 33K: about 1.1 V for the MSPM0 board on the bench.

    `--sweep` converts every ADC0 channel on both sides instead, which is how the supply's channel
    is found: run it with the target powered and again unpowered, and the channel that moves is
    the one.
    """
    probe = one(args)
    if args.sweep:
        for side in ("A", "B"):
            row = []
            for channel in range(16):
                code, mv, _ = read_target(probe, 1, channel | (0x80 if side == "B" else 0))
                row.append(f"{channel:>2}{side} {mv:>4}")
            print("  ".join(row[:8]))
            print("  ".join(row[8:]))
    else:
        code, mv, detect = read_target(probe, 0)
        pin9 = "grounded" if detect else "open"
        print(f"target supply {mv / 1000:.3f} V (code {code}), pin 9 {pin9}")
    probe.release()
    return 0


PIN_NAMES = [(0, "SWCLK/TCK"), (1, "SWDIO/TMS"), (2, "TDI"), (3, "TDO"), (5, "nTRST"), (7, "nRESET")]


def show_pins(level: int) -> str:
    return " ".join(f"{name}={(level >> bit) & 1}" for bit, name in PIN_NAMES)


def cmd_pins(args) -> int:
    """Read the probe's own pin levels, and optionally drive one, without needing a target.

    This separates a probe that is not driving its pins from a probe whose cable has come off:
    the readback is of the probe's own pin, so it follows a drive even with nothing attached.
    The wait is always zero -- a non-zero wait spins until the pin reaches the asked level, which
    hangs the probe when it never does.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    if args.connect:
        print(f"  DAP_Connect -> {probe.connect(1)}")
    read_only = probe.command(bytes([DAP_SWJ_PINS, 0, 0]) + (0).to_bytes(4, "little"))
    print(f"  as found:  {show_pins(read_only[1])}")
    if args.drive is not None:
        select = 1 << args.bit
        for value in (0, 1):
            reply = probe.command(
                bytes([DAP_SWJ_PINS, value << args.bit, select]) + (0).to_bytes(4, "little")
            )
            print(f"  drove bit {args.bit} to {value}: {show_pins(reply[1])}")
    probe.release()
    return 0


def swd_reset(probe: "Probe") -> int:
    """Put the target's debug port into SWD and leave it addressable.

    Line reset, the JTAG-to-SWD switch word, line reset again, then idle. The DPIDR read that
    follows is what actually clears the reset condition, and its value says a target answered.
    """
    probe.swj_sequence(56, b"\xff" * 7)
    probe.swj_sequence(16, b"\x9e\xe7")
    probe.swj_sequence(56, b"\xff" * 7)
    probe.swj_sequence(8, b"\x00")
    reply = probe.command(bytes([DAP_TRANSFER, 0, 1, 0x02]))
    if reply[1] != 1 or reply[2] != 1:
        raise RuntimeError(f"DPIDR read answered count {reply[1]} ack {reply[2]}")
    return int.from_bytes(reply[3:7], "little")


def cmd_j8(args) -> int:
    """Drive J8's pins, which is how a pad gets identified and how a trigger gets placed.

    The firmware marks command processing on all four pins at once, so an analyser clipped to any
    of them sees the marker and the header needs no orientation. That leaves the question of which
    pad is which, and `--walk` answers it: each pin is taken from the marker in turn and held high
    on its own, so whichever pad the analyser or the meter is on names itself.

    A pin the host takes stops marking commands until it is given back, which `--release` does.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")

    def drive(mask: int, levels: int) -> int:
        reply = probe.command(bytes([DAP_VENDOR_J8, mask, levels]))
        if len(reply) < 3 or reply[0] != DAP_VENDOR_J8 or reply[1] != 0:
            raise RuntimeError(f"J8 {mask:#04x}/{levels:#04x} answered {reply.hex()}")
        return reply[2]

    if args.walk:
        for pin in range(4):
            bit = 1 << pin
            read = drive(bit, bit)
            print(f"  J8 pin {pin + 1} (PIO1_{pin}) high, the other three still marking"
                  f"  -- pins read {read:#06b}")
            time.sleep(args.seconds)
            drive(bit, 0)
        drive(0, 0)
        print("  all four handed back to the marker")
        probe.release()
        return 0

    mask = 0 if args.release else args.mask
    read = drive(mask, args.levels & mask)
    owned = "none" if mask == 0 else ", ".join(f"{p + 1}" for p in range(4) if mask & (1 << p))
    print(f"  host owns pin {owned}; pins read {read:#06b}")
    probe.release()
    return 0


def cmd_reboot(args) -> int:
    """Reset the probe, optionally into the ROM bootloader, so a reflash costs no hands.

    The probe answers, waits for the reply to leave the endpoint, drops off the bus and resets.
    So the command succeeding looks like the device disappearing, and there is nothing further to
    read: anything else would time out against a probe that is already gone.

    The ISP jumper remains the recovery path, and is the only one for an image that will not
    enumerate -- which is the case this cannot help with.
    """
    probe = one(args)
    mode = 1 if args.bootloader else 0
    print(f"{probe.serial}  {probe.product}")
    reply = probe.command(bytes([DAP_VENDOR_REBOOT, mode]))
    if len(reply) < 2 or reply[0] != DAP_VENDOR_REBOOT or reply[1] != 0:
        print(f"  refused: {reply.hex()}")
        probe.release()
        return 1
    where = "the ROM bootloader (1fc9:0021)" if mode else "its firmware"
    print(f"  accepted; the probe is dropping off the bus and coming back in {where}")
    probe.release()
    return 0


def clock_ladder(cpu_hz: int, half_bit_cycles: float = 2.5) -> list[tuple[int, int, float]]:
    """Every distinct wire rate the firmware can produce, and a request that selects each.

    Sweeping requested Hz wastes most of its points. `Set_Clock_Delay` turns a request into a
    counted delay, so requests between two steps land on the same step: 12 and 14 MHz both become
    the 10 MHz one. Walking the delay instead gives one point per achievable rate and no
    duplicates.

    ARM's arithmetic, transcribed from DAP.c: a request at or above (CPU/2)/IO_PORT_WRITE_CYCLES
    takes the delay-free path, and below it the count is
    `ceil((ceil((CPU/2)/clock) - IO_PORT_WRITE_CYCLES) / DELAY_SLOW_CYCLES)`, floored at one.

    The period comes from the measured per-half-bit cost rather than ARM's model, which understates
    its own firmware threefold. A bit is `2 * (half_bit_cycles + 3 * delay - 1)`, the minus one
    because PIN_DELAY_SLOW is a `subs` and a `bne` and n iterations cost 3n-1. Ours solves to 2.5
    cycles a half-bit, from 15.01 at delay 2, 33.01 at delay 5 and 153.04 at delay 25 on a 150 MHz
    core. Stock's wire fits 6.0 in the same formula, so sweeping a stock unit wants `--half-bit-cycles 6`.

    **The two firmwares make different ladders, and together they make a finer one.** Ours has
    nothing between 10.00 and 16.67 MHz; stock's delay-free step lands at 15.00, inside that gap.
    """
    io_port_write_cycles = 2
    delay_slow_cycles = 3
    half = cpu_hz // 2

    # The delay-free copy, which a request at or above this threshold selects. Its rate is the
    # same arithmetic with the delay gone, and it is the only step above the delay-1 one.
    fast_from = half // io_port_write_cycles
    fast_wire = cpu_hz / (2.0 * half_bit_cycles)

    seen: dict[int, tuple[int, float]] = {}
    for want in range(1_000_000, 37_000_000, 50_000):
        raw = -(-half // want)  # ceil
        delay = max(1, -(-(raw - io_port_write_cycles) // delay_slow_cycles)) if raw > io_port_write_cycles else 1
        if delay not in seen:
            period = 2.0 * (half_bit_cycles + delay_slow_cycles * delay - 1)
            seen[delay] = (want, cpu_hz / period)
    rows = [(delay, want, wire) for delay, (want, wire) in sorted(seen.items())]
    return [(0, fast_from, fast_wire)] + rows


def cmd_limit(args) -> int:
    """Where the wire stops working, as a number rather than a bracket.

    Repeats a DPIDR read with DAP_TransferBlock at every rate the firmware can produce and checks
    every word that comes back. DPIDR rather than RDBUFF because its value is known, so a bit that
    flips on the wire shows up as a wrong word and not only as a bad acknowledge -- a marginal
    clock corrupts data long before the acknowledge stops decoding.

    No access port, no target memory and no target state, so nothing here can leave the part in a
    different condition than it found it, and a failure at a high rate does not spoil the next
    point. The debug port is reset and its sticky errors cleared between rates for the same reason.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    print(f"  DAP_Connect -> {probe.connect(1)}")

    ladder = clock_ladder(args.cpu_hz, args.half_bit_cycles)
    if args.max_mhz:
        ladder = [row for row in ladder if row[2] <= args.max_mhz * 1e6]

    print(f"\n  {'delay':>5}  {'asked':>9}  {'wire':>9}  {'transfers':>9}  {'bad words':>9}  {'short':>6}  verdict")
    worst_good = 0.0
    best_bad = None
    for delay, want, wire in ladder:
        probe.swj_clock(want)
        try:
            reference = swd_reset(probe)
        except RuntimeError as exc:
            print(f"  {delay:>5}  {want / 1e6:>8.2f}M  {wire / 1e6:>8.2f}M  {'-':>9}  {'-':>9}  {'-':>6}  no DPIDR: {exc}")
            best_bad = wire if best_bad is None else min(best_bad, wire)
            continue

        transfers = bad_words = short = 0
        for _ in range(args.blocks):
            reply = probe.command(
                bytes([DAP_TRANSFER_BLOCK, 0]) + args.words.to_bytes(2, "little") + bytes([0x02])
            )
            count = int.from_bytes(reply[1:3], "little")
            if count != args.words or reply[3] != 1:
                short += 1
                break
            transfers += count
            for i in range(count):
                if int.from_bytes(reply[4 + i * 4 : 8 + i * 4], "little") != reference:
                    bad_words += 1

        ok = bad_words == 0 and short == 0
        if ok:
            worst_good = max(worst_good, wire)
        else:
            best_bad = wire if best_bad is None else min(best_bad, wire)
        print(f"  {delay:>5}  {want / 1e6:>8.2f}M  {wire / 1e6:>8.2f}M  {transfers:>9}  {bad_words:>9}  {short:>6}  "
              f"{'ok' if ok else 'FAILED'}")

        # Whatever happened, leave the port clean for the next rate: a sticky error latches and
        # would make every later point fail for a reason that has nothing to do with its clock.
        probe.swj_clock(1_000_000)
        try:
            swd_reset(probe)
            probe.command(bytes([DAP_TRANSFER, 0, 1, 0x00, 0x1E, 0x00, 0x00, 0x00]))
        except RuntimeError:
            pass

    print()
    if best_bad is None:
        print(f"  every rate up to {worst_good / 1e6:.2f} MHz was clean; the ceiling is above this ladder")
    else:
        print(f"  clean to {worst_good / 1e6:.2f} MHz, failing at {best_bad / 1e6:.2f} MHz")
    probe.release()
    return 0


def cmd_block(args) -> int:
    """Read one DP register over and over with DAP_TransferBlock, as a clean bit-path instrument.

    Every other way of putting a long transfer on the wire measures something else as well. A
    memory read goes through an access port, so the target can answer WAIT and the capture fills
    with retries instead of data. A DP register always answers OK and needs no access port, no
    memory and in fact no target beyond one that drives the line -- so what reaches the analyser is
    the probe's own transfer loop and nothing else.

    RDBUFF is the register to pick: reading it has no side effects on the debug port's state.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    # A pipelined run that stopped early leaves replies queued, and the next run reads them as
    # answers to its own commands. Nothing in the protocol clears that, so drain first.
    print(f"  DAP_Connect -> {probe.connect(1)}")
    print(f"  DAP_SWJ_Clock {args.hz} Hz -> {'OK' if probe.swj_clock(args.hz) == 0 else 'failed'}")
    idcode = swd_reset(probe)
    print(f"  DPIDR 0x{idcode:08x}")
    request = 0x0E if args.register == "rdbuff" else 0x02
    payload = bytes([DAP_TRANSFER_BLOCK, 0]) + args.words.to_bytes(2, "little") + bytes([request])
    started = time.perf_counter()
    words = 0
    # `--pipeline N` keeps N commands in flight instead of waiting for each reply before sending
    # the next. CMSIS-DAP v2 allows it -- DAP_Info reports how many packets the probe will hold --
    # and it is the one thing that separates a gap the firmware owns from a gap the host owns.
    inflight = max(1, args.pipeline)
    sent = 0
    done = 0
    while done < args.blocks:
        while sent < args.blocks and sent - done < inflight:
            probe.out_ep.write(payload, 2000)
            sent += 1
        reply = bytes(probe.in_ep.read(probe.in_ep.wMaxPacketSize, 2000))
        done += 1
        count = int.from_bytes(reply[1:3], "little")
        if count != args.words or reply[3] != 1:
            print(f"  stopped: block returned {count} words, ack {reply[3]}")
            break
        words += count
    elapsed = time.perf_counter() - started
    print(f"  {args.blocks} blocks x {args.words} words = {words} transfers in {elapsed * 1e3:.1f} ms")
    print(f"  {words * 4 / elapsed / 1024:.1f} KiB/s end to end, including USB")
    probe.release()
    return 0


SAMPLE_BEFORE_EDGE = 0xFF
SAMPLE_FILL_MAX = 32
SAMPLE_LEAD = 0x80


def parse_positions(text: str, fill: int) -> list[int]:
    """`before,lead0-12,0-12` into sample positions.

    A bare number is the after shape, that many cycles past the rising edge. `leadN` is the lead
    shape with a high phase of N cycles, sampling in production's own pattern; `before` is the lead
    shape with the whole fill, which is production's placement.
    """
    out = []
    for part in text.split(","):
        part = part.strip()
        flag = 0
        if part == "before":
            out.append(SAMPLE_BEFORE_EDGE)
            continue
        if part.startswith("lead"):
            flag, part = SAMPLE_LEAD, part[4:]
        if "-" in part:
            lo, hi = (int(x) for x in part.split("-"))
            values = range(lo, hi + 1)
        else:
            values = [int(part)]
        for v in values:
            if not 0 <= v <= fill:
                raise SystemExit(f"position {v} is outside 0..{fill}, the fill")
            out.append(flag | v)
    return out


def position_name(at: int) -> str:
    if at == SAMPLE_BEFORE_EDGE:
        return "before"
    if at & SAMPLE_LEAD:
        return f"lead {at & ~SAMPLE_LEAD}"
    return f"+{at}"


def sample_run(probe: "Probe", low: int, fill: int, at: int, count: int, expected: int) -> dict:
    request = bytes([DAP_VENDOR_SAMPLE, low, fill, at]) + count.to_bytes(2, "little") + expected.to_bytes(4, "little")
    reply = probe.command(request)
    if len(reply) < 17 or reply[0] != DAP_VENDOR_SAMPLE:
        raise SystemExit(f"vendor 0x87 answered {reply.hex()}: not this firmware, or too old for it")
    if reply[1] != 0:
        raise SystemExit("vendor 0x87 refused: the port is not plain SWD, or the operands are out of range")
    u16 = lambda i: int.from_bytes(reply[i:i + 2], "little")  # noqa: E731
    return {
        "good": u16(2), "bad_ack": u16(4), "bad_word": u16(6), "last_ack": reply[8],
        "last_word": int.from_bytes(reply[9:13], "little"),
        "cycles": int.from_bytes(reply[13:17], "little") / 32,
    }


def swd_gen(probe: "Probe", latency: int = 0) -> dict:
    """Vendor 0x89: the generated SWD paths' state, setting the read latency first unless 0."""
    reply = probe.command(bytes([DAP_VENDOR_SWD_GEN, latency]))
    if reply[0] != DAP_VENDOR_SWD_GEN:
        raise RuntimeError("this image does not generate its SWD paths (no vendor 0x89)")
    fields = [int.from_bytes(reply[5 + 4 * i : 9 + 4 * i], "little") for i in range(6)]
    names = ("hz", "write_period", "read_period", "code_bytes", "builds", "failures")
    return {"status": reply[1], "latency": reply[2], "masked": bool(reply[3]), "ok": bool(reply[4])} | dict(
        zip(names, fields)
    )


def cmd_gen(args) -> int:
    """The generated SWD paths: what they were built for, and the read latency to build them for.

    `--calibrate` finds the latency rather than trusting the default. At the shortest read period
    a sample is placed one or two cycles into a four-cycle window, so DPIDR reads come back right
    only while the latency the generator assumes is within a cycle of the real one. Sweeping it
    gives a band of passing values whose middle is the target's latency, which is then set.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    if args.calibrate:
        probe.connect(1)
        probe.swj_clock(args.calibrate_hz)
        passing = []
        for latency in range(1, 25):
            swd_gen(probe, latency)
            good = 0
            try:
                reference = swd_reset(probe)
                for _ in range(args.blocks):
                    reply = probe.command(bytes([DAP_TRANSFER_BLOCK, 0]) + (32).to_bytes(2, "little") + bytes([0x02]))
                    count = int.from_bytes(reply[1:3], "little")
                    good += sum(
                        int.from_bytes(reply[4 + 4 * i : 8 + 4 * i], "little") == reference for i in range(count)
                    )
            except RuntimeError:
                pass
            ok = good == 32 * args.blocks
            print(f"  latency {latency:2d}: {good:5d}/{32 * args.blocks} DPIDR reads right  {'ok' if ok else ''}")
            if ok:
                passing.append(latency)
        if not passing:
            print("  no latency read DPIDR correctly; leaving the default")
            swd_gen(probe, 7)
            return 1
        if passing != list(range(passing[0], passing[-1] + 1)):
            print(f"  the passing latencies are not one band: {passing}")
        # Where two cycles have equal margin the generator takes the earlier, so samples lean
        # early: of two middle values the higher one centres them.
        chosen = (passing[0] + passing[-1] + 1) // 2
        print(f"  passing {passing[0]}..{passing[-1]}: latency {chosen}")
        args.latency = chosen
    state = swd_gen(probe, args.latency)
    if state["status"] != 0:
        print(f"  latency {args.latency} refused")
    print(
        f"  clock asked {state['hz'] / 1e6:.3f} MHz: write period {state['write_period']}, "
        f"read period {state['read_period']} cycles; latency {state['latency']}; "
        f"{'masked' if state['masked'] else 'unmasked'}; {'generated' if state['ok'] else 'NOT generated'}, "
        f"{state['code_bytes']} bytes; {state['builds']} builds, {state['failures']} failures"
    )
    return 0 if state["ok"] else 1


def cmd_sample(args) -> int:
    """Where a read has to sample SWDIO: the instrument behind vendor 0x87.

    Each position runs `--count` DPIDR reads. In the after shape the sample sits that many cycles
    past the rising edge, and reads come back one bit ahead once the edge's new bit has arrived.
    In the lead shape the clock is high for that many cycles, then production's own falling edge,
    delay loop and sample: shorten it until reads come back one bit behind, and that is how soon
    after an edge production's pattern can see the bit the edge launched. The clock period stays
    fixed across a sweep either way.

    The period printed is timed by the probe itself, on a dry run that moves no pin.
    """
    probe = one(args)
    ns = 1e9 / args.cpu_hz
    print(f"{probe.serial}  {probe.product}")
    print(f"  DAP_Connect -> {probe.connect(1)}")
    probe.swj_clock(args.hz)
    expected = swd_reset(probe)
    print(f"  DPIDR 0x{expected:08x}, read at {args.hz / 1e6:g} MHz through the production path")

    if args.calibrate:
        print("\n  dry runs, lead shape with the whole fill: one cycle a fill step means the filler is exact")
        print(f"  {'fill':>4}  {'cycles/clock':>12}  {'step':>5}")
        previous = None
        for fill in range(0, SAMPLE_FILL_MAX + 1, 4):
            cycles = sample_run(probe, args.low, fill, SAMPLE_LEAD | fill, 0, expected)["cycles"]
            step = "" if previous is None else f"{(cycles - previous) / 4:.2f}"
            print(f"  {fill:>4}  {cycles:>12.2f}  {step:>5}")
            previous = cycles
        print("\n  dry runs at the fill asked for: the period must not move with the sample")
        step = max(1, args.fill // 6)
        for at in [SAMPLE_LEAD | h for h in range(0, args.fill + 1, step)] + list(range(0, args.fill + 1, step)):
            cycles = sample_run(probe, args.low, args.fill, at, 0, expected)["cycles"]
            print(f"  {position_name(at):>8}  {cycles:.2f} cycles")
        probe.release()
        return 0

    positions = parse_positions(args.at or f"before,lead0-{args.fill},0-{args.fill}", args.fill)
    print(f"\n  low {args.low}, fill {args.fill}, {args.count} reads a position")
    print(f"  {'sample':>8}  {'after edge':>10}  {'period':>9}  {'good':>6}  {'bad ack':>7}  {'bad word':>8}  last bad")
    for at in positions:
        r = sample_run(probe, args.low, args.fill, at, args.count, expected)
        where = position_name(at)
        after = "" if at & SAMPLE_LEAD else f"{at * ns:.1f} ns"
        period = f"{r['cycles'] * ns:.1f} ns"
        last = ""
        if r["bad_ack"]:
            last = f"ack {r['last_ack']}"
        elif r["bad_word"]:
            last = f"word 0x{r['last_word']:08x}"
        print(f"  {where:>8}  {after:>10}  {period:>9}  {r['good']:>6}  {r['bad_ack']:>7}  {r['bad_word']:>8}  {last}")
        # A misread leaves the target in step, since every read is clocked to the end, but a
        # line reset between positions costs nothing and rules it out.
        swd_reset(probe)
    probe.release()
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--serial", help="pick one probe when several are attached")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("list", help="every attached CMSIS-DAP v2 probe").set_defaults(run=cmd_list)
    sub.add_parser("info", help="every DAP_Info field").set_defaults(run=cmd_info)
    sub.add_parser("identify", help="vendor 0x82, our firmware's identity").set_defaults(run=cmd_identify)

    vendor = sub.add_parser("vendor", help="send one vendor command and show the reply")
    vendor.add_argument("--id", type=lambda s: int(s, 0), required=True)
    vendor.add_argument("--payload", help="hex bytes after the id")
    vendor.set_defaults(run=cmd_vendor)

    clock = sub.add_parser("clock", help="emit a bare SWCLK burst for the analyser")
    clock.add_argument("--hz", type=int, required=True, help="what to ask DAP_SWJ_Clock for")
    clock.add_argument("--bursts", type=int, default=200, help="256 clocks each")
    clock.set_defaults(run=cmd_clock)

    sweep = sub.add_parser("sweep", help="one burst per rate, separated by a silent gap")
    sweep.add_argument("--rates", default="50000000,10000000,5000000", help="comma separated Hz")
    sweep.add_argument("--bursts", type=int, default=20, help="256 clocks each")
    sweep.add_argument("--gap", type=float, default=0.3, help="silent seconds between runs")
    sweep.set_defaults(run=cmd_sweep)

    blink = sub.add_parser("blink", help="flash one unit's LED to identify it by eye")
    blink.add_argument("--seconds", type=float, default=10.0)
    blink.set_defaults(run=cmd_blink)

    led = sub.add_parser("led", help="walk our LED through breathing, blinking and lit")
    led.add_argument("--seconds", type=float, default=6.0, help="how long to hold each state")
    led.set_defaults(run=cmd_led)

    target = sub.add_parser("target", help="the target's supply and pin 9 detect, from our firmware")
    target.add_argument("--sweep", action="store_true", help="read every ADC0 channel, in millivolts")
    target.set_defaults(run=cmd_target)

    pins = sub.add_parser("pins", help="read, and optionally drive, the probe's own pins")
    pins.add_argument("--connect", action="store_true", help="DAP_Connect first, so pins are outputs")
    pins.add_argument("--drive", action="store_true", default=None, help="toggle one pin and read back")
    pins.add_argument("--bit", type=int, default=0, help="which pin to drive, default SWCLK")
    pins.set_defaults(run=cmd_pins)

    j8 = sub.add_parser("j8", help="drive the four-pin header beside the MCU")
    j8.add_argument("--walk", action="store_true", help="hold each pin high in turn, to name the pads")
    j8.add_argument("--seconds", type=float, default=3.0, help="how long to hold each one")
    j8.add_argument("--mask", type=lambda s: int(s, 0), default=0x0F, help="pins the host takes")
    j8.add_argument("--levels", type=lambda s: int(s, 0), default=0x00, help="levels for those pins")
    j8.add_argument("--release", action="store_true", help="give all four back to the marker")
    j8.set_defaults(run=cmd_j8)

    limit = sub.add_parser("limit", help="sweep every achievable rate and check the data")
    limit.add_argument("--cpu-hz", type=int, default=150_000_000, help="the probe's core clock")
    limit.add_argument("--half-bit-cycles", type=float, default=2.5,
                       help="measured non-delay cost of a half bit: 2.5 for ours, 6 for stock")
    limit.add_argument("--words", type=int, default=64, help="transfers per block command")
    limit.add_argument("--blocks", type=int, default=50, help="block commands per rate")
    limit.add_argument("--max-mhz", type=float, default=0.0, help="stop above this wire rate")
    limit.set_defaults(run=cmd_limit)

    reboot = sub.add_parser("reboot", help="reset the probe, optionally into the ROM bootloader")
    reboot.add_argument("--bootloader", action="store_true", help="come back in ISP mode, not the firmware")
    reboot.set_defaults(run=cmd_reboot)

    block = sub.add_parser("block", help="repeat a DP read with DAP_TransferBlock")
    block.add_argument("--hz", type=int, default=10000000)
    block.add_argument("--words", type=int, default=64, help="transfers per block command")
    block.add_argument("--blocks", type=int, default=200)
    block.add_argument("--register", choices=("rdbuff", "dpidr"), default="rdbuff")
    block.add_argument("--pipeline", type=int, default=1,
                       help="commands to keep in flight; 1 waits for each reply before sending")
    block.set_defaults(run=cmd_block)

    sample = sub.add_parser("sample", help="sweep where a read samples SWDIO, with vendor 0x87")
    sample.add_argument("--hz", type=int, default=1_000_000, help="clock for the line reset only")
    sample.add_argument("--cpu-hz", type=int, default=150_000_000, help="the probe's core clock")
    sample.add_argument("--low", type=int, default=0, help="extra delay-loop iterations in the low phase")
    sample.add_argument("--fill", type=int, default=24, help=f"filler cycles in the high phase, 0..{SAMPLE_FILL_MAX}")
    sample.add_argument("--at", help="positions, e.g. before,lead0-24,0-24; default: before, every lead, every after")
    sample.add_argument("--count", type=int, default=1000, help="reads at each position")
    sample.add_argument("--calibrate", action="store_true", help="time the filler instead of reading")
    sample.set_defaults(run=cmd_sample)

    gen = sub.add_parser("gen", help="the generated SWD paths: state, latency, calibration")
    gen.add_argument("--latency", type=int, default=0, help="set the read latency, in core cycles")
    gen.add_argument("--calibrate", action="store_true", help="find the latency with DPIDR reads")
    gen.add_argument("--calibrate-hz", type=int, default=37_500_000, help="clock to calibrate at")
    gen.add_argument("--blocks", type=int, default=8, help="32-word DPIDR blocks per latency")
    gen.set_defaults(run=cmd_gen)

    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
