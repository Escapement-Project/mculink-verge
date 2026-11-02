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


SWO_MASK = 1 << 8
XFERCFG_8 = 0x408 + 8 * 16
INTA, INTB, ENABLECLR, ABORT = 0x58, 0x60, 0x28, 0x78
SETINTA, SETINTB, SWTRIG = 1 << 4, 1 << 5, 1 << 2
USART3 = 0x40089000
OVERRUN, ACTIVE = 0x80, 0x01


class Usart:
    """Flexcomm 3's USART page, its error bits write-one-to-clear, as UM11126 has them: the plain
    model would keep every bit swo.c writes to clear them, and report a broken line each poll."""

    W1C = (0x008, 0xE04)  # STAT, FIFOSTAT

    def __init__(self, probe):
        self.page = bytearray(4096)
        probe.uc.mem_unmap(USART3, 4096)
        probe.uc.mmio_map(USART3, 4096, self.read, None, self.write, None)

    def read(self, uc, offset, size, data):
        return int.from_bytes(self.page[offset:offset + size], "little")

    def write(self, uc, offset, size, value, data):
        if offset in self.W1C:
            value = int.from_bytes(self.page[offset:offset + 4], "little") & ~value
        self.page[offset:offset + size] = value.to_bytes(size, "little")


def check_restart(c, elf, offsets):
    """A count CPU 0 reports lost, or that CPU 1 finds lost from the next segment's flag (before
    a copy or during it, or at a stop), reports DAP_SWO_BUFFER_OVERRUN once, delivers none of the
    ring, and starts the ring again with the UART still running: both flags cleared, descriptors
    alternating from CPU 0's count, the index never going back, and bytes after it coming from the
    ring's start. A capture starts the same way, and the flag due alone is a completion, not a loss."""
    from energy_checks import Dma

    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    dma, usart = Dma(probe), Usart(probe)
    shared = probe.sym["dap_shared"]
    ring = shared + offsets["swo_ring"]
    segments, lost = shared + offsets["swo_segments"], shared + offsets["swo_lost"]

    def word(address):
        return int.from_bytes(probe.uc.mem_read(address, 4), "little")

    def flags_of(first):
        return [word(shared + offsets["swo_dma"] + 16 * k) & (SETINTA | SETINTB) for k in range(8)], \
            [SETINTB if (first + k) & 1 else SETINTA for k in range(8)]

    def started(writes, first, what):
        """The ring started from its first byte with CPU 0's count at `first`."""
        cleared = [o for o, v in writes if o in (INTA, INTB) and v & SWO_MASK]
        start = [v for o, v in writes if o == XFERCFG_8 and v & SWTRIG]
        got, want = flags_of(first)
        c.check(INTA in cleared and INTB in cleared, f"{what}: the flags were not both cleared")
        c.check(start and start[-1] & (SETINTA | SETINTB) == want[0] and got == want,
                f"{what}: from count {first}, started {[hex(v) for v in start]}, descriptors {got}, want {want}")
        c.check(probe.uc.mem_read(lost, 1)[0] == 0, f"{what}: swo_lost left set")
        # The channel's register holds the start: nothing of the new ring written yet.
        dma.set(XFERCFG_8, start[-1] if start else 0)

    def at(written_bytes):
        """The DMA `written_bytes` into the ring since it started, its count as CPU 0 left it."""
        dma.set(XFERCFG_8, (1023 - written_bytes % 1024) << 16)

    def command(*request):
        return probe.command(bytes(request))

    def index():
        reply = command(0x1E, 0x04)
        return int.from_bytes(reply[1:5], "little")

    probe.uc.mem_write(segments, u32(5))
    for request in ((0x17, 1), (0x18, 1)):
        c.check(command(*request)[1] == 0, f"SWO setup {bytes(request).hex()} refused")
    c.check(int.from_bytes(command(0x19, *u32(1_000_000))[1:5], "little") != 0, "SWO baud rate refused")
    before = len(dma.writes)
    c.check(command(0x1A, 1)[1] == 0, "SWO capture did not start")
    started(dma.writes[before:], 5, "capture start")

    pattern = bytes(((i * 13) ^ (i >> 7)) & 255 for i in range(8192))
    probe.uc.mem_write(ring, pattern)
    at(100)
    reply = command(0x1C, *u16(500))
    c.check(reply[1] == ACTIVE and int.from_bytes(reply[2:4], "little") == 100 and reply[4:104] == pattern[:100],
            f"SWO data before any loss: {reply[:4].hex()}")
    # The flag due alone is the segment CPU 0 has yet to count.
    dma.set(INTB, SWO_MASK)
    at(10)
    reply = command(0x1B)
    c.check(reply[1] == ACTIVE and int.from_bytes(reply[2:6], "little") == 1024 + 10 - 100,
            f"SWO status with the due segment pending: {reply.hex()}")
    dma.set(INTB, 0)
    probe.uc.mem_write(segments, u32(6))

    cases = (("CPU 0 reported it", "lost", None), ("the next flag up", "next", None),
             ("both flags up", "both", None), ("lost during the copy", "copy", None))
    for what, how, _ in cases:
        counted = word(segments)
        last = index()
        at(300)
        if how == "lost":
            probe.uc.mem_write(lost, b"\x01")
        elif how == "next":
            dma.set(INTB if counted % 2 == 0 else INTA, SWO_MASK)
        elif how == "both":
            dma.set(INTA, SWO_MASK)
            dma.set(INTB, SWO_MASK)
        reads = 0

        def overtake(offset):
            nonlocal reads
            if offset == XFERCFG_8:
                reads += 1
                if reads == 2:
                    dma.set(INTB if counted % 2 == 0 else INTA, SWO_MASK)

        dma.on_read = overtake if how == "copy" else None
        before = len(dma.writes)
        reply = command(0x1C, *u16(500))
        dma.on_read = None
        writes = dma.writes[before:]
        c.check(reply[1] == ACTIVE | OVERRUN and int.from_bytes(reply[2:4], "little") == 0,
                f"{what}: SWO data {reply[:4].hex()}, wanted an overrun and nothing")
        c.check((ENABLECLR, SWO_MASK) in writes and (ABORT, SWO_MASK) in writes,
                f"{what}: the channel was not stopped before the ring restarted")
        c.check(usart.read(None, 0, 4, None) & 1, f"{what}: the restart stopped the UART")
        started(writes, counted, what)
        c.check((index() - last) & 0xFFFFFFFF < 0x80000000, f"{what}: the index went back")
        reply = command(0x1B)
        c.check(reply[1] == ACTIVE and int.from_bytes(reply[2:6], "little") == 0,
                f"{what}: after the restart, status {reply.hex()}")
        at(40)
        reply = command(0x1C, *u16(500))
        c.check(reply[1] == ACTIVE and int.from_bytes(reply[2:4], "little") == 40 and reply[4:44] == pattern[:40],
                f"{what}: after the restart, SWO data {reply[:4].hex()} not from the ring's start")
        dma.set(INTA, 0)
        dma.set(INTB, 0)
        probe.uc.mem_write(segments, u32(counted + 3))

    # A stop that finds the count lost: an overrun, and nothing of the ring.
    at(200)
    probe.uc.mem_write(lost, b"\x01")
    c.check(command(0x1A, 0)[1] == 0, "SWO capture did not stop")
    reply = command(0x1B)
    c.check(reply[1] == OVERRUN and int.from_bytes(reply[2:6], "little") == 0,
            f"a stop with the count lost: status {reply.hex()}")
    c.note("SWO restart: a lost count from CPU 0, the next flag, both flags, during a copy and at a stop;"
           " each one overrun, a ring started from the count, the UART running and the index monotonic")
