# /// script
# requires-python = ">=3.11"
# ///
"""The whole gate: host checks, then both compilers' images through the emulator.

`ninja -C <build> check` holds one image to its baselines. This runs that for gcc and clang, each
in the default configuration and with SWD_CODEGEN and JTAG_CODEGEN off, because an optimisation
that holds under one compiler has been seen not to under the other. A generated image also runs its
two-pin and four-wire clocks against targets that answer by time, at every period up to 40 cycles
and three longer ones. It runs the host checks and image builds together, then runs image checks
and timing sweeps together. Each timing sweep simulates its independent clock points in parallel.

Builds go to build-gate/.

    uv run tools/gate.py
"""

import concurrent.futures
import os
import subprocess
import sys
import time

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUILD = os.path.join(ROOT, "build-gate")

HOST_CHECKS = (
    ("Thumb-2 encoders against gas", ["uv", "run", "tools/thumb_test.py"]),
    ("memcpy at every length and offset", ["uv", "run", "tools/mem_test.py"]),
    ("CPU0-to-CPU1 DAP handoff simulator: sanitizers, both compilers", ["uv", "run", "tools/dap_usb_slots_test.py"]),
    ("SWO ring and baud arithmetic: sanitizers, both compilers", ["uv", "run", "tools/swo_ring_test.py"]),
    ("SWD latency measurement against the generator: sanitizers, both compilers",
     ["uv", "run", "tools/swd_latency_test.py"]),
    ("JTAG latency measurement against the generator, both wires: sanitizers, both compilers",
     ["uv", "run", "tools/jtag_latency_test.py"]),
    ("generators: sanitizers, both compilers, QEMU", ["uv", "run", "tools/gen_host.py"]),
)
IMAGES = [(toolchain, codegen) for toolchain in ("gcc", "clang") for codegen in (False, True)]


def name(toolchain: str, codegen: bool) -> str:
    return f"{toolchain}-{'default' if codegen else 'armwire'}"


def run(args: list[str]) -> tuple[bool, str, float]:
    started = time.monotonic()
    done = subprocess.run(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return done.returncode == 0, done.stdout, time.monotonic() - started


def report(label: str, ok: bool, output: str, seconds: float, failures: list[str]) -> None:
    print(f"{'ok  ' if ok else 'FAIL'} {label} ({seconds:.0f} s)", flush=True)
    if not ok:
        failures.append(label)
        print("\n".join("     " + line for line in output.rstrip().splitlines()[-25:]), flush=True)


def build_image(toolchain: str, codegen: bool) -> tuple[str, bool, str, float, str | None, str, bool]:
    directory = os.path.join(BUILD, name(toolchain, codegen))
    flag = "ON" if codegen else "OFF"
    started = time.monotonic()
    ok, output, _ = run(["cmake", "-B", directory, "-G", "Ninja", "-DBOARD=mcu_link",
                         f"-DTOOLCHAIN={toolchain}", f"-DSWD_CODEGEN={flag}",
                         f"-DJTAG_CODEGEN={flag}", "."])
    if ok:
        jobs = max(1, (os.cpu_count() or 1) // len(IMAGES))
        ok, more, _ = run(["ninja", "-C", directory, "-j", str(jobs)])
        output += more
    return (f"build {name(toolchain, codegen)}", ok, output, time.monotonic() - started,
            directory if ok else None, toolchain, codegen)


def main() -> int:
    started = time.monotonic()
    failures: list[str] = []
    built = []
    initial = {}
    with concurrent.futures.ThreadPoolExecutor(max_workers=len(HOST_CHECKS) + len(IMAGES)) as pool:
        initial.update({pool.submit(run, args): label for label, args in HOST_CHECKS})
        initial.update({pool.submit(build_image, toolchain, codegen): None for toolchain, codegen in IMAGES})
        for future in concurrent.futures.as_completed(initial):
            label = initial[future]
            result = future.result()
            if label is None:
                label, ok, output, seconds, directory, toolchain, codegen = result
                if directory is not None:
                    built.append((toolchain, codegen, directory))
            else:
                ok, output, seconds = result
            report(label, ok, output, seconds, failures)

    tasks = [(f"emulator gate {name(toolchain, codegen)}", ["ninja", "-C", directory, "check"])
             for toolchain, codegen, directory in built]
    tasks += [(f"timed {wire} clocks {name(toolchain, codegen)}",
               ["uv", "run", "tools/jtagcheck.py", os.path.join(directory, "mculink_cjtag_core1.elf"), "--wire", wire])
              for toolchain, codegen, directory in built if codegen for wire in ("cjtag", "jtag")]
    with concurrent.futures.ThreadPoolExecutor(max_workers=max(1, len(tasks))) as pool:
        checks = {pool.submit(run, args): label for label, args in tasks}
        for future in concurrent.futures.as_completed(checks):
            report(checks[future], *future.result(), failures)

    if failures:
        print(f"\n{len(failures)} failed: {', '.join(failures)} ({time.monotonic() - started:.1f} s)")
        return 1
    print(f"\nall clear ({time.monotonic() - started:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
