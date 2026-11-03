#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["libusb1", "pyusb"]
# ///
"""Talk to a CMSIS-DAP v2 probe over its bulk endpoints, without probe-rs in the way.

The bench needs three things probe-rs cannot do: read every `DAP_Info` field including the ones it
ignores, send vendor commands, and emit a bare clock burst with nothing wrapped around it. The
third is what the clock calibration measures -- `DAP_SWJ_Sequence` is the only command on the wire
whose cycles-per-bit is the quantity ARM's clock arithmetic is about.

    uv run tools/dap.py list
    uv run tools/dap.py info --serial <serial>
    uv run tools/dap.py identify
    uv run tools/dap.py clock --hz 10000000 --bursts 200
    uv run tools/dap.py sample --fill 24

Never send `DAP_TransferAbort` and then read: it stalls the bulk endpoint and every later command
times out. Nothing here sends it.
"""

import argparse
import dataclasses
import struct
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
DAP_DELAY = 0x09
DAP_SWJ_PINS = 0x10
DAP_SWJ_CLOCK = 0x11
DAP_SWJ_SEQUENCE = 0x12
DAP_JTAG_SEQUENCE = 0x14
DAP_EXECUTE_COMMANDS = 0x7F
DAP_JTAG_CONFIGURE = 0x15
DAP_SWO_TRANSPORT = 0x17
DAP_SWO_MODE = 0x18
DAP_SWO_BAUDRATE = 0x19
DAP_SWO_CONTROL = 0x1A
DAP_SWO_STATUS = 0x1B
DAP_SWO_DATA = 0x1C
SWO_ACTIVE, SWO_STREAM_ERROR, SWO_OVERRUN = 0x01, 0x40, 0x80
DAP_VENDOR_CJTAG_CONTROL = 0x83
DAP_VENDOR_J8 = 0x84
USB_REBOOT_REQUEST = 0x03
USB_CORE1_STATUS_REQUEST = 0x04  # DAP_SHARED_CORE1_STATUS_REQUEST
USB_CORE1_SAMPLE_REQUEST = 0x05  # DAP_SHARED_CORE1_SAMPLE_REQUEST
DAP_VENDOR_TARGET = 0x86
DAP_VENDOR_TARGET_POWER = 0x8F
DAP_VENDOR_SAMPLE = 0x87
DAP_VENDOR_SWD_GEN = 0x89
DAP_VENDOR_JTAG_GEN = 0x8A
DAP_VENDOR_JTAG_LOG = 0x8D
DAP_VENDOR_DIO = 0x8E
DAP_VENDOR_ENERGY = 0x90

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

    def queue_status(self) -> dict:
        """The shared queues as CPU 0 sees them: which side holds work, if the probe has stopped."""
        raw = bytes(self.device.ctrl_transfer(0xC0, USB_CORE1_STATUS_REQUEST, 1, 0, 256, timeout=1000))
        slots = raw[0]
        names = ("request_head", "request_tail", "rx_armed_slot", "hid_head", "hid_tail",
                 "response_head", "response_tail", "reply_in_flight", "hid_response_head", "hid_response_tail",
                 "hid_reply_in_flight", "mailbox_cpu1", "mailbox_cpu0")
        state = dict(zip(names, raw[1:14]), slots=slots)
        per = [struct.unpack_from("<HHHH", raw, 14 + 8 * i) for i in range(slots)]
        state["request_len"], state["hid_request_len"], state["response_len"], state["hid_response_len"] = (
            [p[k] for p in per] for k in range(4))
        return state

    def swo_status(self) -> dict[str, int]:
        """SWO capture's DMA and USART registers as CPU 0 reads them (DAP_SHARED_SWO_STATUS)."""
        raw = bytes(self.device.ctrl_transfer(0xC0, USB_CORE1_STATUS_REQUEST, 2, 0, 72, timeout=1000))
        names = ("segments", "dma_ctrl", "enableset", "active", "busy", "inta", "errint", "cfg", "ctlstat",
                 "xfercfg", "pselid", "usart_cfg", "usart_stat", "fifocfg", "fifostat", "iocon_p0_3", "intb",
                 "lost")
        # Firmware before INTB and swo_lost answers 16 words.
        return dict(zip(names, struct.unpack(f"<{len(raw) // 4}I", raw[:len(raw) // 4 * 4])))

    def core1_status(self) -> dict[str, int]:
        """CPU 1's progress and last fault, answered by CPU 0 however CPU 1 is doing."""
        raw = bytes(self.device.ctrl_transfer(0xC0, USB_CORE1_STATUS_REQUEST, 0, 0, 40, timeout=1000))
        # Images before the stale count answer 36 bytes, and the dict leaves it out.
        names = ("commands", "command", "exception", "cfsr", "hfsr", "bfar", "pc", "lr", "xpsr", "stale")
        return dict(zip(names, struct.unpack(f"<{len(raw) // 4}I", raw)))

    def reboot(self, bootloader: bool = False) -> None:
        """Reset through CPU 0's USB control path, without waiting on DAP execution."""
        self.device.ctrl_transfer(0x40, USB_REBOOT_REQUEST, int(bootloader), 0, 0, timeout=1000)

    def swj_clock(self, hz: int) -> int:
        return self.command(bytes([DAP_SWJ_CLOCK]) + hz.to_bytes(4, "little"))[1]

    def swj_sequence(self, bits: int, data: bytes) -> int:
        # A count of 256 is encoded as 0; anything above needs several commands.
        count = 0 if bits == 256 else bits
        return self.command(bytes([DAP_SWJ_SEQUENCE, count]) + data)[1]

    def jtag_sequence(self, sequences: list[tuple[int, int, bool, int]]) -> list[int]:
        """Run `(clocks, tms, capture, tdi)` sequences; return each captured one's TDO bits."""
        request = bytearray([DAP_JTAG_SEQUENCE, len(sequences)])
        for clocks, tms, capture, tdi in sequences:
            request.append((clocks & 0x3F) | (tms << 6) | (capture << 7))
            request += (tdi & ((1 << clocks) - 1)).to_bytes((clocks + 7) // 8, "little")
        reply = self.command(bytes(request))
        if reply[1] != 0:
            raise RuntimeError(f"DAP_JTAG_Sequence status {reply[1]}")
        captured, offset = [], 2
        for clocks, _, capture, _ in sequences:
            if capture:
                length = (clocks + 7) // 8
                captured.append(int.from_bytes(reply[offset : offset + length], "little"))
                offset += length
        return captured

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
        except (usb.core.USBError, ValueError):
            # Some firmware builds omit string descriptors. The vendor-class bulk interface
            # still identifies the CMSIS-DAP transport well enough to use its commands.
            device_serial = product = manufacturer = ""
        if serial is not None and device_serial != serial:
            continue
        for configuration in device:
            for interface in configuration:
                name = ""
                if interface.iInterface:
                    try:
                        name = usb.util.get_string(device, interface.iInterface) or ""
                    except (usb.core.USBError, ValueError):
                        name = ""
                if interface.bInterfaceClass != 0xFF or (name and CMSIS_DAP not in name.lower()):
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


def cmd_swo(args) -> int:
    """Capture SWO with DAP_SWO_Data, polling as fast as the replies come, and save the bytes."""
    probe = one(args)
    for request in (bytes([DAP_SWO_CONTROL, 0]), bytes([DAP_SWO_TRANSPORT, 1]), bytes([DAP_SWO_MODE, 1])):
        status = probe.command(request)[1]
        if status != 0:
            sys.exit(f"0x{request[0]:02x} {request[1]} answered {status:#x}")
    baud = int.from_bytes(probe.command(bytes([DAP_SWO_BAUDRATE]) + args.baud.to_bytes(4, "little"))[1:5], "little")
    if baud == 0:
        sys.exit(f"the probe refused {args.baud} baud")
    if probe.command(bytes([DAP_SWO_CONTROL, 1]))[1] != 0:
        sys.exit("capture did not start")

    data = bytearray()
    polls = empty = 0
    flagged = {SWO_STREAM_ERROR: [], SWO_OVERRUN: []}
    limit = args.limit.to_bytes(2, "little")

    def poll() -> int:
        nonlocal polls, empty
        reply = probe.command(bytes([DAP_SWO_DATA]) + limit)
        count = int.from_bytes(reply[2:4], "little")
        polls += 1
        empty += count == 0
        for bit, at in flagged.items():
            if reply[1] & bit:
                at.append(len(data))
        data.extend(reply[4:4 + count])
        return count

    started = time.monotonic()
    while time.monotonic() - started < args.seconds:
        poll()
    elapsed = time.monotonic() - started
    probe.command(bytes([DAP_SWO_CONTROL, 0]))
    while poll():
        pass
    status = probe.command(bytes([DAP_SWO_STATUS]))
    probe.command(bytes([DAP_SWO_MODE, 0]))
    probe.release()

    open(args.out, "wb").write(data)
    print(f"{baud} baud: {len(data)} bytes in {elapsed:.2f} s, {len(data) / elapsed / 1e3:.1f} KB/s, "
          f"{polls} polls ({empty} empty), saved to {args.out}")
    for bit, name in ((SWO_STREAM_ERROR, "stream error"), (SWO_OVERRUN, "overrun")):
        at = flagged[bit]
        print(f"  {name}: {len(at)} replies" + (f", first at bytes {at[:8]}" if at else ""))
    print(f"  after stop: status {status[1]:#04x}, {int.from_bytes(status[2:6], 'little')} bytes left")
    return 0


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


# Vendor 0x82's board byte, as src/board_pins.h numbers it.
BOARDS = {0: "MCU-Link", 1: "MCU-Link Pro", 2: "on-board MCU-Link"}


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
    active = "yes" if flags & 2 else "no"  # cJTAG framing, in whichever format
    print(f"  magic CJTAG, format {version}, flags 0x{flags:02x}")
    print(f"  OScan1 supported {supported}, cJTAG framing active now {active}")
    print(f"  build {build!r}")
    # Format 3 adds the board and 4 the scan formats. Read each at its format only: a v1 HID reply
    # is padded.
    if version >= 3 and len(body) > 8 + length:
        board = body[8 + length]
        print(f"  board {BOARDS.get(board, f'unknown ({board})')}")
    if version >= 4 and len(body) >= 13 + length:
        formats = int.from_bytes(body[9 + length : 13 + length], "little")
        print(f"  cJTAG scan formats {[n for n in range(32) if formats >> n & 1]} (SCNFMT)")
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
        # Images from before the step was reported answer 0 for every failure.
        step = {1: "offset calibration", 2: "gain calibration", 3: "conversion"}.get(reply[2])
        raise RuntimeError(f"the probe's ADC did not answer{f' (its {step} timed out)' if step else ''}")
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


def cmd_target_power(args) -> int:
    """Switch the Pro's target regulator or report the state last set since boot."""
    probe = one(args)
    modes = {"off": 0, "on": 1, "read": 2}
    reply = probe.command(bytes([DAP_VENDOR_TARGET_POWER, modes[args.state]]))
    if len(reply) < 3 or reply[0] != DAP_VENDOR_TARGET_POWER:
        raise RuntimeError(f"vendor 0x8F answered {reply.hex()}; is this firmware new enough?")
    if reply[1] != 0:
        raise RuntimeError("the target supply is unavailable on this board or the request was rejected")
    print(f"target supply {'on' if reply[2] else 'off'}")
    probe.release()
    return 0


ENERGY_RESULTS = ("ok", "stopped", "no sample", "unavailable", "bad rate", "bad argument", "ADC error",
                  "DMA error")
ENERGY_RANGES = {0: "range unknown", 1: "350 mA setting, low range", 2: "350 mA setting, high range",
                 3: "50 mA setting, low range", 4: "50 mA setting, high range"}


def energy(probe: Probe, request: bytes) -> tuple[int, bytes]:
    reply = probe.command(bytes([DAP_VENDOR_ENERGY]) + request)
    if len(reply) < 3 or reply[0] != DAP_VENDOR_ENERGY or reply[1] != 0:
        raise RuntimeError(f"vendor 0x90 answered {reply.hex()}; is this a Pro with energy capture?")
    return reply[2], reply[3:]


def energy_result(result: int) -> str:
    return ENERGY_RESULTS[result] if result < len(ENERGY_RESULTS) else f"result {result}"


def show_energy_sample(probe: Probe) -> str:
    result, body = energy(probe, bytes([1]))
    if result != 0:
        return energy_result(result)
    voltage, current, sequence = struct.unpack_from("<HHI", body)
    line = f"#{sequence}: voltage code {voltage}, current code {current}"
    if len(body) >= 18:
        mv, na, range_id, calibrated = struct.unpack_from("<IiBB", body, 8)
        line += f"; {mv} mV, {na / 1000:.3f} uA, {ENERGY_RANGES.get(range_id, f'range {range_id}')}"
        line += "" if calibrated else ", uncalibrated"
    return line


def show_energy_total(probe: Probe) -> str:
    result, body = energy(probe, bytes([3]))
    nj, pairs = struct.unpack_from("<QQ", body)
    return f"{energy_result(result)}: {nj} nJ over {pairs} pairs"


def show_energy_calibration(probe: Probe) -> str:
    lines = []
    for index, name in ((0, "low range"), (1, "high range")):
        result, body = energy(probe, bytes([4, index]))
        points = [struct.unpack_from("<HI", body, 1 + 6 * i) for i in range(body[0])]
        text = ", ".join(f"code {code} at {na / 1000:.3f} uA" for code, na in points)
        slopes = ", ".join(f"{(b[1] - a[1]) / (b[0] - a[0]):.2f}" for a, b in zip(points, points[1:]))
        lines.append(f"{name} ({energy_result(result)}): {text or 'no points'}" +
                     (f"; nA per code {slopes}" if slopes else ""))
    return "\n".join(lines)


def cmd_energy(args) -> int:
    """The Pro's energy capture through vendor 0x90 (HOST-INTERFACE.md).

    `watch` starts a capture at `--rate`, prints the newest sample every `--every` seconds for
    `--seconds`, then the total, and stops. The probe calibrates as it boots. A start calibrates
    again, disconnecting J9 for about 0.2 s, only after a failed calibration or when the jumper
    setting has changed.
    """
    probe = one(args)
    if args.action in ("start", "watch"):
        result, body = energy(probe, bytes([0]) + args.rate.to_bytes(4, "little"))
        if result != 0:
            raise RuntimeError(f"start: {energy_result(result)}")
        print(f"started at {int.from_bytes(body[:4], 'little')} pairs/s")
    if args.action == "latest":
        print(show_energy_sample(probe))
    elif args.action == "calibration":
        print(show_energy_calibration(probe))
    elif args.action == "total":
        print(show_energy_total(probe))
    elif args.action == "watch":
        end = time.monotonic() + args.seconds
        while time.monotonic() < end:
            time.sleep(args.every)
            print(show_energy_sample(probe))
        print(show_energy_total(probe))
    if args.action in ("stop", "watch"):
        result, _ = energy(probe, bytes([2]))
        print(f"stopped: {energy_result(result)}")
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


def dio_bits(value: int) -> str:
    return "".join("1" if value & (1 << n) else "0" for n in range(4))


def cmd_dio(args) -> int:
    """The Pro's J19 DIO pins (src/dio.h): set the pins in --mask, then print what the probe reports.
    With no --mask it only reads, which shows what a host before this one left driving."""
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    reply = probe.command(bytes([DAP_VENDOR_DIO, args.mask, args.outputs & args.mask, args.levels & args.mask]))
    if len(reply) < 7 or reply[0] != DAP_VENDOR_DIO:
        raise RuntimeError(f"DIO answered {reply.hex()}")
    status, outputs, latch, read, pins, count = reply[1:7]
    groups = list(reply[7 : 7 + count])
    print(f"  {'ok' if status == 0 else 'REFUSED'}; pins {dio_bits(pins)} (DIO0 first), groups "
          f"{', '.join(dio_bits(g) for g in groups) or 'none'}")
    print(f"  outputs {dio_bits(outputs)}  latch {dio_bits(latch)}  read {dio_bits(read)}")
    probe.release()
    return 0 if status == 0 else 1


def cmd_core1(args) -> int:
    """What CPU 1 last did, from CPU 0: the commands it has answered, the one it is in, its fault."""
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    if args.sample:
        # CPU 1 takes an NMI and stops in its fault handler; only a probe reset brings it back.
        probe.device.ctrl_transfer(0x40, USB_CORE1_SAMPLE_REQUEST, 0, 0, 0, timeout=1000)
        deadline = time.perf_counter() + 1.0
        while probe.core1_status()["exception"] == 0 and time.perf_counter() < deadline:
            time.sleep(0.01)
    try:
        status = probe.core1_status()
    except usb.core.USBError as error:
        print(f"  refused: USB control request failed ({error}); an image older than this command")
        probe.release()
        return 1
    command = status["command"].to_bytes(4, "little")
    print(f"  {status['commands']} replies published; current or last command {command.hex(' ')}")
    if "stale" in status:
        print(f"  {status['stale']} commands from a host that had gone, dropped unrun")
    if status["exception"] == 0:
        print("  no fault recorded")
    else:
        print(f"  FAULT: exception {status['exception']}, PC {status['pc']:#010x}, LR {status['lr']:#010x}, "
              f"xPSR {status['xpsr']:#010x}")
        print(f"         CFSR {status['cfsr']:#010x}, HFSR {status['hfsr']:#010x}, BFAR {status['bfar']:#010x}")
    if args.queues:
        q = probe.queue_status()
        print(f"  bulk requests: head {q['request_head']} tail {q['request_tail']} armed {q['rx_armed_slot']}, "
              f"lengths {q['request_len']}")
        print(f"  HID requests:  head {q['hid_head']} tail {q['hid_tail']}, lengths {q['hid_request_len']}")
        print(f"  bulk replies:  head {q['response_head']} tail {q['response_tail']}, lengths {q['response_len']}, "
              f"head in flight {bool(q['reply_in_flight'])}")
        print(f"  HID replies:   head {q['hid_response_head']} tail {q['hid_response_tail']}, "
              f"lengths {q['hid_response_len']}, head in flight {bool(q['hid_reply_in_flight'])}")
        print(f"  mailbox:       CPU 1's register {q['mailbox_cpu1']:#04x}, CPU 0's {q['mailbox_cpu0']:#04x}")
    if args.swo:
        print("  SWO: " + ", ".join(f"{k} {v:#x}" for k, v in probe.swo_status().items()))
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
    try:
        probe.reboot(bootloader=bool(mode))
    except usb.core.USBError as error:
        print(f"  refused: USB control request failed ({error})")
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
    `--register abort` writes zero to ABORT instead, which sets none of its bits: the write path's
    instrument, with the data in the request and a four-byte reply.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    # A pipelined run that stopped early leaves replies queued, and the next run reads them as
    # answers to its own commands. Nothing in the protocol clears that, so drain first.
    print(f"  DAP_Connect -> {probe.connect(1)}")
    print(f"  DAP_SWJ_Clock {args.hz} Hz -> {'OK' if probe.swj_clock(args.hz) == 0 else 'failed'}")
    idcode = swd_reset(probe)
    print(f"  DPIDR 0x{idcode:08x}")
    request = {"rdbuff": 0x0E, "dpidr": 0x02, "abort": 0x00}[args.register]
    data = bytes(4) if args.register == "abort" else b""
    if args.command == "transfer":
        # The same transfers as DAP_Transfer, one request byte each, which is what a host sends for
        # a run too short for a block of its own.
        if args.words > 127:
            print("  DAP_Transfer: at most 127 words")
            return 2
        payload = bytes([DAP_TRANSFER, 0, args.words]) + (bytes([request]) + data) * args.words
    else:
        payload = bytes([DAP_TRANSFER_BLOCK, 0]) + args.words.to_bytes(2, "little") + bytes([request])
        payload += data * args.words
    if len(payload) > probe.out_ep.wMaxPacketSize:
        print(f"  {len(payload)} bytes of request do not fit a {probe.out_ep.wMaxPacketSize}-byte packet")
        return 2
    if args.queue:
        run = queued_blocks(probe, payload, args.blocks, args.queue, args.words)
        if run.error is not None:
            print(f"  stopped after {run.done} blocks: {run.error}")
        print(f"  {run.done} blocks x {args.words} words, {args.queue} queued, in {run.seconds * 1e3:.1f} ms")
        print(f"  {run.words * 4 / run.seconds / 1024:.1f} KiB/s, {run.done / run.seconds:.0f} commands/s, "
              f"host CPU {100 * run.cpu / run.seconds:.0f}% of the run")
        return 0
    run = pipelined_blocks(probe, payload, args.blocks, max(1, args.pipeline), args.words)
    if run.error is not None:
        print(f"  stopped: {run.error}")
    print(f"  {args.blocks} commands x {args.words} words = {run.words} transfers in {run.seconds * 1e3:.1f} ms,"
          f" {run.seconds / args.blocks * 1e6:.1f} us each")
    print(f"  {run.words * 4 / run.seconds / 1024:.1f} KiB/s end to end, including USB")
    probe.release()
    return 0


@dataclasses.dataclass
class Run:
    """What one timed run of a repeated command came to."""

    done: int  # commands answered with every transfer OK
    words: int  # the transfers those carried
    seconds: float
    cpu: float  # host CPU time over the run
    error: str | None  # why the run stopped early, if it did


def transfer_counts(payload: bytes, reply: bytes) -> tuple[int, int]:
    """The transfer count and last acknowledge of a DAP_Transfer or DAP_TransferBlock reply."""
    if payload[0] == DAP_TRANSFER:
        return reply[1], reply[2]
    return int.from_bytes(reply[1:3], "little"), reply[3]


def pipelined_blocks(probe: Probe, payload: bytes, blocks: int, inflight: int, words: int) -> Run:
    """Run `blocks` copies of one command with `inflight` of them sent ahead of their replies.

    CMSIS-DAP v2 allows that -- DAP_Info reports how many packets the probe will hold -- and it is
    the one thing that separates a gap the firmware owns from a gap the host owns. A run that stops
    early leaves replies queued, which the next run would read as its own: drain before going on.
    """
    started = time.perf_counter()
    cpu = time.process_time()
    sent = done = moved = 0
    error = None
    while done < blocks:
        while sent < blocks and sent - done < inflight:
            probe.out_ep.write(payload, 2000)
            sent += 1
        reply = bytes(probe.in_ep.read(probe.in_ep.wMaxPacketSize, 2000))
        count, ack = transfer_counts(payload, reply)
        if reply[0] != payload[0] or count != words or ack != 1:
            error = f"command 0x{payload[0]:02x} returned {count} words, ack {ack}"
            break
        done += 1
        moved += count
    return Run(done, moved, time.perf_counter() - started, time.process_time() - cpu, error)


def queued_blocks(probe: Probe, payload: bytes, blocks: int, depth: int, words: int) -> Run:
    """Run `blocks` copies of one command with `depth` of them queued in the host controller.

    The synchronous loop in `pipelined_blocks` cannot go deeper than the probe's request slots: a
    write the probe refuses blocks the only thread, which then never reads the reply that would free
    a slot. libusb's asynchronous API queues the writes and keeps `depth` reads posted, so the probe
    takes the next command the moment a slot frees and never waits on the host. That makes this the
    ceiling any host could reach through the current USB path. It releases `probe`; find the probe
    again to go on.
    """
    import usb1

    serial = probe.serial
    number = probe.interface.bInterfaceNumber
    out_address = probe.out_ep.bEndpointAddress
    in_address = probe.in_ep.bEndpointAddress
    size = probe.in_ep.wMaxPacketSize
    probe.release()

    with usb1.USBContext() as context:
        handle = None
        for device in context.getDeviceIterator(skip_on_error=True):
            if (device.getVendorID(), device.getProductID()) != (0x1FC9, 0x0143):
                continue
            candidate = device.open()
            if candidate.getSerialNumber() == serial:
                handle = candidate
                break
            candidate.close()
        if handle is None:
            sys.exit(f"{serial} vanished between setup and the queued run")
        with handle.claimInterface(number):
            state = {"sent": 0, "posted": 0, "done": 0, "words": 0, "owed": 0, "error": None}
            free_out = [handle.getTransfer() for _ in range(depth)]
            reads = [handle.getTransfer() for _ in range(depth)]

            def send() -> None:
                if state["sent"] >= blocks:
                    return
                if not free_out:
                    state["owed"] += 1
                    return
                transfer = free_out.pop()
                transfer.setBulk(out_address, payload, callback=on_out, timeout=5000)
                transfer.submit()
                state["sent"] += 1

            def on_out(transfer) -> None:
                free_out.append(transfer)
                if transfer.getStatus() != usb1.TRANSFER_COMPLETED:
                    state["error"] = state["error"] or f"write status {transfer.getStatus()}"
                    return
                if state["owed"]:
                    state["owed"] -= 1
                    send()

            def post(transfer) -> None:
                transfer.setBulk(in_address, size, callback=on_in, timeout=5000)
                transfer.submit()
                state["posted"] += 1

            def on_in(transfer) -> None:
                if transfer.getStatus() != usb1.TRANSFER_COMPLETED:
                    state["error"] = state["error"] or f"read status {transfer.getStatus()}"
                    return
                reply = bytes(transfer.getBuffer()[: transfer.getActualLength()])
                count, ack = transfer_counts(payload, reply)
                if reply[0] != payload[0] or count != words or ack != 1:
                    state["error"] = state["error"] or f"reply {reply[:4].hex()} to block {state['done']}"
                    return
                state["done"] += 1
                state["words"] += count
                if state["posted"] < blocks:
                    post(transfer)
                send()

            started = time.perf_counter()
            cpu = time.process_time()
            for transfer in reads[: min(depth, blocks)]:
                post(transfer)
            for _ in range(min(depth, blocks)):
                send()
            while state["done"] < blocks and state["error"] is None:
                context.handleEventsTimeout(1.0)
            elapsed = time.perf_counter() - started
            cpu = time.process_time() - cpu
            if state["error"] is not None:
                for transfer in reads + [t for t in free_out]:
                    try:
                        transfer.cancel()
                    except usb1.USBError:
                        pass
        handle.close()
    return Run(state["done"], state["words"], elapsed, cpu, state["error"])


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


SWD_GEN_MODE_NONE, SWD_GEN_MODE_MEASURE, SWD_GEN_MODE_PIN, SWD_GEN_MODE_AUTOMATIC = range(4)
LATENCY_OUTCOMES = ("never measured", "measured", "two latencies left, the later used",
                    "several latencies left", "nothing read right",
                    "no latency agrees with every read",
                    "on a cycle boundary, the build with most to spare used")


def swd_gen(probe: "Probe", latency: int = 0, mode: int = SWD_GEN_MODE_NONE) -> dict:
    """Vendor 0x89: the generated SWD paths' state, setting the read latency first unless 0, then
    measuring it, pinning it or letting the probe measure it on its own (src/dap_vendor.c)."""
    reply = probe.command(bytes([DAP_VENDOR_SWD_GEN, latency, mode]))
    if reply[0] != DAP_VENDOR_SWD_GEN:
        raise RuntimeError("this image does not generate its SWD paths (no vendor 0x89)")
    fields = [int.from_bytes(reply[5 + 4 * i : 9 + 4 * i], "little") for i in range(6)]
    names = ("hz", "write_period", "read_period", "code_bytes", "builds", "failures")
    state = {"status": reply[1], "latency": reply[2], "masked": bool(reply[3]), "ok": bool(reply[4])} | dict(
        zip(names, fields)
    )
    if len(reply) >= 58:
        u32 = [int.from_bytes(reply[34 + 4 * i : 38 + 4 * i], "little") for i in range(6)]
        state["sweep"] = {
            "automatic": bool(reply[29]), "outcome": reply[30], "latency": reply[31], "tries": reply[32],
            "periods": reply[33], "word": u32[0], "cycles": u32[1], "runs": u32[2],
            "consistent": u32[3] | (u32[4] << 32), "rescues": u32[5],
        }
    return state


def jtag_gen(probe: "Probe", two_pin: int = 0, four_wire: int = 0, mode: int = SWD_GEN_MODE_NONE) -> dict:
    """Vendor 0x8A: the generated JTAG paths' state, setting each wire's latency first unless 0,
    then measuring the wire in use, pinning the latencies or letting the probe measure them on its
    own (src/dap_vendor.c)."""
    reply = probe.command(bytes([DAP_VENDOR_JTAG_GEN, two_pin, four_wire, mode]))
    if reply[0] != DAP_VENDOR_JTAG_GEN:
        raise RuntimeError("this image does not generate its JTAG paths (no vendor 0x8A)")
    fields = [int.from_bytes(reply[6 + 4 * i : 10 + 4 * i], "little") for i in range(6)]
    names = ("hz", "period", "frame", "code_bytes", "builds", "failures")
    state = {"status": reply[1], "two_pin": reply[2], "four_wire": reply[3], "masked": bool(reply[4]),
             "ok": reply[5]} | dict(zip(names, fields))
    if len(reply) >= 60:
        u32 = [int.from_bytes(reply[36 + 4 * i : 40 + 4 * i], "little") for i in range(6)]
        state["sweep"] = {
            "automatic": bool(reply[30]), "outcome": reply[31], "wire": reply[32], "latency": reply[33],
            "tries": reply[34], "periods": reply[35], "word": u32[0], "cycles": u32[1], "runs": u32[2],
            "consistent": u32[3] | (u32[4] << 32), "rescues": u32[5],
        }
    if len(reply) >= 64:
        state["restores"] = int.from_bytes(reply[60:64], "little")
    return state


def latencies(mask: int) -> str:
    """Bit n - 1 for latency n, as runs: '5-8,11'."""
    runs, n = [], 1
    while mask >> (n - 1):
        if mask >> (n - 1) & 1:
            start = n
            while mask >> n & 1:
                n += 1
            runs.append(f"{start}" if start == n else f"{start}-{n}")
        n += 1
    return ",".join(runs) or "none"


# The generator's wires, JTAG_GEN_* (src/jtag_gen.h).
JLOG_WIRES = ("JTAG", "OScan1", "OScan2", "OScan5", "OScan6")


def describe_sweep(sweep: dict, cpu_hz: int = 150_000_000, word: str = "DPIDR") -> str:
    outcome = sweep["outcome"]
    text = LATENCY_OUTCOMES[outcome] if outcome < len(LATENCY_OUTCOMES) else f"outcome {outcome}"
    if "wire" in sweep and outcome != 0:
        wire = sweep["wire"]
        text = f"{JLOG_WIRES[wire] if wire < len(JLOG_WIRES) else f'wire {wire}'} {text}"
    if sweep["latency"]:
        text += f": latency {sweep['latency']}"
    if outcome != 0:
        text += (f"; agrees with {latencies(sweep['consistent'])}; {word} 0x{sweep['word']:08x}; "
                 f"{sweep['tries']} generations at {sweep['periods']} periods; "
                 f"{sweep['cycles'] / cpu_hz * 1e3:.2f} ms")
    return (f"{text}; {sweep['runs']} measurements, {sweep['rescues']} connections rescued; "
            f"{'automatic' if sweep['automatic'] else 'pinned'}")


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
    if args.measure:
        probe.connect(1)
        if args.hz:
            probe.swj_clock(args.hz)
        try:
            print(f"  DPIDR 0x{swd_reset(probe):08x} by line reset at the latency in use")
        except RuntimeError as exc:
            print(f"  line reset at the latency in use: {exc}")
    mode = (SWD_GEN_MODE_MEASURE if args.measure else SWD_GEN_MODE_PIN if args.pin
            else SWD_GEN_MODE_AUTOMATIC if args.automatic else SWD_GEN_MODE_NONE)
    state = swd_gen(probe, args.latency, mode)
    if state["status"] != 0:
        print(f"  latency {args.latency}, mode {mode} refused")
    print(
        f"  clock asked {state['hz'] / 1e6:.3f} MHz: write period {state['write_period']}, "
        f"read period {state['read_period']} cycles; latency {state['latency']}; "
        f"{'masked' if state['masked'] else 'unmasked'}; {'generated' if state['ok'] else 'NOT generated'}, "
        f"{state['code_bytes']} bytes; {state['builds']} builds, {state['failures']} failures"
    )
    if "sweep" in state:
        print(f"  last measurement: {describe_sweep(state['sweep'])}")
    return 0 if state["ok"] else 1


def cmd_jgen(args) -> int:
    """The generated JTAG paths: what they were built for, and each wire's read latency.

    `--measure` measures the wire in use, which needs the port already in JTAG with the chain
    configured and the DAP at the last transfer's index, as a host leaves it.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.product}")
    mode = (SWD_GEN_MODE_MEASURE if args.measure else SWD_GEN_MODE_PIN if args.pin
            else SWD_GEN_MODE_AUTOMATIC if args.automatic else SWD_GEN_MODE_NONE)
    state = jtag_gen(probe, args.two_pin, args.four_wire, mode)
    if state["status"] != 0:
        print(f"  latencies {args.two_pin}/{args.four_wire}, mode {mode} refused")
    print(
        f"  clock asked {state['hz'] / 1e6:.3f} MHz: period {state['period']}, JTAG clock {state['frame']} cycles; "
        f"latency cJTAG {state['two_pin']}, JTAG {state['four_wire']}; "
        f"{'masked' if state['masked'] else 'unmasked'}; {'generated' if state['ok'] & 1 else 'NOT generated'}"
        f"{', transfers' if state['ok'] & 2 else ''}{', looped' if state['ok'] & 4 else ''}, "
        f"{state['code_bytes']} bytes; {state['builds']} builds, {state['failures']} failures"
        + (f", {state['restores']} restored" if "restores" in state else "")
    )
    if "sweep" in state:
        print(f"  last measurement: {describe_sweep(state['sweep'], word='IDCODE')}")
    return 0 if state["ok"] & 1 else 1


JLOG_COMMANDS = {0x02: "Connect", 0x03: "Disconnect", 0x04: "TransferConfigure", 0x05: "Transfer",
                 0x06: "TransferBlock", 0x11: "SWJ_Clock", 0x12: "SWJ_Sequence", 0x14: "JTAG_Sequence",
                 0x15: "JTAG_Configure", 0x83: "cJTAG control", 0x8A: "JTAG gen", 0x8B: "JTAG pacing"}
JLOG_CHANGED = ("first", "owner", "wire", "period", "latency", "chain", "idle", "transfers")
CORE_HZ = 150e6


def cmd_jlog(args) -> int:
    """Vendor 0x8D: the JTAG generator's last generations, oldest first, each with the command it
    ran in and what had changed since the one before."""
    probe = one(args)
    entries = []
    try:
        for back in range(args.last):
            reply = probe.command(bytes([DAP_VENDOR_JTAG_LOG, back]))
            if reply[1] != 0:
                break
            count, = struct.unpack_from("<I", reply, 2)
            command, wire, changed, flags, latency, before, after, idle, paced = reply[6:15]
            period, at, cycles = struct.unpack_from("<HII", reply, 15)
            entries.append((count - 1 - back, command, wire, changed, flags, latency, before, after, idle, paced,
                            period, at, cycles))
    finally:
        probe.release()
    if not entries:
        print("no generations logged")
        return 0
    newest = entries[0][11]
    for (n, command, wire, changed, flags, latency, before, after, idle, paced, period, at,
         cycles) in reversed(entries):
        why = ",".join(name for bit, name in enumerate(JLOG_CHANGED) if changed >> bit & 1) or "nothing"
        since = ((at - newest + (1 << 31)) & 0xFFFFFFFF) - (1 << 31)
        print(f"#{n:<5} {since / CORE_HZ * 1e3:>+9.2f} ms "
              f"{JLOG_COMMANDS.get(command, f'0x{command:02X}'):<17} "
              f"{JLOG_WIRES[wire] if wire < len(JLOG_WIRES) else wire:<9} {why:<22} "
              f"{'xfers' if flags & 1 else 'scans'}{' learned' if flags & 2 else ''}{' restored' if flags & 8 else ''}"
              f"{'' if flags & 4 else ' FAILED'}  "
              f"period {period} lat {latency} chain {before}/{after} idle {idle} (paced {paced})  "
              f"{cycles / CORE_HZ * 1e3:.2f} ms")
    return 0


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


def capture_pin_writes(path: str, tmsc: int, tckc: int, windows: list[tuple[float, float]]) -> list[tuple[int, int]]:
    """A capture's TCKC/TMSC changes inside `windows`, as `(pin bit, level)` writes in order.

    Where both change in one sample the analyser cannot say which went first, so the order is the
    one that keeps the wire legal: a falling clock before the data moves, the data before a
    rising clock. Escapes are unaffected, since there the clock does not change.

    Each window starts from the levels the capture holds there. Getting to them from where the
    last window ended must not clock the target or move TMSC with TCKC high, so a gap that would
    is refused rather than bridged.
    """
    import csv

    with open(path) as f:
        rows = csv.reader(f)
        next(rows)
        samples = [(float(r[0]), (int(r[1 + tmsc]), int(r[1 + tckc]))) for r in rows]
    writes, now = [], None
    for start, until in windows:
        held = samples[0][1]
        for t, levels in samples:
            if t > start:
                break
            held = levels
        if now is None:
            # From DAP_Connect both are high, so lowering the clock before the data keeps the
            # move out of a high clock.
            writes += [(0, held[1]), (1, held[0])]
        elif held != now:
            if now[1] != held[1] or held[1] == 1:
                raise ValueError(f"window at {start}: {now} -> {held} would clock or escape")
            writes.append((1, held[0]))
        prev = held
        for t, levels in samples:
            if t <= start:
                continue
            if t > until:
                break
            if levels == prev:
                continue
            data = [(1, levels[0])] if levels[0] != prev[0] else []
            clock = [(0, levels[1])] if levels[1] != prev[1] else []
            writes += clock + data if levels[1] == 0 else data + clock
            prev = levels
        now = prev
    return writes


class PinProgram:
    """TCKC/TMSC as single-pin writes, tracking the levels so TMSC never moves with TCKC high
    except in an escape, where moving it with the clock held high is the point."""

    def __init__(self):
        self.writes, self.tck, self.tmsc = [(0, 1), (1, 1)], 1, 1

    def clock(self, tms: str) -> "PinProgram":
        for bit in tms:
            self.writes.append((0, 0))
            if int(bit) != self.tmsc:
                self.tmsc = int(bit)
                self.writes.append((1, self.tmsc))
            self.writes.append((0, 1))
        self.tck = 1
        return self

    def escape(self, edges: int) -> "PinProgram":
        if not self.tck:
            raise ValueError("an escape starts with TCKC high")
        for _ in range(edges):
            self.tmsc ^= 1
            self.writes.append((1, self.tmsc))
        return self


def zbs() -> str:
    """A zero-bit scan as TMS from Run-Test/Idle: through Capture-DR to Update-DR, never shifting."""
    return "10110"


def dr_scan(clocks: int) -> str:
    """A DR scan spending `clocks` clocks in Shift-DR, as TMS from Run-Test/Idle back to it.

    A cJTAG command part is this count, so one clock too few is a different command: STFMT's
    opcode written one short is STC2, and STC2 9 switches a CC13xx to four-pin.
    """
    return "100" + "0" * (clocks - 1) + "110"


# The command group the J-Link sends once per pass: two zero-bit scans raise the control level, a
# one-clock scan locks it, then STC1 1 (SEDGE=1, sample on the rising edge) and STFMT 9 (OScan1).
# The format changes at the Update-DR of the last scan.
CJTAG_SELECT_OSCAN1 = zbs() + zbs() + dr_scan(1) + dr_scan(1) + dr_scan(1) + dr_scan(3) + dr_scan(9)


class TapWalk:
    """TMS, TDI and capture flags for a run of IR and DR scans, from Run-Test/Idle.

    `via_idle=False` goes from one Update state straight into the next scan, which is how a TAP
    the ICEPick has just switched in is kept off the chain until the scan that follows.
    """

    def __init__(self):
        self.tms, self.tdi, self.cap, self.at_update = "", "", "", False

    def _clock(self, tms: str, tdi: str = "", cap: str = ""):
        self.tms += tms
        self.tdi += tdi or "1" * len(tms)
        self.cap += cap or "0" * len(tms)

    def scan(self, ir: bool, value: int, bits: int, via_idle: bool = True) -> "TapWalk":
        if self.at_update and via_idle:
            self._clock("0")
        # Run-Test/Idle and both Update states go to Select-DR on TMS high; then Select-IR for an
        # IR scan, Capture, and the edge into Shift.
        self._clock("1" + ("1" if ir else "") + "00")
        data = format(value & ((1 << bits) - 1), f"0{bits}b")[::-1]
        self._clock("0" * (bits - 1) + "1", data, "1" * bits)
        self._clock("1")  # Update
        self.at_update = True
        return self

    def idle(self, clocks: int) -> "TapWalk":
        self._clock("0" * clocks)
        self.at_update = False
        return self


def run_walk(probe: Probe, walk: TapWalk) -> list[int]:
    """Clock a TapWalk, returning each scan's captured TDO as an integer, in order."""
    sequences, at = [], 0
    while at < len(walk.tms):
        n, key = 1, (walk.tms[at], walk.cap[at])
        while at + n < len(walk.tms) and n < 64 and (walk.tms[at + n], walk.cap[at + n]) == key:
            n += 1
        tdi = int(walk.tdi[at : at + n][::-1], 2)
        sequences.append((n, int(key[0]), key[1] == "1", tdi))
        at += n
    captured = []
    for start in range(0, len(sequences), 32):
        chunk = sequences[start : start + 32]
        for (n, _, cap, _), bits in zip([q for q in chunk if q[2]], probe.jtag_sequence(chunk)):
            captured.append(format(bits, f"0{n}b")[::-1])
    # Join the per-sequence captures back into one string per scan.
    scans, flat, pos = [], "".join(captured), 0
    for run in walk.cap.split("0"):
        if run:
            scans.append(int(flat[pos : pos + len(run)][::-1], 2))
            pos += len(run)
    return scans


def tms_bits(pattern: int, count: int) -> str:
    """TI's `WriteTmsPattern(pattern, count)`: `count` TMS bits of `pattern`, least significant first."""
    return "".join(str(pattern >> k & 1) for k in range(count))


# TI's XDS110 sequence for OScan1 on a CC13xx/CC26xx, from a TI engineer's post on the TI E2E
# forum (thread 766675). "raw" steps are one TCKC clock per TMS
# bit with no framing; "csm" steps are raw clocks the adapter's control state machine consumes
# and the TAP controller never sees; "frames" steps are OScan1 frames.
TI_OSCAN1 = [
    ("raw", tms_bits(0xFF, 8) + tms_bits(0, 1)),  # Test-Logic-Reset, Run-Test/Idle
    ("raw", tms_bits(0x03, 3) + tms_bits(0, 64) + tms_bits(0x03, 3)),  # BYPASS: control level 0
    ("raw", (tms_bits(0x05, 4) + tms_bits(0x03, 2)) * 2),  # two zero-bit scans
    ("raw", tms_bits(0x05, 4) + tms_bits(0x0D, 4) + tms_bits(0x05, 4)),  # 1-bit scan locks level 2
    ("raw", tms_bits(0x0D, 4) + tms_bits(0x05, 4) + tms_bits(0x0D, 4) + tms_bits(0x05, 4)),  # STC1 1
    ("raw", tms_bits(0x31, 6) + tms_bits(0x05, 4) + tms_bits(0x1C01, 13)),  # STFMT 9, ends Select-DR
    ("csm", 4),
    ("frames", tms_bits(0x02, 3)),  # Capture-DR, Exit1-DR, Pause-DR
    ("frames", tms_bits(0x03, 2) + tms_bits(0x15, 6) + tms_bits(0x08, 4) + tms_bits(0x01, 2)),  # STMC 4: deselect
    ("csm", 7),
    ("frames", tms_bits(0x05, 4)),
    ("frames", tms_bits(0x01, 2) + tms_bits(0x40, 7) + tms_bits(0x01, 1) + tms_bits(0x05, 4) + tms_bits(0x03, 3)),  # select controller 0
    ("csm", 7),
    ("frames", tms_bits(0x05, 4)),
    ("frames", tms_bits(0x03, 2) + tms_bits(0x05, 4) + tms_bits(0x01, 2) + tms_bits(0x01, 1) + tms_bits(0x01, 2)),  # ECL
    ("csm", 7),
]


# TAP states in the numbering vendor 0x83 mode 3 takes (src/cjtag.h), and IEEE 1149.1's next state
# for TMS 0 and TMS 1.
TAP_STATES = ["tlr", "rti", "seldr", "capdr", "shdr", "ex1dr", "paudr", "ex2dr", "upddr",
              "selir", "capir", "shir", "ex1ir", "pauir", "ex2ir", "updir"]
TAP_NEXT = {
    "tlr": ("rti", "tlr"), "rti": ("rti", "seldr"), "seldr": ("capdr", "selir"), "capdr": ("shdr", "ex1dr"),
    "shdr": ("shdr", "ex1dr"), "ex1dr": ("paudr", "upddr"), "paudr": ("paudr", "ex2dr"), "ex2dr": ("shdr", "upddr"),
    "upddr": ("rti", "seldr"), "selir": ("capir", "tlr"), "capir": ("shir", "ex1ir"), "shir": ("shir", "ex1ir"),
    "ex1ir": ("pauir", "updir"), "pauir": ("pauir", "ex2ir"), "ex2ir": ("shir", "updir"), "updir": ("rti", "seldr"),
}
OSCAN1 = 9


def tap_walk(state: str, tms: str) -> str:
    for bit in tms:
        state = TAP_NEXT[state][int(bit)]
    return state


def frame_oscan1(probe: Probe, scan_format: int = OSCAN1, tap: str = "rti") -> None:
    """Vendor 0x83 mode 2 for OScan1, mode 3 for another format with the TAP state it starts in. A
    probe that refuses it leaves the wire unframed, and every read after the activation comes back
    as noise rather than an error."""
    request = [DAP_VENDOR_CJTAG_CONTROL, 2] if scan_format == OSCAN1 else [
        DAP_VENDOR_CJTAG_CONTROL, 3, scan_format, TAP_STATES.index(tap)]
    reply = probe.command(bytes(request))
    if reply[1] != 0:
        raise RuntimeError(f"the probe refused framing format {scan_format} (vendor {bytes(request).hex()} answered {reply.hex()})")


def command_frames(opcode: int, operand: int) -> str:
    """A TAP.7 command as TMS from Run-Test/Idle, two DR scans through Pause-DR as TI writes them,
    back to Run-Test/Idle; the check packet's raw clocks follow it as a "csm" step."""
    def part(n: int) -> str:
        return "1010" + ("11" if n == 0 else "10" + "0" * (n - 1) + "11")
    return part(opcode) + part(operand) + "0"


def ti_steps(scan_format: int, switch: str) -> list:
    """TI's sequence after its Test-Logic-Reset, selecting `scan_format`: in JScan in place of its
    STFMT 9 ("jscan"), or as a command in OScan1 frames before the window closes ("command")."""
    steps = list(TI_OSCAN1[1:])
    if scan_format == OSCAN1:
        return steps
    if switch == "jscan":
        stfmt = steps.index(("raw", tms_bits(0x31, 6) + tms_bits(0x05, 4) + tms_bits(0x1C01, 13)))
        steps[stfmt] = ("raw", tms_bits(0x31, 6) + tms_bits(0x05, 4) + "1" + "0" * scan_format + "111")
        return steps[: stfmt + 1] + [("format", scan_format)] + steps[stfmt + 1 :]
    # The last three steps are ECL's, which closes the window. After a command's last Update-DR
    # comes one SP that does not move the TAP, then the 4-clock check packet (1149.7 Table 21-5):
    # TI's 7 raw clocks are OScan1's 3-bit SP and the CP. Outside Shift an OScan2, 3, 6 or 7 SP is
    # one TMS bit, an OScan5 SP OScan1's three (Table 25-1).
    sp = 1 if scan_format in (OSCAN1 + 1, OSCAN1 + 2, OSCAN1 + 5, OSCAN1 + 6) else 3
    ecl = [step if step[0] != "csm" else ("csm", sp + 4) for step in steps[-3:]]
    return steps[:-3] + [("frames", command_frames(3, scan_format)), ("csm", 7), ("format", scan_format)] + ecl


def run_ti_activation(probe: Probe, steps=TI_OSCAN1) -> None:
    """Drive TI's activation: raw clocks as pin writes, frames through our OScan1 framing.

    One pin program follows the wire's real levels across steps, so a step never assumes a TMSC
    level the previous one did not leave. Raw clocks change TMSC only with TCKC low.
    """
    wire = PinProgram()
    wire.writes = []  # DAP_Connect leaves both pins high, which is what PinProgram assumes
    framing = False
    # The format the probe frames in from the next time framing starts, and where the TAP is, which
    # a format other than OScan1 needs. Steps start from the reset the prefix or TI's first step does.
    scan_format, tap = OSCAN1, "tlr"

    def flush():
        if wire.writes:
            replay_pins(probe, wire.writes)
            wire.writes = []

    for kind, arg in steps:
        if kind == "format":
            scan_format = arg
            if framing:
                frame_oscan1(probe, scan_format, tap)
            continue
        if kind == "pause":
            wire.writes.append((2, arg))
            continue
        if kind == "wait":
            flush()
            probe.command(bytes([DAP_DELAY]) + arg.to_bytes(2, "little"))
            continue
        if kind == "escape":
            if not wire.tck:
                wire.writes.append((0, 1))
                wire.tck = 1
            wire.escape(arg)
        elif kind in ("raw", "csm"):
            wire.clock(arg if kind == "raw" else "0" * arg)
            if kind == "raw":
                tap = tap_walk(tap, arg)
            if framing:
                wire.writes.append((0, 0))  # a frame starts with TCKC low
                wire.tck = 0
                flush()
                frame_oscan1(probe, scan_format, tap)
        else:
            if not framing:
                wire.writes.append((0, 0))
                wire.tck = 0
                flush()
                frame_oscan1(probe, scan_format, tap)
                framing = True
            walk = TapWalk()
            walk.tms, walk.tdi, walk.cap = arg, "1" * len(arg), "0" * len(arg)
            run_walk(probe, walk)
            tap = tap_walk(tap, arg)
            # Our frames end with TCKC low and TMSC at the last TMS bit.
            wire.tck, wire.tmsc = 0, int(arg[-1])
    flush()


def replay_pins(probe: Probe, writes: list[tuple[int, int]]) -> None:
    """Drive `writes` with DAP_SWJ_Pins, many to a packet so an escape lands in microseconds."""
    per_packet = (probe.out_ep.wMaxPacketSize - 2) // 7
    for at in range(0, len(writes), per_packet):
        chunk = writes[at : at + per_packet]
        request = bytearray([DAP_EXECUTE_COMMANDS, len(chunk)])
        for bit, level in chunk:
            if bit == 2:
                # A pause inside the packet, `level` microseconds, so it costs no USB round trip.
                request += bytes([DAP_DELAY]) + level.to_bytes(2, "little")
            else:
                request += bytes([DAP_SWJ_PINS, level << bit, 1 << bit]) + bytes(4)
        reply = probe.command(bytes(request))
        if reply[1] != len(chunk):
            raise RuntimeError(f"DAP_ExecuteCommands ran {reply[1]} of {len(chunk)}")


def icepick_read_cpuid(probe: Probe, args) -> None:
    """Route a CC13xx ICEPick to its Cortex-M DAP and read CPUID through DAP_Transfer.

    The ICEPick steps are probe-rs's icepick.rs, which ports OpenOCD's icepick.cfg.
    """
    ir_connect, ir_router, ir_bypass = 0x07, 0x02, 0x3F
    sysctrl = args.sysctrl
    sd_tap0 = (1 << 31) | (0x20 << 24) | (1 << 20) | (1 << 8) | (1 << 6) | (1 << 3)
    walk = TapWalk().scan(True, ir_connect, 6).scan(False, 0x89, 8).idle(1)
    walk.scan(True, ir_router, 6).scan(False, sysctrl, 32).scan(True, ir_router, 6).scan(False, 0, 32).idle(1)
    print(f"  ICEPick SYSCTRL reads back 0x{run_walk(probe, walk)[-1]:08x}")
    idcode = run_walk(probe, TapWalk().scan(True, 0x04, 6).scan(False, 0, 32).idle(1))[-1]
    print(f"  ICEPick IDCODE through IR 0x04 afterwards: 0x{idcode:08x}")
    walk = TapWalk().scan(True, ir_router, 6).scan(False, sd_tap0, 32, via_idle=False)
    walk.scan(True, ir_bypass, 6, via_idle=False).idle(args.join_idle)
    run_walk(probe, walk)
    print("  SD_TAP0 selected, ICEPick in BYPASS")
    # The secondary TAP answers only once its power domain is up, which takes a while after the
    # selection; until then the chain reads all ones.
    pattern = 0x0123456789ABCDEF_FEDCBA98
    selected = time.perf_counter()
    for attempt in range(200):
        chain = run_walk(probe, TapWalk().scan(False, pattern, 96).idle(1))[0]
        if chain != (1 << 96) - 1:
            break
        time.sleep(0.005)
    waited = (time.perf_counter() - selected) * 1e3
    print(f"  96-bit DR scan of 0x{pattern:024x}: 0x{chain:024x} after {waited:.0f} ms, {attempt + 1} scans")
    for delay in range(0, 40):
        if (chain >> delay) & ((1 << (96 - delay)) - 1) == pattern & ((1 << (96 - delay)) - 1) and delay:
            print(f"  the pattern comes back {delay} bits late: 0x{chain & ((1 << delay) - 1):x} ahead of it")
            break
    # DP CTRL/STAT write, read; SELECT AP 0 bank 0xF; AP IDR read; CSW, TAR, DRW.
    for order in ((4, 6), (6, 4)):
        reply = probe.command(bytes([DAP_JTAG_CONFIGURE, 2, *order]))
        index = 0 if order[0] == 4 else 1
        request = bytes([DAP_TRANSFER, index, 2, 0x04]) + (0x5000_0000).to_bytes(4, "little") + bytes([0x06])
        reply = probe.command(request)
        if reply[1] == 2 and reply[2] == 1:
            print(f"  chain IR lengths {order}: CTRL/STAT 0x{int.from_bytes(reply[3:7], 'little'):08x}")
            break
        print(f"  chain IR lengths {order}: DAP_Transfer count {reply[1]} ack {reply[2]}")
    else:
        return
    steps = [
        (0x08, 0x0000_00F0), (0x0F, None),  # SELECT bank 0xF, AP IDR
        (0x08, 0x0000_0000), (0x01, 0x2300_0052), (0x05, 0xE000_ED00), (0x0F, None),  # CPUID
    ]
    request = bytearray([DAP_TRANSFER, index, len(steps)])
    for req, value in steps:
        request.append(req)
        if value is not None:
            request += value.to_bytes(4, "little")
    reply = probe.command(bytes(request))
    words = [int.from_bytes(reply[3 + 4 * i : 7 + 4 * i], "little") for i in range(2)]
    print(f"  DAP_Transfer count {reply[1]} ack {reply[2]}: AP IDR 0x{words[0]:08x}, CPUID 0x{words[1]:08x}")
    for address in args.read:
        request = bytes([DAP_TRANSFER, index, 2, 0x05]) + address.to_bytes(4, "little") + bytes([0x0F])
        reply = probe.command(request)
        print(f"  read 0x{address:08x}: 0x{int.from_bytes(reply[3:7], 'little'):08x} (count {reply[1]} ack {reply[2]})")
    if args.sweep:
        sweep_block_reads(probe, index, args)


def block_read(probe: Probe, index: int, address: int, words: int) -> tuple[bytes, int]:
    """TAR, then a DAP_TransferBlock of DRW reads; returns the data and the final ack."""
    probe.command(bytes([DAP_TRANSFER, index, 1, 0x05]) + address.to_bytes(4, "little"))
    reply = probe.command(bytes([DAP_TRANSFER_BLOCK, index]) + words.to_bytes(2, "little") + bytes([0x0F]))
    done = reply[1] | reply[2] << 8
    return bytes(reply[4 : 4 + 4 * done]), reply[3]


def sweep_block_reads(probe: Probe, index: int, args) -> None:
    """Read the same ROM block at rising clock rates and compare it with the slowest read."""
    address, words = args.sweep_address, args.sweep_words
    probe.swj_clock(1_000_000)
    reference, ack = block_read(probe, index, address, words)
    print(f"  reference at 1 MHz: {len(reference) // 4} words, ack {ack}, first 0x{reference[:4][::-1].hex()}")
    for hz in args.sweep:
        probe.swj_clock(hz)
        bad = errors = 0
        started = time.perf_counter()
        for _ in range(args.sweep_repeat):
            data, ack = block_read(probe, index, address, words)
            errors += ack != 1
            bad += data != reference
        elapsed = time.perf_counter() - started
        rate = args.sweep_repeat * words * 4 / elapsed / 1024
        print(f"  {hz / 1e6:6.2f} MHz: {args.sweep_repeat - bad}/{args.sweep_repeat} blocks match, {errors} bad acks, {rate:.0f} KiB/s with USB")
    probe.swj_clock(1_000_000)


def cmd_cjtag(args) -> int:
    """Activate OScan1, by default with TI's sequence, then read the first TAP's IDCODE over cJTAG.

    The TAP is reset with TMS high and walked straight to Shift-DR, which is how the working J-Link
    read does it: a reset leaves IDCODE selected, so no instruction is shifted. With framing on,
    every one of these clocks leaves the probe as a three-packet OScan1 frame.
    """
    probe = one(args)
    print(f"{probe.serial}  {probe.vendor} / {probe.product}")
    status = probe.connect(2)
    print(f"  DAP_Connect -> {status} ({'JTAG' if status == 2 else 'failed'})")
    status = probe.swj_clock(args.hz)
    print(f"  DAP_SWJ_Clock {args.hz} Hz -> {'OK' if status == 0 else f'status {status}'}")
    reply = probe.command(bytes([DAP_JTAG_CONFIGURE, 1, args.ir_length]))
    print(f"  DAP_JTAG_Configure one TAP, IR {args.ir_length} -> status {reply[1]}")
    if args.nreset_first:
        probe.command(bytes([DAP_SWJ_PINS, 0x00, 0x80]) + bytes(4))
        time.sleep(0.02)
        probe.command(bytes([DAP_SWJ_PINS, 0x80, 0x80]) + bytes(4))
        time.sleep(args.settle / 1000)
        print(f"  pulsed nRESET before the activation, then waited {args.settle} ms")
    if args.ti or not (args.synth or args.replay or args.no_activate):
        escapes = [("escape", int(n)) for n in args.ti_escape.split(",") if n]
        prefix = "e10,h200,l2,e7,l36" if args.ti_prefix == "jlink" else args.ti_prefix
        for token in filter(None, prefix.split(",")):
            n = int(token[1:])
            if token[0] in "wp":
                # w: a DAP_Delay command of its own; p: one inside the pin packet. Both in
                # microseconds, and never 0, which hangs ARM's firmware.
                escapes.append(("wait" if token[0] == "w" else "pause", n))
            else:
                escapes.append(("escape", n) if token[0] == "e" else ("raw", ("1" if token[0] == "h" else "0") * n))
        steps = (TI_OSCAN1[:1] + ti_steps(args.format, args.switch))[args.ti_from :]
        run_ti_activation(probe, escapes + steps)
        how = "" if args.format == OSCAN1 else f", then format {args.format} selected ({args.switch})"
        print(f"  TI's XDS110 OScan1 sequence (TI E2E 766675){how}")
    elif args.synth:
        program = PinProgram().escape(args.escape_a).clock("1" * args.tap_reset + "00")
        program.escape(args.escape_b).clock("0" * args.idle + CJTAG_SELECT_OSCAN1 + args.raw + "0" * args.pad)
        replay_pins(probe, program.writes)
        print(f"  synthetic activation: {len(program.writes)} pin writes")
        reply = probe.command(bytes([DAP_VENDOR_CJTAG_CONTROL, 2]))
        print(f"  vendor 0x83 02 (frame as OScan1, no activation) -> status {reply[1]}")
    elif args.replay:
        windows = [tuple(float(x) for x in w.split(":")) for w in args.window] or [(args.start, args.until)]
        writes = capture_pin_writes(args.replay, args.tmsc, args.tckc, windows)
        # Clock low before the data moves, data before the clock rises: a clock with TMS low.
        writes += [(0, 0), (1, 0), (0, 1)] * args.pad
        replay_pins(probe, writes)
        print(f"  replayed {len(writes)} pin writes from {args.replay}")
        reply = probe.command(bytes([DAP_VENDOR_CJTAG_CONTROL, 2]))
        print(f"  vendor 0x83 02 (frame as OScan1, no activation) -> status {reply[1]}")
    ones = (1 << 64) - 1 if args.tdi else 0
    if args.pre_tms:
        # One sequence per run of equal TMS, carrying that run's TDI bits; with framing on, every
        # clock is a frame. --pre-tdi gives TDI per clock, otherwise --tdi holds for all of them.
        tdi_bits = args.pre_tdi or str(args.tdi) * len(args.pre_tms)
        if len(tdi_bits) != len(args.pre_tms):
            sys.exit("--pre-tdi needs one bit per --pre-tms bit")
        runs, at = [], 0
        while at < len(args.pre_tms):
            n = min(len(args.pre_tms[at:]) - len(args.pre_tms[at:].lstrip(args.pre_tms[at])), 63)
            tdi = int(tdi_bits[at : at + n][::-1], 2)
            runs.append((n, int(args.pre_tms[at]), False, tdi))
            at += n
        probe.jtag_sequence(runs)
        print(f"  framed TMS {args.pre_tms} TDI {tdi_bits}")
    for attempt in range(args.repeat):
        if args.nreset and attempt:
            # nRESET is bit 7; nothing else on the wire moves.
            probe.command(bytes([DAP_SWJ_PINS, 0x00, 0x80]) + bytes(4))
            time.sleep(0.02)
            probe.command(bytes([DAP_SWJ_PINS, 0x80, 0x80]) + bytes(4))
            time.sleep(0.05)
            print("  pulsed nRESET")
        # A count of zero would encode 64 clocks, so no reset means no sequence at all.
        reset = [(args.reset_clocks, 1, False, ones)] if args.reset_clocks else []
        tdo = probe.jtag_sequence(
            reset
            + [
                (1, 0, False, ones),  # Run-Test/Idle
                (1, 1, False, ones),  # Select-DR-Scan
                (2, 0, False, ones),  # Capture-DR, Shift-DR
                (31, 0, True, ones),
                (1, 1, True, ones),  # the last bit, into Exit1-DR
                (1, 1, False, ones),  # Update-DR
                (1, 0, False, ones),  # Run-Test/Idle
            ]
        )
        idcode = tdo[0] | (tdo[1] << 31)
        verdict = "matches" if idcode == args.expect else f"expected 0x{args.expect:08x}"
        if idcode == 1 << 31:
            verdict += "; TDO equals the TMS sent, so nothing drove the wire"
        print(f"  read {attempt}: IDCODE 0x{idcode:08x} ({verdict})")
    if args.mem:
        icepick_read_cpuid(probe, args)
    if args.tlr_after:
        # Framed TMS-high clocks; the TAP stays in Test-Logic-Reset when the run ends.
        probe.jtag_sequence([(args.tlr_after, 1, False, 0)])
        print(f"  {args.tlr_after} framed TMS-high clocks: Test-Logic-Reset")
    if not args.keep:
        probe.command(bytes([DAP_VENDOR_CJTAG_CONTROL, 0]))
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

    target_power = sub.add_parser("target-power", help="switch the Pro's target supply")
    target_power.add_argument("state", choices=("off", "on", "read"))
    target_power.set_defaults(run=cmd_target_power)

    energy_parser = sub.add_parser("energy", help="the Pro's energy capture: start, read, stop")
    energy_parser.add_argument("action", choices=("start", "latest", "total", "stop", "watch", "calibration"))
    energy_parser.add_argument("--rate", type=int, default=1000, help="pairs per second to start at")
    energy_parser.add_argument("--seconds", type=float, default=5.0, help="how long `watch` runs")
    energy_parser.add_argument("--every", type=float, default=0.5, help="seconds between `watch` samples")
    energy_parser.set_defaults(run=cmd_energy)

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

    dio = sub.add_parser("dio", help="drive and read the Pro's J19 DIO pins")
    dio.add_argument("--mask", type=lambda s: int(s, 0), default=0, help="pins to change, bit n DIOn; 0 only reads")
    dio.add_argument("--outputs", type=lambda s: int(s, 0), default=0, help="which of them drive")
    dio.add_argument("--levels", type=lambda s: int(s, 0), default=0, help="their output levels")
    dio.set_defaults(run=cmd_dio)

    limit = sub.add_parser("limit", help="sweep every achievable rate and check the data")
    limit.add_argument("--cpu-hz", type=int, default=150_000_000, help="the probe's core clock")
    limit.add_argument("--half-bit-cycles", type=float, default=2.5,
                       help="measured non-delay cost of a half bit: 2.5 for ours, 6 for stock")
    limit.add_argument("--words", type=int, default=64, help="transfers per block command")
    limit.add_argument("--blocks", type=int, default=50, help="block commands per rate")
    limit.add_argument("--max-mhz", type=float, default=0.0, help="stop above this wire rate")
    limit.set_defaults(run=cmd_limit)

    core1 = sub.add_parser("core1", help="CPU 1's progress and last fault, from CPU 0")
    core1.add_argument("--queues", action="store_true", help="also the shared queues' state")
    core1.add_argument("--swo", action="store_true", help="also SWO capture's DMA and USART registers")
    core1.add_argument("--sample", action="store_true",
                       help="stop CPU 1 with an NMI to record where it is; reset the probe afterwards")
    core1.set_defaults(run=cmd_core1)

    reboot = sub.add_parser("reboot", help="reset the probe, optionally into the ROM bootloader")
    reboot.add_argument("--bootloader", action="store_true", help="come back in ISP mode, not the firmware")
    reboot.set_defaults(run=cmd_reboot)

    block = sub.add_parser("block", help="repeat a DP read, or an ABORT write, with DAP_TransferBlock")
    block.add_argument("--hz", type=int, default=10000000)
    block.add_argument("--words", type=int, default=64, help="transfers per block command")
    block.add_argument("--blocks", type=int, default=200)
    block.add_argument("--register", choices=("rdbuff", "dpidr", "abort"), default="rdbuff",
                       help="abort writes zero to DP ABORT")
    block.add_argument("--command", choices=("block", "transfer"), default="block",
                       help="DAP_TransferBlock, or the same reads as one DAP_Transfer")
    block.add_argument("--pipeline", type=int, default=1,
                       help="commands to keep in flight; 1 waits for each reply before sending")
    block.add_argument("--queue", type=int, default=0,
                       help="queue this many commands asynchronously instead, so the host never waits")
    block.set_defaults(run=cmd_block)

    swo = sub.add_parser("swo", help="capture SWO through DAP_SWO_Data and save it")
    swo.add_argument("--baud", type=int, default=1_000_000)
    swo.add_argument("--seconds", type=float, default=2.0)
    swo.add_argument("--limit", type=int, default=508, help="bytes to ask for in each DAP_SWO_Data")
    swo.add_argument("--out", default="swo.bin")
    swo.set_defaults(run=cmd_swo)

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
    gen.add_argument("--measure", action="store_true", help="have the probe measure the latency itself")
    gen.add_argument("--hz", type=int, default=0, help="with --measure, the clock to measure at")
    gen.add_argument("--pin", action="store_true", help="stop the probe measuring the latency on its own")
    gen.add_argument("--automatic", action="store_true", help="let the probe measure the latency on its own")
    gen.set_defaults(run=cmd_gen)

    jgen = sub.add_parser("jgen", help="the generated JTAG paths: state, latencies, measurement")
    jgen.add_argument("--two-pin", type=int, default=0, help="set the cJTAG latency, in core cycles")
    jgen.add_argument("--four-wire", type=int, default=0, help="set the JTAG latency, in core cycles")
    jgen.add_argument("--measure", action="store_true", help="have the probe measure the wire in use")
    jgen.add_argument("--pin", action="store_true", help="stop the probe measuring the latencies on its own")
    jgen.add_argument("--automatic", action="store_true", help="let the probe measure the latencies on its own")
    jgen.set_defaults(run=cmd_jgen)
    jlog = sub.add_parser("jlog", help="the JTAG generator's last generations and what each was for")
    jlog.add_argument("--last", type=int, default=16, help="how many, newest back (the probe keeps 16)")
    jlog.set_defaults(run=cmd_jlog)

    cjtag = sub.add_parser("cjtag", help="activate OScan1 and read an IDCODE over cJTAG")
    cjtag.add_argument("--hz", type=int, default=1_000_000, help="TCKC rate to ask for")
    cjtag.add_argument("--ir-length", type=int, default=6, help="the first TAP's IR (ICEPick: 6)")
    cjtag.add_argument("--reset-clocks", type=int, default=9, help="TMS-high clocks in the reset")
    cjtag.add_argument("--repeat", type=int, default=1, help="IDCODE reads after one activation")
    cjtag.add_argument("--expect", type=lambda s: int(s, 0), default=0x3BB4102F)
    cjtag.add_argument("--no-activate", action="store_true", help="drive no activation and leave framing off")
    cjtag.add_argument("--keep", action="store_true", help="leave OScan1 framing on afterwards")
    cjtag.add_argument("--tlr-after", type=int, default=0, help="framed TMS-high clocks at the end of the run")
    cjtag.add_argument("--read", type=lambda v: int(v, 0), action="append", default=[], help="--mem: a word to read, repeatable")
    cjtag.add_argument("--sysctrl", type=lambda v: int(v, 0), default=0x81000080,
                       help="--mem: ICEPick SYSCTRL write; bit 7 is KeepPoweredinTLR (probe-rs sets it)")
    cjtag.add_argument("--nreset", action="store_true", help="pulse nRESET before every read after the first")
    cjtag.add_argument("--replay", help="drive the activation from this capture's CSV instead")
    cjtag.add_argument("--tmsc", type=int, default=0, help="the capture's TMSC channel")
    cjtag.add_argument("--tckc", type=int, default=1, help="the capture's TCKC channel")
    cjtag.add_argument("--start", type=float, default=0.0, help="replay from this capture time")
    cjtag.add_argument("--until", type=float, default=1e9, help="replay up to this capture time")
    cjtag.add_argument("--window", action="append", default=[], help="start:until, repeatable; replaces --start/--until")
    cjtag.add_argument("--nreset-first", action="store_true", help="pulse nRESET before the activation")
    cjtag.add_argument("--settle", type=float, default=50, help="ms to wait after --nreset-first")
    cjtag.add_argument("--mem", action="store_true", help="route the ICEPick to the Cortex-M DAP and read CPUID")
    cjtag.add_argument("--sweep", type=lambda v: [int(float(x) * 1e6) for x in v.split(",")], default=[],
                       help="--mem: MHz list to read a ROM block at, checked against 1 MHz")
    cjtag.add_argument("--sweep-address", type=lambda v: int(v, 0), default=0x1000_0000)
    cjtag.add_argument("--sweep-words", type=int, default=64)
    cjtag.add_argument("--sweep-repeat", type=int, default=20)
    cjtag.add_argument("--join-idle", type=int, default=10, help="--mem: Run-Test/Idle clocks after the TAP selection")
    cjtag.add_argument("--pre-tms", default="", help="TMS bits to send framed before the reads")
    cjtag.add_argument("--tdi", type=int, default=1, choices=(0, 1), help="TDI level for every frame")
    cjtag.add_argument("--pre-tdi", default="", help="TDI bit per --pre-tms bit")
    cjtag.add_argument("--synth", action="store_true", help="drive the activation decoded from the J-Link's")
    cjtag.add_argument("--ti", action="store_true", help="drive TI's XDS110 OScan1 sequence, the default")
    cjtag.add_argument("--ti-escape", default="", help="--ti: escapes to send first, e.g. 10 or 10,7")
    cjtag.add_argument("--ti-prefix", default="",
                       help="--ti: steps first: eN escape, hN/lN TMS-high/low clocks; 'jlink' = e10,h200,l2,e7,l36")
    cjtag.add_argument("--ti-from", type=int, default=0, help="--ti: skip TI's first N steps")
    cjtag.add_argument("--format", type=int, default=OSCAN1, help="--ti: scan format (SCNFMT) to end in: 9 OScan1, 10 OScan2, 13 OScan5, 14 OScan6")
    cjtag.add_argument("--switch", choices=("command", "jscan"), default="command",
                       help="--format: select it with an OScan1 command before the window closes, or in JScan")
    cjtag.add_argument("--escape-a", type=int, default=10, help="synth: edges in the first escape")
    cjtag.add_argument("--tap-reset", type=int, default=200, help="synth: TMS-high clocks after it")
    cjtag.add_argument("--escape-b", type=int, default=7, help="synth: edges in the second escape")
    cjtag.add_argument("--idle", type=int, default=36, help="synth: TMS-low clocks before the commands")
    cjtag.add_argument("--raw", default="", help="synth: TMS clocks sent unframed after the command scans")
    cjtag.add_argument("--pad", type=int, default=0, help="TMS-low clocks after the activation, before framing")
    cjtag.set_defaults(run=cmd_cjtag)

    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
