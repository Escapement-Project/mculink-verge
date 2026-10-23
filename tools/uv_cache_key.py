"""Print a stable cache key for this repository's uv script metadata."""

import hashlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


def inputs():
    for path in sorted((ROOT / "tools").rglob("*.py")):
        lines = path.read_text(encoding="utf-8").splitlines(keepends=True)
        try:
            start = next(index for index, line in enumerate(lines) if line.rstrip("\r\n") == "# /// script")
        except StopIteration:
            continue
        end = next((index for index in range(start + 1, len(lines))
                    if lines[index].rstrip("\r\n") == "# ///"), None)
        if end is None:
            raise ValueError(f"unterminated uv script metadata in {path}")
        yield path.relative_to(ROOT).as_posix(), "".join(lines[start:end + 1])

    for name in ("pyproject.toml", "uv.toml", "uv.lock"):
        path = ROOT / name
        if path.is_file():
            yield name, path.read_text(encoding="utf-8")


def main() -> None:
    digest = hashlib.sha256()
    for name, contents in inputs():
        digest.update(name.encode())
        digest.update(b"\0")
        digest.update(contents.encode())
        digest.update(b"\0")
    print(digest.hexdigest())


if __name__ == "__main__":
    main()
