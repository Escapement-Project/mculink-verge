# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn", "capstone"]
# ///
"""Disassemble the instructions between two clock edges, for one class of gap.

`cycles.py --explain` names the longest gap in a transfer. That one is the first word of a block
and carries the command dispatch, so it is paid once and says nothing about the cost per word. The
gap that costs is whichever one repeats, and the per-bit classes under it -- a read bit at five
cycles against a write bit at four, a driven cJTAG bit at nine against a sampled one at six --
are where a difference between two paths actually lives.

So: group every gap by length, show the distribution, and print the instructions of one gap of the
length asked for. Two changes to ARM's SWD path came straight off this output, a word
rebuilt from its own bytes and a parity recomputed once a word, and neither was visible in a
disassembly listing of the function, because both sit in code the optimiser hoisted to the top of
a loop body that is thousands of instructions long.

    uv run tools/window.py                       # the gap that repeats, on a SWD read
    uv run tools/window.py --wire cjtag --write --span 9
"""

import argparse
import collections
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from capstone import CS_ARCH_ARM, CS_MODE_THUMB, Cs  # noqa: E402
from cycles import Model, measure  # noqa: E402


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("elf", nargs="?", default="build/mculink_cjtag_core1.elf", help="CPU 1's image")
    ap.add_argument("--wire", choices=["swd", "jtag", "cjtag"], default="swd")
    ap.add_argument("--write", action="store_true", help="measure a write block, not a read")
    ap.add_argument("--clock", type=float, default=50.0, help="requested wire clock, MHz")
    ap.add_argument("--words", type=int, default=64)
    ap.add_argument("--cpu", type=float, default=144.0, help="core clock, MHz")
    ap.add_argument(
        "--span",
        type=int,
        help="cycles in the gap to disassemble; the default is the longest one that repeats",
    )
    args = ap.parse_args()

    _, _, probe = measure(
        args.elf,
        args.wire,
        int(args.clock * 1e6),
        args.words,
        not args.write,
        Model(),
        explain=True,
    )
    tally = collections.Counter(span for span, _ in probe.windows)
    shown = " ".join(f"{span}cyc x{n}" for span, n in tally.most_common(8))
    print(f"{args.wire} {'write' if args.write else 'read'}, {args.clock:g} MHz asked: {shown}")

    span = args.span
    if span is None:
        # The longest gap that happens about once a word. A gap seen two or three times is a block
        # prologue, and the one seen thousands of times is a single bit.
        repeated = [s for s, n in tally.items() if n >= args.words // 2]
        if not repeated:
            print("  no gap repeats often enough to be a per-word cost")
            return 1
        span = max(repeated)
    hits = [w for s, w in probe.windows if s == span]
    if not hits:
        print(f"  no gap of {span} cycles; the lengths above are what there is")
        return 1

    print(f"\n{len(hits)} gaps of {span} cycles. The middle one:")
    md = Cs(CS_ARCH_ARM, CS_MODE_THUMB)
    for address in hits[len(hits) // 2]:
        code = bytes(probe.uc.mem_read(address, 4))
        insn = next(md.disasm(code, address), None)
        if insn:
            print(f"  {address:#08x}  {insn.mnemonic:9s} {insn.op_str}")
        else:
            print(f"  {address:#08x}  ?")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
