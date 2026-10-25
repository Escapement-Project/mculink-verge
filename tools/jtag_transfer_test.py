# /// script
# requires-python = ">=3.11"
# ///
"""Check extracted JTAG command logic under both host sanitizers."""

import os
from pathlib import Path
import subprocess
import tempfile

from extract_jtag_transfer import SOURCE, TARGET, render

ROOT = Path(__file__).resolve().parent.parent


def run(args, env=None):
    subprocess.run(args, cwd=ROOT, env=env, check=True)


def main():
    if (ROOT / TARGET).read_text() != render((ROOT / SOURCE).read_text()):
        raise SystemExit("re-run tools/extract_jtag_transfer.py")
    with tempfile.TemporaryDirectory() as directory:
        Path(directory, "cmsis_compiler.h").write_text(
            "#define __STATIC_FORCEINLINE static inline __attribute__((always_inline))\n#define __ASM __asm\n")
        for compiler in ("gcc", "clang"):
            executable = str(Path(directory, compiler))
            run([compiler, "-std=gnu23", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", f"-I{directory}", "-Isrc", "-Iexternal/CMSIS-DAP/Firmware/Include",
                 "tools/jtag_transfer_test.c", "-o", executable])
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = ":".join(filter(None, (env.get("ASAN_OPTIONS"), "detect_leaks=0")))
            run([executable], env)
            print(f"  {compiler}: posted reads, IR changes, timestamps, match, retry, abort and chain bounds")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
