# /// script
# requires-python = ">=3.11"
# ///
"""Write the energy converter's vectors from the firmware's own converter, under gcc and clang
sanitizers, and hold tools/energy_vectors.json to them. `--write` replaces the file instead: a change
to the conversion changes the vectors, and every host converter checks itself against them."""

import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
SOURCE = pathlib.Path(__file__).with_name("energy_vectors.c")
VECTORS = pathlib.Path(__file__).with_name("energy_vectors.json")


def run(args: list[str], env: dict[str, str]) -> str:
    done = subprocess.run(args, cwd=ROOT, env=env, text=True, capture_output=True)
    if done.returncode:
        raise SystemExit(f"{' '.join(args)}\n{done.stdout}{done.stderr}")
    return done.stdout


def main() -> int:
    outputs = {}
    with tempfile.TemporaryDirectory() as directory:
        for compiler in ("gcc", "clang"):
            if shutil.which(compiler) is None:
                raise SystemExit(f"{compiler} is not installed")
            executable = os.path.join(directory, compiler)
            run([compiler, "-std=gnu23", "-O1", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                 "-fno-sanitize-recover=all", "-Isrc", str(SOURCE), "-o", executable], os.environ.copy())
            env = os.environ.copy()
            env["ASAN_OPTIONS"] = ":".join(filter(None, (env.get("ASAN_OPTIONS"), "detect_leaks=0")))
            outputs[compiler] = run([executable], env)
    if outputs["gcc"] != outputs["clang"]:
        raise SystemExit("gcc and clang builds of the converter disagree")
    json.loads(outputs["gcc"])
    if "--write" in sys.argv:
        VECTORS.write_text(outputs["gcc"])
        print(f"  wrote {VECTORS.relative_to(ROOT)}")
        return 0
    if not VECTORS.exists() or VECTORS.read_text() != outputs["gcc"]:
        raise SystemExit(f"{VECTORS.relative_to(ROOT)} is not what the converter writes: run with --write, and give "
                         "every host converter the new file")
    print("  gcc and clang: the converter writes tools/energy_vectors.json as committed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
