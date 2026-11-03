# /// script
# requires-python = ">=3.11"
# ///
"""The whole gate: host checks and both compilers' images through the emulator.

`ninja -C <build> check` holds one image to its baselines. This runs that for gcc and clang, each
in the default configuration and with SWD_CODEGEN and JTAG_CODEGEN off, because an optimisation
that holds under one compiler has been seen not to under the other. A generated image also runs its
two-pin and four-wire clocks against targets that answer by time, at every period up to 40 cycles
and three longer ones, all of its clocks the same way built for energy capture's stall, and every
one of tools/equiv.py's matrices against the same compiler's ARM paths. It runs the host
checks and image builds together, then runs image checks, timing sweeps and the equivalence
matrices together. Each timing sweep simulates its independent clock points in parallel.

The parallel stages share a CPU budget across host checks, builds, emulator wire checks, timed
sweeps and the equivalence matrices. Builds go to build-gate/.

CI can run the host checks separately. For firmware, it can select one compiler or one image
configuration.

    uv run tools/gate.py
    uv run tools/gate.py --part host
    uv run tools/gate.py --part firmware --toolchain clang
    uv run tools/gate.py --part all --toolchain gcc
    uv run tools/gate.py --part firmware --toolchain gcc --codegen on
"""

import argparse
import concurrent.futures
import os
import re
import subprocess
import sys
import time
from host_resources import available_cpus

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
BUILD = os.path.join(ROOT, "build-gate")
GENERATOR_CHECK = "generators: sanitizers, both compilers, QEMU"

HOST_CHECKS = (
    ("Linked stack analysis fixtures", ["uv", "run", "tools/stack_usage_test.py"]),
    ("Schedule insertion: sanitizers, both compilers", ["uv", "run", "tools/gen_schedule_test.py"]),
    ("Block request/result lengths: sanitizers, both compilers", ["uv", "run", "tools/dap_block_test.py"]),
    ("JTAG command loop: sanitizers, both compilers", ["uv", "run", "tools/jtag_transfer_test.py"]),
    ("Console line coding: sanitizers, both compilers", ["uv", "run", "tools/vcom_line_test.py"]),
    ("Energy samples and accumulation: sanitizers, both compilers", ["uv", "run", "tools/energy_samples_test.py"]),
    ("Energy stream messages and timeline: sanitizers, both compilers", ["uv", "run", "tools/energy_stream_test.py"]),
    ("Energy converter vectors as committed: sanitizers, both compilers", ["uv", "run", "tools/energy_vectors.py"]),
    ("Unsigned byte decoding: sanitizers, both compilers", ["uv", "run", "tools/byte_io_test.py"]),
    ("DAP packet framing: sanitizers, both compilers", ["uv", "run", "tools/dap_packet_test.py"]),
    ("MS OS 2.0 descriptor sets: sanitizers, both compilers", ["uv", "run", "tools/ms_os_20_test.py"]),
    ("CPU quota detection for container worker budgets", ["uv", "run", "tools/host_resources_test.py"]),
    ("Thumb-2 encoders against gas", ["uv", "run", "tools/thumb_test.py"]),
    ("memcpy at every length and offset", ["uv", "run", "tools/mem_test.py"]),
    ("CPU0-to-CPU1 DAP handoff simulator: sanitizers, both compilers", ["uv", "run", "tools/dap_usb_slots_test.py"]),
    ("SWO ring and baud arithmetic: sanitizers, both compilers", ["uv", "run", "tools/swo_ring_test.py"]),
    ("SWD latency measurement against the generator: sanitizers, both compilers",
     ["uv", "run", "tools/swd_latency_test.py"]),
    ("JTAG latency measurement against the generator, both wires: sanitizers, both compilers",
     ["uv", "run", "tools/jtag_latency_test.py"]),
    (GENERATOR_CHECK, ["uv", "run", "tools/gen_host.py"]),
)
IMAGES = [(toolchain, codegen) for toolchain in ("gcc", "clang") for codegen in (False, True)]
TIMED_WIRES = ("cjtag", "cjtag2", "cjtag5", "cjtag6", "jtag")


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


def unaligned_access_allowed(directory: str) -> list[str]:
    """What compiles C or links an image with unaligned access left on (CMakeLists.txt says why
    nothing may). TinyUSB's board support has replaced a library's options before."""
    ok, listing, _ = run(["ninja", "-C", directory, "-t", "commands", "mculink_cjtag.elf"])
    if not ok:
        return ["ninja could not list the build's commands"]
    found = []
    for command in listing.splitlines():
        compiled = re.search(r"\s-c\s+(\S+\.c)(\s|$)", command)
        linked = re.search(r"\s-o\s+(\S*mculink_cjtag(?:_core1)?\.elf)(\s|$)", command)
        what = compiled or linked
        if what and command.rfind("-mno-unaligned-access") <= command.rfind("-munaligned-access"):
            found.append(os.path.relpath(what.group(1), ROOT) if compiled else what.group(1))
    return found


def build_image(
    toolchain: str, codegen: bool, jobs: int, cycles_jobs: int
) -> tuple[str, bool, str, float, str | None, str, bool]:
    directory = os.path.join(BUILD, name(toolchain, codegen))
    flag = "ON" if codegen else "OFF"
    check_jobs = cycles_jobs if codegen else min(2, cycles_jobs)
    started = time.monotonic()
    configure = ["cmake", "-B", directory, "-G", "Ninja", "-DBOARD=mcu_link",
                 f"-DTOOLCHAIN={toolchain}", f"-DSWD_CODEGEN={flag}", f"-DJTAG_CODEGEN={flag}",
                 f"-DCYCLES_CHECK_JOBS={check_jobs}"]
    if toolchain == "clang" and not codegen:
        configure.append("-DCMAKE_EXPORT_COMPILE_COMMANDS=ON")
    configure.append(".")
    ok, output, _ = run(configure)
    if ok:
        ok, more, _ = run(["ninja", "-C", directory, "-j", str(jobs)])
        output += more
    if ok:
        allowed = unaligned_access_allowed(directory)
        if allowed:
            ok = False
            output += "built with unaligned access allowed: " + ", ".join(allowed) + "\n"
    return (f"build {name(toolchain, codegen)}", ok, output, time.monotonic() - started,
            directory if ok else None, toolchain, codegen)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--part", choices=("all", "host", "firmware"), default="all")
    parser.add_argument("--toolchain", choices=("gcc", "clang"),
                        help="limit the firmware part to one compiler")
    parser.add_argument("--codegen", choices=("on", "off"),
                        help="limit the firmware part to one code-generation configuration")
    args = parser.parse_args()
    if args.toolchain and args.part == "host":
        parser.error("--toolchain cannot be used with --part host")
    if args.codegen and args.part == "host":
        parser.error("--codegen cannot be used with --part host")

    os.environ["MCULINK_CHECK_ALLOCATIONS"] = "1"
    started = time.monotonic()
    failures: list[str] = []
    built = []
    initial = {}
    cpu_count = available_cpus()
    run_host = args.part in ("all", "host")
    images = [image for image in IMAGES
              if (not args.toolchain or image[0] == args.toolchain)
              and (not args.codegen or image[1] == (args.codegen == "on"))]
    if args.part == "host":
        images = []
    elif args.part == "all" and not args.toolchain:
        images = IMAGES
    generated_images = sum(codegen for _, codegen in images)
    generator_jobs = 1
    if run_host and cpu_count >= 4:
        generator_jobs = 2
    if run_host and cpu_count >= 8:
        generator_jobs = 4
    host_check_count = len(HOST_CHECKS) if run_host else 0
    build_jobs = max(1, (cpu_count - min(host_check_count, cpu_count) - (generator_jobs - 1))
                     // max(1, len(images)))
    # Wide runners share one clock pool; small runners reuse outer task slots per wire.
    pool_all_wires = cpu_count >= 16
    if pool_all_wires and generated_images:
        cycles_jobs = min(6, max(1, cpu_count // 5))
    elif generated_images:
        cycles_jobs = min(6, max(1, cpu_count // max(1, generated_images * len(TIMED_WIRES))))
    else:
        cycles_jobs = 1
    reserved_generator_workers = generator_jobs - 1
    host_checks = []
    if run_host:
        host_checks = [
            (label, [*check_args, "--jobs", str(generator_jobs)]
             if label == GENERATOR_CHECK else check_args)
            for label, check_args in HOST_CHECKS
        ]
    initial_workers = min(len(host_checks) + len(images), max(1, cpu_count - reserved_generator_workers))
    with concurrent.futures.ThreadPoolExecutor(max_workers=initial_workers) as pool:
        initial.update({pool.submit(run, args): label for label, args in host_checks})
        initial.update({pool.submit(build_image, toolchain, codegen, build_jobs, cycles_jobs): None
                        for toolchain, codegen in images})
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

    image_checks = [(f"emulator gate {name(toolchain, codegen)}", ["ninja", "-C", directory, "check"])
                    for toolchain, codegen, directory in built]
    timed_images = [(toolchain, directory) for toolchain, codegen, directory in built if codegen]
    timed_checks = []
    if pool_all_wires and timed_images:
        timed_checks.append((
            "timed clocks generated images",
            ["uv", "run", "tools/jtagcheck.py",
             *(os.path.join(directory, "mculink_cjtag_core1.elf") for _, directory in timed_images),
             "--wire", *TIMED_WIRES],
        ))
    elif timed_images:
        timed_checks.extend(
            (f"timed {wire} clocks {name(toolchain, True)}",
             ["uv", "run", "tools/jtagcheck.py", os.path.join(directory, "mculink_cjtag_core1.elf"),
              "--wire", wire])
            for toolchain, directory in timed_images
            for wire in TIMED_WIRES
        )
    # Built for a stall, as energy capture runs them; nothing else here does. Two-pin builds ignore
    # the stall, so their check is that no sample comes after the fall that ends its window.
    timed_checks.extend(
        check
        for toolchain, directory in timed_images
        for check in (
            (f"timed SWD clocks with a stall {name(toolchain, True)}",
             ["uv", "run", "tools/gencheck.py", os.path.join(directory, "mculink_cjtag_core1.elf"), "--stall", "2"]),
            (f"timed JTAG clocks with a stall {name(toolchain, True)}",
             ["uv", "run", "tools/jtagcheck.py", os.path.join(directory, "mculink_cjtag_core1.elf"), "--wire",
              *TIMED_WIRES, "--stall", "2"]),
        )
    )
    image_jobs = sum(cycles_jobs if codegen else min(2, cycles_jobs) for _, codegen, _ in built)
    jobs_per_timed_check = max(
        1,
        (cpu_count - image_jobs) // max(1, len(timed_checks)),
    )
    if pool_all_wires:
        # At 16 CPUs, balancing the two pools around ten workers shortened the full gate.
        jobs_per_timed_check = max(10, jobs_per_timed_check)
    timed_checks = [(label, [*args, "--jobs", str(jobs_per_timed_check)])
                    for label, args in timed_checks]
    directories = {(toolchain, codegen): directory for toolchain, codegen, directory in built}
    equiv_checks = [
        (f"equivalence {toolchain}, every matrix",
         ["uv", "run", "tools/equiv.py", os.path.join(directories[(toolchain, False)], "mculink_cjtag_core1.elf"),
          os.path.join(directories[(toolchain, True)], "mculink_cjtag_core1.elf"),
          "--wire", "all", "--ignore-samples", "--jobs", str(max(1, cpu_count // 2))])
        for toolchain in ("gcc", "clang") if (toolchain, False) in directories and (toolchain, True) in directories
    ]
    tasks = image_checks + timed_checks + equiv_checks
    if tasks:
        with concurrent.futures.ThreadPoolExecutor(max_workers=min(len(tasks), cpu_count)) as pool:
            checks = {pool.submit(run, command): label for label, command in tasks}
            for future in concurrent.futures.as_completed(checks):
                report(checks[future], *future.result(), failures)

    for toolchain, codegen, directory in built:
        if toolchain == "clang" and codegen:
            report("clang-tidy first-party C", *run([sys.executable, "tools/clang_tidy.py", directory]), failures)

    if failures:
        print(f"\n{len(failures)} failed: {', '.join(failures)} ({time.monotonic() - started:.1f} s)")
        return 1
    print(f"\nall clear ({time.monotonic() - started:.1f} s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
