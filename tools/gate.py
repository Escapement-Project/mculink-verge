# /// script
# requires-python = ">=3.11"
# ///
"""The whole gate: host checks, then both compilers' images through the emulator.

`ninja -C <build> check` holds one image to its baselines. This runs that for gcc and clang, each
in the default configuration and with SWD_CODEGEN, because an optimisation that holds under one
compiler has been seen not to under the other. Before the images it runs the checks that need no
image: the Thumb-2 encoders against the assembler, memcpy at every length and offset, and the
transfer generator under sanitizers and on an emulated Cortex-M33 with both compilers.

Builds go to build-gate/. The image checks run in parallel; each is single-threaded.

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
    ("generator: sanitizers, both compilers, QEMU", ["uv", "run", "tools/gen_host.py"]),
)
IMAGES = [(toolchain, codegen) for toolchain in ("gcc", "clang") for codegen in (False, True)]


def name(toolchain: str, codegen: bool) -> str:
    return f"{toolchain}-{'codegen' if codegen else 'default'}"


def run(args: list[str]) -> tuple[bool, str, float]:
    started = time.monotonic()
    done = subprocess.run(args, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    return done.returncode == 0, done.stdout, time.monotonic() - started


def report(label: str, ok: bool, output: str, seconds: float, failures: list[str]) -> None:
    print(f"{'ok  ' if ok else 'FAIL'} {label} ({seconds:.0f} s)", flush=True)
    if not ok:
        failures.append(label)
        print("\n".join("     " + line for line in output.rstrip().splitlines()[-25:]), flush=True)


def main() -> int:
    failures: list[str] = []
    for label, args in HOST_CHECKS:
        report(label, *run(args), failures)

    built = []
    for toolchain, codegen in IMAGES:
        directory = os.path.join(BUILD, name(toolchain, codegen))
        ok, output, seconds = run(["cmake", "-B", directory, "-G", "Ninja", "-DBOARD=mcu_link",
                                   f"-DTOOLCHAIN={toolchain}", f"-DSWD_CODEGEN={'ON' if codegen else 'OFF'}", "."])
        if ok:
            ok, more, extra = run(["ninja", "-C", directory])
            output, seconds = output + more, seconds + extra
        report(f"build {name(toolchain, codegen)}", ok, output, seconds, failures)
        if ok:
            built.append((toolchain, codegen, directory))

    with concurrent.futures.ThreadPoolExecutor(max_workers=len(built) or 1) as pool:
        checks = {pool.submit(run, ["ninja", "-C", directory, "check"]): (toolchain, codegen)
                  for toolchain, codegen, directory in built}
        for future in concurrent.futures.as_completed(checks):
            toolchain, codegen = checks[future]
            report(f"emulator gate {name(toolchain, codegen)}", *future.result(), failures)

    if failures:
        print(f"\n{len(failures)} failed: {', '.join(failures)}")
        return 1
    print("\nall clear")
    return 0


if __name__ == "__main__":
    sys.exit(main())
