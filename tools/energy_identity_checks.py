"""Bounded RNG initialization against MMIO models, using the linked CPU 1 code."""

from cycles import RETURN_MAGIC, SCRATCH, u32
from energy_checks import machine
from unicorn.arm_const import UC_ARM_REG_PC

RNG = 0x4003A000
SYSCON = 0x40000000
PMC = 0x40020000
MASK = 1 << 13


def check_rng(c, elf, symbols):
    for fault in (None, "assert", "release", "statistics", "health", "refresh"):
        probe = machine(elf, symbols)
        syscon = bytearray(4096)
        writes = []
        draws = []
        enabled = False
        values = [0, 0x12345678, 0xFFFFFFFF, 0x87654321]

        def read_syscon(uc, offset, size, data):
            return int.from_bytes(syscon[offset:offset + size], "little")

        def write_syscon(uc, offset, size, value, data):
            writes.append((SYSCON + offset, value))
            for set_at, base in ((0x128, 0x108), (0x228, 0x208)):
                if offset in (set_at, set_at + 0x20):
                    if (fault == "assert" and offset == 0x128) or (fault == "release" and offset == 0x148):
                        return
                    word = int.from_bytes(syscon[base:base + 4], "little")
                    word = word | value if offset == set_at else word & ~value
                    syscon[base:base + 4] = u32(word)
                    return
            syscon[offset:offset + size] = value.to_bytes(size, "little")

        config = 0

        def read_rng(uc, offset, size, data):
            if offset == 0:
                draws.append(len(draws))
                return values[draws[-1] % 4]
            if offset == 8:
                return 0 if fault == "refresh" else 31 << 8
            if offset == 0xC:
                return config
            if offset == 0x14:
                return 0 if fault == "statistics" else 0x800 if fault == "health" else 0x200
            return 0

        def write_rng(uc, offset, size, value, data):
            nonlocal config, enabled
            writes.append((RNG + offset, value))
            if offset == 0xC:
                config = value
            if offset == 0x10:
                enabled = value != 0

        def write_pmc(uc, offset, size, value, data):
            writes.append((PMC + offset, value))

        for base, read, write in ((SYSCON, read_syscon, write_syscon), (RNG, read_rng, write_rng),
                                  (PMC, lambda *args: 0, write_pmc)):
            probe.uc.mem_unmap(base, 4096)
            probe.uc.mmio_map(base, 4096, read, None, write, None)
        boot_at = probe.sym["identity_boot"]
        probe.uc.mem_write(boot_at, bytes([0xA5]) * 16)
        result = probe.call("energy_identity_boot", boot_at, timeout_insns=40_000_000)
        c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, f"RNG {fault}: unbounded wait")
        c.check(result == int(fault is None), f"RNG {fault}: result {result}")
        c.check(not enabled and (int.from_bytes(syscon[0x208:0x20C], "little") & MASK) == 0
                and writes[-1] == (PMC + 0xC0, 1 << 22), f"RNG {fault}: clock/power left on")
        if fault is None:
            c.check(draws == list(range(4)) and bytes(probe.uc.mem_read(boot_at, 16)) == b"".join(map(u32, values)),
                    f"RNG boot words: draws {draws}, bytes {bytes(probe.uc.mem_read(boot_at, 16)).hex()}")
        else:
            c.check(not draws, f"RNG {fault}: consumed randomness after failure")
    c.note("RNG linked code: four hardware words; bounded reset assert/release, statistics, health and refresh failures;"
           " clock and power disabled on every exit")
