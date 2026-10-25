# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone", "unicorn"]
# ///
"""Check linked stack analysis against assembled control-flow fixtures."""

import pathlib
import json
import subprocess
import tempfile

from cycles import Machine, Target, SRAMX
from stack_usage import analyze, StackObserver


def main():
    source = ".syntax unified\n.thumb\n.text\n"
    fixtures = {
        "leaf": "push {r4,lr}\npop {r4,pc}",
        "caller": "push {lr}\nsub sp,#8\nbl leaf\nadd sp,#8\npop {pc}",
        "tail": "b leaf",
        "branch": "push {lr}\ncmp r0,#0\nbeq 1f\nsub sp,#16\nbl leaf\nadd sp,#16\n1: pop {pc}",
        "loop": "1: sub sp,#8\nadd sp,#8\nb 1b",
        "unbalanced": "1: sub sp,#8\nb 1b",
        "indirect": "push {lr}\nblx r0\npop {pc}",
        "dynamic": "sub sp,sp,r0\nbx lr",
        "recursive": "push {lr}\nbl recursive\npop {pc}",
        "table": "tbb [pc,r0]",
        "writeback": "str lr,[sp,#-4]!\nldr pc,[sp],#4",
        "predicated": "cmp r0,#0\nit eq\nsubeq sp,#8\nbx lr",
    }
    for name, body in fixtures.items():
        source += f".global {name}\n.thumb_func\n{name}:\n{body}\n.size {name}, .-{name}\n"
    source += ".global __StackTop, __StackLimit\n.equ __StackTop,0x20010000\n.equ __StackLimit,0x2000f000\n"
    source += ".data\n.word 0\n"
    with tempfile.TemporaryDirectory() as directory:
        directory = pathlib.Path(directory)
        asm, obj, elf = (directory / name for name in ("fixture.S", "fixture.o", "fixture.elf"))
        asm.write_text(source)
        subprocess.run(["arm-none-eabi-as", "-mcpu=cortex-m33", str(asm), "-o", str(obj)], check=True)
        subprocess.run(["arm-none-eabi-ld", "-Ttext=0x1000", "-e", "caller", str(obj), "-o", str(elf)], check=True)
        report = analyze(str(elf))
        observer = StackObserver()
        observer.install()
        try:
            probe = Machine(str(elf), Target([]), restart_at=set())
            probe.call("caller")
            probe.call("branch", 1)
            probe.call("writeback")
            probe.sym["generated"] = SRAMX[0]
            probe.uc.mem_write(SRAMX[0], bytes.fromhex("10b510bd"))  # push/pop r4,lr/pc
            probe.call("generated")
        finally:
            observer.finish(directory / "observed.json", True)
        observed = {row["entry"]: row["observed_peak_bytes"]
                    for row in json.loads((directory / "observed.json").read_text())["calls"]}
        assert observed == {"caller": 20, "branch": 28, "writeback": 4, "generated": 8}, observed
    entries = {entry["name"]: entry for entry in report["functions"]}
    for name, expected in (("leaf", 8), ("caller", 20), ("tail", 8), ("branch", 28), ("loop", 8), ("writeback", 4)):
        assert entries[name]["synchronous_bound_bytes"] == expected, (name, entries[name])
    for name in ("unbalanced", "indirect", "dynamic", "recursive", "table", "predicated"):
        assert entries[name]["synchronous_bound_bytes"] is None and entries[name]["unresolved"], (name, entries[name])
    assert report["stack_reservation_bytes"] == 4096
    print("  stack analysis: direct/tail calls, branch peaks, loops, writeback and unresolved paths")


if __name__ == "__main__":
    main()
