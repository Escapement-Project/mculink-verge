"""CPU 0's DIO edge bookkeeping (src/dio_edges.c) against its pin interrupt, run on the linked image."""

from cycles import SCRATCH
from energy_checks import ORDINARY_GPIO, machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_PRIMASK

STATICS = ("resting", "dropped", "rest_from", "rest_counts", "begins_seen", "edges_stream", "enabled",
           "spi_turns_seen", "sync", "cleared", "credit", "credit_stamp", "burst", "run_cost")
# PINT's RISE register: a pin's rise stays latched, as the model never clears it.
PINT_RISE = 0x4000401C
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
        for name in ("resting", "dropped", "enabled"):
            byte(statics[name], 0)
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
    c.check(read(statics["dropped"]) == 0 and read(statics["enabled"]) == 1,
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
                byte(statics["dropped"], 1)
                took = True
            step += 1

        hook = probe.uc.hook_add(UC_HOOK_CODE, interrupt)
        probe.uc.ctl_flush_tb()
        probe.call("dio_edges_task", timeout_insns=10_000)
        probe.uc.hook_del(hook)
        if took and read(statics["resting"]) == 1 and read(statics["dropped"]) == 0:
            lost.append(cut)
    probe.uc.ctl_flush_tb()
    c.check(not lost, f"DIO edges: a rest started at instruction {lost} of a new stream's pass lost its LOST mark")
    # A rest already under way: the new stream loses the edges until it ends too.
    reset()
    byte(statics["resting"], 1)
    byte(statics["dropped"], 1)
    probe.call("dio_edges_task", timeout_insns=10_000)
    c.check(read(statics["resting"]) == 1 and read(statics["dropped"]) == 1,
            "DIO edges: a stream begun during a rest did not mark its edges lost")
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
    turns = symbols["dap_shared"][0] + shared["dio_spi_turns"]
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
        for name, value in (("sync", 1), ("dropped", 0), ("cleared", 0), ("present", 1)):
            byte(name, value)
        word(turns, 2)
        word(integration + layout["stream.edge_entries_written"], 0)
        word(integration + layout["stream.edge_entries_read"], 0)
        pins.take = take if spi else None
        probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
        written = int.from_bytes(probe.uc.mem_read(integration + layout["stream.edge_entries_written"], 4), "little")
        entry = bytes(probe.uc.mem_read(integration + layout["stream.edge_entries"], 8))
        dropped = probe.uc.mem_read(statics["dropped"], 1)[0]
        found[("run", spi)] = (written, entry[6] if written else None, dropped)

        # The header's pins and levels, asked for all four.
        word(turns, 2)
        pins.ports[:] = [0, 0]
        pins.take = take if spi else None
        returned = probe.call("dio_edges_pins", 0xF, SCRATCH, SCRATCH + 4, timeout_insns=10_000) & 0xFF
        found[("header", spi)] = (returned, probe.uc.mem_read(SCRATCH, 1)[0])
    c.check(found[("run", False)] == (1, 0, 0), f"DIO edges: a sync run with SPI away recorded {found[('run', False)]}")
    c.check(found[("run", True)] == (0, None, 1),
            f"DIO edges: SPI took the pins as the sync run read them; (entries, levels, dropped) {found[('run', True)]}")
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
    for name in ("sync", "resting", "dropped"):
        probe.uc.mem_write(statics[name], b"\x00")
    probe.uc.mem_write(turns, (3).to_bytes(4, "little"))
    probe.uc.mem_write(PINT_RISE, (1).to_bytes(4, "little"))
    runs = 0
    while runs < 64 and probe.uc.mem_read(statics["resting"], 1)[0] == 0:
        probe.call("PIN_INT0_IRQHandler", timeout_insns=10_000)
        runs += 1
    c.check(runs <= 33 and probe.uc.mem_read(statics["resting"], 1)[0] == 1,
            f"DIO edges: runs under SPI did not rest after a burst of 32 ({runs} runs)")
    c.note("DIO edges: levels read after SPI took the pins are recorded neither by the pin interrupt nor in a header,"
           " and runs under SPI still rest")
