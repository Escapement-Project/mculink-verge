# /// script
# requires-python = ">=3.11"
# ///
"""Build the MS OS 2.0 descriptor set under gcc and clang sanitizers."""

import os
import pathlib
import shutil
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = pathlib.Path(__file__).with_name("ms_os_20_test.c")


def run(args: list[str], env: dict[str, str]) -> None:
    done = subprocess.run(args, cwd=ROOT, env=env, text=True, capture_output=True)
    if done.returncode:
        raise SystemExit(f"{' '.join(args)}\n{done.stdout}{done.stderr}")


def main() -> int:
    with tempfile.TemporaryDirectory() as directory:
        for compiler in ("gcc", "clang"):
            if shutil.which(compiler) is None:
                raise SystemExit(f"{compiler} is not installed")
            executable = os.path.join(directory, compiler)
            run([compiler, "-std=gnu23", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-Isrc", str(SOURCE), "-o", executable], os.environ.copy())
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = ":".join(filter(None, (env.get("ASAN_OPTIONS"), "detect_leaks=0")))
            run([executable], env)
            print(f"  {compiler}: MS OS 2.0 sets: every length field, and the CMSIS-DAP-only set unchanged")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
