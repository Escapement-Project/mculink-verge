# /// script
# requires-python = ">=3.11"
# ///
"""Run the latency measurement (src/latency.h) against the real JTAG generator on both wires, with
a target modelled by the latency it answers at, under gcc and clang sanitizers.

The generator builds here as it does for tools/gen_host.py, next to that harness's stand-in headers.
"""

import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCES = ("jtag_gen.c", "jtag_gen.h", "gen.c", "gen.h", "hot.h", "thumb.h", "latency.h")
HARNESS = ("DAP_config.h", "DAP.h")
GEN_BASE = ["-DSWD_GEN_BASE=0x38100000U", "-DJTAG_GEN_BASE=0x38100000U"]


def run(args: list[str], cwd: str, env: dict[str, str]) -> str:
    done = subprocess.run(args, cwd=cwd, env=env, text=True, capture_output=True)
    if done.returncode:
        raise SystemExit(f"{' '.join(args)}\n{done.stdout}{done.stderr}")
    return done.stdout


def main() -> int:
    with tempfile.TemporaryDirectory() as work:
        for name in SOURCES:
            shutil.copy(ROOT / "src" / name, work)
        for name in HARNESS:
            shutil.copy(ROOT / "tools" / "gen_host" / name, work)
        shutil.copy(pathlib.Path(__file__).with_name("jtag_latency_test.c"), work)
        for compiler in ("gcc", "clang"):
            if shutil.which(compiler) is None:
                raise SystemExit(f"{compiler} is not installed")
            executable = os.path.join(work, compiler)
            run([compiler, "-std=c11", "-O2", "-fsanitize=address,undefined", "-fno-sanitize-recover=all", *GEN_BASE,
                 "-I.", "jtag_gen.c", "gen.c", "jtag_latency_test.c", "-o", executable], work, os.environ.copy())
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = ":".join(filter(None, (env.get("ASAN_OPTIONS"), "detect_leaks=0")))
            report = run([executable], work, env)
            print(f"  {compiler}: every latency read right after measuring, on both wires\n{report.rstrip()}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
