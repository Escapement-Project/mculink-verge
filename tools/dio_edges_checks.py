"""CPU 0's DIO edge bookkeeping (src/dio_edges.c) against its pin interrupt, run on the linked image."""

from cycles import SCRATCH
from energy_checks import ORDINARY_GPIO, machine
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_READ
from unicorn.arm_const import UC_ARM_REG_PRIMASK

STATICS = ("resting", "rest_from", "rest_counts", "begins_seen", "edges_stream", "enabled",
           "spi_turns_seen", "sync", "cleared", "credit", "credit_stamp", "burst", "run_cost")
# PINT's RISE register: a pin's rise stays latched, as the model never clears it.
PINT_RISE = 0x4000401C
# The stamping timer (src/energy_monitor.c).
CTIMER4_TC = 0x4002A008
# An entry (src/energy_stream.h): stamp, lost_from, rise, fall, levels, flags.
ENTRY_BYTES, ENTRY_LOST_FROM, ENTRY_LEVELS, ENTRY_FLAGS = 12, 4, 10, 11
# The four DIO pins high (src/dio_pins.h): P0_20, P0_26, P1_2, P1_3.
SPI_LEVELS = ((1 << 20) | (1 << 26), (1 << 2) | (1 << 3))


def edge_statics(symbols):
    """dio_edges.c's statics. LTO renames `present`, and aux.c has one too: take the one beside
    `resting`."""
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
        word(statics["rest_counts"], 96_000)
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
    c.note(f"DIO edges: a rest starting at any of {len(trace)} instructions of a new stream's pass keeps its LOST mark,"
           " and one under way marks the new stream's edges lost")


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
        for name, value in (("burst", 1000), ("credit", 1000), ("run_cost", 1), ("credit_stamp", 0),
                            ("spi_turns_seen", 2)):
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
    c.check(found[("run", False)] == (1, 0, 0, None), f"DIO edges: a sync run with SPI away recorded {found[('run', False)]}")
    c.check(found[("run", True)] == (0, None, 1, 0xABCD),
            f"DIO edges: SPI took the pins as the sync run read them; (entries, levels, dropped, from) {found[('run', True)]}")
    # Two recorded runs' stamps are at least this far apart: the next run reads its stamp only after
    # this one returns, and each instruction takes a cycle of CPU 0's 150 MHz clock or more. DIO
    # lost messages rely on it: a loss from just after one run ends at a later one.
    apart = found["after stamp"] * 96 / 150
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
    probe = machine(elf, symbols)
    Pins(probe)
    probe.uc.mem_write(stream - layout["stream"] + layout["stream.edge_entries_written"], bytes(8))
    probe.uc.mem_write(statics["edges_stream"], stream.to_bytes(4, "little"))
    for name, value in (("burst", 32), ("credit", 32), ("run_cost", 1), ("credit_stamp", 0), ("spi_turns_seen", 2)):
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
    c.check(runs <= 33 and probe.uc.mem_read(statics["resting"], 1)[0] == 1,
            f"DIO edges: runs under SPI did not rest after a burst of 32 ({runs} runs)")
    # A rest with SPI away marks the edges lost from the run that started it.
    probe = machine(elf, symbols)
    Pins(probe)
    probe.uc.mem_write(stream - layout["stream"] + layout["stream.edge_entries_written"], bytes(8))
    probe.uc.mem_write(statics["edges_stream"], stream.to_bytes(4, "little"))
    for name, value in (("burst", 32), ("credit", 0), ("run_cost", 1000), ("credit_stamp", 0x5555), ("spi_turns_seen", 2)):
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
