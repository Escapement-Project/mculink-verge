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
DAP_SWJ_CLOCK = 0x11
DAP_SWJ_SEQUENCE = 0x12

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

    args = parser.parse_args()
    return args.run(args)


if __name__ == "__main__":
    raise SystemExit(main())
