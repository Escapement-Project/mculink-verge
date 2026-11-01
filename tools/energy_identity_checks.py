"""Bounded RNG initialization against MMIO models, using the linked CPU 1 code."""

from cycles import RETURN_MAGIC, u32
from energy_checks import INTEGRATION_AT, machine
from unicorn.arm_const import UC_ARM_REG_PC

RNG = 0x4003A000
SYSCON = 0x40000000
PMC = 0x40020000
DWT = 0xE0001000
MASK = 1 << 13


def check_rng(c, elf, symbols, slots):
    from image import shared_offsets, short_enums

    fields = shared_offsets(slots, "energy_available", "energy_control_request", "energy_integration",
                            small_enums=short_enums(elf))
    capture_id_offset = shared_offsets(slots, "capture_id", small_enums=short_enums(elf),
                                      struct_type="struct energy_integration")["capture_id"]
    for fault in (None, "assert", "release", "statistics", "health", "refresh", "flat", "equal",
                  "delayed", "wrap", "poll-limit", "late-1", "late-2", "late-3"):
        probe = machine(elf, symbols)
        syscon = bytearray(4096)
        writes = []
        draws = []
        shifts = []
        enabled = False
        values = [0, 0x12345678, 0xFFFFFFFF, 0x87654321]
        late = int(fault[-1]) if fault and fault.startswith("late-") else None
        success = fault in (None, "delayed", "wrap")
        polls = 0
        config = 0
        initial_counter = 0xFFFFFFF0 if fault == "wrap" else 0
        counter = initial_counter
        last_counter = initial_counter
        step = 0 if fault == "poll-limit" else 1 if success else 10000

        def read_dwt(uc, offset, size, data):
            nonlocal counter, last_counter
            if offset == 4:
                last_counter = counter
                counter = (counter + step) & 0xFFFFFFFF
                return last_counter
            return 0

        def read_syscon(uc, offset, size, data):
            nonlocal polls
            if offset == 0x108:
                polls += 1
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

        def statistics():
            if fault == "statistics":
                return 0xF0
            if fault == "health":
                return 0x800
            if fault in ("flat", "poll-limit") or (late is not None and len(draws) >= late):
                return 0
            if fault == "equal":
                return 0x220
            if fault in ("delayed", "wrap") and polls < 3_000_000:
                return 0x110
            return 0x200

        def read_rng(uc, offset, size, data):
            nonlocal polls
            if offset == 0:
                draws.append(len(draws))
                return values[draws[-1] % 4]
            if offset == 8:
                polls += 1
                return 0 if fault == "refresh" else 31 << 8
            if offset == 0xC:
                return config
            if offset == 0x14:
                polls += 1
                return statistics()
            return 0

        def write_rng(uc, offset, size, value, data):
            nonlocal config, enabled
            writes.append((RNG + offset, value))
            if offset == 0xC:
                before, after = (config >> 5) & 7, (value >> 5) & 7
                if before != after:
                    c.check(not enabled, f"RNG {fault}: precision changed before resetting statistics")
                    c.check(after == before + 1, f"RNG {fault}: precision wrapped or skipped")
                    shifts.append(after)
                config = value
            if offset == 0x10:
                enabled = value != 0

        def write_pmc(uc, offset, size, value, data):
            writes.append((PMC + offset, value))

        for base, read, write in ((SYSCON, read_syscon, write_syscon), (RNG, read_rng, write_rng),
                                  (PMC, lambda *args: 0, write_pmc), (DWT, read_dwt, lambda *args: None)):
            probe.uc.mem_unmap(base, 4096)
            probe.uc.mmio_map(base, 4096, read, None, write, None)
        boot_at = probe.sym["identity_boot"]
        probe.uc.mem_write(boot_at, bytes([0xA5]) * 16)
        if late is None:
            result = probe.call("energy_identity_boot", boot_at, timeout_insns=640_000_000)
            expected = int(success)
        else:
            shared = probe.sym["dap_shared"]
            probe.uc.mem_write(shared + fields["energy_available"], b"\x01")
            probe.uc.mem_write(shared + fields["energy_integration"], u32(INTEGRATION_AT))
            published_at = INTEGRATION_AT + capture_id_offset
            retained = bytes(range(1, 25))
            probe.uc.mem_write(published_at, retained)
            result = probe.call("energy_monitor_start", 1000, timeout_insns=640_000_000)
            expected = 9
            c.check(bytes(probe.uc.mem_read(published_at, 24)) == retained,
                    f"RNG {fault}: partial draw changed the published identity")
            c.check(bytes(probe.uc.mem_read(probe.sym["capture_counter"], 8)) == bytes(8),
                    f"RNG {fault}: failed partial draw advanced capture identity")
            c.check(bytes(probe.uc.mem_read(probe.sym["identity_failed"], 1)) == b"\x01",
                    f"RNG {fault}: partial draw did not latch identity failure")
            c.check(bytes(probe.uc.mem_read(shared + fields["energy_control_request"], 4)) == bytes(4),
                    f"RNG {fault}: failed identity requested capture mutation")
        c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, f"RNG {fault}: unbounded wait")
        c.check(result == expected, f"RNG {fault}: result {result}, expected {expected}")
        c.check(polls <= 8_000_064, f"RNG {fault}: polling exceeded the global bound")
        elapsed = (last_counter - initial_counter) & 0xFFFFFFFF
        c.check(elapsed <= 150_000_000 + step, f"RNG {fault}: elapsed guard exceeded one second")
        if fault == "poll-limit":
            c.check(polls >= 8_000_000, "RNG stopped counter: poll bound not exercised")
        elif not success:
            c.check(elapsed >= 144_000_000, f"RNG {fault}: time bound not exercised")
        if fault in ("assert", "release"):
            c.check((RNG + 0x10, 1) not in writes, f"RNG {fault}: reset timeout entered accumulation")
        c.check(not enabled and (int.from_bytes(syscon[0x208:0x20C], "little") & MASK) == 0
                and writes[-1] == (PMC + 0xC0, 1 << 22), f"RNG {fault}: clock/power left on")
        expected_draws = list(range(4 if success else late or 0))
        c.check(draws == expected_draws, f"RNG {fault}: draws {draws}, expected {expected_draws}")
        expected_boot = b"".join(map(u32, values)) if success else bytes([0xA5]) * 16
        c.check(bytes(probe.uc.mem_read(boot_at, 16)) == expected_boot,
                f"RNG {fault}: boot bytes {bytes(probe.uc.mem_read(boot_at, 16)).hex()}")
        if fault == "health":
            c.check(shifts == list(range(1, 8)), f"RNG health: unsupported precision sequence {shifts}")
    c.note("RNG linked code: four fresh words after strict health checks; delayed and wrapping-clock success;"
           " reset/statistics/health/refresh timeouts and stopped-counter poll bound;"
           " partial failures preserve boot and published identity; clock/power disabled on every exit")
