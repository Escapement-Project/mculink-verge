# /// script
# requires-python = ">=3.11"
# ///
"""Run the focused clang-tidy checks on the C files owned by this project."""

import json
import os
import re
import shlex
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), ".."))
SOURCE_DIR = os.path.join(ROOT, "src")


def version(executable: str, pattern: str) -> str | None:
    result = subprocess.run([executable, "--version"], check=False, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True)
    match = re.search(pattern, result.stdout)
    if result.returncode != 0 or match is None:
        return None
    return match.group(1)


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: clang_tidy.py <build-directory>", file=sys.stderr)
        return 2

    build_dir = os.path.abspath(sys.argv[1])
    database_path = os.path.join(build_dir, "compile_commands.json")
    try:
        with open(database_path, encoding="utf-8") as database_file:
            database = json.load(database_file)
    except (OSError, json.JSONDecodeError) as error:
        print(f"cannot read {database_path}: {error}", file=sys.stderr)
        return 2

    sources = sorted({os.path.abspath(entry["file"]) for entry in database
                      if os.path.dirname(os.path.abspath(entry["file"])) == SOURCE_DIR
                      and entry["file"].endswith(".c")})
    if not sources:
        print(f"no project C sources found in {database_path}", file=sys.stderr)
        return 2

    command = next((entry.get("arguments") or shlex.split(entry["command"])
                    for entry in database if os.path.abspath(entry["file"]) in sources), None)
    if not command or os.path.basename(command[0]) not in ("clang", "clang.exe"):
        print("compilation database does not use Clang", file=sys.stderr)
        return 2

    clang_version = version(command[0], r"clang version\s+(\d+\.\d+\.\d+)")
    tidy_version = version("clang-tidy", r"LLVM version\s+(\d+\.\d+\.\d+)")
    if clang_version is None or tidy_version is None or clang_version != tidy_version:
        print(f"clang and clang-tidy versions differ: {clang_version}, {tidy_version}", file=sys.stderr)
        return 2

    result = subprocess.run(["clang-tidy", "-p", build_dir, *sources], cwd=ROOT, check=False)
    return result.returncode


if __name__ == "__main__":
    sys.exit(main())
