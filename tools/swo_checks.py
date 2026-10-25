"""Exercise linked SWO data copies, including a writer overtaking the copy."""

from cycles import Machine, Model, SCRATCH, Target, u16, u32
from unicorn import UC_HOOK_MEM_WRITE

SEGMENT_BYTES = 1024
RING_BYTES = 8 * SEGMENT_BYTES
USABLE_BYTES = RING_BYTES - SEGMENT_BYTES


def copy_cases(elf, ring_offset, *, profile=False):
    probe = Machine(elf, Target([]), restart_at=set(), model=Model(engine=profile), timed=profile,
                    record_trace=False)
    ring = probe.sym["dap_shared"] + ring_offset
    pattern = bytes(((i * 7) ^ (i >> 8)) & 255 for i in range(RING_BYTES))
    probe.uc.mem_write(ring, pattern)
    probe.uc.mem_write(probe.sym["transport"], b"\x01")
    response = SCRATCH + 0x400
    rows = []
    for packet in (64, 512):
        maximum = packet - 4
        for count in (0, 1, 2, 3, 4, 7, 16, maximum):
            for start in (0, 1, RING_BYTES - 1, RING_BYTES - maximum // 2, 0xFFFFFFFF - 3):
                for overtake in (False, True) if count else (False,):
                    end = (start + count) & 0xFFFFFFFF
                    for name, value in (("read_position", start), ("written_at_stop", end), ("stamp_index", end)):
                        probe.uc.mem_write(probe.sym[name], u32(value))
                    probe.uc.mem_write(probe.sym["errors"], b"\x00")
                    probe.uc.mem_write(SCRATCH, b"\x1c" + u16(count))
                    probe.uc.mem_write(response, b"\xA5" * (packet + 1))

                    def advance(uc, access, address, size, value, user):
                        if address == response + 4 + count // 2:
                            uc.mem_write(probe.sym["written_at_stop"], u32((start + USABLE_BYTES + 1) & 0xFFFFFFFF))

                    hook = probe.uc.hook_add(UC_HOOK_MEM_WRITE, advance,
                                             begin=response + 4, end=response + 3 + count) if overtake else None
                    probe.begin()
                    produced = probe.call("DAP_ExecuteCommandSized", SCRATCH, 3, packet, response) & 0xFFFF
                    stats = probe.end()
                    if hook is not None:
                        probe.uc.hook_del(hook)
                    got = bytes(probe.uc.mem_read(response, packet + 1))
                    returned = 0 if overtake else count
                    assert produced == returned + 4, (elf, packet, count, start, overtake, produced)
                    assert got[0] == 0x1C and int.from_bytes(got[2:4], "little") == returned
                    assert bool(got[1] & 0x80) == overtake, (elf, got[:4], overtake)
                    if not overtake:
                        assert got[4:4 + count] == bytes(pattern[(start + i) % RING_BYTES] for i in range(count))
                    assert got[4 + count:] == b"\xA5" * (packet - count - 3)
                    position = int.from_bytes(probe.uc.mem_read(probe.sym["read_position"], 4), "little")
                    expected = (start + 2 * SEGMENT_BYTES + 1) & 0xFFFFFFFF if overtake else end
                    assert position == expected, (elf, packet, count, start, overtake, position, expected)
                    rows.append({"packet": packet, "count": count, "start": start, "overtake": overtake,
                                 "instructions": stats.instructions, "model_cycles": stats.span})
    return rows


def check_copy(c, elf, ring_offset):
    try:
        rows = copy_cases(elf, ring_offset)
    except AssertionError as error:
        c.problems.append(f"SWO data copy: {error}")
    else:
        c.note(f"SWO data: {len(rows)} copies, byte order, reply bounds, counter wrap and post-copy overruns")
