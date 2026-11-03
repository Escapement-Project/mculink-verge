"""Energy DMA ownership checks executed by image.py against both linked images."""

import itertools
import struct

from cycles import Machine, Model, RETURN_MAGIC, SCRATCH, Target, sections, symbols, u32
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK, UC_ARM_REG_R0

DMA_BASE = 0x40082000
ENERGY_MASK = 1 << 21
XFERCFG_21 = 0x408 + 21 * 16
SWO_MASK = 1 << 8
ADC_BASE = 0x400A0000


class Dma:
    def __init__(self, probe):
        self.page = bytearray(4096)
        self.writes = []
        # Called with each read's offset before it is answered, to change state at a chosen read.
        self.on_read = None
        probe.uc.mem_unmap(DMA_BASE, 4096)
        probe.uc.mmio_map(DMA_BASE, 4096, self.read, None, self.write, None)

    def get(self, offset):
        return int.from_bytes(self.page[offset:offset + 4], "little")

    def set(self, offset, value):
        self.page[offset:offset + 4] = u32(value)

    def read(self, uc, offset, size, data):
        if self.on_read is not None:
            self.on_read(offset)
        return int.from_bytes(self.page[offset:offset + size], "little")

    def write(self, uc, offset, size, value, data):
        self.writes.append((offset, value))
        if offset in (0x40, 0x58, 0x60):
            self.set(offset, self.get(offset) & ~value)
        elif offset in (0x20, 0x48):
            self.set(offset, self.get(offset) | value)
        elif offset in (0x28, 0x50):
            self.set(offset - 8, self.get(offset - 8) & ~value)
        else:
            self.page[offset:offset + size] = value.to_bytes(size, "little")


class Adc:
    def __init__(self, probe):
        self.page = bytearray(4096)
        self.fifo = []
        self.active_reads = 0
        self.resets = 0
        self.software_commands = []
        probe.uc.mem_unmap(ADC_BASE, 4096)
        probe.uc.mmio_map(ADC_BASE, 4096, self.read, None, self.write, None)

    def read(self, uc, offset, size, data):
        if offset == 0x14 and self.active_reads:
            self.active_reads -= 1
            return 0x800
        if offset == 0x300:
            return self.fifo.pop(0) if self.fifo else 0
        return int.from_bytes(self.page[offset:offset + size], "little")

    def write(self, uc, offset, size, value, data):
        if offset == 0x14:
            previous = int.from_bytes(self.page[offset:offset + size], "little")
            self.page[offset:offset + size] = (previous & ~value).to_bytes(size, "little")
            return
        if offset == 0x10 and value & 0x100:
            self.fifo.clear()
            self.resets += 1
            value &= ~0x100
        if offset == 0x34 and value & 1:
            commands = tuple(int.from_bytes(self.page[at:at + 4], "little") for at in range(0x100, 0x118, 4))
            self.software_commands.append(commands)
            self.fifo.append(0x81002345)  # An on-demand conversion, not energy trigger 7.
            if (commands[0] >> 5) & 3 == 3:
                # A dual single-ended conversion: side B's result after side A's, both in FIFO0.
                self.fifo.append(0x81001234)
        self.page[offset:offset + size] = value.to_bytes(size, "little")


def machine(elf, symbols):
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    probe.sym.update({name: address for name, (address, _) in symbols.items()})
    return probe


# struct energy_integration's members (src/dap_shared.h), which image.py lays out.
INTEGRATION_FIELDS = ("low_range", "high_range", "scale_q32", "full_scale_mv", "rearm_below_na", "segments_at_start",
                      "segment_words", "calibrated", "running", "consumed_bytes", "decoder", "on_high_range", "rearm_blocked",
                      "previous_low", "held", "held_mv", "held_na", "held_weight_q16", "held_before_na",
                      "previous_known", "previous_na", "switch_weight_q16", "rearm_between_pairs", "trigger_ticks",
                      "rearm_from_ticks", "rearm_until_ticks", "descriptors", "ring", "setting",
                      "stream.written", "stream.sent", "stream.length", "stream.active", "stream.holds",
                      "stream.holds_written", "stream.next_sequence", "stream.next_raw", "stream.next_counts",
                      "stream.timeline_pending",
                      "capture_id", "capture_id_sequence")
# Where a machine without CPU 0's image keeps the struct: SRAM0, which CPU 1's image leaves alone.
INTEGRATION_AT = 0x20008000


class Integration:
    """struct energy_integration in an emulated machine's memory."""

    def __init__(self, probe, at, layout):
        self.probe, self.at, self.layout = probe, at, layout

    def put(self, field, value, size, offset=0):
        self.probe.uc.mem_write(self.at + self.layout[field] + offset, value.to_bytes(size, "little"))

    def get(self, field, size, offset=0):
        return int.from_bytes(self.probe.uc.mem_read(self.at + self.layout[field] + offset, size), "little")

    def address(self, field):
        return self.at + self.layout[field]

    def read_slots(self, size, index):
        return bytes(self.probe.uc.mem_read(self.probe.sym["energy_stream_slots"] + 512 * index, size))

    def write(self, field, data, offset=0):
        self.probe.uc.mem_write(self.address(field) + offset, data)

    def read(self, field, size, offset=0):
        return bytes(self.probe.uc.mem_read(self.address(field) + offset, size))

    def decoder_size(self):
        return self.layout["on_high_range"] - self.layout["decoder"]


def attach_integration(probe, offsets, layout, at=INTEGRATION_AT):
    """Points dap_shared.energy_integration at `at`, as CPU 0 does before it starts CPU 1."""
    probe.uc.mem_write(probe.sym["dap_shared"] + offsets["energy_integration"], u32(at))
    return Integration(probe, at, layout)


def dual_machine(elf0, symbols0, elf1, symbols1):
    """CPU 1's image and statics with CPU 0's beside them, in one memory: CPU 0's image carries
    CPU 1's code at its flash origin, and the two cores' RAM does not overlap. Names are CPU 1's;
    CPU 0's DMA interrupt is "cpu0:DMA0_IRQHandler"."""
    probe = Machine(elf0, Target([]), restart_at=set(), model=Model())
    size, vma, lma = sections(elf1)[".data"]
    probe.uc.mem_write(vma, bytes(probe.uc.mem_read(lma, size)))
    probe.sym = {name: address for name, (address, _) in symbols1.items()}
    probe.sym["energy_stream_slots"] = symbols(elf1)["energy_stream_slots"]
    probe.install_allocator_traps(probe.sym, elf1)
    probe.sym["cpu0:DMA0_IRQHandler"] = symbols0["DMA0_IRQHandler"][0]
    return probe


ORDINARY_GPIO = 0x4008C000
IOCON_BASE = 0x40001000
BOARD_KIND_PRO = 1


class Circuit:
    """The Pro's energy circuit as calibration sees it (BOARDS.md): the probe's 3.3 V on the shunts,
    loads from node T to ground, the divider, and the sense amplifier's gain of 30. The amplifier's
    offset and output floor are the ones a unit showed on the bench, in codes."""

    OFFSET = -9
    FLOOR = 57

    LOADS = {22: 6650.0, 23: 66.5, 26: 1650.0, 29: 16.5}  # PIO1 pin: ohms
    CAL_DISC = 24

    def __init__(self, setting_ma, gain=1.0, switch_ohms=0.24, supply=3.3, full_scale=3.0, contact=0.0,
                 offset=OFFSET):
        self.low = 50.0 if setting_ma == 350 else 200.0
        # Q3 at a gate-source voltage of -3.3 V, and J17's contact.
        self.high = (0.24 if setting_ma == 350 else 2.0) + 0.0165 + contact
        self.gain, self.switch_ohms, self.supply, self.full_scale = gain, switch_ohms, supply, full_scale
        self.offset = offset

    def shunt(self, low_range):
        return self.low if low_range else 1 / (1 / self.low + 1 / self.high)

    def code(self, volts):
        return max(0, min(65535, round(volts / self.full_scale * 65536)))

    def current_code(self, amps, low_range):
        ideal = self.code(30 * self.gain * amps * self.shunt(low_range))
        return min(65535, max(self.FLOOR, ideal + self.offset))

    def node(self, port1, low_range):
        """The shunt current and node T's voltage with the probe's 3.3 V on the shunts."""
        if not port1 >> self.CAL_DISC & 1:
            return 0.0, 0.0
        loads = [ohms + self.switch_ohms for pin, ohms in self.LOADS.items() if port1 >> pin & 1]
        node_ohms = 1 / (1 / 660e3 + sum(1 / ohms for ohms in loads))
        amps = self.supply / (0.05 + self.shunt(low_range) + node_ohms)
        return amps, amps * node_ohms

    def codes(self, port1, low_range):
        amps, volts = self.node(port1, low_range)
        return self.code(volts / 2), self.current_code(amps, low_range)

    def comparator(self, port1, low_range):
        """CMP0_OUT: low while the amplifier's output is over the ladder's step 28."""
        amps, _ = self.node(port1, low_range)
        return 0 if 30 * self.gain * amps * self.shunt(low_range) > 28 / 31 * self.full_scale else 1


class Gpio:
    """Ports 0 and 1 of the ordinary GPIO, with U18 behind them: clock P0_17, reset P0_29, Q on
    P0_30. `comparator` gives CMP0_OUT's level, which P0_29 carries in function 7. `took_29` records
    whether anything made P0_29 an output."""

    def __init__(self, probe, comparator=lambda: 1):
        self.probe = probe
        self.out = [0, 0]
        self.dir = [0, 0]
        self.q = 0
        self.comparator = comparator
        self.took_29 = False
        probe.uc.mem_unmap(ORDINARY_GPIO, 0x4000)
        probe.uc.mmio_map(ORDINARY_GPIO, 0x4000, self.read, None, self.write, None)

    def function_29(self):
        return int.from_bytes(self.probe.uc.mem_read(IOCON_BASE + 29 * 4, 4), "little") & 0xF

    def reset_level(self):
        if self.function_29() == 0 and self.dir[0] >> 29 & 1:
            return self.out[0] >> 29 & 1
        return self.comparator() if self.function_29() == 7 else 1

    def read(self, uc, offset, size, data):
        register, port = offset & ~0x7F, (offset & 0x7F) // 4
        if port > 1:
            return 0
        if register == 0x2000:
            return self.dir[port]
        if register == 0x2100:
            level = self.out[port] & self.dir[port]
            if port == 0:
                level = (level & ~(3 << 29)) | self.reset_level() << 29 | self.q << 30
            return level
        return 0

    def write(self, uc, offset, size, value, data):
        register, port = offset & ~0x7F, (offset & 0x7F) // 4
        if port > 1:
            return
        clock = self.out[0] >> 17 & 1
        if register == 0x2000:
            self.dir[port] = value
        elif register == 0x2200:
            self.out[port] |= value
        elif register == 0x2280:
            self.out[port] &= ~value
        elif register == 0x2300:
            self.out[port] ^= value
        elif register == 0x2380:
            self.dir[port] |= value
        elif register == 0x2400:
            self.dir[port] &= ~value
        self.took_29 |= bool(self.dir[0] >> 29 & 1)
        if not self.reset_level():
            self.q = 0
        elif not clock and self.out[0] >> 17 & 1:
            self.q = 1


class CircuitAdc(Adc):
    """Answers each software-triggered pair from the circuit's state at the trigger."""

    def __init__(self, probe, circuit, gpio):
        self.circuit, self.gpio, self.states = circuit, gpio, []
        super().__init__(probe)

    def write(self, uc, offset, size, value, data):
        if offset == 0x34 and value & 1:
            self.software_commands.append(
                tuple(int.from_bytes(self.page[at:at + 4], "little") for at in range(0x100, 0x118, 4)))
            loads = self.gpio.out[1] & sum(1 << pin for pin in Circuit.LOADS)
            self.states.append((self.gpio.q, loads))
            voltage, current = self.circuit.codes(self.gpio.out[1], self.gpio.q == 1)
            # One dual single-ended conversion: the voltage's result, then the current's.
            self.fifo += [0x81000000 | voltage, 0x81000000 | current]
            self.page[offset:offset + size] = value.to_bytes(size, "little")
            return
        super().write(uc, offset, size, value, data)


def firmware_target_na(table, current_code, voltage_code, full_scale_mv=3000):
    """energy_shunt_na, then energy_target_na, as src/energy_samples.h computes them."""
    codes, currents, slopes, count = table
    segment = 0
    while segment + 2 < count and current_code >= codes[segment + 1]:
        segment += 1
    product = (current_code - codes[segment]) * slopes[segment]
    shunt = max(0, currents[segment] + (abs(product) // 65536) * (1 if product >= 0 else -1))
    mv = voltage_code * 2 * full_scale_mv >> 16
    return max(0, shunt - (mv * DIVIDER_NA_PER_MV_Q16 >> 16))


def check_control(c, elf0, elf1, symbols0, symbols1, offsets0, offsets1, layout):
    probe = machine(elf0, symbols0)
    dma = Dma(probe)
    shared = probe.sym["dap_shared"]

    def put(field, value, size=4):
        probe.uc.mem_write(shared + offsets0[field], value.to_bytes(size, "little"))

    def get(field, size=4):
        return int.from_bytes(probe.uc.mem_read(shared + offsets0[field], size), "little")

    # CPU 0's own struct, with no capture running, so the integration returns at once.
    integrate = probe.sym["energy_integrate_segment"]
    trace = []
    hook = probe.uc.hook_add(UC_HOOK_CODE, lambda uc, at, size, data: trace.append(at))
    probe.uc.ctl_flush_tb()
    dma.set(0x58, ENERGY_MASK | SWO_MASK)
    probe.call("DMA0_IRQHandler")
    probe.uc.hook_del(hook)
    probe.uc.ctl_flush_tb()
    c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "energy IRQ did not return")

    # Admit the quiet request at every instruction, including before/inside/after the callback.
    for cut in range(len(trace)):
        put("energy_control_request", 0)
        put("energy_control_ack", 0)
        put("energy_segments", 0)
        put("swo_segments", 0)
        put("energy_dma_error", 0, 1)
        dma.set(0x40, 0)
        dma.set(0x58, ENERGY_MASK | SWO_MASK)
        step = 0
        calls = 0

        def interrupt(uc, at, size, data):
            nonlocal step, calls
            if step == cut:
                put("energy_control_request", 3)
            if at == integrate:
                calls += 1
            c.check(get("energy_control_ack") == 0, "quiet acknowledged inside the DMA handler")
            step += 1

        hook = probe.uc.hook_add(UC_HOOK_CODE, interrupt)
        probe.uc.ctl_flush_tb()
        probe.call("DMA0_IRQHandler")
        probe.uc.hook_del(hook)
        probe.uc.ctl_flush_tb()
        c.check(calls == 1 and get("energy_segments") == 1 and get("swo_segments") == 1,
                f"energy IRQ admission/count changed at instruction {cut}")
        c.check(get("energy_control_ack") == 0, "IRQ acknowledged before main-loop service")
        probe.call("energy_control_task")
        c.check(get("energy_control_ack") == 3, "main loop did not acknowledge quiet generation")
        # SWO's second segment raises INTB (src/dma_ring.h).
        dma.set(0x58, ENERGY_MASK)
        dma.set(0x60, ENERGY_MASK | SWO_MASK)
        dma.set(0x40, ENERGY_MASK)
        probe.call("DMA0_IRQHandler")
        c.check(get("energy_segments") == 1 and get("swo_segments") == 2,
                "quiet energy completion changed its count or blocked SWO")
        c.check(dma.get(0x58) == ENERGY_MASK and dma.get(0x60) == ENERGY_MASK and dma.get(0x40) == ENERGY_MASK,
                "quiet ISR consumed energy status owned by CPU1")
        dma.set(0x60, 0)
        put("energy_control_request", 4)
        probe.call("energy_control_task")
        c.check(get("energy_control_ack") == 4, "resume generation not acknowledged")
        dma.set(0x40, 0)
        dma.set(0x58, ENERGY_MASK)
        probe.call("DMA0_IRQHandler")
        c.check(get("energy_segments") == 2, "resumed energy handler stayed suppressed")

    # CPU 0's re-arm clocks U18 with P0_29 left on the comparator: the low range comes back only
    # while the comparator holds the reset high, including when it falls between the task's
    # check of P0_29 and its clock edge.
    for label, expected in (("high", 1), ("low", 0), ("falling after the check", 0)):
        rearm = machine(elf0, symbols0)
        reads = 0

        def comparator():
            return {"high": 1, "low": 0}.get(label, 1 if reads == 0 else 0)

        gpio = Gpio(rearm, comparator)
        plain_read = gpio.read

        def counted_read(uc, offset, size, data):
            nonlocal reads
            value = plain_read(uc, offset, size, data)
            reads += offset == 0x2100
            return value

        gpio.read = counted_read
        rearm.uc.mem_unmap(ORDINARY_GPIO, 0x4000)
        rearm.uc.mmio_map(ORDINARY_GPIO, 0x4000, gpio.read, None, gpio.write, None)
        rearm.uc.mem_write(IOCON_BASE + 29 * 4, u32(7))
        iocon_writes = []
        rearm.uc.hook_add(UC_HOOK_MEM_WRITE, lambda uc, access, at, size, value, data: iocon_writes.append(value),
                          begin=IOCON_BASE + 29 * 4, end=IOCON_BASE + 29 * 4 + 3)
        gpio.out[0] = gpio.dir[0] = 1 << 17
        for field, value in (("energy_range_retry_pending", 1), ("energy_range_retry_enabled", 1)):
            rearm.uc.mem_write(shared + offsets0[field], bytes([value]))
        rearm.call("energy_range_retry_task")
        c.check(gpio.q == expected and not iocon_writes and not gpio.took_29 and gpio.out[0] >> 17 & 1,
                f"re-arm with the comparator {label}: latch {gpio.q}, P0_29 mux writes {iocon_writes}, "
                f"P0_29 made an output {gpio.took_29}")
    c.note("CPU 0's re-arm sets the latch only through the comparator's reset, and never takes P0_29")

    # Where the rate leaves room, the re-arm waits for the converter to be between pairs, by CTIMER2
    # alone (a half period of 100 ticks here, the window 20 to 60 ticks after the trigger's rise),
    # and stays pending until then.
    for label, count, em3, fires in (("in the gap", 30, 1, True), ("converting", 5, 1, False),
                                     ("late in the gap", 80, 1, False), ("in the period's low half", 30, 0, False),
                                     ("at the window's end", 59, 1, True)):
        rearm = machine(elf0, symbols0)
        gpio = Gpio(rearm, lambda: 1)
        rearm.uc.mem_unmap(ORDINARY_GPIO, 0x4000)
        rearm.uc.mmio_map(ORDINARY_GPIO, 0x4000, gpio.read, None, gpio.write, None)
        gpio.out[0] = gpio.dir[0] = 1 << 17
        integration = rearm.sym["energy_integration"]
        for field, value, size in (("rearm_between_pairs", 1, 1), ("trigger_ticks", 100, 4), ("rearm_from_ticks", 20, 4),
                                   ("rearm_until_ticks", 60, 4)):
            rearm.uc.mem_write(integration + layout[field], value.to_bytes(size, "little"))
        rearm.uc.mem_write(CTIMER2_BASE + 0x08, u32(count))
        rearm.uc.mem_write(CTIMER2_BASE + 0x3C, u32(em3 << 3))
        for field, value in (("energy_range_retry_pending", 1), ("energy_range_retry_enabled", 1)):
            rearm.uc.mem_write(shared + offsets0[field], bytes([value]))
        rearm.call("energy_range_retry_task")
        pending = rearm.uc.mem_read(shared + offsets0["energy_range_retry_pending"], 1)[0]
        c.check(gpio.q == int(fires) and pending == int(not fires),
                f"re-arm {label}: latch {gpio.q}, still pending {pending}")
    c.note("a re-arm waits for the first half of the gap between pairs, by CTIMER2, and stays pending until it comes")

    probe.uc.reg_write(UC_ARM_REG_PRIMASK, 1)
    put("energy_control_request", 7)
    probe.call("energy_control_task")
    c.check(probe.uc.reg_read(UC_ARM_REG_PRIMASK) == 1, "energy service changed caller IRQ mask")

    # Run CPU1's actual pause with absent/stale acknowledgements and a stuck DMA channel.
    for label, request, ack, busy in (("absent ack", 0, 0, 0), ("stale quiet ack", 4, 3, 0),
                                       ("DMA busy", 0, 0, ENERGY_MASK)):
        cpu1 = machine(elf1, symbols1)
        dma1 = Dma(cpu1)
        cpu1.uc.mem_write(cpu1.sym["identity_initialized"], b"\x01")
        base = cpu1.sym["dap_shared"]
        energy = attach_integration(cpu1, offsets1, layout)
        energy.put("running", 1, 1)
        energy.put("decoder", 2, 1)  # the phase
        energy.put("stream.active", 1, 1)
        for name, value, size in (("energy_available", 1, 1), ("energy_control_request", request, 4),
                                  ("energy_control_ack", ack, 4), ("energy_accumulator_pj", 12345, 8),
                                  ("energy_accumulator_pairs", 123, 8)):
            cpu1.uc.mem_write(base + offsets1[name], value.to_bytes(size, "little"))
        dma1.set(0x38, busy)
        dma1.set(0x58, ENERGY_MASK)
        result = cpu1.call("energy_monitor_pause")
        c.check(cpu1.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and result == 0,
                f"{label}: pause did not fail within its bound")
        c.check(energy.get("running", 1) == 1 and energy.get("decoder", 1) == 2,
                f"{label}: capture state reused without ownership")
        c.check(not any(offset == 0x78 for offset, _ in dma1.writes) and dma1.get(0x58) == ENERGY_MASK,
                f"{label}: DMA aborted/status cleared before ownership")
        c.check(int.from_bytes(cpu1.uc.mem_read(ADC_BASE + 0x10, 4), "little") & 0x100 == 0,
                f"{label}: FIFO reset before ownership")
        c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_accumulator_pj"], 8)) == struct.pack("<Q", 12345),
                f"{label}: old total lost")
        c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_dma_error"], 1)) == b"\x01",
                f"{label}: failure not latched")
        # A loop that comes back late echoes the last request (energy_control_task). That has to
        # leave the state with CPU 0, whose loop then ends the stream (stream_fault, src/aux.c).
        last = int.from_bytes(cpu1.uc.mem_read(base + offsets1["energy_control_request"], 4), "little")
        c.check(last & 1 == 0, f"{label}: a failed pause left quiet asked for ({last:#x}); a late loop would hand"
                               " the state, and its open stream, to a core that has given up")
        if label == "absent ack":
            reply = cpu1.command(bytes([0x90, 0]) + u32(1000))
            c.check(len(reply) == 7 and reply[2] == 7, "start did not report missing ownership")
            reply = cpu1.command(bytes([0x90, 2]))
            c.check(len(reply) == 3 and reply[2] == 7, "stop did not report missing ownership")
            c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_accumulator_pj"], 8)) == struct.pack("<Q", 12345),
                    "failed start/stop reset the retained total")

    # A pause while CPU 1 holds the state already, as a start's calibration makes after the start's
    # own pause: it must not ask again. Unanswered, a second request would time out and be
    # withdrawn, handing CPU 0 the state and the stream CPU 1 goes on writing.
    held = machine(elf1, symbols1)
    Dma(held)
    held_base = held.sym["dap_shared"]
    attach_integration(held, offsets1, layout).put("stream.active", 1, 1)
    for name, value, size in (("energy_available", 1, 1), ("energy_control_request", 3, 4),
                              ("energy_control_ack", 3, 4)):
        held.uc.mem_write(held_base + offsets1[name], value.to_bytes(size, "little"))
    result = held.call("energy_monitor_pause")
    after = int.from_bytes(held.uc.mem_read(held_base + offsets1["energy_control_request"], 4), "little")
    c.check(result == 1 and after == 3, f"a pause with the state held already returned {result}, left request {after:#x}")

    cpu1 = machine(elf1, symbols1)
    dma1 = Dma(cpu1)
    base = cpu1.sym["dap_shared"]
    cpu1.uc.mem_write(base + offsets1["energy_available"], b"\x01")
    energy = attach_integration(cpu1, offsets1, layout)
    energy.put("running", 1, 1)
    energy.put("segment_words", SEGMENT, 4)  # as a start leaves it
    pending = 0

    def acknowledge(uc, at, size, data):
        nonlocal pending
        request = int.from_bytes(uc.mem_read(base + offsets1["energy_control_request"], 4), "little")
        ack = int.from_bytes(uc.mem_read(base + offsets1["energy_control_ack"], 4), "little")
        if request != ack:
            pending += 1
            c.check(int.from_bytes(uc.mem_read(ADC_BASE + 0x10, 4), "little") & 0x100 == 0,
                    "pause reset FIFO before delayed acknowledgement")
            if pending == 30:
                uc.mem_write(base + offsets1["energy_control_ack"], u32(request))

    hook = cpu1.uc.hook_add(UC_HOOK_CODE, acknowledge)
    cpu1.uc.ctl_flush_tb()
    result = cpu1.call("energy_monitor_pause")
    cpu1.uc.hook_del(hook)
    cpu1.uc.ctl_flush_tb()
    c.check(result == 1 and pending == 30 and energy.get("running", 1) == 0,
            "pause did not wait for delayed ownership before stopping")
    c.check(any(offset == 0x78 for offset, _ in dma1.writes) and
            int.from_bytes(cpu1.uc.mem_read(ADC_BASE + 0x10, 4), "little") & 0x100 != 0,
            "acknowledged pause did not stop/reset capture")
    c.note(f"energy quiet admission across {len(trace)} IRQ instructions, SWO isolation and bounded ownership failures")


CTIMER2_BASE = 0x40028000
CTIMER2_MR3 = CTIMER2_BASE + 0x24


class Trigger:
    """CTimer 2's TCR, the conversion trigger's enable, writes logged beside the ADC's and DMA0's
    status reads."""

    def __init__(self, probe, log, running):
        self.tcr = 1 if running else 0
        self.log = log
        probe.uc.mem_unmap(CTIMER2_BASE, 4096)
        probe.uc.mmio_map(CTIMER2_BASE, 4096, self.read, None, self.write, None)

    def read(self, uc, offset, size, data):
        return self.tcr if offset == 0x04 else 0

    def write(self, uc, offset, size, value, data):
        self.log.append(("tcr", offset, value))
        if offset == 0x04:
            self.tcr = value


class BusyAdc(Adc):
    def __init__(self, probe, log, busy):
        self.log = log
        super().__init__(probe)
        self.active_reads = busy

    def read(self, uc, offset, size, data):
        if offset == 0x14:
            self.log.append(("adc",))
        return super().read(uc, offset, size, data)


class BusyDma(Dma):
    def __init__(self, probe, log, busy):
        self.log = log
        self.busy = busy
        super().__init__(probe)

    def read(self, uc, offset, size, data):
        if offset == 0x38:
            self.log.append(("dma",))
            if self.busy:
                self.busy -= 1
                return ENERGY_MASK
        return super().read(uc, offset, size, data)


def check_hold(c, elf1, symbols1, offsets1, layout):
    """A latency measurement's hold: the trigger stops before anything waits, the wait lasts until
    the ADC and DMA0's energy channel are idle, and release starts the trigger again. Without a
    running capture, or with its timer already stopped by a pause that failed, nothing moves."""
    if "swd_sweep_last" not in symbols1 and "jtag_sweep_last" not in symbols1:
        c.note("no latency measurement in this image, so no hold to check")
        return
    if not c.check("energy_monitor_hold" in symbols1 and "energy_monitor_release" in symbols1,
                   "the latency measurement's hold is missing from the image"):
        return
    for available, running, timer_on, expect in ((1, 1, True, True), (1, 0, True, False), (1, 1, False, False),
                                                 (0, 1, True, False)):
        label = (f"hold, energy {'available' if available else 'unavailable'}, capture "
                 f"{'running' if running else 'stopped'}, timer {'on' if timer_on else 'off'}")
        cpu1 = machine(elf1, symbols1)
        log = []
        trigger = Trigger(cpu1, log, timer_on)
        adc = BusyAdc(cpu1, log, 3)
        dma = BusyDma(cpu1, log, 3)
        cpu1.uc.mem_write(cpu1.sym["dap_shared"] + offsets1["energy_available"], bytes([available]))
        attach_integration(cpu1, offsets1, layout).put("running", running, 1)
        held = cpu1.call("energy_monitor_hold")
        if expect:
            c.check(held == 1 and log[:1] == [("tcr", 0x04, 0)], f"{label}: {held}, first access {log[:1]}")
            c.check(adc.active_reads == 0 and dma.busy == 0 and trigger.tcr == 0,
                    f"{label}: trigger {trigger.tcr}, ADC {adc.active_reads} and DMA {dma.busy} busy polls left")
        else:
            c.check(held == 0 and not [entry for entry in log if entry[0] == "tcr"],
                    f"{label}: {held}, wrote {log}")
        before = len(log)
        cpu1.call("energy_monitor_release", held)
        writes = [entry for entry in log[before:] if entry[0] == "tcr"]
        c.check(writes == ([("tcr", 0x04, 1)] if expect else []), f"{label}: release wrote {writes}")
    c.note("latency-measurement hold stops, drains and restarts the energy trigger, and only a running one on a"
           " probe with energy capture")


# The fixture's calibration: this shunt current at every code.
SHUNT_NA = 1_000_000
# The fixture's capture: a rate the timer makes exactly, with full-size segments.
FIXTURE_RATE = 16000
# A full segment: ENERGY_SEGMENT_MAX_WORDS, the most a start picks.
SEGMENT = 512
FIXTURE_SCALE = (1 << 32) // FIXTURE_RATE
DIVIDER_NA_PER_MV_Q16 = (65536 * 1_000_000 + 330_000) // 660_000


def power_pw(voltage_code):
    """One sample's power at the plain board's full scale, less the voltage divider's current."""
    mv = voltage_code * 2 * 3300 >> 16
    return mv * (SHUNT_NA - (mv * DIVIDER_NA_PER_MV_Q16 >> 16))


def check_samples(c, elf0, elf1, symbols0, symbols1, offsets, layout, model_syscon):
    symbols = symbols1

    def result(command, code):
        return 0x80070000 | command << 24 | code

    def setup():
        probe = dual_machine(elf0, symbols0, elf1, symbols1)
        dma = Dma(probe)
        adc = Adc(probe)
        model_syscon(probe)
        base = probe.sym["dap_shared"]

        def put(name, value, size=4):
            probe.uc.mem_write(base + offsets[name], value.to_bytes(size, "little"))

        def get(name, size=4):
            return int.from_bytes(probe.uc.mem_read(base + offsets[name], size), "little")

        def local(name, value):
            probe.uc.mem_write(probe.sym[name], value.to_bytes(symbols[name][1], "little"))

        put("energy_available", 1, 1)
        energy = attach_integration(probe, offsets, layout, symbols0["energy_integration"][0])
        probe.energy = energy

        def flag(counted=None):
            """The offset of the flag DMA0 raises for segment `counted` of the capture, by default the
            next one CPU 0 has not counted: INTA for the ring's first half, INTB for the second."""
            if counted is None:
                counted = get("energy_segments") - energy.get("segments_at_start", 4)
            return 0x58 if counted % 2 == 0 else 0x60

        def integrate():
            """A completed segment, through CPU 0's whole interrupt, which integrates it and then
            counts it."""
            dma.set(flag(), dma.get(flag()) | ENERGY_MASK)
            probe.call("cpu0:DMA0_IRQHandler")

        probe.integrate = integrate
        probe.flag = flag
        for name, value in (("current_calibrated", 1), ("accumulator_started", 1), ("actual_rate_hz", FIXTURE_RATE),
                            ("requested_rate_hz", FIXTURE_RATE), ("current_range", 1), ("adc_ready", 1),
                            ("inputmux_ready", 1)):
            local(name, value)
        if "identity_initialized" in probe.sym:
            local("identity_initialized", 1)
        # What a start publishes for CPU 0: the plain board's full scale, the 350 mA setting's limit.
        # The capture is under way on the high range, which every sample here reads.
        for field, value, size in (("running", 1, 1), ("calibrated", 1, 1), ("scale_q32", FIXTURE_SCALE, 8),
                                   ("segment_words", SEGMENT, 4), ("full_scale_mv", 3300, 4),
                                   ("rearm_below_na", 1500000, 4), ("previous_low", 0, 1)):
            energy.put(field, value, size)
        calibration = struct.pack("<4H7IB3x", 0, 65535, 0, 0, SHUNT_NA, SHUNT_NA, 0, 0, 0, 0, 0, 2)
        for name in ("low_range_calibration", "high_range_calibration"):
            c.check(symbols[name][1] == len(calibration), "calibration fixture layout changed")
            probe.uc.mem_write(probe.sym[name], calibration)
        for field in ("low_range", "high_range"):
            energy.write(field, calibration)
        probe.uc.mem_write(0x40013010, u32(0x40000000))  # Enable the SDK's 96 MHz FRO source.

        def acknowledge(uc, access, at, size, value, data):
            put("energy_control_ack", value)

        # Ownership interleavings are checked separately; these tests model an idle CPU0.
        address = base + offsets["energy_control_request"]
        hook = probe.uc.hook_add(UC_HOOK_MEM_WRITE, acknowledge, begin=address, end=address + 3)
        probe.uc.ctl_flush_tb()

        def written(words, pending=False, run_out=False):
            """run_out: at a segment's end, the descriptor that has just run out, whose XFERCOUNT
            reads 0x3FF, rather than the next one loaded."""
            segment = energy.get("segment_words", 4)
            dma.set(0x58, 0)
            dma.set(0x60, 0)
            dma.set(flag(), ENERGY_MASK if pending else 0)
            left = 1024 if run_out and words % segment == 0 else segment - words % segment
            dma.set(0x408 + 21 * 16, (left - 1) << 16)

        def feed(words, start=0):
            ring = 2 * energy.get("segment_words", 4)
            for i, word in enumerate(words):
                energy.write("ring", u32(word), ((start + i) % ring) * 4)

        def pause():
            value = probe.call("energy_monitor_pause")
            c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and value == 1,
                    "energy tail pause did not return successfully")

        def total(voltage_codes, scale=FIXTURE_SCALE):
            expected_q32 = sum(power_pw(v) * scale for v in voltage_codes)
            pairs = get("energy_accumulator_pairs", 8)
            c.check(pairs == len(voltage_codes), f"tail sample count {pairs}, against {len(voltage_codes)}")
            c.check(get("energy_accumulator_pj", 8) == expected_q32 >> 32 and
                    get("energy_accumulator_fraction_q32") == expected_q32 & 0xffffffff,
                    "tail energy or fractional carry mismatch")

        return probe, dma, adc, put, get, local, written, feed, pause, total, hook

    limits = ((3323, 7), (6596, 6), (13001, 5), (25263, 4), (47808, 3), (86330, 2), (100000, 1))
    # Each limit's last request and the next, and the segment sizes' boundaries.
    rates = (1, 10, 1000, 3323, 3324, 4572, 4573, 6596, 6597, 6598, 9143, 9144, 13002, 13003, 18289, 18290, 25269,
             25270, 36571, 36572, 47832, 47833, 73115, 73116, 86408, 86409, 100000)

    def check_commands(words, average, calibration):
        tail = 0 if calibration else 2
        # Voltage and current as one dual single-ended conversion of CH0 (CMDL[CTYPE] 3), both
        # sampling for ADCK7 (CMDH[STS] 2).
        expected = (0xE0, (tail << 24) | (average << 12) | (2 << 8))
        c.check(words[:2] == expected, "ADC voltage/current channel, resolution, averaging, timing or chain changed")
        if not calibration:
            c.check(words[2:4] == (0x82, 0), "ADC range command is not a fresh unaveraged side-A conversion")

    for rate in rates:
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        local("requested_rate_hz", rate)
        value = probe.call("energy_capture_start", 1)
        c.check(value == 1 and probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "ADC configuration setup failed")
        actual = int.from_bytes(probe.uc.mem_read(probe.sym["actual_rate_hz"], 4), "little")
        average = next(average for limit, average in limits if actual <= limit)
        words = tuple(int.from_bytes(adc.page[at:at + 4], "little") for at in range(0x100, 0x118, 4))
        check_commands(words, average, False)
        # The pair, 27 + 28 N cycles of the ADC's FRO_HF / 8 clock, fits the conversion timer's
        # period of two match intervals, whatever rate the request rounded to.
        ticks = int.from_bytes(probe.uc.mem_read(CTIMER2_MR3, 4), "little") + 1
        c.check((27 + 28 * (1 << average)) * 8 <= 2 * ticks,
                f"{rate} pairs/s: {1 << average} averages overrun a period of {ticks} ticks a half")
        # The stand-in's weight is half the share of a period the current's conversions span, and a
        # re-arm waits for the first half of a pair's gap where that is 2 us or more (src/energy_pairs.h).
        window, busy = 28 * (1 << average) * 8, (27 + 28 * (1 << average) + 4) * 8
        weight = min(32768, ((window << 16) + 2 * ticks) // (4 * ticks))
        gap = max(0, 2 * ticks - busy)
        timing = tuple(probe.energy.get(name, size) for name, size in (
            ("switch_weight_q16", 4), ("rearm_between_pairs", 1), ("trigger_ticks", 4), ("rearm_from_ticks", 4),
            ("rearm_until_ticks", 4)))
        c.check(timing == (weight, int(gap >= 192), ticks, busy, busy + gap // 2),
                f"{actual} pairs/s: switch timing {timing}, against {(weight, int(gap >= 192), ticks, busy, busy + gap // 2)}")
        c.check(int.from_bytes(adc.page[0xBC:0xC0], "little") == 0x01000001,
                "ADC monitor is not hardware trigger 7 starting at command 1")
        # About 1 ms of the ring's seven words a pair: a power of two, no longer than 1 ms and over
        # 0.5, within 16 to 512 words.
        segment = probe.energy.get("segment_words", 4)
        counts = [probe.energy.get("descriptors", 4, 16 * i) >> 16 & 0x3ff for i in range(2)]
        c.check(segment & (segment - 1) == 0 and 16 <= segment <= SEGMENT and
                (segment * 1000 <= actual * 7 or segment == 16) and (segment * 2000 > actual * 7 or segment == SEGMENT) and
                counts == [segment - 1] * 2 and dma.get(0x408 + 21 * 16) >> 16 & 0x3ff == segment - 1,
                f"{actual} pairs/s: segments of {segment} words, descriptors count {counts}")
        # The first transfer and descriptor 0 fill the ring's first half and raise INTA (bit 4); descriptor 1
        # fills the second and raises INTB (bit 5).
        flags = [dma.get(0x408 + 21 * 16) >> 4 & 3] + [probe.energy.get("descriptors", 4, 16 * i) >> 4 & 3 for i in range(2)]
        c.check(flags == [1, 1, 2], f"{actual} pairs/s: the halves' interrupt flags are {flags}, not A, A, B")
        probe.uc.hook_del(hook)
    c.note("ADC monitor command registers at averaging boundaries use the actual timer rate; segments hold"
           " about 1 ms of pairs")

    # A start hands CPU 0 what CPU 1 holds, whatever the struct held before: the tables, whether
    # they are good, the rate's scale, the board's full scale and the setting's re-arm limit.
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    low = struct.pack("<4H7IB3x", 0, 30000, 65535, 0, 0, 900000, 2000000, 0, 30 << 16, 23 << 16, 0, 3)
    high = struct.pack("<4H7IB3x", 100, 65535, 0, 0, 5000, 50000000, 0, 0, 760 << 16, 0, 0, 2)
    probe.uc.mem_write(probe.sym["low_range_calibration"], low)
    probe.uc.mem_write(probe.sym["high_range_calibration"], high)
    local("current_range", 2)
    local("requested_rate_hz", 1000)
    for field, size in (("low_range", len(low)), ("high_range", len(high)), ("calibrated", 1), ("scale_q32", 8),
                        ("full_scale_mv", 4), ("rearm_below_na", 4), ("segment_words", 4), ("previous_known", 1)):
        probe.energy.write(field, bytes([0xee]) * size)
    value = probe.call("energy_capture_start", 1)
    c.check(probe.energy.get("previous_known", 1) == 0, "a start kept a pair before it for the stand-in")
    actual = int.from_bytes(probe.uc.mem_read(probe.sym["actual_rate_hz"], 4), "little")
    published = (probe.energy.read("low_range", len(low)), probe.energy.read("high_range", len(high)),
                 probe.energy.get("calibrated", 1), probe.energy.get("scale_q32", 8),
                 probe.energy.get("full_scale_mv", 4), probe.energy.get("rearm_below_na", 4),
                 probe.energy.get("segment_words", 4))
    c.check(value == 1 and published == (low, high, 1, (1 << 32) // actual, 3300, 400000, 16),
            f"a start published {published[2:]} and {'the' if published[:2] == (low, high) else 'other'} tables")
    probe.uc.hook_del(hook)
    c.note("a start publishes CPU 1's tables and constants to CPU 0's integration")

    if "target_sense_convert_pair_for_calibration" in symbols:
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        probe.energy.put("running", 0, 1)
        local("requested_rate_hz", 0)
        value = probe.call("target_sense_convert_pair_for_calibration", SCRATCH, SCRATCH + 4)
        c.check(value == 0 and probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "ADC calibration pair failed")
        check_commands(adc.software_commands[-1], 7, True)
        c.check(int.from_bytes(adc.page[0xA0:0xA4], "little") == 0x01000000,
                "ADC calibration is not software trigger 0 starting at command 1")
        c.check(bytes(probe.uc.mem_read(SCRATCH, 2)) == b"\x45\x23" and
                bytes(probe.uc.mem_read(SCRATCH + 4, 2)) == b"\x34\x12", "ADC calibration pair lost its results")
        probe.uc.hook_del(hook)
    c.note("ADC calibration pair at 128x averaging " +
           ("passed" if "target_sense_convert_pair_for_calibration" in symbols else "not run: entry inlined"))

    if c.check("current_calibrate" in symbols and "energy_monitor_calibrate" in symbols,
               "calibration entries missing"):
        # The ADC model answers every on-demand conversion with 0x2345, which CURR_RANGE_SET reads
        # as the 50 mA setting (2). current_calibrate is a stub that fails and records the setting
        # it was entered with.
        def calibrating(calibrated, setting, available=1, ack_after=0, pauses_fail_after=None):
            """ack_after: CPU 0 not yet in its loop, acknowledging each request only after that many
            instructions. pauses_fail_after: pauses past this many fail, as an unreadable setting."""
            probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
            put("energy_available", available, 1)
            probe.energy.put("running", 0, 1)
            local("current_calibrated", calibrated)
            local("current_range", setting)
            if ack_after:
                probe.uc.hook_del(hook)
            state = {"entered": [], "early": 0, "acks": 0, "waited": 0, "pauses": 0}

            def step(uc, at, size, data):
                if ack_after:
                    request = get("energy_control_request")
                    if request != get("energy_control_ack"):
                        state["waited"] += 1
                        if state["waited"] >= ack_after:
                            put("energy_control_ack", request)
                            state["acks"] += 1
                            state["waited"] = 0
                if at == probe.sym["energy_monitor_pause"]:
                    state["pauses"] += 1
                    state["early"] += ack_after and state["acks"] == 0
                    if pauses_fail_after is not None and state["pauses"] > pauses_fail_after:
                        uc.reg_write(UC_ARM_REG_R0, 0)
                        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
                if at == probe.sym["current_calibrate"]:
                    state["entered"].append(bytes(uc.mem_read(probe.sym["current_range"], 1))[0])
                    uc.reg_write(UC_ARM_REG_R0, 0)
                    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

            call_hook = probe.uc.hook_add(UC_HOOK_CODE, step)
            probe.uc.ctl_flush_tb()
            return probe, local, state, lambda: (probe.uc.hook_del(call_hook), ack_after or probe.uc.hook_del(hook))

        for calibrated, setting, label in ((0, 2, "nothing calibrated"), (1, 1, "the setting moved")):
            probe, local, state, done = calibrating(calibrated, setting)
            reply = probe.command(bytes([0x90, 0]) + u32(1000))
            c.check(state["entered"] == [2], f"start with {label} calibrated with settings {state['entered']}")
            c.check(reply == bytes([0x90, 0, 6]) + bytes(4), f"start with {label}: reply {reply.hex()}")
            done()
        probe, local, state, done = calibrating(1, 2)
        reply = probe.command(bytes([0x90, 0]) + u32(1000))
        c.check(not state["entered"] and reply[:3] == bytes([0x90, 0, 0]),
                f"start in the calibrated setting calibrated {state['entered']}, reply {reply.hex()}")
        done()
        # The start's own pause succeeds; the setting's conversions do not.
        probe, local, state, done = calibrating(1, 2, pauses_fail_after=1)
        reply = probe.command(bytes([0x90, 0]) + u32(1000))
        kept = (bytes(probe.uc.mem_read(probe.sym["current_calibrated"], 1))[0],
                bytes(probe.uc.mem_read(probe.sym["current_range"], 1))[0])
        c.check(not state["entered"] and reply[:3] == bytes([0x90, 0, 6]) and kept == (1, 2),
                f"unreadable setting: calibrated {state['entered']}, reply {reply.hex()}, kept {kept}")
        done()
        probe, local, state, done = calibrating(0, 0, ack_after=500)
        probe.call("energy_monitor_calibrate")
        c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and state["entered"] == [2] and not state["early"],
                f"boot calibration: settings {state['entered']}, {state['early']} pauses before CPU 0's loop answered")
        done()
        probe, local, state, done = calibrating(0, 0, available=0, ack_after=500)
        probe.call("energy_monitor_calibrate")
        c.check(not state["entered"] and not state["pauses"],
                f"boot calibration without energy capture: settings {state['entered']}, {state['pauses']} pauses")
        done()
    c.note("a start calibrates only after a failure or with the setting moved, and keeps the tables when the"
           " setting cannot be read; boot calibration waits for CPU 0's loop, reads the setting, and runs"
           " only with energy capture")

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    written(0)
    pause()
    total([])
    probe.uc.hook_del(hook)

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    feed([result(1, 32768), result(1, 1), result(2, 0), result(1, 16384), result(1, 1)])
    adc.fifo = [result(2, 0)]
    adc.active_reads = 3
    written(5)
    pause()
    total([32768, 16384])
    c.check(adc.active_reads == 0 and not adc.fifo and adc.resets == 1, "settled ADC tail was not drained")
    reply = probe.command(bytes([0x90, 2]))
    c.check(reply == bytes([0x90, 0, 0]), "stop after pause failed")
    total([32768, 16384])
    probe.uc.hook_del(hook)

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    codes = [8192 * (i % 3 + 1) for i in range((SEGMENT - 1) // 9)]
    words = [0] * SEGMENT
    for i, code in enumerate(codes):
        words[i * 9:i * 9 + 3] = [result(1, code), result(1, 1), result(2, 0)]
    words[-1] = result(1, 49152)
    feed(words)
    probe.integrate()
    put("energy_segments", 1)
    total(codes)
    feed([result(1, 1), result(2, 0), result(1, 4096)], SEGMENT)
    written(SEGMENT + 3)
    reply = probe.command(bytes([0x90, 1]))
    c.check(len(reply) == 21 and reply[2] == 0 and int.from_bytes(reply[3:5], "little") == 49152 and
            int.from_bytes(reply[7:11], "little") == len(codes) + 1, "latest lost a tuple spanning DMA segments")
    # The calibrated fields: millivolts, and nanoamps less the divider's share.
    mv = 49152 * 2 * 3300 >> 16
    calibrated = (mv, SHUNT_NA - (mv * DIVIDER_NA_PER_MV_Q16 >> 16), 1)
    got = struct.unpack_from("<IixB", reply, 11)
    c.check(got == calibrated, f"latest's calibrated fields {got}, not {calibrated}")
    # A current past i32's nanoamps reads as the largest, not as a negative one.
    table = bytes(probe.uc.mem_read(probe.sym["high_range_calibration"], struct.calcsize("<4H7IB3x")))
    probe.uc.mem_write(probe.sym["high_range_calibration"],
                       struct.pack("<4H7IB3x", 0, 65535, 0, 0, 4_000_000_000, 4_000_000_000, 0, 0, 0, 0, 0, 2))
    got = struct.unpack_from("<i", probe.command(bytes([0x90, 1])), 15)[0]
    probe.uc.mem_write(probe.sym["high_range_calibration"], table)
    c.check(got == 2**31 - 1, f"latest's current past i32 read {got} nA")
    # Calibrated fields on bit 5, in nA; bit 1 meant uA, and a host that knows only it reads none.
    # Markers, bit 7, come with the capture.
    info = probe.command(bytes([0x82]))
    extended = int.from_bytes(info[14 + info[8]:18 + info[8]], "little")
    c.check(extended & 0b10100111 == 0b10100101, f"energy's extended capabilities {extended:#x}")
    total(codes)  # Reading latest must not integrate the same tail twice.
    pause()
    codes.append(49152)
    total(codes)
    # A real on-demand sense request pauses/resumes the monitor without resetting session numbering.
    reply = probe.command(bytes([0x86, 1, 0]))
    c.check(len(reply) == 7 and reply[1] == 0, "on-demand sense/resume failed")
    c.check((adc.software_commands[-1][1] >> 8) & 7 == 5, "on-demand sensing lost its ADCK35 sample time")
    c.check(probe.energy.get("running", 1) == 1, "sense did not resume capture")
    # The sense read borrowed command 1; the resume gives it back to the dual pair.
    average = next(average for limit, average in limits if FIXTURE_RATE <= limit)
    restored = tuple(int.from_bytes(adc.page[at:at + 4], "little") for at in range(0x100, 0x110, 4))
    c.check(restored == (0xE0, (2 << 24) | (average << 12) | (2 << 8), 0x82, 0),
            f"after a sense read, the capture's commands are {[hex(w) for w in restored]}")
    total(codes)
    feed([result(1, 1), result(2, 0), result(1, 32768), result(1, 1), result(2, 0)])
    written(5)
    reply = probe.command(bytes([0x90, 1]))
    c.check(reply[2] == 0 and int.from_bytes(reply[7:11], "little") == len(codes) + 1,
            "session sequence reset or an old partial tuple crossed restart")
    pause()
    total(codes + [32768])
    # The actual startup routine resets a new session only after release succeeds.
    value = probe.call("energy_capture_start", 1)
    c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and value == 1, "new capture setup failed")
    total([])
    probe.uc.hook_del(hook)

    # The fixture's full-size segments, and the sizes a start picks at 100,000, 5,000 and 1,000 pairs/s.
    # After an odd number of segments through CPU 0, the drain runs past the ring's end.
    for (rate, size), counted in itertools.product(((FIXTURE_RATE, SEGMENT), (100000, 512), (5000, 32), (1000, 16)),
                                                   (3, 4)):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        if rate != FIXTURE_RATE:
            local("requested_rate_hz", rate)
            c.check(probe.call("energy_capture_start", 1) == 1, f"{rate} pairs/s: start failed")
        segment = probe.energy.get("segment_words", 4)
        if not c.check(segment == size, f"{rate} pairs/s: segments of {segment} words, not {size}"):
            probe.uc.hook_del(hook)
            continue
        codes = []
        stream = []
        # Segments through CPU 0, one more completed but not yet counted, and part of the next.
        for i in range((counted + 1) * size + min(17, size - 1)):
            sample, phase = divmod(i, 9)
            code = 4096 * (sample % 15 + 1)
            stream.append(result(2 if phase == 2 else 1, code if phase == 0 else 1 if phase == 1 else 0) if phase < 3 else 0)
            if phase == 2:
                codes.append(code)
        for index in range(counted):
            feed(stream[index * size:(index + 1) * size], index * size)
            probe.integrate()
            put("energy_segments", index + 1)
        feed(stream[counted * size:], counted * size)
        # A descriptor that has just run out, counted or not yet.
        for pending in (False, True):
            words = (counted + pending) * size
            whole = (words - 3) // 9 + 1
            written(words, pending=pending, run_out=True)
            reply = probe.command(bytes([0x90, 1]))
            c.check(reply[2] == 0 and int.from_bytes(reply[3:5], "little") == codes[whole - 1] and
                    int.from_bytes(reply[7:11], "little") == whole,
                    f"{size}-word segments, {counted} through CPU 0, run out{' and pending' if pending else ''}:"
                    f" latest {reply.hex()}")
        # The same ring read a segment short after that: latest keeps the pair it already gave.
        written(counted * size, pending=False, run_out=True)
        again = probe.command(bytes([0x90, 1]))
        c.check(again == reply, f"{size}-word segments, {counted} through CPU 0: latest went back from "
                                f"{reply.hex()} to {again.hex()}")
        written(len(stream), pending=True)
        reply = probe.command(bytes([0x90, 1]))
        c.check(reply[2] == 0 and int.from_bytes(reply[3:5], "little") == codes[-1] and
                int.from_bytes(reply[7:11], "little") == len(codes),
                f"{size}-word segments, {counted} through CPU 0: latest {reply.hex()}")
        dma.set(0x58, SWO_MASK)
        dma.set(probe.flag(), dma.get(probe.flag()) | ENERGY_MASK)
        pause()
        total(codes, (1 << 32) // rate)
        c.check(dma.get(0x58) == SWO_MASK and dma.get(0x60) == 0, "tail drain cleared another channel's completion")
        # Repeated pauses do not consume the settled extent again.
        pause()
        total(codes, (1 << 32) // rate)
        probe.uc.hook_del(hook)

    # Two completions CPU 0 never took leave more than the ring holds: an error, at every size.
    for rate in (FIXTURE_RATE, 1000, 10):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        if rate != FIXTURE_RATE:
            local("requested_rate_hz", rate)
            c.check(probe.call("energy_capture_start", 1) == 1, f"{rate} pairs/s: start failed")
        put("energy_segments", get("energy_segments") + 2)
        written(0, pending=True)
        value = probe.call("energy_monitor_pause")
        c.check(value == 0 and get("energy_dma_error", 1) == 1,
                f"{rate} pairs/s: a drain past the ring returned {value}, error {get('energy_dma_error', 1)}")
        probe.uc.hook_del(hook)

    # CPU 0 stays inside the ring whatever CPU 1 left in the segment size.
    for words in (0, 3, 1000, 0x10000, 0xFFFFFFFF):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        probe.energy.put("segment_words", words, 4)
        probe.integrate()
        consumed = probe.energy.get("consumed_bytes", 4)
        c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and consumed <= 8192,
                f"segments of {words:#x} words: CPU 0 consumed {consumed} bytes of an 8192-byte ring")
        probe.uc.hook_del(hook)

    for byte_wrap in (True, False):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        segment_bytes = 4 * SEGMENT
        probe.energy.put("consumed_bytes", (1 << 32) - segment_bytes if byte_wrap else 0, 4)
        probe.energy.put("segments_at_start", 0 if byte_wrap else 0xffffffff, 4)
        put("energy_segments", (1 << 32) // segment_bytes - 1 if byte_wrap else 0xffffffff)
        put("energy_accumulator_pairs", 0xffffffff, 8)
        decoder = bytearray(probe.energy.decoder_size())
        decoder[12:16] = u32(0xffffffff)
        decoder[16] = 1
        probe.energy.write("decoder", bytes(decoder))
        words = [result(2 if i % 9 == 2 else 1, 32768 if i % 9 == 0 else 1 if i % 9 == 1 else 0)
                 if i % 9 < 3 else 0 for i in range(SEGMENT)]
        pairs = (SEGMENT + 6) // 9
        feed(words, SEGMENT if byte_wrap else 0)
        probe.integrate()
        put("energy_segments", (1 << 32) // segment_bytes if byte_wrap else 0)
        written(0)
        reply = probe.command(bytes([0x90, 1]))
        c.check(reply[2] == 0 and int.from_bytes(reply[7:11], "little") == pairs - 1 and
                get("energy_accumulator_pairs", 8) == 0xffffffff + pairs,
                "energy byte/segment/sample counter wrap lost samples")
        probe.uc.hook_del(hook)
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    feed([result(1, 32768), result(1, 1), result(2, 0)])
    written(3)
    pause()
    total([32768])
    probe.uc.hook_del(hook)
    probe.uc.ctl_flush_tb()
    value = probe.call("energy_capture_start", 1)
    c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and value == 0,
            "failed new-session release was accepted")
    total([32768])
    c.check(not any(offset == 0x20 for offset, _ in dma.writes), "failed start enabled DMA")

    # A new start forgets the pair latest gave in the capture before, whose sequence was higher.
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    feed([result(1, 32768), result(1, 1), result(2, 0)] * 2)
    written(6)
    before = probe.command(bytes([0x90, 1]))
    c.check(probe.call("energy_capture_start", 1) == 1, "the second start failed")
    feed([result(1, 16384), result(1, 1), result(2, 0)])
    written(3)
    after = probe.command(bytes([0x90, 1]))
    c.check(int.from_bytes(before[7:11], "little") == 2 and int.from_bytes(after[7:11], "little") == 1 and
            int.from_bytes(after[3:5], "little") == 16384,
            f"latest after a new start answered {after.hex()}, before it {before.hex()}")
    probe.uc.hook_del(hook)

    # A segment that stays on the high range under the 350 mA setting's 1.5 mA asks for the low
    # range back; one sample over, or a segment that ends on the low range, does not. The cases run
    # in turn on one machine, so a segment over the limit must not hold back the next.
    samples = SEGMENT * 3 // 10
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    # 100 nA a code, so 10000 is 1 mA and 20000 is 2 mA.
    probe.energy.write("high_range", struct.pack("<4H7IB3x", 0, 65535, 0, 0, 0, 6553500, 0, 0, 100 << 16, 0, 0, 2))
    for segment, (label, currents, ranges, expected) in enumerate((
            ("one sample over", [10000] * (samples - 1) + [20000], [0] * samples, 0),
            ("no pair, after one over", [], [], 0),
            ("under the limit, after one over", [10000] * samples, [0] * samples, 1),
            ("ends on the low range", [10000] * samples, [0] * (samples - 1) + [65535], 0),
            ("under the limit again", [10000] * samples, [0] * samples, 1))):
        put("energy_range_retry_pending", 0, 1)
        words = [word for current, state in zip(currents, ranges)
                 for word in (result(1, 32768), result(1, current), result(2, state))]
        feed(words + [0] * (SEGMENT - len(words)), segment * SEGMENT)
        put("energy_segments", segment)
        probe.integrate()
        c.check(get("energy_range_retry_pending", 1) == expected, f"low-range re-arm, {label}")
    probe.uc.hook_del(hook)
    c.note("a high-range segment under the low range's limit re-arms it; one over, a low-range end or no pair"
           " does not")

    # The same at sizes a start picks, with pairs running on across segment ends as in a capture: a
    # pair counts in the segment its range word lands in. One over the limit has its current at the
    # end of one segment and its range at the start of the next; a later one on the low range ends
    # a segment.
    for rate, size in ((1000, 16), (FIXTURE_RATE, 64)):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        local("requested_rate_hz", rate)
        c.check(probe.call("energy_capture_start", 1) == 1 and probe.energy.get("segment_words", 4) == size,
                f"{rate} pairs/s: start or segment size for the re-arm check")
        probe.energy.write("high_range", struct.pack("<4H7IB3x", 0, 65535, 0, 0, 0, 6553500, 0, 0, 100 << 16, 0, 0, 2))
        pairs = [(10000, False)] * 200
        straddle = next(i for i in range(len(pairs)) if (3 * i + 2) % size == 0)
        pairs[straddle] = (20000, False)
        ends_low = next(i for i in range(len(pairs))
                        if (3 * i + 2) % size == size - 1 and (3 * i + 2) // size > (3 * straddle + 2) // size)
        pairs[ends_low] = (10000, True)
        words = [word for current, low in pairs
                 for word in (result(1, 32768), result(1, current), result(2, 65535 if low else 0))]
        base = get("energy_segments")
        got, want = [], []
        for index in range(len(words) // size):
            put("energy_range_retry_pending", 0, 1)
            feed(words[index * size:(index + 1) * size], index * size)
            put("energy_segments", base + index)
            probe.integrate()
            got.append(get("energy_range_retry_pending", 1))
            done = [pair for i, pair in enumerate(pairs) if (3 * i + 2) // size == index]
            want.append(int(bool(done) and not done[-1][1] and
                            not any(not low and current >= 15000 for current, low in done)))
        c.check(got == want and 0 in want and 1 in want,
                f"{size}-word segments: re-arm by segment {got}, against {want}")
        probe.uc.hook_del(hook)
    c.note("re-arm by segment at 16 and 64 words, pairs straddling segment ends")

    # A pair that switched up takes the next pair's current less the previous pair's share, the
    # weight; one that switched down takes the next pair's, or keeps its own where re-arms land
    # between pairs; either keeps its own if the next pair switched too, or at a pause; a start
    # counts as the low range, with no pair before it. Distinct tables (30 and 6000 nA a code) make
    # the swap show: a blended code read on the high range is hundreds of milliamps.
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    per_code = {True: 30, False: 6000}
    for field, name, low in (("low_range", "low_range_calibration", True), ("high_range", "high_range_calibration", False)):
        k = per_code[low]
        table = struct.pack("<4H7IB3x", 0, 65535, 0, 0, 0, k * 65535, 0, 0, k << 16, 0, 0, 2)
        # CPU 1's own, which a start publishes again, and CPU 0's copy.
        probe.uc.mem_write(probe.sym[name], table)
        probe.energy.write(field, table)
    probe.energy.put("previous_low", 1, 1)  # as a start leaves it

    def expected(capture, weight, between):
        """The rule, for one capture's pairs of (voltage code, current code, low range)."""
        q32 = 0
        for i, (voltage, code, low) in enumerate(capture):
            before = capture[i - 1] if i else None
            switched = low != (before[2] if before else True)
            following = capture[i + 1] if i + 1 < len(capture) else None
            na = code * per_code[low]
            if switched and not (low and between) and following and following[2] == low:
                share = weight if (not low and before) else 0
                before_na = before[1] * per_code[before[2]] if before else 0
                na = (following[1] * per_code[low] * (65536 - share) + before_na * share + 32768) >> 16
            mv = voltage * 2 * 3300 >> 16
            q32 += mv * max(na - (mv * DIVIDER_NA_PER_MV_Q16 >> 16), 0) * FIXTURE_SCALE
        return q32

    def switching():
        return probe.energy.get("switch_weight_q16", 4), probe.energy.get("rearm_between_pairs", 1) == 1

    def words(capture):
        return [w for voltage, code, low in capture for w in (result(1, voltage), result(1, code), result(2, 65535 if low else 0))]

    def accumulated():
        return (get("energy_accumulator_pairs", 8), get("energy_accumulator_pj", 8), get("energy_accumulator_fraction_q32"))

    def resume(label):
        c.check(probe.command(bytes([0x86, 1, 0]))[1] == 0 and probe.energy.get("running", 1) == 1, f"{label}: resume failed")

    # One capture: up mid-segment (a blended code), down then straight back up, down on segment 0's
    # last pair, and up on the ring's last pair, whose next pair is still in the ADC's FIFO at the
    # pause. Segment 0 goes through CPU 0's interrupt, the rest through CPU 1's drain.
    # At 1024 words: 100, 99, 138 and 60 pairs, the switch down on pair 340.
    last_whole = (SEGMENT - 3) // 3
    run = SEGMENT * 100 // 1024
    shape = ([(50000, True)] * run + [(59000, False)] + [(300, False)] * (run - 1) + [(40000, True), (59000, False)] +
             [(300, False)] * (last_whole - 2 * run - 2) + [(40000, True)] + [(20000, True)] * (run * 3 // 5) +
             [(59000, False)])
    first = [(30000 + i % 7 * 1000, code, low) for i, (code, low) in enumerate(shape)]
    tail = (41000, 300, False)
    probe.energy.put("switch_weight_q16", 20000, 4)
    probe.energy.put("rearm_between_pairs", 1, 1)
    feed(words(first))
    probe.integrate()
    written(3 * len(first))
    adc.fifo = words([tail])
    pause()
    q32 = expected(first + [tail], 20000, True)
    c.check(accumulated() == (len(first) + 1, q32 >> 32, q32 & 0xffffffff),
            f"range switches: {accumulated()[:2]}, against {len(first) + 1} pairs and {q32 >> 32} pJ")

    # A resume after a capture that ended high: the first low pair keeps its own, the first switch up
    # weighs the pair before it with the start's own weight, and a switch on the last pair keeps its
    # own at the pause.
    resume("resume")
    weight, between = switching()
    c.check(weight != 0, "a start left no weight for the stand-in")
    second = [(36000, 1000, True), (37000, 2000, True), (38000, 59000, False), (38500, 300, False), (39000, 3000, True),
              (39500, 3500, True), (40000, 59000, False)]
    feed(words(second))
    written(3 * len(second))
    pause()
    q32 += expected(second, weight, between)
    c.check(accumulated() == (len(first) + 1 + len(second), q32 >> 32, q32 & 0xffffffff),
            f"range switch across a resume: {accumulated()[:2]}, against {q32 >> 32} pJ")

    # A capture that fails while a pair waits drops it; the next session starts clean. CPU 0's
    # interrupt takes a segment that ends on a switch, then the failure stops the drain.
    resume("resume before a failure")
    size = probe.energy.get("segment_words", 4)
    pairs_in = (size - 1) // 3
    segment = ([0] * ((size - 1) % 3) + words([(36000, 10000, True)] * (pairs_in - 1) + [(37000, 59000, False)]) +
               [result(1, 36000)])
    feed(segment)
    probe.integrate()
    c.check(probe.energy.get("held", 1) == 1, "the segment's last pair is not waiting")
    put("energy_dma_error", 1, 1)
    pause()
    local("requested_rate_hz", FIXTURE_RATE)
    c.check(probe.call("energy_capture_start", 1) == 1, "new session after a failure did not start")
    # Without room between pairs, a down-switch takes the next pair's current.
    probe.energy.put("rearm_between_pairs", 0, 1)
    third = [(36000, 1000, True), (37000, 2000, True), (38000, 59000, False), (38500, 300, False), (39000, 3000, True),
             (39500, 3500, True)]
    feed(words(third))
    written(3 * len(third))
    pause()
    q32 = expected(third, probe.energy.get("switch_weight_q16", 4), False)
    c.check(accumulated() == (len(third), q32 >> 32, q32 & 0xffffffff),
            f"new session after a failure with a pair waiting: {accumulated()[:2]}, against {len(third)} pairs")
    probe.uc.hook_del(hook)
    c.note("a pair across a range switch takes the weighted stand-in up, the next pair's current or its own"
           " down, its own if the next one switched too, across segments, the FIFO's tail and a pause; a start"
           " counts as the low range with no pair before it; a failed capture's waiting pair stays out of the"
           " next session")

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    adc.active_reads = -1
    value = probe.call("energy_monitor_pause")
    c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and value == 0 and adc.resets == 0,
            "stuck ADC did not fail without discarding pending data")
    probe.uc.hook_del(hook)
    # "late": the second half's segment and the first's after it completed before CPU 0 took the first,
    # so both flags are up. "out of turn": CPU 0 takes the first half's flag when the second's is due.
    for fault in ("IRQ error", "pending error", "FIFO overflow", "late", "out of turn"):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        words = [result(1, 32768), result(1, 1), result(2, 0)] + [0] * (SEGMENT - 3)
        feed(words)
        probe.integrate()
        put("energy_segments", 1)
        written(SEGMENT)
        total([32768])
        if fault == "IRQ error":
            put("energy_dma_error", 1, 1)
        elif fault == "pending error":
            dma.set(0x40, ENERGY_MASK)
        elif fault == "late":
            dma.set(0x58, ENERGY_MASK)
            dma.set(0x60, ENERGY_MASK)
        elif fault == "out of turn":
            dma.set(0x58, ENERGY_MASK)
            probe.call("cpu0:DMA0_IRQHandler")
            c.check(dma.get(0x58) == 0 and get("energy_segments") == 2,
                    f"{fault}: CPU 0 left the flag up or did not count it")
        else:
            adc.page[0x14:0x18] = u32(2)
        reply = probe.command(bytes([0x90, 1]))
        c.check(len(reply) == 21 and reply[2] == 7, f"{fault}: latest hid capture failure")
        reply = probe.command(bytes([0x90, 3]))
        c.check(len(reply) == 19 and reply[2] == 7 and int.from_bytes(reply[3:11], "little") == (power_pw(32768) * FIXTURE_SCALE >> 32) // 1000 and
                int.from_bytes(reply[11:19], "little") == 1, f"{fault}: total status/layout lost retained data")
        feed(words, SEGMENT)
        probe.integrate()
        total([32768])
        reply = probe.command(bytes([0x90, 2]))
        c.check(reply == bytes([0x90, 0, 7]), f"{fault}: stop hid capture failure")
        c.check(probe.command(bytes([0x90, 1]))[2] == 7 and probe.command(bytes([0x90, 3]))[2] == 7,
                f"{fault}: stop cleared sticky status")
        reply = probe.command(bytes([0x90, 0]) + u32(0))
        c.check(reply[2] == 4 and probe.command(bytes([0x90, 3]))[2] == 7,
                f"{fault}: failed start cleared sticky status")
        local("requested_rate_hz", FIXTURE_RATE)
        # Both halves' flags, as a capture that failed can leave them.
        dma.set(0x58, dma.get(0x58) | ENERGY_MASK)
        dma.set(0x60, ENERGY_MASK)
        c.check(probe.call("energy_capture_start", 1) == 1, f"{fault}: new-session recovery failed")
        c.check(dma.get(0x58) & ENERGY_MASK == 0 and dma.get(0x60) == 0, f"{fault}: a start left a flag up")
        total([])
        c.check(probe.command(bytes([0x90, 1]))[2] == 2 and probe.command(bytes([0x90, 3]))[2] == 2,
                f"{fault}: successful start did not clear failure")
        probe.uc.hook_del(hook)
    # CPU 0's own checks of the flags it takes, at an even count and an odd one: both halves' (a
    # segment and the next completed before the interrupt ran), or the other half's alone. The
    # capture fails, the segment is not integrated, and the flags are cleared.
    for counted, flags, label in ((0, (0x58, 0x60), "both"), (1, (0x58, 0x60), "both"), (0, (0x60,), "INTB")):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        words = [result(1, 32768), result(1, 1), result(2, 0)] + [0] * (SEGMENT - 3)
        feed(words)
        feed(words, SEGMENT)
        if counted:
            probe.integrate()
        pairs = get("energy_accumulator_pairs", 8)
        for offset in flags:
            dma.set(offset, dma.get(offset) | ENERGY_MASK)
        probe.call("cpu0:DMA0_IRQHandler")
        c.check(get("energy_dma_error", 1) == 1 and get("energy_accumulator_pairs", 8) == pairs and
                dma.get(0x58) == 0 and dma.get(0x60) == 0 and get("energy_segments") == counted + 1,
                f"CPU 0 took {label} at count {counted}: error {get('energy_dma_error', 1)}, pairs"
                f" {get('energy_accumulator_pairs', 8)} from {pairs}, flags {dma.get(0x58):#x} {dma.get(0x60):#x}")
        probe.uc.hook_del(hook)

    # CPU 1's latest pair while CPU 0 is late: the second half completes just before CPU 1 reads the
    # ring's position, so the half it then decodes is being rewritten; CPU 0 takes and clears both
    # flags before CPU 1's last check, and has not stored its error yet. The reply is a failure.
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    words = [result(1, 32768), result(1, 1), result(2, 0)] + [0] * (SEGMENT - 3)
    feed(words)
    written(SEGMENT // 2, pending=True)
    reads = {0x40: 0, XFERCFG_21: 0}

    def race(offset):
        if offset in reads:
            reads[offset] += 1
        if offset == XFERCFG_21 and reads[offset] == 1:
            dma.set(0x60, ENERGY_MASK)
        if offset == 0x40 and reads[offset] == 2:
            dma.set(0x58, 0)
            dma.set(0x60, 0)

    dma.on_read = race
    reply = probe.command(bytes([0x90, 1]))
    dma.on_read = None
    c.check(reads[0x40] >= 2 and reply[2] == 7, f"latest beside a late CPU 0 answered {reply[:3].hex()}, "
                                               f"ERRINT read {reads[0x40]} times")
    probe.uc.hook_del(hook)

    # DMA0 completes the next segment halfway through CPU 0's integration of this one: DMA0 was filling
    # this half again, so the capture fails. Completed just after, it is the next segment's to take. A
    # code hook paces it: AGENTS.md's unicorn trap rules out a memory hook on the ring.
    steps = 0
    for during in (False, True):
        label = "next segment completed during the read" if during else "next segment completed after the read"
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        words = [result(1, 32768), result(1, 1), result(2, 0)] + [0] * (SEGMENT - 3)
        feed(words)
        feed(words, SEGMENT)
        executed = 0

        def step(uc, at, size, data):
            nonlocal executed
            executed += 1
            if during and executed == steps // 2:
                dma.set(0x60, dma.get(0x60) | ENERGY_MASK)

        counter = probe.uc.hook_add(UC_HOOK_CODE, step)
        probe.uc.ctl_flush_tb()
        probe.integrate()
        probe.uc.hook_del(counter)
        probe.uc.ctl_flush_tb()
        if not during:
            steps = executed
            dma.set(0x60, ENERGY_MASK)
        c.check(get("energy_dma_error", 1) == (1 if during else 0) and get("energy_segments") == 1,
                f"{label}: error {get('energy_dma_error', 1)}, {get('energy_segments')} segments counted")
        probe.call("cpu0:DMA0_IRQHandler")
        reply = probe.command(bytes([0x90, 3]))
        c.check(reply[2] == (7 if during else 0) and int.from_bytes(reply[11:19], "little") == (1 if during else 2) and
                dma.get(0x60) == 0, f"{label}: mode 3 answered {reply.hex()}")
        probe.uc.hook_del(hook)
    c.note("energy partial/full/pending segments, FIFO tail, split tuples, Q32 carry and session resume/reset;"
           " each half's own flag, and a segment taken late, out of turn or overtaken during its read fails")
    check_stream(c, setup, result, offsets, local_symbols=symbols)
    check_identity(c, setup, offsets)
    from energy_lifecycle import check_sequences
    check_sequences(c, setup, result, FIXTURE_RATE)


def check_calibration(c, elf, symbols, offsets, model_syscon):
    """The Pro's calibration against a model of its circuit, in both jumper settings."""

    def run(setting_ma, **parts):
        probe = machine(elf, symbols)
        model_syscon(probe)
        circuit = Circuit(setting_ma, **parts)
        gpio = Gpio(probe, lambda: circuit.comparator(gpio.out[1], gpio.q == 1))
        adc = CircuitAdc(probe, circuit, gpio)
        for name, value in (("board_kind", BOARD_KIND_PRO), ("adc_ready", 1),
                            ("current_range", 1 if setting_ma == 350 else 2)):
            probe.uc.mem_write(probe.sym[name], value.to_bytes(symbols[name][1], "little"))
        probe.uc.mem_write(IOCON_BASE + 29 * 4, u32(7))  # CMP0_OUT, as pins_init leaves it
        delay = probe.sym["calibration_settle"]

        def skip(uc, at, size, data):
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

        hook = probe.uc.hook_add(UC_HOOK_CODE, skip, begin=delay, end=delay)
        probe.uc.ctl_flush_tb()
        ok = probe.call("current_calibrate") == 1 and probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC
        probe.uc.hook_del(hook)
        tables = {}
        for low in (True, False):
            name = "low_range_calibration" if low else "high_range_calibration"
            fields = struct.unpack("<4H7IB3x", bytes(probe.uc.mem_read(probe.sym[name], symbols[name][1])))
            tables[low] = (fields[0:4], fields[4:8], fields[8:11], fields[11])
        return probe, gpio, circuit, adc, ok, tables

    def readout(probe, high):
        """Vendor 0x90 mode 4: the result, then the table as the struct holds it. Set only now:
        with energy available, calibration's reads would wait on CPU 0, which this machine lacks."""
        probe.uc.mem_write(probe.sym["dap_shared"] + offsets["energy_available"], b"\x01")
        reply = probe.command(bytes([0x90, 4, 1 if high else 0]))
        c.check(len(reply) == 28 and reply[1] == 0, f"mode 4 reply {reply.hex()}")
        points = [struct.unpack_from("<HI", reply, 4 + 6 * i) for i in range(4)]
        return reply[2], (tuple(code for code, _ in points), tuple(na for _, na in points), reply[3])

    # Loads converted by latch state, and the high range's points. With nominal parts, R1 lands 18
    # codes over the 350 mA setting's high range floor, too close to keep.
    plans = {350: ({22, 26}, {22, 26, 23, 29}, 3), 50: ({22}, {22, 26, 23}, 3)}
    for setting_ma, (low_loads, high_loads, high_points) in plans.items():
        probe, gpio, circuit, adc, ok, tables = run(setting_ma)
        c.check(ok, f"{setting_ma} mA: calibration failed on nominal parts")
        c.check(tables[True][3] == len(low_loads) + 1 and tables[False][3] == high_points,
                f"{setting_ma} mA: points {tables[True][3]} low, {tables[False][3]} high")
        loads_seen = {low: {pin for q, loads in adc.states if q == low for pin in Circuit.LOADS if loads >> pin & 1}
                      for low in (1, 0)}
        c.check(loads_seen[1] == low_loads and loads_seen[0] == high_loads,
                f"{setting_ma} mA: loads by latch state {loads_seen}")
        c.check(all((words[0] >> 5) & 3 == 3 and (words[1] >> 12) & 7 == 7 for words in adc.software_commands),
                f"{setting_ma} mA: a calibration conversion was not one dual pair averaged 128 times")
        c.check(gpio.out[1] & ((1 << Circuit.CAL_DISC) | sum(1 << pin for pin in Circuit.LOADS)) == 0 and
                gpio.function_29() == 7 and not gpio.dir[0] >> 29 & 1 and not gpio.out[0] >> 17 & 1,
                f"{setting_ma} mA: calibration left the target isolated, a load on or the comparator off the latch")
        # Target currents across each range, at two supplies, read back through a copy of
        # src/energy_samples.h's arithmetic, which tools/energy_samples_test.c holds to the C. The high
        # range's include where it starts to matter: the re-arm limit and the trip.
        trip = 28 / 31 * 3.0 / 30 / circuit.shunt(True)
        rearm = 1.5e-3 if setting_ma == 350 else 400e-6
        worst = 0.0
        for low in (True, False):
            top = trip if low else setting_ma / 1000
            step = circuit.full_scale / 65536 / 30 / circuit.shunt(low)
            extra = () if low else (rearm / top, trip / top)
            for volts in (3.3, 1.8):
                for fraction in (0.01, 0.05, 0.2, 0.5, 0.9) + extra:
                    target = top * fraction
                    if not low and circuit.current_code(target, False) <= circuit.FLOOR:
                        continue
                    shunt_amps = target + volts / 660e3
                    measured = firmware_target_na(tables[low], circuit.current_code(shunt_amps, low),
                                                  circuit.code(volts / 2)) / 1e9
                    error = abs(measured - target)
                    worst = max(worst, error / target)
                    c.check(error <= max(0.002 * target, 3 * step),
                            f"{setting_ma} mA {'low' if low else 'high'} range, {target * 1e6:.1f} uA at {volts} V: "
                            f"read {measured * 1e6:.3f} uA")
        c.note(f"calibration {setting_ma} mA: loads by range as planned, worst read-back {worst:.3%}")
        for low in (True, False):
            result, (codes, currents, count) = readout(probe, not low)
            c.check(result == 0 and (codes, currents, count) == (tables[low][0], tables[low][1], tables[low][3]),
                    f"{setting_ma} mA: mode 4 differs from the {'low' if low else 'high'} range's table")

    # J17's contact can double the 350 mA setting's high range resistance (BOARDS.md). Then R1
    # clears the floor and is a point, unless the amplifier's offset pins it at the floor.
    for offset, kept in ((Circuit.OFFSET, True), (-110, False)):
        _, _, circuit, _, ok, tables = run(350, contact=0.25, offset=offset)
        currents, count = tables[False][1], tables[False][3]
        c.check(ok and count == (4 if kept else 3) and abs(currents[0] - (500_000 if kept else 2_000_000)) < 50_000,
                f"350 mA, 0.5 ohm high range, offset {offset}: {count} points from {currents[0]} nA")
        for target in (0.8e-3, 1.1e-3, 1.5e-3):
            code = circuit.current_code(target + 3.3 / 660e3, False)
            measured = firmware_target_na(tables[False], code, circuit.code(3.3 / 2)) / 1e9
            c.check(abs(measured - target) <= 0.002 * target,
                    f"350 mA, 0.5 ohm high range, offset {offset}: {target * 1e6:.0f} uA read {measured * 1e6:.3f} uA")
    _, _, _, _, ok, tables = run(350, gain=1.03)
    c.check(ok and tables[True][3] == 2, "350 mA: a saturated R3 point did not drop out of the low range")
    _, _, _, _, ok, _ = run(50, gain=1.025)
    c.check(ok, "50 mA: calibration failed 2.5% above nominal gain")
    probe, _, _, _, ok, _ = run(50, gain=1.04)
    c.check(not ok, "50 mA: calibration passed with its only low-range load saturated")
    result, (codes, currents, count) = readout(probe, False)
    c.check(result == 6 and count == 1 and codes[1:] == (0, 0, 0) and currents[1:] == (0, 0, 0),
            f"mode 4 after a failed calibration: result {result}, {count} points, {codes}, {currents}")
    c.note("calibration drops a saturated point and fails when a range keeps fewer than two; mode 4 reports"
           " the kept points")


CTIMER4_TC = 0x4002A008


def check_stream(c, setup, result, offsets, local_symbols):
    """Verge Aux's stream (src/energy_stream.h) through both cores' integration and CPU 1's starts,
    pauses, holds and stops: the messages each produces, and each pair's time from the stamps."""

    def messages(probe):
        energy = probe.energy
        written, sent = energy.get("stream.written", 4), energy.get("stream.sent", 4)
        out = []
        for i in range(sent, written):
            at = i % 16
            length = energy.get("stream.length", 2, 2 * at)
            out.append(energy.read_slots(length, at))
        energy.put("stream.sent", written, 4)
        return out

    def u16_at(m, at):
        return int.from_bytes(m[at:at + 2], "little")

    def u32_at(m, at):
        return int.from_bytes(m[at:at + 4], "little")

    clock = 96_000_000
    ticks = (clock + FIXTURE_RATE) // (2 * FIXTURE_RATE)
    period = 2 * ticks

    def ns(counts):
        return counts * 1_000_000_000 // clock

    def start(probe, local, stamp, streaming=True):
        probe.uc.mem_write(CTIMER4_TC, u32(stamp))
        local("requested_rate_hz", FIXTURE_RATE)
        local("stream_on_start", int(streaming))
        ok = probe.call("energy_capture_start", 1) == 1
        local("stream_on_start", 0)
        return ok

    def pairs(n, first=0):
        words = []
        for i in range(n):
            words += [result(1, 1000 + first + i), result(1, 2000 + first + i), result(2, 65535 if i % 2 else 0)]
        return words

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    stamp = 0xFFFF0000
    c.check(start(probe, local, stamp), "stream: an armed start failed")
    header = messages(probe)
    c.check(len(header) == 1 and len(header[0]) == 75 and header[0][:2] == b"\xa1\x01", f"stream: start sent {header}")
    if header and len(header[0]) == 75:
        h = header[0]
        c.check(u16_at(h, 68) == probe.energy.get("switch_weight_q16", 4) and h[70] == probe.energy.get("rearm_between_pairs", 1)
                and u32_at(h, 71) == 0, f"stream: header's step 5 fields {h[68:].hex()}")
        c.check(u32_at(h, 2) == clock // period and h[6] == 1 and u16_at(h, 7) == probe.energy.get("full_scale_mv", 4)
                and u32_at(h, 9) == 1 and h[13] == 1, f"stream: header fields {h[:14].hex()}")
        c.check(h[14] == 2 and u16_at(h, 15) == 0 and u16_at(h, 21) == 65535 and h[39] == 2 and h[64] == 0,
                f"stream: header tables {h[14:66].hex()}")
    # CPU 1 drains ten pairs at a pause: one message, timed from the start's stamp.
    feed(pairs(10))
    written(30)
    pause()
    got = messages(probe)
    c.check(len(got) == 1 and got[0][0] == 0xA0 and got[0][1] == 0 and u32_at(got[0], 2) == 1 and
            u16_at(got[0], 14) == 10 and int.from_bytes(got[0][6:14], "little") == ns(ticks),
            f"stream: drained pairs {[m[:16].hex() for m in got]}")
    if got and len(got[0]) == 16 + 50:
        c.check(all(u16_at(got[0], 16 + 5 * i) == 1000 + i and u16_at(got[0], 18 + 5 * i) == 2000 + i and
                    got[0][20 + 5 * i] == i % 2 for i in range(10)), "stream: drained pair codes or ranges")
    # The resume restarts the timer 5 ms of counts after the start's stamp; CPU 0's interrupt
    # integrates a whole segment: the first message after it says time jumped.
    probe.uc.mem_write(CTIMER4_TC, u32((stamp + 480_000) & 0xFFFFFFFF))
    c.check(probe.call("energy_capture_start", 0) == 1, "stream: resume failed")
    segment = probe.energy.get("segment_words", 4)
    whole = segment // 3
    # Enough segments to fill a message, then the next one completed with nothing in it.
    filled = -(-99 // whole)
    for index in range(filled):
        feed(pairs(whole, 10 + index * whole) + [0] * (segment - 3 * whole), index * segment)
        probe.integrate()
    feed([0] * segment, filled * segment)
    put("energy_segments", get("energy_segments") + 1)
    got = messages(probe)
    c.check(len(got) >= 1 and got[0][1] == 1 and u32_at(got[0], 2) == 11 and
            int.from_bytes(got[0][6:14], "little") == ns(ticks + 480_000),
            f"stream: first message after a resume {[m[:16].hex() for m in got]}")
    # Without a stream, CPU 0's batches go untimed and move the timeline on at each end: after three
    # segments it stands where timing each pair puts it, which a stream started then (mode 6) uses
    # for its first sequence and time.
    quiet, quiet_dma, _, _, quiet_get, quiet_local, _, quiet_feed, _, _, quiet_hook = setup()
    quiet_stamp = 0x7FFFFF00
    c.check(start(quiet, quiet_local, quiet_stamp, streaming=False), "stream: a start with no stream failed")
    size = quiet.energy.get("segment_words", 4)
    per = size // 3
    for index in range(3):
        quiet_feed(pairs(per, index * per) + [0] * (size - 3 * per), index * size)
        quiet.integrate()
    done = 3 * per
    timeline = (quiet.energy.get("stream.next_sequence", 4), quiet.energy.get("stream.next_raw", 4),
                quiet.energy.get("stream.next_counts", 8))
    expected = (done + 1, (quiet_stamp + ticks + done * period) & 0xFFFFFFFF, ticks + done * period)
    c.check(timeline == expected, f"stream: timeline after {done} untimed pairs {timeline}, not {expected}")
    quiet.uc.hook_del(quiet_hook)
    # A hold: CPU 1 records when the first trigger after the stop was due and when it came, from the
    # conversion timer's frozen count and output (here 0 and low: the next toggle rises). Only the
    # generated paths measure a read latency, so a build without them holds nothing.
    if "energy_monitor_hold" in probe.sym:
        probe.uc.mem_write(CTIMER4_TC, u32(0x1234))
        held = probe.call("energy_monitor_hold")
        probe.uc.mem_write(CTIMER4_TC, u32(0x1234 + 777))
        probe.call("energy_monitor_release", held)
        due = 0x1234 + ticks - 1
        c.check(held == 1 and probe.energy.get("stream.holds_written", 4) == 1 and
                probe.energy.read("stream.holds", 8) == u32(due) + u32(due + 777),
                f"stream: hold recorded as {probe.energy.read('stream.holds', 8).hex()}")
    # A new start ends the stream with reason 3 before its own header; a stop ends it with 2.
    last = probe.energy.get("decoder", 4, 12)
    c.check(start(probe, local, 0), "stream: a second armed start failed")
    got = messages(probe)
    c.check(len(got) >= 2 and got[-2][:2] == b"\xa2\x03" and u32_at(got[-2], 2) == last and got[-1][0] == 0xA1,
            f"stream: restart sent {[m[:6].hex() for m in got]}")
    c.check(probe.command(bytes([0x90, 2])) == b"\x90\x00\x00", "stream: stop failed")
    got = messages(probe)
    c.check(got == [bytes([0xA2, 2]) + u32(0)], f"stream: stop sent {[m.hex() for m in got]}")
    # A fault CPU 0's interrupt finds ends it with reason 4.
    c.check(start(probe, local, 0), "stream: a third armed start failed")
    messages(probe)
    put("energy_dma_error", 1, 1)
    probe.integrate()
    got = messages(probe)
    c.check(got == [bytes([0xA2, 4]) + u32(0)], f"stream: fault sent {[m.hex() for m in got]}")
    # A start after that failure. Once it hands the state back, CPU 0's loop ends a stream it finds
    # with a failure recorded (stream_fault, src/aux.c), so the failure has to be gone by then.
    released = []

    def release(uc, access, at, size, value, data):
        if value & 1 == 0:
            released.append((probe.energy.get("stream.active", 1), get("energy_dma_error", 1)))

    request = probe.sym["dap_shared"] + offsets["energy_control_request"]
    release_hook = probe.uc.hook_add(UC_HOOK_MEM_WRITE, release, begin=request, end=request + 3)
    probe.uc.ctl_flush_tb()
    c.check(start(probe, local, 0), "stream: a start after a fault failed")
    probe.uc.hook_del(release_hook)
    probe.uc.ctl_flush_tb()
    c.check(released == [(1, 0)], f"stream: at the handback after a fault, (active, failure) {released}")
    got = messages(probe)
    c.check(len(got) == 1 and got[0][0] == 0xA1 and get("energy_dma_error", 1) == 0,
            f"stream: a start after a fault sent {[m[:2].hex() for m in got]}")
    # A start whose handback and retake both time out, as when CPU 0's loop stalls through both.
    # When the loop comes back it echoes the last request (energy_control_task); it has to keep the
    # state then, so that its loop ends the stream the start began (stream_fault, src/aux.c).
    stalled, _, _, stalled_put, stalled_get, stalled_local, _, _, _, _, stalled_hook = setup()
    stalled.uc.hook_del(stalled_hook)
    stalled.uc.ctl_flush_tb()
    c.check(not start(stalled, stalled_local, 0), "stream: a start with no acknowledgement succeeded")
    stalled_put("energy_control_ack", stalled_get("energy_control_request"))
    left = (stalled.energy.get("stream.active", 1), stalled_get("energy_dma_error", 1),
            stalled_get("energy_control_ack") & 1)
    c.check(left[0] == 0 or left[1:] == (1, 0),
            f"stream: after a start's handback and retake timed out, (active, failure, quiet) {left}: nothing ends it")
    # Mode 5 belongs to Verge Aux: on CMSIS-DAP it is refused and starts nothing.
    local("dap_vendor_on_aux", 0)
    reply = probe.command(bytes([0x90, 5]) + u32(FIXTURE_RATE) + b"\x00")
    c.check(reply == b"\x90\xff" and not messages(probe), f"stream: CMSIS-DAP mode 5 answered {reply.hex()}")
    probe.uc.hook_del(hook)
    c.note("energy stream: header, pairs drained by CPU 1 and integrated by CPU 0, times across a resume, a hold's"
           " record, ends at a restart, a stop and a fault, a start after a fault, a start whose handback timed out,"
           " mode 5 refused on CMSIS-DAP")


def check_identity(c, setup, offsets):
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    local("current_range", 2)
    local("requested_rate_hz", 0)
    local("accumulator_started", 0)
    probe.energy.put("running", 0, 1)
    info = probe.command(b"\x82")
    c.check(int.from_bytes(info[14 + info[8]:18 + info[8]], "little") & (1 << 8), "capture identity capability missing")
    before = probe.command(b"\x90\x0c")
    c.check(before == b"\x90\x00\x01" + bytes(29), f"initial identity status {before.hex()}")
    a = probe.command(b"\x90\x08" + u32(1000))
    c.check(len(a) == 31 and a[:3] == b"\x90\x00\x00" and int.from_bytes(a[23:], "little") == 1,
            f"identified start A {a.hex()}")
    b = probe.command(b"\x90\x08" + u32(1000))
    c.check(len(b) == 31 and b[:3] == b"\x90\x00\x00" and int.from_bytes(b[23:], "little") == 2,
            f"identified start B {b.hex()}")
    written(0)
    for token in (a[7:], bytes(24)):
        snapshot = probe.energy.read("capture_id", 24), get("energy_control_request"), get("energy_dma_error", 1)
        reply = probe.command(b"\x90\x0b" + token)
        c.check(reply == b"\x90\x00\x08" and probe.energy.get("running", 1) == 1 and
                snapshot == (probe.energy.read("capture_id", 24), get("energy_control_request"), get("energy_dma_error", 1)),
                "stale stop paused or changed B")
    status = probe.command(b"\x90\x0c")
    c.check(status == b"\x90\x00\x00\x01" + u32(1000) + b[7:], f"running identity status {status.hex()}")
    local("capture_counter", (1 << 64) - 1)
    exhausted = probe.command(b"\x90\x08" + u32(1000))
    c.check(exhausted == b"\x90\x00\x09" + bytes(28) and probe.energy.get("running", 1) == 1
            and probe.command(b"\x90\x0c") == status, "counter exhaustion changed running capture")
    for _ in range(2):
        c.check(probe.command(b"\x90\x0b" + b[7:]) == b"\x90\x00\x00", "matching/repeated stop failed")
    c.check(probe.command(b"\x90\x0c") == b"\x90\x00\x01\x00" + bytes(4) + b[7:], "retained identity lost")
    put("energy_dma_error", 1, 1)
    c.check(probe.command(b"\x90\x0b" + b[7:]) == b"\x90\x00\x07", "matching fault stop lost fault")
    probe.uc.hook_del(hook)
    for mode in (9, 10, 13):
        request = bytes([0x90, mode]) + (u32(1000) + b"\x00" if mode == 9 else b"\x00" if mode == 10 else b[7:])
        c.check(probe.command(request) == b"\x90\xff", f"CMSIS-DAP accepted stream mode {mode}")
    for reacquire in (False, True):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        local("current_range", 2)
        local("dap_vendor_on_aux", 1)
        a = probe.command(b"\x90\x09" + u32(1000) + b"\x00")
        c.check(a[:3] == b"\x90\x00\x00", "protected release fixture start failed")
        sent = probe.energy.get("stream.sent", 4)
        queued = probe.energy.get("stream.written", 4)
        h = probe.energy.read_slots(92, sent % 16)
        c.check(queued == sent + 1 and h[:2] == b"\xa1\x02" and h[68:] == a[7:],
                "identified start reply/header boundary mismatch")
        written(0)
        probe.uc.hook_del(hook)
        failing = False
        def enter_new_start(uc, at, size, data):
            nonlocal failing
            if uc.reg_read(UC_ARM_REG_R0) == 1:
                failing = True
        enter_hook = probe.uc.hook_add(UC_HOOK_CODE, enter_new_start,
                                       begin=probe.sym["energy_capture_start"], end=probe.sym["energy_capture_start"])
        def refuse_release(uc, access, at, size, value, data):
            if not failing or (value & 1 and reacquire):
                put("energy_control_ack", value)
        base = probe.sym["dap_shared"]
        request_offset = offsets["energy_control_request"]
        failure_hook = probe.uc.hook_add(UC_HOOK_MEM_WRITE, refuse_release,
                                        begin=base + request_offset, end=base + request_offset + 3)
        probe.uc.ctl_flush_tb()
        before_written = probe.energy.get("stream.written", 4)
        failed = probe.command(b"\x90\x09" + u32(2000) + b"\x00")
        c.check(failed == b"\x90\x00\x07" + bytes(28), f"failed identified release answered {failed.hex()}")
        c.check(probe.energy.read("capture_id", 24) == a[7:], "failed start published candidate identity")
        for index in range(before_written, probe.energy.get("stream.written", 4)):
            at = index % 16
            length = probe.energy.get("stream.length", 2, at * 2)
            message = probe.energy.read_slots(length, at)
            c.check(message[:1] != b"\xa1", "failed release exposed a candidate header")
        probe.uc.hook_del(failure_hook)
        probe.uc.hook_del(enter_hook)
        def acknowledge(uc, access, at, size, value, data):
            put("energy_control_ack", value)
        success_hook = probe.uc.hook_add(UC_HOOK_MEM_WRITE, acknowledge,
                                        begin=base + request_offset, end=base + request_offset + 3)
        probe.uc.ctl_flush_tb()
        retry = probe.command(b"\x90\x09" + u32(2000) + b"\x00")
        c.check(retry[:3] == b"\x90\x00\x00" and retry[7:] != a[7:] and
                int.from_bytes(retry[23:], "little") == 2, "retry after failed release reused published token")
        probe.uc.hook_del(success_hook)
    c.note("capture identities: initial/coherent/retained status; A replaced by B, stale/zero stop before pause;"
           " counter exhaustion leaves B running; repeated/fault cleanup; Aux-only modes refused on CMSIS-DAP")
