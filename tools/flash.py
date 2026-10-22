#!/usr/bin/env -S uv run --script
# /// script
# requires-python = ">=3.11"
# dependencies = ["pyusb"]
# ///
"""Flash this firmware onto an MCU-Link that is sitting in ISP mode, and check what came up.

A unit enters ISP mode with the **ISP jumper fitted before it is plugged in**: the ROM bootloader
samples the pin at reset, so the jumper decides whether the bootloader or the firmware runs. The
whole sequence is: fit the jumper, plug in, run this, unplug, remove the jumper, plug in.

    uv run tools/flash.py                    # flash build/mculink_cjtag.elf
    uv run tools/flash.py --check            # just say what every attached unit is running
    uv run tools/flash.py --cycle --image <LinkServer>/MCU-LINK/probe_firmware/MCU-LINK_CMSIS-DAP_V3_172.s19

rblhost takes S-record only, so the ELF is converted on the way through. It also addresses a probe
by VID:PID alone, which is why this refuses to run with two units in ISP mode at once -- it cannot
tell them apart and would flash whichever answers.

A flashed unit is deliberately not reset. With the jumper still fitted a reset returns it to the
bootloader, so it stays there until it is unplugged.

Recovery from a bad image is the same path with LinkServer's stock image.

rblhost comes with LinkServer, in its MCU-LINK/bin directory: put it on the PATH or name it in
RBLHOST.
"""

import argparse
import glob
import os
import pathlib
import shutil
import subprocess
import sys
import time

RBLHOST = pathlib.Path(os.environ.get("RBLHOST") or shutil.which("rblhost") or "rblhost")
OBJCOPY = "arm-none-eabi-objcopy"
ISP = ("1fc9", "0021")
MINI_ISP = ("1fc9", "0022")
PROBE = ("1fc9", "0143")
# Stock's way into the bootloader, as LinkServer's `probe update` drives it on V3.172; versions
# before V3.122 have no jumper-free update.
STOCK_ISP = 0x81
STOCK_ISP_ACCEPTED = bytes([0x81, 0x00, 0x01])
STOCK_ISP_SINCE = (3, 122)


def read(device: pathlib.Path, name: str) -> str:
    try:
        return (device / name).read_text().strip()
    except OSError:
        return ""


def present(ids: tuple[str, str]) -> list[pathlib.Path]:
    found = []
    for path in glob.glob("/sys/bus/usb/devices/*/idVendor"):
        device = pathlib.Path(path).parent
        if (read(device, "idVendor"), read(device, "idProduct")) == ids:
            found.append(device)
    return sorted(found)


def check() -> int:
    """What every attached MCU-Link is running, without touching any of them."""
    isp = present(ISP) + present(MINI_ISP)
    firmware = present(PROBE)
    for device in isp:
        print(f"{device.name}: in ISP mode, waiting for an image")
    for device in firmware:
        print(f"{device.name}: in firmware mode")

    if not firmware:
        if not isp:
            print("nothing attached")
        return 0

    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    import dap  # noqa: PLC0415  -- only needed for this path, and it needs pyusb

    try:
        probes = dap.find()
    except ValueError as error:
        print(f"  DAP details unavailable: {error}")
        return 0
    for probe in probes:
        try:
            reply = probe.command(bytes([0x82]))
            body = reply[1:]
            if body[:5] == b"CJTAG":
                length = body[7]
                build = body[8 : 8 + length].decode(errors="replace")
                flags = body[6]
                print(f"{probe.serial}: ours, build {build}, flags 0x{flags:02x}")
            else:
                print(f"{probe.serial}: stock V{firmware_version(probe)} -- {probe.product}")
        except RuntimeError:
            # Stock V3.172 echoes a bare 0x82; a firmware answering ID_DAP_Invalid lands here.
            print(f"{probe.serial}: stock V{firmware_version(probe)} -- {probe.product}")
        except Exception as error:
            print(f"{probe.serial}: did not answer vendor 0x82 ({error})")
        probe.release()
    return 0


def firmware_version(probe) -> str:
    """DAP_Info's product firmware version, which stock fills with its release number."""
    return probe.info(0x09).rstrip(b"\x00").decode(errors="replace")


def release_number(version: str) -> tuple[int, int] | None:
    """Stock's `3.172` as `(3, 172)`; None for anything else."""
    major, _, minor = version.partition(".")
    try:
        return int(major), int(minor)
    except ValueError:
        return None


def wait_for(ids: tuple[str, str], seconds: float = 15.0) -> list[pathlib.Path]:
    """Poll until something shows up as these ids, because enumeration is not instant."""
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        found = present(ids)
        if found:
            return found
        time.sleep(0.25)
    return []


def cycle(elf: pathlib.Path, serial: str | None) -> int:
    """Reboot a unit running ours or stock into the bootloader, flash, reset, and say what came back.

    No jumper and no hands. Ours takes a USB control request that asks CPU 0 to enter the ROM
    bootloader, without relying on CPU 1's DAP worker. Stock takes what LinkServer's own jumper-free
    update sends, seen on the bus with usbmon: vendor command 0x81 with 0x01, then DAP_Disconnect,
    after which the unit re-enumerates as the bootloader about two seconds later. The ROM clears the
    boot mode on entry, so `rblhost reset` returns the unit to its firmware rather than to the
    bootloader again.
    """
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    import dap  # noqa: PLC0415

    # The wait below takes the first bootloader it sees, so one already there would be flashed in
    # this unit's place.
    waiting = present(ISP) + present(MINI_ISP)
    if waiting:
        names = ", ".join(device.name for device in waiting)
        sys.exit(f"a unit is already in ISP mode ({names}); flash it alone first")
    probes = [p for p in dap.find() if serial is None or p.serial == serial]
    if len(probes) != 1:
        names = ", ".join(p.serial for p in dap.find())
        sys.exit(f"need exactly one probe to reflash; attached: {names or 'none'}. Use --serial")
    probe = probes[0]
    try:
        ours = probe.command(bytes([0x82]))[1:6] == b"CJTAG"
    except RuntimeError:  # a firmware answering 0x82 with ID_DAP_Invalid
        ours = False
    if not ours:
        version = firmware_version(probe)
        number = release_number(version)
        if number is None or number < STOCK_ISP_SINCE:
            probe.release()
            sys.exit(
                f"{probe.serial}: stock V{version} has no jumper-free update before V3.122. "
                "Use the ISP jumper"
            )
    print(f"{probe.serial}: asking {'our' if ours else 'the stock'} firmware for the ROM bootloader")
    try:
        if ours:
            probe.reboot(bootloader=True)
        else:
            reply = probe.command(bytes([STOCK_ISP, 0x01]))
            if reply[:3] != STOCK_ISP_ACCEPTED:
                raise RuntimeError(f"answered {reply[:3].hex()}, not {STOCK_ISP_ACCEPTED.hex()}")
            probe.disconnect()
    except Exception as error:
        probe.release()
        sys.exit(f"the probe did not accept the request ({error}). Use the ISP jumper")
    probe.release()

    if not wait_for(ISP):
        sys.exit("the unit never came back as 1fc9:0021. Use the ISP jumper")
    if flash(elf) != 0:
        return 1

    print("resetting out of the bootloader")
    rblhost("reset")
    if not wait_for(PROBE):
        sys.exit("the unit never came back as a probe. Fit the ISP jumper and flash by hand")
    time.sleep(1.0)
    return check()


def rblhost(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(RBLHOST), "--usb", "0x1fc9,0x0021", *args],
        capture_output=True,
        text=True,
        timeout=600,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--elf", "--image", dest="elf", type=pathlib.Path,
                        default=pathlib.Path("build/mculink_cjtag.elf"),
                        help="our ELF, or an S-record such as LinkServer's stock image")
    parser.add_argument("--check", action="store_true", help="report, flash nothing")
    parser.add_argument("--cycle", action="store_true",
                        help="reboot a unit running ours or stock, flash it, reset it: no jumper")
    parser.add_argument("--serial", help="pick one unit when several are attached")
    args = parser.parse_args()

    if args.check:
        return check()

    if not RBLHOST.exists():
        sys.exit(f"{RBLHOST} not found; LinkServer provides it")
    if not args.elf.exists():
        sys.exit(f"{args.elf} not found; build first")

    if args.cycle:
        return cycle(args.elf, args.serial)

    return flash(args.elf)


def flash(elf: pathlib.Path) -> int:
    waiting = present(ISP)
    if present(MINI_ISP):
        sys.exit("an MCU-Link mini is in ISP mode; this image is for the full MCU-Link")
    if not waiting:
        sys.exit(
            "no unit in ISP mode. Fit the ISP jumper, then plug the unit in -- the bootloader\n"
            "samples the pin at reset, so a jumper fitted afterwards does nothing."
        )
    if len(waiting) > 1:
        names = ", ".join(device.name for device in waiting)
        sys.exit(f"{len(waiting)} units in ISP mode ({names}); rblhost cannot tell them apart")
    port = waiting[0].name

    # An S-record goes as it is, which is how LinkServer's stock image arrives.
    srec = elf
    if elf.suffix != ".s19":
        srec = elf.with_suffix(".s19")
        subprocess.run([OBJCOPY, "-O", "srec", str(elf), str(srec)], check=True)
    print(f"{port}: flashing {srec} ({srec.stat().st_size} bytes of S-record)")

    # The bootloader enumerates before it will answer, so this is a poll and not a single ask.
    for _ in range(20):
        answered = rblhost("get-property", "1")
        if "Success." in answered.stdout:
            break
        time.sleep(0.5)
    else:
        sys.exit(f"{port}: the ISP bootloader did not answer: {answered.stdout.strip()[-200:]}")

    done = rblhost("flash-image", str(srec), "erase", "0")
    if done.returncode != 0 or "completed successfully" not in done.stdout:
        sys.exit(f"{port}: rblhost exit {done.returncode}: {(done.stdout + done.stderr).strip()[-400:]}")

    print(f"{port}: written")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
