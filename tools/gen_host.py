# /// script
# requires-python = ">=3.11"
# ///
"""Build the SWD and two-pin generators away from the firmware and check that every build agrees.

The generators are ordinary C whose output is code, so a compiler that miscompiles one, or
undefined behaviour that one compiler exploits and another does not, changes the wire without
failing to build. Four builds sweep the same SWD and JTAG configurations, each without a stall and
with one, and print a hash of each result:

- the host, under gcc and clang, with AddressSanitizer and UndefinedBehaviorSanitizer, so any
  undefined behaviour stops the run where it happens;
- QEMU's Cortex-M33 board (mps2-an505), under gcc and clang, with the firmware's own flags.

All four have to print the same thing. The sources are copied next to tools/gen_host's stand-in
headers, which the generator's quoted includes would otherwise resolve to src/.

    uv run tools/gen_host.py
"""

import argparse
import hashlib
from pathlib import Path
import concurrent.futures
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.join(HERE, "..", "src")
HARNESS = os.path.join(HERE, "gen_host")

GEN_BASE = ["-DSWD_GEN_BASE=0x38100000U", "-DJTAG_GEN_BASE=0x38100000U"]
# CPU 1's flags, from CMakeLists.txt, since the generators run on CPU 1.
M33 = ["-mcpu=cortex-m33+nodsp+nofp", "-mthumb", "-mfloat-abi=soft", "-mno-unaligned-access", "-O3",
       "-fno-strict-aliasing", "-ffunction-sections", "-fdata-sections"]


def run(args: list[str], cwd: str, merge: bool = False, env: dict[str, str] | None = None) -> str:
    done = subprocess.run(args, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT if merge else subprocess.PIPE, text=True, env=env)
    if done.returncode != 0:
        raise SystemExit(f"{' '.join(args)}\n{done.stdout}{done.stderr or ''}")
    return done.stdout


def host(cc: str, work: str) -> str:
    exe = os.path.join(work, f"host-{cc}")
    run([cc, "-std=gnu23", "-g", "-O2", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", *GEN_BASE, "-I.",
         "swd_gen.c", "jtag_gen.c", "gen.c", "gen_main.c", "host_main.c", "-o", exe], work)
    env = os.environ.copy()
    env["ASAN_OPTIONS"] = ":".join(filter(None, (env.get("ASAN_OPTIONS"), "detect_leaks=0")))
    return run([exe], work, env=env)


def qemu(cc: str, work: str) -> str:
    elf = os.path.join(work, f"qemu-{cc}.elf")
    if cc == "gcc":
        compile_ = ["arm-none-eabi-gcc", "-std=gnu23", *M33, *GEN_BASE, "-I."]
        lto = ["-flto=auto", "-fno-fat-lto-objects"]
        libc = ["-ffreestanding", "-fno-builtin", "-fno-tree-loop-distribute-patterns"]
        link = ["arm-none-eabi-gcc", *M33, "-flto=auto", "-nostdlib", "-Wl,-T,qemu.ld", "-Wl,--gc-sections"]
        libs = ["-lgcc"]
    else:
        compile_ = ["clang", "-std=gnu23", "--target=arm-none-eabi", *M33, *GEN_BASE, "-I."]
        lto = ["-flto=thin"]
        libc = ["-ffreestanding", "-fno-builtin"]
        link = ["clang", "--target=arm-none-eabi", *M33, "-flto=thin", "-nostdlib", "-fuse-ld=lld",
                "-Wl,-T,qemu.ld", "-Wl,--gc-sections", "-Wl,-e,reset"]
        libs = []
    objects = []
    for source, flags in (("swd_gen.c", lto), ("jtag_gen.c", lto), ("gen.c", lto), ("gen_main.c", lto), ("qemu_start.c", []), ("libc_min.c", libc)):
        obj = os.path.join(work, f"{cc}-{source}.o")
        run([*compile_, *flags, "-c", source, "-o", obj], work)
        objects.append(obj)
    run([*link, *objects, *libs, "-o", elf], work)
    return run(["timeout", "600", "qemu-system-arm", "-M", "mps2-an505", "-nographic", "-semihosting",
                "-kernel", elf], work, merge=True)  # semihosting writes to stderr


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--jobs", type=int, choices=(1, 2, 4), default=1,
                    help="independent compiler variants to run at once")
    ap.add_argument("--output", type=Path, help="save the agreed configuration hashes for a later comparison")
    ap.add_argument("--reference", type=Path, help="require the sweep to match a saved output exactly")
    args = ap.parse_args()
    for tool in ("gcc", "clang", "arm-none-eabi-gcc", "qemu-system-arm"):
        if shutil.which(tool) is None:
            raise SystemExit(f"{tool} is not installed")
    with tempfile.TemporaryDirectory() as work:
        for name in ("swd_gen.c", "swd_gen.h", "jtag_gen.c", "jtag_gen.h", "gen.c", "gen.h", "hot.h", "thumb.h"):
            shutil.copy(os.path.join(SRC, name), work)
        for name in os.listdir(HARNESS):
            shutil.copy(os.path.join(HARNESS, name), work)
        results = {}
        checks = (
            ("host gcc, ASan+UBSan", host, "gcc"),
            ("host clang, ASan+UBSan", host, "clang"),
            ("Cortex-M33 gcc, QEMU", qemu, "gcc"),
            ("Cortex-M33 clang, QEMU", qemu, "clang"),
        )
        if args.jobs == 1:
            for label, fn, cc in checks:
                results[label] = fn(cc, work)
                print(f"  {label}: {len(results[label].splitlines())} configurations")
        else:
            with concurrent.futures.ThreadPoolExecutor(max_workers=args.jobs) as pool:
                futures = {pool.submit(fn, cc, work): label for label, fn, cc in checks}
                for future in concurrent.futures.as_completed(futures):
                    label = futures[future]
                    results[label] = future.result()
                    print(f"  {label}: {len(results[label].splitlines())} configurations")
    reference = results["host gcc, ASan+UBSan"]
    lines = len(reference.splitlines())
    if lines == 0:
        raise SystemExit("the sweep printed nothing")
    differ = [label for label, out in results.items() if out != reference]
    if differ:
        for label in differ:
            ours = results[label].splitlines()
            first = next((i for i, (a, b) in enumerate(zip(ours, reference.splitlines())) if a != b), len(ours))
            print(f"  {label} differs from host gcc first at configuration {first}")
        return 1
    if args.reference and args.reference.read_text() != reference:
        raise SystemExit(f"generated output differs from {args.reference}")
    if args.output:
        args.output.write_text(reference)
    print(f"  sweep sha256 {hashlib.sha256(reference.encode()).hexdigest()}")
    built = sum(1 for line in reference.splitlines() if line.split()[2] == "1")
    transfers = sum(1 for line in reference.splitlines() if line.startswith("4a") and line.split()[-1] == "1")
    print(f"  all four agree on {lines} configurations ({built} built, {lines - built} refused;"
          f" JTAG transfers in {transfers})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
