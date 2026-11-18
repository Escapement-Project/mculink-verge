# /// script
# requires-python = ">=3.11"
# dependencies = ["capstone", "unicorn"]
# ///
"""Inspect stack use in linked Thumb code, and list the control flow it cannot follow.

It reports a synchronous bound only where it understands every edge it can reach. An indirect
call, a jump table, recursion or a dynamic SP change leaves no bound. Exception stacking and
nesting are not in it: add them yourself. The report is not a stack bound for the whole firmware.
"""

import argparse
from dataclasses import dataclass, field
import json
import subprocess

from capstone import Cs, CS_ARCH_ARM, CS_MODE_THUMB, CS_MODE_MCLASS
from capstone.arm import ARM_OP_IMM, ARM_OP_REG, ARM_OP_MEM, ARM_CC_AL, ARM_CC_INVALID
from cycles import Machine, STACK_TOP, image_bytes, load_address, sections, symbols
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_SP


@dataclass
class Function:
    name: str
    address: int
    size: int
    local: int = 0
    calls: list = field(default_factory=list)
    unknown: set = field(default_factory=set)


class StackObserver:
    """Record executed SP minima without treating unexecuted paths as bounded."""

    def __init__(self):
        self.rows = {}
        self.original = Machine.call
        self.original_init = Machine.__init__

    def install(self):
        def initialize(probe, elf, *args, **kwargs):
            probe._stack_report_elf = elf
            self.original_init(probe, elf, *args, **kwargs)

        def call(probe, name, *args, **kwargs):
            low = STACK_TOP

            def sample(uc, address, size, data):
                nonlocal low
                low = min(low, uc.reg_read(UC_ARM_REG_SP))

            hook = probe.uc.hook_add(UC_HOOK_CODE, sample)
            probe.uc.ctl_flush_tb()
            try:
                return self.original(probe, name, *args, **kwargs)
            finally:
                probe.uc.hook_del(hook)
                probe.uc.ctl_flush_tb()
                key = (probe._stack_report_elf, name)
                self.rows[key] = max(self.rows.get(key, 0), STACK_TOP - low)

        Machine.__init__ = initialize
        Machine.call = call

    def finish(self, path, passed):
        Machine.call = self.original
        Machine.__init__ = self.original_init
        report = {"image_checks_passed": passed,
                  "scope": "executed calls only; entry SP reset per call; excludes caller prefixes and exception entry/nesting",
                  "calls": [{"elf": elf, "entry": name, "observed_peak_bytes": peak}
                            for (elf, name), peak in sorted(self.rows.items(), key=lambda row: row[1], reverse=True)]}
        with open(path, "w") as output:
            json.dump(report, output, indent=2)


def inspect(function, code):
    decoder = Cs(CS_ARCH_ARM, CS_MODE_THUMB | CS_MODE_MCLASS)
    decoder.detail = True
    pending = [(function.address, 0)]
    seen = {}
    end = function.address + function.size
    while pending:
        address, depth = pending.pop()
        if address in seen:
            if seen[address] != depth:
                function.unknown.add(f"inconsistent SP at {address:#x}")
            continue
        seen[address] = depth
        if not function.address <= address < end:
            function.unknown.add(f"fallthrough outside function at {address:#x}")
            continue
        insn = next(decoder.disasm(code[address - function.address:], address, count=1), None)
        if insn is None:
            function.unknown.add(f"undecodable instruction at {address:#x}")
            continue
        mnemonic = insn.mnemonic.split(".")[0]
        if mnemonic.startswith("it"):
            function.unknown.add(f"IT-controlled execution at {address:#x}")
        if mnemonic in ("svc", "udf", "bkpt") or (mnemonic == "msr" and insn.op_str.startswith(("msp", "psp"))):
            function.unknown.add(f"exception or stack-bank change at {address:#x}")
        operands = insn.operands
        delta = 0
        _, writes = insn.regs_access()
        writes = {insn.reg_name(reg) for reg in writes}
        returns = mnemonic == "bx" and insn.op_str == "lr"
        if mnemonic in ("push", "vpush", "pop", "vpop"):
            width = sum(8 if insn.reg_name(op.reg).startswith("d") else 4 for op in operands)
            delta = width if mnemonic.endswith("push") else -width
            returns |= "pc" in writes
        elif mnemonic in ("add", "sub") and operands[0].type == ARM_OP_REG and insn.reg_name(operands[0].reg) == "sp":
            if operands[-1].type != ARM_OP_IMM or (len(operands) == 3 and insn.reg_name(operands[1].reg) != "sp"):
                function.unknown.add(f"dynamic SP at {address:#x}")
                continue
            delta = operands[-1].imm * (1 if mnemonic == "sub" else -1)
        elif insn.writeback and any(op.type == ARM_OP_MEM and insn.reg_name(op.mem.base) == "sp" for op in operands):
            memory = next(op for op in operands if op.type == ARM_OP_MEM)
            if memory.mem.index:
                function.unknown.add(f"indexed SP writeback at {address:#x}")
                continue
            adjustment = operands[-1].imm if operands[-1].type == ARM_OP_IMM else memory.mem.disp
            delta = -adjustment
            returns |= mnemonic.startswith("ldr") and "pc" in writes
        elif "sp" in writes:
            function.unknown.add(f"unhandled SP write at {address:#x}: {insn.mnemonic} {insn.op_str}")
            continue
        if delta and insn.cc not in (ARM_CC_AL, ARM_CC_INVALID):
            function.unknown.add(f"conditional SP change at {address:#x}")
            continue
        depth += delta
        function.local = max(function.local, depth)
        if depth < 0:
            function.unknown.add(f"SP above entry at {address:#x}")
            continue
        if returns:
            if insn.cc not in (ARM_CC_AL, ARM_CC_INVALID):
                function.unknown.add(f"conditional return at {address:#x}")
            if depth:
                function.unknown.add(f"unbalanced return at {address:#x}")
            continue
        if mnemonic in ("bl", "blx"):
            if operands[0].type == ARM_OP_IMM:
                function.calls.append((operands[0].imm & ~1, depth))
            else:
                function.unknown.add(f"indirect call at {address:#x}: {insn.op_str}")
        elif (mnemonic.startswith("b") and mnemonic not in ("bic", "bics", "bfi", "bfc", "bkpt")) or mnemonic in ("cbz", "cbnz"):
            target = operands[-1]
            if target.type == ARM_OP_IMM:
                destination = target.imm & ~1
                if function.address <= destination < end:
                    pending.append((destination, depth))
                else:
                    function.calls.append((destination, depth))
                conditional = mnemonic in ("cbz", "cbnz") or insn.cc not in (ARM_CC_AL, ARM_CC_INVALID)
                if not conditional:
                    continue
            else:
                function.unknown.add(f"indirect branch at {address:#x}: {insn.mnemonic} {insn.op_str}")
                continue
        elif "pc" in writes or mnemonic in ("tbb", "tbh"):
            function.unknown.add(f"unresolved control flow at {address:#x}: {insn.mnemonic} {insn.op_str}")
            continue
        pending.append((address + insn.size, depth))
    return function


def resolve(functions):
    cache = {}

    def visit(address, active):
        if address in active:
            return 0, {f"recursive call at {address:#x}"}
        if address not in functions:
            return 0, {f"unresolved target {address:#x}"}
        if address in cache:
            return cache[address]
        function = functions[address]
        peak, unknown = function.local, set(function.unknown)
        for target, depth in function.calls:
            size, missing = visit(target, active | {address})
            peak = max(peak, depth + size)
            unknown.update(missing)
        # A recursion-dependent result must not be reused for a different traversal root.
        if not any(reason.startswith("recursive") for reason in unknown):
            cache[address] = peak, unknown
        return peak, unknown

    return {address: visit(address, set()) for address in functions}


def analyze(elf):
    functions = {}
    rows = subprocess.run(["arm-none-eabi-nm", "-S", elf], check=True, capture_output=True, text=True).stdout
    regions = sections(elf)
    raw, base = image_bytes(elf), load_address(elf)
    for row in rows.splitlines():
        words = row.split()
        if len(words) != 4 or words[2] not in ("T", "t", "W", "w"):
            continue
        address, size = int(words[0], 16), int(words[1], 16)
        if not size or address in functions:
            continue
        for name, (length, vma, lma) in regions.items():
            if name in (".text", ".data") and vma <= address and address + size <= vma + length:
                start = address - vma + lma - base
                functions[address] = inspect(Function(words[3], address, size), raw[start:start + size])
                break
    bounds = resolve(functions)
    entries = []
    for address, function in functions.items():
        peak, unknown = bounds[address]
        entries.append({"name": function.name, "address": address, "analyzed_peak_bytes": peak,
                        "synchronous_bound_bytes": None if unknown else peak, "unresolved": sorted(unknown)})
    linked = symbols(elf)
    reservation = (linked["__StackTop"] - linked["__StackLimit"] if "__StackTop" in linked else
                   linked["__core1_stack_top"] - linked["__core1_stack_bottom"] if "__core1_stack_bottom" in linked else None)
    return {"elf": elf, "stack_reservation_bytes": reservation,
            "scope": "synchronous linked control flow; unresolved paths have no bound; excludes exception nesting",
            "functions": sorted(entries, key=lambda entry: entry["analyzed_peak_bytes"], reverse=True)}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf", nargs="+")
    parser.add_argument("--output", help="write the full JSON report")
    args = parser.parse_args()
    reports = [analyze(elf) for elf in args.elf]
    if args.output:
        with open(args.output, "w") as output:
            json.dump(reports, output, indent=2)
    for report in reports:
        bounded = sum(entry["synchronous_bound_bytes"] is not None for entry in report["functions"])
        print(f"{report['elf']}: {report['stack_reservation_bytes']} bytes reserved; {bounded}/{len(report['functions'])} functions bounded")
        for entry in report["functions"][:5]:
            bound = entry["synchronous_bound_bytes"]
            print(f"  {entry['name']}: " + (f"{bound} bytes synchronous bound" if bound is not None else
                  f"unresolved; analyzed paths reach {entry['analyzed_peak_bytes']} bytes"))


if __name__ == "__main__":
    main()
