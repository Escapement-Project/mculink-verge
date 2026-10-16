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

rblhost takes S-record only, so the ELF is converted on the way through. It also addresses a probe
by VID:PID alone, which is why this refuses to run with two units in ISP mode at once -- it cannot
tell them apart and would flash whichever answers.

A flashed unit is deliberately not reset. With the jumper still fitted a reset returns it to the
bootloader, so it stays there until it is unplugged.

Recovery from a bad image is the same path with LinkServer's stock image.
"""

import argparse
import glob
import pathlib
import subprocess
import sys

RBLHOST = pathlib.Path("/opt/linkserver/MCU-LINK/bin/rblhost")
OBJCOPY = "arm-none-eabi-objcopy"
ISP = ("1fc9", "0021")
MINI_ISP = ("1fc9", "0022")
PROBE = ("1fc9", "0143")


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
    sys.path.insert(0, str(pathlib.Path(__file__).parent))
    import dap  # noqa: PLC0415  -- only needed for this path, and it needs pyusb

    isp = present(ISP) + present(MINI_ISP)
    for device in isp:
        print(f"{device.name}: in ISP mode, waiting for an image")
    probes = dap.find()
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
                print(f"{probe.serial}: stock -- {probe.product}")
        except Exception as error:
            print(f"{probe.serial}: did not answer vendor 0x82 ({error})")
        probe.release()
    if not isp and not probes:
        print("nothing attached")
    return 0


def rblhost(*args: str) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        [str(RBLHOST), "--usb", "0x1fc9,0x0021", *args],
        capture_output=True,
        text=True,
        timeout=600,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--elf", type=pathlib.Path, default=pathlib.Path("build/mculink_cjtag.elf"))
    parser.add_argument("--check", action="store_true", help="report, flash nothing")
    args = parser.parse_args()

    if args.check:
        return check()

    if not RBLHOST.exists():
        sys.exit(f"{RBLHOST} not found; LinkServer provides it")
    if not args.elf.exists():
        sys.exit(f"{args.elf} not found; build first")

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

    srec = args.elf.with_suffix(".s19")
    subprocess.run([OBJCOPY, "-O", "srec", str(args.elf), str(srec)], check=True)
    print(f"{port}: flashing {srec} ({srec.stat().st_size} bytes of S-record)")

    answered = rblhost("get-property", "1")
    if "Success." not in answered.stdout:
        sys.exit(f"{port}: the ISP bootloader did not answer: {answered.stdout.strip()[-200:]}")

    done = rblhost("flash-image", str(srec), "erase", "0")
    if done.returncode != 0 or "completed successfully" not in done.stdout:
        sys.exit(f"{port}: rblhost exit {done.returncode}: {(done.stdout + done.stderr).strip()[-400:]}")

    print(f"{port}: written. Unplug it, remove the ISP jumper, plug it back in.")
    print("Then: uv run tools/flash.py --check")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
