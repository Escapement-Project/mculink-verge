# /// script
# requires-python = ">=3.11"
# ///
"""Run the command framing parser under gcc and clang sanitizers."""

import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = pathlib.Path(__file__).with_name("dap_packet_test.c")
ARM_DAP_H = ROOT / "external" / "CMSIS-DAP" / "Firmware" / "Include" / "DAP.h"


def arm_constants(directory: str) -> str:
    """A DAP.h of ARM's own constants, the part of the header before its includes: the rest needs
    CMSIS's compiler support, which a host compiler lacks. The names src/dap_packet.h uses then hold
    ARM's values, not copies."""
    text = ARM_DAP_H.read_text()
    head = text[: text.index("#include")]
    if "#define ID_DAP_Transfer " not in head:
        raise SystemExit(f"{ARM_DAP_H} no longer defines its command ids before its includes")
    include = os.path.join(directory, "arm")
    os.makedirs(include)
    pathlib.Path(include, "DAP.h").write_text(head + "#endif\n")
    return include


def run(args: list[str], env: dict[str, str]) -> None:
    done = subprocess.run(args, cwd=ROOT, env=env, text=True, capture_output=True)
    if done.returncode:
        raise SystemExit(f"{' '.join(args)}\n{done.stdout}{done.stderr}")


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        arm = arm_constants(directory)
        for compiler in ("gcc", "clang"):
            if shutil.which(compiler) is None:
                raise SystemExit(f"{compiler} is not installed")
            executable = os.path.join(directory, compiler)
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = ":".join(filter(None, (env.get("ASAN_OPTIONS"), "detect_leaks=0")))
            for features in ([], ["-DGENERATED_WIRE", "-DCODE_LAB",
                                  "-DMARKER_PINS", "-DDAP_TIMING_DIAGNOSTICS"]):
                run([compiler, "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                     "-fno-sanitize-recover=all", *features, "-Isrc", f"-I{arm}", str(SOURCE), "-o", executable],
                    env)
                run([executable], env)
            print(f"  {compiler}: exact request allocations, response ceilings and command grammars")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
