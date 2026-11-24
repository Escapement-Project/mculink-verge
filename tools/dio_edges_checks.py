"""CPU 0's DIO edge bookkeeping (src/dio_edges.c) against its pin interrupt, run on the linked image."""

from cycles import SCRATCH
from energy_checks import ORDINARY_GPIO, machine
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import UC_ARM_REG_PRIMASK

STATICS = ("resting", "rest_from", "begins_seen", "edges_stream", "enabled", "spi_turns_seen", "sync", "cleared",
           "credit", "credit_stamp", "quiet_written", "quiet_from")
# The interrupt's rate budget in counts of the 150 MHz stamping clock (src/dio_edges.c): a run's
# cost, the most credit kept, 32 runs' worth, and a rest, 1 ms.
RUN_COST = 150_000_000 // 100_000
CREDIT_MAX = 32 * RUN_COST
REST_COUNTS = 150_000_000 // 1000
# PINT's RISE register: a pin's rise stays latched, as the model never clears it.
PINT_RISE = 0x4000401C
# The stamping timer (src/energy_monitor.c).
CTIMER4_TC = 0x4002A008
# An entry (src/energy_stream.h): stamp, lost_from, rise, fall, levels, flags.
ENTRY_BYTES, ENTRY_LOST_FROM, ENTRY_LEVELS, ENTRY_FLAGS = 12, 4, 10, 11
# The four DIO pins high (src/dio_pins.h): P0_20, P0_26, P1_2, P1_3.
SPI_LEVELS = ((1 << 20) | (1 << 26), (1 << 2) | (1 << 3))


def edge_statics(symbols):
    """dio_edges.c's statics. LTO renames `present`, and verge_aux.c has one too: take the one
    beside `resting`."""
    found = {name: symbols[name][0] for name in STATICS}
    candidates = [address for name, (address, _) in symbols.items() if name == "present" or name.startswith("present.")]
    found["present"] = min(candidates, key=lambda address: abs(address - found["resting"]))
    return found


def check_edge_rest(c, elf, symbols, shared, layout):
    """A rest the pin interrupt starts while the loop begins a new stream's edges keeps its LOST
    mark, wherever the interrupt lands: the loop must not write back a `resting` it read before."""
    probe = machine(elf, symbols)
    statics = edge_statics(symbols)
    integration = symbols["energy_integration"][0]
    stream = integration + layout["stream"]
    dropped = integration + layout["stream.edge_dropped"]
    dropped_from = integration + layout["stream.edge_dropped_from"]
    dap_shared = symbols["dap_shared"][0]

    def byte(address, value):
        probe.uc.mem_write(address, bytes([value]))

    def word(address, value):
        probe.uc.mem_write(address, value.to_bytes(4, "little"))

    def read(address):
        return probe.uc.mem_read(address, 1)[0]

    def reset():
        """A stream that has just begun, with pin 0 reported, CPU 0 owning its state and SPI off."""
        byte(statics["present"], 1)
        word(statics["edges_stream"], stream)
        for name in ("resting", "enabled"):
            byte(statics[name], 0)
        byte(dropped, 0)
        for name in ("begins_seen", "spi_turns_seen", "rest_from"):
            word(statics[name], 0)
        byte(integration + layout["stream.active"], 1)
        byte(integration + layout["stream.edge_mask"], 1)
        byte(integration + layout["stream.timeline_pending"], 0)
        word(integration + layout["stream.edge_begins"], 1)
        word(integration + layout["stream.edge_entries_written"], 0)
        word(integration + layout["stream.edge_entries_read"], 0)
        word(dap_shared + shared["energy_control_ack"], 0)
        word(dap_shared + shared["dio_spi_turns"], 0)
        probe.uc.reg_write(UC_ARM_REG_PRIMASK, 0)

    trace = []
    reset()
    hook = probe.uc.hook_add(UC_HOOK_CODE, lambda uc, at, size, data: trace.append(at))
    probe.uc.ctl_flush_tb()
    probe.call("dio_edges_task", timeout_insns=10_000)
    probe.uc.hook_del(hook)
    c.check(read(dropped) == 0 and read(statics["enabled"]) == 1,
            "DIO edges: a new stream's first pass left edges marked lost or its pin off")

    lost = []
    for cut in range(len(trace)):
        reset()
        step = 0
        took = False

        def interrupt(uc, at, size, data):
            nonlocal step, took
            # The pin interrupt's rest (PIN_INT0_IRQHandler), where it can be taken: not under
            # PRIMASK, and not while a rest has its interrupts off.
            if step == cut and uc.reg_read(UC_ARM_REG_PRIMASK) == 0 and read(statics["resting"]) == 0:
                word(statics["rest_from"], 0)
                byte(statics["resting"], 1)
                if read(dropped) == 0:
                    word(dropped_from, 0)
                byte(dropped, 1)
                took = True
            step += 1

        hook = probe.uc.hook_add(UC_HOOK_CODE, interrupt)
        probe.uc.ctl_flush_tb()
        probe.call("dio_edges_task", timeout_insns=10_000)
        probe.uc.hook_del(hook)
        if took and read(statics["resting"]) == 1 and read(dropped) == 0:
            lost.append(cut)
    probe.uc.ctl_flush_tb()
    c.check(not lost, f"DIO edges: a rest started at instruction {lost} of a new stream's pass lost its LOST mark")
    # A rest already under way: the new stream loses the edges until it ends too, from the rest's
    # start, not from an older loss that was the last stream's.
    reset()
    byte(statics["resting"], 1)
    word(statics["rest_from"], 0x1234)
    # The rest began just now: the pass does not end it.
    word(CTIMER4_TC, 0x1234)
    byte(dropped, 1)
    word(dropped_from, 0x9999)
    probe.call("dio_edges_task", timeout_insns=10_000)
    since = int.from_bytes(probe.uc.mem_read(dropped_from, 4), "little")
    c.check(read(statics["resting"]) == 1 and read(dropped) == 1 and since == 0x1234,
            f"DIO edges: a stream begun during a rest marked its edges lost {read(dropped)} from {since:#x}")
    # A rest lasts 1 ms: a pass one count short of it keeps resting, and one at it ends the rest.
    ended = []
    for elapsed in (REST_COUNTS - 1, REST_COUNTS):
        reset()
        byte(statics["resting"], 1)
        word(statics["rest_from"], 0x1234)
        word(CTIMER4_TC, 0x1234 + elapsed)
        probe.call("dio_edges_task", timeout_insns=10_000)
        ended.append(read(statics["resting"]) == 0)
    c.check(ended == [False, True], f"DIO edges: a rest ended {ended[0]} one count short of 1 ms and {ended[1]} at it")
    c.note(f"DIO edges: a rest starting at any of {len(trace)} instructions of a new stream's pass keeps its LOST mark,"
           " one under way marks the new stream's edges lost, and a rest lasts 1 ms to the count")


class Pins:
    """GPIO's PIN registers. `take`, if set, runs at the first read, as SPI taking the pins just
    before it: CPU 1 counts, then the pins carry SPI's levels."""

    def __init__(self, probe):
        self.ports = [0, 0]
        self.take = None
        probe.uc.mem_unmap(ORDINARY_GPIO, 0x4000)
        probe.uc.mmio_map(ORDINARY_GPIO, 0x4000, self.read, None, lambda *args: None, None)

    def read(self, uc, offset, size, data):
        if offset not in (0x2100, 0x2104):
            return 0
        if self.take is not None:
            take, self.take = self.take, None
            take()
        return self.ports[(offset - 0x2100) // 4]


def check_edge_spi(c, elf, symbols, shared, layout):
    """Levels read after SPI took the pins are not recorded as theirs: CPU 1 counts `dio_spi_turns`
    before it moves them (src/dio.c), so a count read after the levels tells. Both readers: the pin
    interrupt's sync run, which `resync` pends right after the loop's last look at the count, and the
    header of a stream Verge Aux starts on a running capture (`dio_edges_pins`)."""
    statics = edge_statics(symbols)
    integration = symbols["energy_integration"][0]
    stream = integration + layout["stream"]
    dropped = integration + layout["stream.edge_dropped"]
    dropped_from = integration + layout["stream.edge_dropped_from"]
    turns = symbols["dap_shared"][0] + shared["dio_spi_turns"]
    spi_stamp = symbols["dap_shared"][0] + shared["dio_spi_stamp"]
    found = {}
    for spi in (False, True):
        probe = machine(elf, symbols)
        pins = Pins(probe)

        def take():
            probe.uc.mem_write(turns, (3).to_bytes(4, "little"))
            pins.ports[:] = SPI_LEVELS

        def byte(name, value):
            probe.uc.mem_write(statics[name], bytes([value]))

        def word(address, value):
            probe.uc.mem_write(address, value.to_bytes(4, "little"))

        # The interrupt's sync run, with credit to spare and the pins turned on under count 2.
        word(statics["edges_stream"], stream)
        for name, value in (("credit", CREDIT_MAX), ("credit_stamp", 0), ("spi_turns_seen", 2)):
            word(statics[name], value)
        for name, value in (("sync", 1), ("cleared", 0), ("present", 1)):
            byte(name, value)
        probe.uc.mem_write(dropped, b"\x00")
        word(spi_stamp, 0xABCD)
        word(turns, 2)
        word(integration + layout["stream.edge_entries_written"], 0)
        word(integration + layout["stream.edge_entries_read"], 0)
        pins.take = take if spi else None
        # Instructions from the stamp's read to the return, on the run that records an entry.
        executed, stamped = [0], []
        counter = probe.uc.hook_add(UC_HOOK_CODE, lambda uc, at, size, data: executed.__setitem__(0, executed[0] + 1))
        reader = probe.uc.hook_add(UC_HOOK_MEM_READ, lambda uc, access, address, size, value, data: stamped.append(executed[0]),
                                   begin=CTIMER4_TC, end=CTIMER4_TC + 3)
        probe.uc.ctl_flush_tb()
        probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
        probe.uc.hook_del(counter)
        probe.uc.hook_del(reader)
        if not spi:
            found["after stamp"] = executed[0] - stamped[0] if stamped else 0
        written = int.from_bytes(probe.uc.mem_read(integration + layout["stream.edge_entries_written"], 4), "little")
        entry = bytes(probe.uc.mem_read(integration + layout["stream.edge_entries"], ENTRY_BYTES))
        lost = probe.uc.mem_read(dropped, 1)[0]
        since = int.from_bytes(probe.uc.mem_read(dropped_from, 4), "little")
        found[("run", spi)] = (written, entry[ENTRY_LEVELS] if written else None, lost, since if lost else None)
        if spi:
            # SPI gives the pins back and the loop turns them on again: the run it asks for records the
            # levels, and its entry says edges were lost from the stamp CPU 1 wrote as SPI took them.
            word(turns, 4)
            word(statics["spi_turns_seen"], 4)
            byte("sync", 1)
            probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
            written = int.from_bytes(probe.uc.mem_read(integration + layout["stream.edge_entries_written"], 4), "little")
            entry = bytes(probe.uc.mem_read(integration + layout["stream.edge_entries"], ENTRY_BYTES))
            found["after"] = (written, entry[ENTRY_FLAGS] & 1, int.from_bytes(entry[ENTRY_LOST_FROM:ENTRY_LOST_FROM + 4], "little"),
                              probe.uc.mem_read(dropped, 1)[0])

        # The header's pins and levels, asked for all four.
        word(turns, 2)
        pins.ports[:] = [0, 0]
        pins.take = take if spi else None
        returned = probe.call("dio_edges_pins", 0xF, SCRATCH, SCRATCH + 4, timeout_insns=10_000) & 0xFF
        found[("header", spi)] = (returned, probe.uc.mem_read(SCRATCH, 1)[0])
    # A run that took an edge as SPI took the pins: the edge is timed by the run, so the gap starts at
    # the run's stamp when that comes first, and at SPI's otherwise.
    for tc, expect in ((0x100, 0x100), (0xFF00, 0xABCD)):
        probe = machine(elf, symbols)
        pins = Pins(probe)
        word = lambda address, value: probe.uc.mem_write(address, value.to_bytes(4, "little"))
        word(statics["edges_stream"], stream)
        for name, value in (("credit", CREDIT_MAX), ("credit_stamp", 0), ("spi_turns_seen", 2)):
            word(statics[name], value)
        for name, value in (("sync", 0), ("cleared", 0), ("present", 1)):
            probe.uc.mem_write(statics[name], bytes([value]))
        probe.uc.mem_write(dropped, b"\x00")
        word(spi_stamp, 0xABCD)
        word(turns, 2)
        word(PINT_RISE, 1)
        word(CTIMER4_TC, tc)
        word(integration + layout["stream.edge_entries_written"], 0)
        word(integration + layout["stream.edge_entries_read"], 0)

        def take_now():
            word(turns, 3)
            pins.ports[:] = SPI_LEVELS
        pins.take = take_now
        probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
        lost = probe.uc.mem_read(dropped, 1)[0]
        since = int.from_bytes(probe.uc.mem_read(dropped_from, 4), "little")
        c.check((lost, since) == (1, expect),
                f"DIO edges: a run that took an edge at {tc:#x} as SPI took the pins at 0xabcd; (dropped, from) {(lost, since)}")
    c.check(found[("run", False)] == (1, 0, 0, None), f"DIO edges: a sync run with SPI away recorded {found[('run', False)]}")
    c.check(found[("run", True)] == (0, None, 1, 0xABCD),
            f"DIO edges: SPI took the pins as the sync run read them; (entries, levels, dropped, from) {found[('run', True)]}")
    # Two recorded runs' stamps are at least this far apart: the next run reads its stamp only after
    # this one returns, and each instruction takes a cycle of CPU 0's clock or more, which is no
    # faster than the 150 MHz stamping clock. DIO lost messages rely on it: a loss from just after
    # one run ends at a later one.
    apart = found["after stamp"]
    c.check(apart >= 2, f"DIO edges: a recording run takes {found['after stamp']} instructions after its stamp,"
                        f" so two runs' stamps may be {apart:.1f} counts apart")
    c.note(f"DIO edges: a recording run takes {found['after stamp']} instructions after its stamp, so two"
           f" runs' stamps are at least {apart:.0f} counts apart")
    c.check(found["after"] == (1, 1, 0xABCD, 0),
            f"DIO edges: the run after SPI; (entries, lost, lost from, still dropped) {found['after']}")
    c.check(found[("header", False)] == (0xF, 0), f"DIO edges: a header with SPI away gave {found[('header', False)]}")
    c.check(found[("header", True)] == (0, 0),
            f"DIO edges: SPI took the pins as a header read them; (pins, levels) {found[('header', True)]}")
    # Runs that find SPI has the pins still pay for themselves and rest: with the pins turned on as
    # SPI took them, every SPI clock edge would run the interrupt and starve the loop that turns
    # them off.
    # No credit but a second since the last run fills the credit to its cap, 32 runs; 10 runs'
    # worth of credit and no time pays for 10. The stamp stays put, so no credit comes back.
    for credit, since, paid in ((0, (-150_000_000) & 0xFFFFFFFF, 32), (10 * RUN_COST, 0, 10)):
        probe = machine(elf, symbols)
        Pins(probe)
        probe.uc.mem_write(stream - layout["stream"] + layout["stream.edge_entries_written"], bytes(8))
        probe.uc.mem_write(statics["edges_stream"], stream.to_bytes(4, "little"))
        for name, value in (("credit", credit), ("credit_stamp", since), ("spi_turns_seen", 2)):
            probe.uc.mem_write(statics[name], value.to_bytes(4, "little"))
        for name in ("sync", "resting"):
            probe.uc.mem_write(statics[name], b"\x00")
        probe.uc.mem_write(dropped, b"\x00")
        probe.uc.mem_write(turns, (3).to_bytes(4, "little"))
        probe.uc.mem_write(PINT_RISE, (1).to_bytes(4, "little"))
        runs = 0
        while runs < 64 and probe.uc.mem_read(statics["resting"], 1)[0] == 0:
            probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
            runs += 1
        c.check(runs == paid + 1 and probe.uc.mem_read(statics["resting"], 1)[0] == 1,
                f"DIO edges: runs under SPI with {credit} credit rested at run {runs}, not after {paid}")
    # A rest with SPI away marks the edges lost from the run that started it.
    probe = machine(elf, symbols)
    Pins(probe)
    probe.uc.mem_write(stream - layout["stream"] + layout["stream.edge_entries_written"], bytes(8))
    probe.uc.mem_write(statics["edges_stream"], stream.to_bytes(4, "little"))
    for name, value in (("credit", 0), ("credit_stamp", 0x5555), ("spi_turns_seen", 2)):
        probe.uc.mem_write(statics[name], value.to_bytes(4, "little"))
    for name in ("sync", "resting"):
        probe.uc.mem_write(statics[name], b"\x00")
    probe.uc.mem_write(dropped, b"\x00")
    probe.uc.mem_write(turns, (2).to_bytes(4, "little"))
    probe.uc.mem_write(CTIMER4_TC, (0x5555).to_bytes(4, "little"))
    probe.uc.mem_write(PINT_RISE, (1).to_bytes(4, "little"))
    probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
    rested = (probe.uc.mem_read(statics["resting"], 1)[0], probe.uc.mem_read(dropped, 1)[0],
              int.from_bytes(probe.uc.mem_read(dropped_from, 4), "little"))
    c.check(rested == (1, 1, 0x5555), f"DIO edges: a rest; (resting, dropped, from) {rested}")
    c.note("DIO edges: levels read after SPI took the pins are recorded neither by the pin interrupt nor in a header,"
           " runs under SPI still rest, and each loss carries its start: SPI's stamp, a rest's run, a begun stream's rest")


# A quiet DIO stream sends its time every 200 ms (src/dio_edges.c), and a DIO time message's type.
QUIET_COUNTS = 150_000_000 // 5
DIO_TIME = 0xA6
# Verge Aux's stream slots (src/energy_stream.h): 16, each the largest message.
SLOTS, SLOT_BYTES = 16, 512


def check_dio_time(c, elf, symbols, shared, layout):
    """A quiet DIO stream's time (dio_time): sent once nothing has gone out for 200 ms, to the count,
    with the stamping timer's time then; held back while an entry waits, a loss is found or waiting,
    or the stream waits for its sync run. Wherever the pin interrupt lands in the pass, the edge it
    records stamps after any time the pass sends: a host promises the time, and refuses an edge
    before it."""
    statics = edge_statics(symbols)
    integration = symbols["energy_integration"][0]
    stream = integration + layout["stream"]
    slots = symbols["energy_stream_slots"][0]
    dap_shared = symbols["dap_shared"][0]
    origin_raw, origin_counts, sent = 0x10000, 1000, 3

    def put(probe, address, value, size=4):
        probe.uc.mem_write(address, value.to_bytes(size, "little"))

    def get(probe, address, size=4):
        return int.from_bytes(probe.uc.mem_read(address, size), "little")

    def field(probe, name, value, size=1):
        put(probe, integration + layout[name], value, size)

    def ready(quiet_for, **held):
        """A DIO stream of pin 0, quiet since `origin_raw`, which CPU 0 owns, its pin on."""
        probe = machine(elf, symbols)
        put(probe, statics["present"], 1, 1)
        put(probe, statics["edges_stream"], stream)
        put(probe, statics["enabled"], 1, 1)
        for name in ("resting", "sync"):
            put(probe, statics[name], 0, 1)
        put(probe, statics["begins_seen"], 1)
        put(probe, statics["spi_turns_seen"], 0)
        put(probe, statics["quiet_written"], sent)
        put(probe, statics["quiet_from"], origin_raw)
        for name, value in (("stream.active", 1), ("stream.pairless", 1), ("stream.edge_mask", 1),
                            ("stream.timeline_pending", 0), ("stream.edge_waiting", held.get("waiting", 0)),
                            ("stream.edge_dropped", held.get("dropped", 0)), ("stream.edge_lost", held.get("lost", 0))):
            field(probe, name, value)
        for name, value in (("stream.edge_begins", 1), ("stream.edge_entries_written", held.get("entries", 0)),
                            ("stream.edge_entries_read", 0), ("stream.written", sent), ("stream.sent", sent),
                            ("stream.next_raw", origin_raw), ("stream.clock_hz", 150_000_000)):
            field(probe, name, value, 4)
        field(probe, "stream.next_counts", origin_counts, 8)
        field(probe, "stream.edge_known", origin_counts, 8)
        # An entry waiting, just recorded: too new for the loop to send.
        entry = (origin_raw + quiet_for - 10).to_bytes(4, "little") + bytes(ENTRY_BYTES - 4)
        probe.uc.mem_write(integration + layout["stream.edge_entries"], entry)
        put(probe, dap_shared + shared["energy_control_ack"], 0)
        put(probe, dap_shared + shared["dio_spi_turns"], 0)
        put(probe, CTIMER4_TC, origin_raw + quiet_for)
        probe.uc.reg_write(UC_ARM_REG_PRIMASK, 0)
        return probe

    def times(probe):
        """The DIO time messages the pass published, in counts."""
        out = []
        for index in range(sent, get(probe, integration + layout["stream.written"])):
            at = index % SLOTS
            length = get(probe, integration + layout["stream.length"] + 2 * at, 2)
            message = bytes(probe.uc.mem_read(slots + SLOT_BYTES * at, length))
            c.check(length == 10 and message[:2] == bytes([DIO_TIME, 0]), f"DIO time: a pass published {message.hex()}")
            ns = int.from_bytes(message[2:10], "little")
            out.append(-(-ns * 150_000_000 // 1_000_000_000))
        return out

    found = {}
    for quiet_for in (QUIET_COUNTS - 1, QUIET_COUNTS):
        probe = ready(quiet_for)
        probe.call("dio_edges_task", timeout_insns=20_000)
        found[quiet_for] = (times(probe), get(probe, integration + layout["stream.edge_known"], 8))
    c.check(found[QUIET_COUNTS - 1] == ([], origin_counts), f"DIO time: one count short of 200 ms sent {found[QUIET_COUNTS - 1]}")
    at = origin_counts + QUIET_COUNTS
    c.check(found[QUIET_COUNTS] == ([at], at), f"DIO time: at 200 ms sent {found[QUIET_COUNTS]}, wanted ([{at}], {at})")
    held = {}
    for reason in ("entries", "dropped", "lost", "waiting"):
        probe = ready(QUIET_COUNTS, **{reason: 1})
        probe.call("dio_edges_task", timeout_insns=20_000)
        held[reason] = times(probe)
    c.check(all(not sent_times for sent_times in held.values()), f"DIO time: sent while held back: {held}")
    # A message the stream sent meanwhile starts the quiet again.
    probe = ready(QUIET_COUNTS)
    put(probe, statics["quiet_written"], sent - 1)
    probe.call("dio_edges_task", timeout_insns=20_000)
    c.check(times(probe) == [] and get(probe, statics["quiet_from"]) == origin_raw + QUIET_COUNTS,
            "DIO time: a message sent meanwhile did not start the quiet again")

    # The interrupt at each instruction of a sending pass, where it can be taken: not under PRIMASK.
    # The stamping timer moves a count an instruction, no faster than it runs.
    trace = []
    probe = ready(QUIET_COUNTS)
    hook = probe.uc.hook_add(UC_HOOK_CODE, lambda uc, address, size, data: trace.append(address))
    probe.uc.ctl_flush_tb()
    probe.call("dio_edges_task", timeout_insns=20_000)
    probe.uc.hook_del(hook)
    early = []
    for cut in range(len(trace)):
        probe = ready(QUIET_COUNTS)
        step = 0
        stamped = []

        def interrupt(uc, address, size, data):
            nonlocal step
            now = get(probe, CTIMER4_TC)
            if step == cut and uc.reg_read(UC_ARM_REG_PRIMASK) == 0:
                written = get(probe, integration + layout["stream.edge_entries_written"])
                probe.uc.mem_write(integration + layout["stream.edge_entries"] + ENTRY_BYTES * (written % 256),
                                   now.to_bytes(4, "little") + bytes(ENTRY_BYTES - 4))
                put(probe, integration + layout["stream.edge_entries_written"], written + 1)
                stamped.append(now)
            put(probe, CTIMER4_TC, now + 1)
            step += 1

        hook = probe.uc.hook_add(UC_HOOK_CODE, interrupt)
        probe.uc.ctl_flush_tb()
        probe.call("dio_edges_task", timeout_insns=20_000)
        probe.uc.hook_del(hook)
        for time in times(probe):
            if stamped and origin_counts + (stamped[0] - origin_raw) <= time:
                early.append((cut, stamped[0], time))
    probe.uc.ctl_flush_tb()
    c.check(not early, f"DIO time: an edge the interrupt took during the pass stamped at or before the time sent: {early[:3]}")
    # SPI taking the pins at each instruction of the pass, CPU 1's stamp then its count: a time sent
    # must come before SPI's stamp, since the loss SPI starts begins after the time.
    turns = dap_shared + shared["dio_spi_turns"]
    spi_stamp = dap_shared + shared["dio_spi_stamp"]
    crossed = []
    for cut in range(len(trace)):
        probe = ready(QUIET_COUNTS)
        step = 0
        taken = []

        def take(uc, address, size, data):
            nonlocal step
            now = get(probe, CTIMER4_TC)
            if step == cut:
                put(probe, spi_stamp, now)
                put(probe, turns, 1)
                taken.append(now)
            put(probe, CTIMER4_TC, now + 1)
            step += 1

        hook = probe.uc.hook_add(UC_HOOK_CODE, take)
        probe.uc.ctl_flush_tb()
        probe.call("dio_edges_task", timeout_insns=20_000)
        probe.uc.hook_del(hook)
        for time in times(probe):
            if taken and origin_counts + (taken[0] - origin_raw) <= time:
                crossed.append((cut, taken[0], time))
    probe.uc.ctl_flush_tb()
    c.check(not crossed, f"DIO time: a time sent at or after SPI took the pins: {crossed[:3]}")
    # The next pass, a count later, sends nothing: the quiet starts again at the time.
    probe = ready(QUIET_COUNTS)
    probe.call("dio_edges_task", timeout_insns=20_000)
    put(probe, CTIMER4_TC, origin_raw + QUIET_COUNTS + 1)
    probe.call("dio_edges_task", timeout_insns=20_000)
    c.check(len(times(probe)) == 1, f"DIO time: two passes 200 ms into the quiet sent {times(probe)}")
    c.note(f"DIO time: sent at 200 ms of quiet to the count and once, held back by a waiting entry, a loss or the sync,"
           f" and with the interrupt or SPI's take at any of {len(trace)} instructions of the pass, sent only before them")


# INPUTMUX's PINTSEL registers, and CTIMER4's TCR (src/dio_edges.c).
PINTSEL = 0x400060C0
CTIMER4_TCR = 0x4002A004
BOARD_PLAIN, BOARD_PRO = 0, 1


def j8_static(symbols, statics):
    """dio_edges.c's `j8`, which LTO may rename: the one nearest `resting`."""
    candidates = [address for name, (address, _) in symbols.items() if name == "j8" or name.startswith("j8.")]
    return min(candidates, key=lambda address: abs(address - statics["resting"]))


def check_j8_edges(c, elf, symbols, shared, layout, model_syscon):
    """The standalone MCU-Link's DIO edges (src/dio_edges.c): J8's pins as the pin interrupts'
    selects and their levels from GPIO port 1; the stamping timer started where no energy capture
    does it; and a DIO stream's origin moved on by CPU 0's loop, which nothing else moves."""
    statics = edge_statics(symbols)
    j8 = j8_static(symbols, statics)
    integration = symbols["energy_integration"][0]
    stream = integration + layout["stream"]
    dap_shared = symbols["dap_shared"][0]

    def put(probe, address, value, size=4):
        probe.uc.mem_write(address, value.to_bytes(size, "little"))

    def get(probe, address, size=4):
        return int.from_bytes(probe.uc.mem_read(address, size), "little")

    # dio_edges_init on each board, with and without energy capture. gcc's LTO drops the stream,
    # which main always passes, into a clone that takes the board and the jumpers alone.
    clone = next((name for name in symbols if name.startswith("dio_edges_init.constprop")), None)
    for board, energy, selects, timer in ((BOARD_PLAIN, 0, [32, 33, 34, 35], 1), (BOARD_PRO, 0, [20, 26, 34, 35], 1),
                                          (BOARD_PRO, 1, [20, 26, 34, 35], 0)):
        probe = machine(elf, symbols)
        model_syscon(probe)
        put(probe, dap_shared + shared["energy_available"], energy, 1)
        if clone is None:
            probe.call("dio_edges_init", stream, board, 0, timeout_insns=200_000)
        else:
            probe.call(clone, board, 0, timeout_insns=200_000)
            c.check(get(probe, statics["edges_stream"]) == stream, f"{clone} did not record energy_integration's stream")
        got = [get(probe, PINTSEL + 4 * pin) for pin in range(4)]
        c.check(got == selects, f"DIO edges on board {board}: pin interrupt selects {got}, wanted {selects}")
        c.check(get(probe, CTIMER4_TCR) & 1 == timer,
                f"DIO edges on board {board} with energy {energy}: stamping timer {'not ' if timer else ''}started")
        c.check(get(probe, j8, 1) == (board == BOARD_PLAIN) and get(probe, statics["present"], 1) == 1,
                f"DIO edges on board {board}: present or j8 wrong")

    # The interrupt's sync run and a stream header's levels, read on each board from ports that
    # tell the two apart: port 0 all high, port 1 0b1010.
    for on_j8, run_levels, header_pins in ((1, 0x0A, 0x0F), (0, 0x0B, 0x0F)):
        probe = machine(elf, symbols)
        pins = Pins(probe)
        pins.ports[:] = [0xFFFFFFFF, 0x0A]
        put(probe, j8, on_j8, 1)
        put(probe, statics["present"], 1, 1)
        put(probe, statics["edges_stream"], stream)
        for name, value in (("credit", CREDIT_MAX), ("credit_stamp", 0), ("spi_turns_seen", 0)):
            put(probe, statics[name], value)
        put(probe, statics["sync"], 1, 1)
        put(probe, dap_shared + shared["dio_spi_turns"], 0)
        put(probe, integration + layout["stream.edge_entries_written"], 0)
        probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
        entry = bytes(probe.uc.mem_read(integration + layout["stream.edge_entries"], ENTRY_BYTES))
        c.check(get(probe, integration + layout["stream.edge_entries_written"]) == 1 and entry[ENTRY_LEVELS] == run_levels,
                f"DIO edges, j8 {on_j8}: the interrupt read levels {entry[ENTRY_LEVELS]:#x}, wanted {run_levels:#x}")
        returned = probe.call("dio_edges_pins", 0xF, SCRATCH, SCRATCH + 4, timeout_insns=10_000) & 0xFF
        levels = probe.uc.mem_read(SCRATCH, 1)[0]
        c.check(returned == header_pins and levels == (run_levels & header_pins),
                f"DIO edges, j8 {on_j8}: a header's pins {returned:#x} and levels {levels:#x}, wanted {header_pins:#x}")
        # Mode 15 runs where the pin interrupts are and energy capture is not.
        for present, energy, ready in ((1, 0, 1), (1, 1, 0), (0, 0, 0)):
            put(probe, statics["present"], present, 1)
            put(probe, dap_shared + shared["energy_available"], energy, 1)
            got = probe.call("dio_edges_stream_ready", timeout_insns=1_000) & 1
            c.check(got == ready, f"DIO stream ready with present {present}, energy {energy}: {got}")

    # The loop moves a DIO stream's origin to the stamping timer, across its wrap; a capture's
    # stream it leaves to its pairs.
    for pairless in (1, 0):
        probe = machine(elf, symbols)
        put(probe, statics["present"], 1, 1)
        put(probe, statics["edges_stream"], stream)
        put(probe, integration + layout["stream.active"], 1, 1)
        put(probe, integration + layout["stream.pairless"], pairless, 1)
        put(probe, integration + layout["stream.edge_mask"], 0, 1)
        put(probe, integration + layout["stream.timeline_pending"], 0, 1)
        put(probe, integration + layout["stream.next_raw"], 0xFFFFFF00)
        put(probe, integration + layout["stream.next_counts"], 5, 8)
        put(probe, dap_shared + shared["energy_control_ack"], 0)
        put(probe, dap_shared + shared["dio_spi_turns"], 0)
        put(probe, CTIMER4_TC, 0x100)
        probe.uc.reg_write(UC_ARM_REG_PRIMASK, 0)
        probe.call("dio_edges_task", timeout_insns=10_000)
        raw = get(probe, integration + layout["stream.next_raw"])
        counts = get(probe, integration + layout["stream.next_counts"], 8)
        want = (0x100, 5 + 0x200) if pairless else (0xFFFFFF00, 5)
        c.check((raw, counts) == want, f"DIO stream origin, pairless {pairless}: {raw:#x}, {counts}; wanted {want}")
    c.note("DIO edges on J8: selects PIO1_0 to PIO1_3, levels from port 1, the timer started without energy capture, "
           "mode 15 ready only without energy capture, a DIO stream's origin moved by the loop")
