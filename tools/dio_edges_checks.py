"""CPU 0's DIO edge bookkeeping (src/dio_edges.c) against its pin interrupt, run on the linked image."""

from energy_checks import machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_PRIMASK

STATICS = ("resting", "dropped", "rest_from", "rest_counts", "begins_seen", "edges_stream", "enabled",
           "spi_turns_seen")


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
