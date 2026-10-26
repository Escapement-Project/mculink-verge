"""Energy DMA ownership checks executed by image.py against both linked images."""

import struct

from cycles import Machine, Model, RETURN_MAGIC, SCRATCH, Target, u32
from unicorn import UC_HOOK_CODE, UC_HOOK_MEM_WRITE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_PRIMASK, UC_ARM_REG_R0

DMA_BASE = 0x40082000
ENERGY_MASK = 1 << 21
SWO_MASK = 1 << 8
ADC_BASE = 0x400A0000


class Dma:
    def __init__(self, probe):
        self.page = bytearray(4096)
        self.writes = []
        probe.uc.mem_unmap(DMA_BASE, 4096)
        probe.uc.mmio_map(DMA_BASE, 4096, self.read, None, self.write, None)

    def get(self, offset):
        return int.from_bytes(self.page[offset:offset + 4], "little")

    def set(self, offset, value):
        self.page[offset:offset + 4] = u32(value)

    def read(self, uc, offset, size, data):
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
            if commands[1] >> 24 == 2:
                self.fifo.append(0x82001234)
        self.page[offset:offset + size] = value.to_bytes(size, "little")


def machine(elf, symbols):
    probe = Machine(elf, Target([]), restart_at=set(), model=Model())
    probe.sym.update({name: address for name, (address, _) in symbols.items()})
    return probe


ORDINARY_GPIO = 0x4008C000
IOCON_BASE = 0x40001000
BOARD_KIND_PRO = 1


class Circuit:
    """The Pro's energy circuit as calibration sees it (PRO.md): the probe's 3.3 V on the shunts,
    loads from node T to ground, the divider, and the sense amplifier's gain of 30. The amplifier's
    offset and output floor are the ones a unit showed on the bench, in codes."""

    OFFSET = -9
    FLOOR = 57

    LOADS = {22: 6650.0, 23: 66.5, 26: 1650.0, 29: 16.5}  # PIO1 pin: ohms
    CAL_DISC = 24

    def __init__(self, setting_ma, gain=1.0, switch_ohms=0.24, supply=3.3, full_scale=3.0):
        self.low = 50.0 if setting_ma == 350 else 200.0
        self.high = (0.24 if setting_ma == 350 else 2.0) + 0.0165  # Q3 at a gate-source voltage of -3.3 V
        self.gain, self.switch_ohms, self.supply, self.full_scale = gain, switch_ohms, supply, full_scale

    def shunt(self, low_range):
        return self.low if low_range else 1 / (1 / self.low + 1 / self.high)

    def code(self, volts):
        return max(0, min(65535, round(volts / self.full_scale * 65536)))

    def current_code(self, amps, low_range):
        ideal = self.code(30 * self.gain * amps * self.shunt(low_range))
        return min(65535, max(self.FLOOR, ideal + self.OFFSET))

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
            self.fifo += [0x81000000 | voltage, 0x82000000 | current]
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


def check_control(c, elf0, elf1, symbols0, symbols1, offsets0, offsets1):
    probe = machine(elf0, symbols0)
    dma = Dma(probe)
    shared = probe.sym["dap_shared"]

    def put(field, value, size=4):
        probe.uc.mem_write(shared + offsets0[field], value.to_bytes(size, "little"))

    def get(field, size=4):
        return int.from_bytes(probe.uc.mem_read(shared + offsets0[field], size), "little")

    stub = SCRATCH + 2048
    probe.uc.mem_write(stub, bytes.fromhex("00bf") * 8 + bytes.fromhex("7047"))
    put("energy_segment_handler", stub | 1)
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
            if at == stub:
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
        dma.set(0x58, ENERGY_MASK | SWO_MASK)
        dma.set(0x40, ENERGY_MASK)
        probe.call("DMA0_IRQHandler")
        c.check(get("energy_segments") == 1 and get("swo_segments") == 2,
                "quiet energy completion changed its count or blocked SWO")
        c.check(dma.get(0x58) == ENERGY_MASK and dma.get(0x40) == ENERGY_MASK,
                "quiet ISR consumed energy status owned by CPU1")
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

    probe.uc.reg_write(UC_ARM_REG_PRIMASK, 1)
    put("energy_control_request", 7)
    probe.call("energy_control_task")
    c.check(probe.uc.reg_read(UC_ARM_REG_PRIMASK) == 1, "energy service changed caller IRQ mask")

    # Run CPU1's actual pause with absent/stale acknowledgements and a stuck DMA channel.
    for label, request, ack, busy in (("absent ack", 0, 0, 0), ("stale quiet ack", 4, 3, 0),
                                       ("DMA busy", 0, 0, ENERGY_MASK)):
        cpu1 = machine(elf1, symbols1)
        dma1 = Dma(cpu1)
        base = cpu1.sym["dap_shared"]
        for name, value, size in (("capture_running", 1, 1), ("sample_decoder", 2, 1)):
            cpu1.uc.mem_write(cpu1.sym[name], value.to_bytes(size, "little"))
        for name, value, size in (("energy_available", 1, 1), ("energy_control_request", request, 4),
                                  ("energy_control_ack", ack, 4), ("energy_accumulator_pj", 12345, 8),
                                  ("energy_accumulator_pairs", 123, 8)):
            cpu1.uc.mem_write(base + offsets1[name], value.to_bytes(size, "little"))
        dma1.set(0x38, busy)
        dma1.set(0x58, ENERGY_MASK)
        result = cpu1.call("energy_monitor_pause")
        c.check(cpu1.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and result == 0,
                f"{label}: pause did not fail within its bound")
        c.check(bytes(cpu1.uc.mem_read(cpu1.sym["capture_running"], 1)) == b"\x01" and
                bytes(cpu1.uc.mem_read(cpu1.sym["sample_decoder"], 1)) == b"\x02",
                f"{label}: capture state reused without ownership")
        c.check(not any(offset == 0x78 for offset, _ in dma1.writes) and dma1.get(0x58) == ENERGY_MASK,
                f"{label}: DMA aborted/status cleared before ownership")
        c.check(int.from_bytes(cpu1.uc.mem_read(ADC_BASE + 0x10, 4), "little") & 0x100 == 0,
                f"{label}: FIFO reset before ownership")
        c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_accumulator_pj"], 8)) == struct.pack("<Q", 12345),
                f"{label}: old total lost")
        c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_dma_error"], 1)) == b"\x01",
                f"{label}: failure not latched")
        if label == "absent ack":
            reply = cpu1.command(bytes([0x90, 0]) + u32(1000))
            c.check(len(reply) == 7 and reply[2] == 7, "start did not report missing ownership")
            reply = cpu1.command(bytes([0x90, 2]))
            c.check(len(reply) == 3 and reply[2] == 7, "stop did not report missing ownership")
            c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_accumulator_pj"], 8)) == struct.pack("<Q", 12345),
                    "failed start/stop reset the retained total")

    cpu1 = machine(elf1, symbols1)
    dma1 = Dma(cpu1)
    base = cpu1.sym["dap_shared"]
    cpu1.uc.mem_write(base + offsets1["energy_available"], b"\x01")
    cpu1.uc.mem_write(cpu1.sym["capture_running"], b"\x01")
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
    c.check(result == 1 and pending == 30 and bytes(cpu1.uc.mem_read(cpu1.sym["capture_running"], 1)) == b"\x00",
            "pause did not wait for delayed ownership before stopping")
    c.check(any(offset == 0x78 for offset, _ in dma1.writes) and
            int.from_bytes(cpu1.uc.mem_read(ADC_BASE + 0x10, 4), "little") & 0x100 != 0,
            "acknowledged pause did not stop/reset capture")
    c.note(f"energy quiet admission across {len(trace)} IRQ instructions, SWO isolation and bounded ownership failures")


CTIMER2_BASE = 0x40028000


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


def check_hold(c, elf1, symbols1):
    """A latency measurement's hold: the trigger stops before anything waits, the wait lasts until
    the ADC and DMA0's energy channel are idle, and release starts the trigger again. Without a
    running capture, or with its timer already stopped by a pause that failed, nothing moves."""
    if "swd_sweep_last" not in symbols1 and "jtag_sweep_last" not in symbols1:
        c.note("no latency measurement in this image, so no hold to check")
        return
    if not c.check("energy_monitor_hold" in symbols1 and "energy_monitor_release" in symbols1,
                   "the latency measurement's hold is missing from the image"):
        return
    for running, timer_on, expect in ((1, True, True), (0, True, False), (1, False, False)):
        label = f"hold, capture {'running' if running else 'stopped'}, timer {'on' if timer_on else 'off'}"
        cpu1 = machine(elf1, symbols1)
        log = []
        trigger = Trigger(cpu1, log, timer_on)
        adc = BusyAdc(cpu1, log, 3)
        dma = BusyDma(cpu1, log, 3)
        cpu1.uc.mem_write(cpu1.sym["capture_running"], bytes([running]))
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
    c.note("latency-measurement hold stops, drains and restarts the energy trigger, and only a running one")


# The fixture's calibration: this shunt current at every code.
SHUNT_NA = 1_000_000
DIVIDER_NA_PER_MV_Q16 = (65536 * 1_000_000 + 330_000) // 660_000


def power_pw(voltage_code):
    """One sample's power at the plain board's full scale, less the voltage divider's current."""
    mv = voltage_code * 2 * 3300 >> 16
    return mv * (SHUNT_NA - (mv * DIVIDER_NA_PER_MV_Q16 >> 16))


def check_samples(c, elf, symbols, offsets, model_syscon):
    def result(command, code):
        return 0x80070000 | command << 24 | code

    def setup():
        probe = machine(elf, symbols)
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
        for name, value in (("capture_running", 1), ("current_calibrated", 1), ("accumulator_started", 1),
                            ("actual_rate_hz", 1024), ("requested_rate_hz", 1024), ("energy_scale_q32", 4194304),
                            ("current_range", 1), ("adc_ready", 1), ("inputmux_ready", 1)):
            local(name, value)
        calibration = struct.pack("<4H7IB3x", 0, 65535, 0, 0, SHUNT_NA, SHUNT_NA, 0, 0, 0, 0, 0, 2)
        for name in ("low_range_calibration", "high_range_calibration"):
            c.check(symbols[name][1] == len(calibration), "calibration fixture layout changed")
            probe.uc.mem_write(probe.sym[name], calibration)
        probe.uc.mem_write(0x40013010, u32(0x40000000))  # Enable the SDK's 96 MHz FRO source.

        def acknowledge(uc, access, at, size, value, data):
            put("energy_control_ack", value)

        # Ownership interleavings are checked separately; these tests model an idle CPU0.
        address = base + offsets["energy_control_request"]
        hook = probe.uc.hook_add(UC_HOOK_MEM_WRITE, acknowledge, begin=address, end=address + 3)
        probe.uc.ctl_flush_tb()

        def written(words, pending=False):
            dma.set(0x58, ENERGY_MASK if pending else 0)
            dma.set(0x408 + 21 * 16, ((1024 - words % 1024) - 1) << 16)

        def feed(words, start=0):
            for i, word in enumerate(words):
                probe.uc.mem_write(probe.sym["energy_ring"] + ((start + i) % 2048) * 4, u32(word))

        def pause():
            value = probe.call("energy_monitor_pause")
            c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and value == 1,
                    "energy tail pause did not return successfully")

        def total(voltage_codes):
            expected_q32 = sum(power_pw(v) * 4194304 for v in voltage_codes)
            c.check(get("energy_accumulator_pairs", 8) == len(voltage_codes), "tail sample count mismatch")
            c.check(get("energy_accumulator_pj", 8) == expected_q32 >> 32 and
                    get("energy_accumulator_fraction_q32") == expected_q32 & 0xffffffff,
                    "tail energy or fractional carry mismatch")

        return probe, dma, adc, put, get, local, written, feed, pause, total, hook

    limits = ((1900, 7), (3800, 6), (7600, 5), (15200, 4), (29197, 3), (54794, 2), (97560, 1), (100000, 0))
    rates = (1000, 1900, 1901, 3800, 3801, 7600, 7601, 15200, 15201, 29197, 29198,
             54794, 54795, 97560, 97561, 100000)

    def check_commands(words, average, calibration):
        tail = 0 if calibration else 3
        expected = (0x80, (2 << 24) | (average << 12), 0xA0, (tail << 24) | (average << 12))
        c.check(words[:4] == expected, "ADC voltage/current channel, resolution, averaging, timing or chain changed")
        if not calibration:
            c.check(words[4:] == (0x82, 0), "ADC range command is not a fresh unaveraged side-A conversion")

    for rate in rates:
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        local("requested_rate_hz", rate)
        value = probe.call("energy_capture_start", 1)
        c.check(value == 1 and probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "ADC configuration setup failed")
        actual = int.from_bytes(probe.uc.mem_read(probe.sym["actual_rate_hz"], 4), "little")
        average = next(average for limit, average in limits if actual <= limit)
        words = tuple(int.from_bytes(adc.page[at:at + 4], "little") for at in range(0x100, 0x118, 4))
        check_commands(words, average, False)
        c.check(int.from_bytes(adc.page[0xBC:0xC0], "little") == 0x01000001,
                "ADC monitor is not hardware trigger 7 starting at command 1")
        probe.uc.hook_del(hook)
    c.note("ADC monitor command registers at averaging boundaries use the actual timer rate")

    if "target_sense_convert_pair_for_calibration" in symbols:
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        local("capture_running", 0)
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

    if c.check("current_calibrate" in symbols, "calibration entry missing"):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        local("capture_running", 0)
        calls = 0

        def calibration(uc, at, size, data):
            nonlocal calls
            if at == probe.sym["current_calibrate"]:
                calls += 1
                uc.reg_write(UC_ARM_REG_R0, 0)
                uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

        call_hook = probe.uc.hook_add(UC_HOOK_CODE, calibration)
        probe.uc.ctl_flush_tb()
        reply = probe.command(bytes([0x90, 0]) + u32(1000))
        c.check(calls == 1, f"energy start calibrated {calls} times")
        c.check(reply == bytes([0x90, 0, 6]) + bytes(4), f"mock calibration failure reply: {reply.hex()}")
        probe.uc.hook_del(call_hook)
        probe.uc.hook_del(hook)
    c.note("energy start calibrates once and reports a failed calibration")

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    written(0)
    pause()
    total([])
    probe.uc.hook_del(hook)

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    feed([result(1, 32768), result(2, 1), result(3, 0), result(1, 16384), result(2, 1)])
    adc.fifo = [result(3, 0)]
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
    codes = [8192 * (i % 3 + 1) for i in range(113)]
    words = [0] * 1024
    for i, code in enumerate(codes):
        words[i * 9:i * 9 + 3] = [result(1, code), result(2, 1), result(3, 0)]
    words[-1] = result(1, 49152)
    feed(words)
    probe.call("energy_monitor_process_segment")
    put("energy_segments", 1)
    total(codes)
    feed([result(2, 1), result(3, 0), result(1, 4096)], 1024)
    written(1027)
    reply = probe.command(bytes([0x90, 1]))
    c.check(len(reply) == 21 and reply[2] == 0 and int.from_bytes(reply[3:5], "little") == 49152 and
            int.from_bytes(reply[7:11], "little") == 114, "latest lost a tuple spanning DMA segments")
    total(codes)  # Reading latest must not integrate the same tail twice.
    pause()
    codes.append(49152)
    total(codes)
    # A real on-demand sense request pauses/resumes the monitor without resetting session numbering.
    reply = probe.command(bytes([0x86, 1, 0]))
    c.check(len(reply) == 7 and reply[1] == 0, "on-demand sense/resume failed")
    c.check((adc.software_commands[-1][1] >> 8) & 7 == 5, "on-demand sensing lost its ADCK35 sample time")
    c.check(bytes(probe.uc.mem_read(probe.sym["capture_running"], 1)) == b"\x01", "sense did not resume capture")
    total(codes)
    feed([result(2, 1), result(3, 0), result(1, 32768), result(2, 1), result(3, 0)])
    written(5)
    reply = probe.command(bytes([0x90, 1]))
    c.check(reply[2] == 0 and int.from_bytes(reply[7:11], "little") == 115,
            "session sequence reset or an old partial tuple crossed restart")
    pause()
    total(codes + [32768])
    # The actual startup routine resets a new session only after release succeeds.
    value = probe.call("energy_capture_start", 1)
    c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and value == 1, "new capture setup failed")
    total([])
    probe.uc.hook_del(hook)

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    codes = []
    stream = []
    for i in range(5 * 1024 + 17):
        sample, phase = divmod(i, 9)
        code = 4096 * (sample % 15 + 1)
        stream.append(result(phase + 1, code if phase == 0 else 1 if phase == 1 else 0) if phase < 3 else 0)
        if phase == 2:
            codes.append(code)
    for segment in range(4):
        feed(stream[segment * 1024:(segment + 1) * 1024], segment * 1024)
        probe.call("energy_monitor_process_segment")
        put("energy_segments", segment + 1)
    feed(stream[4 * 1024:], 4 * 1024)
    written(len(stream), pending=True)
    dma.set(0x58, ENERGY_MASK | SWO_MASK)
    pause()
    total(codes)
    c.check(dma.get(0x58) == SWO_MASK, "tail drain cleared another channel's completion")
    # Repeated pauses do not consume the settled extent again.
    pause()
    total(codes)
    probe.uc.hook_del(hook)

    for byte_wrap in (True, False):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        local("consumed_bytes", 0xfffff000 if byte_wrap else 0)
        local("energy_segments_at_start", 0 if byte_wrap else 0xffffffff)
        put("energy_segments", 0xfffff if byte_wrap else 0xffffffff)
        put("energy_accumulator_pairs", 0xffffffff, 8)
        decoder = bytearray(symbols["sample_decoder"][1])
        decoder[12:16] = u32(0xffffffff)
        decoder[16] = 1
        probe.uc.mem_write(probe.sym["sample_decoder"], bytes(decoder))
        words = [result(i % 9 + 1, 32768 if i % 9 == 0 else 1 if i % 9 == 1 else 0)
                 if i % 9 < 3 else 0 for i in range(1024)]
        feed(words, 1024 if byte_wrap else 0)
        probe.call("energy_monitor_process_segment")
        put("energy_segments", 0x100000 if byte_wrap else 0)
        written(0)
        reply = probe.command(bytes([0x90, 1]))
        c.check(reply[2] == 0 and int.from_bytes(reply[7:11], "little") == 113 and
                get("energy_accumulator_pairs", 8) == 0xffffffff + 114,
                "energy byte/segment/sample counter wrap lost samples")
        probe.uc.hook_del(hook)
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    feed([result(1, 32768), result(2, 1), result(3, 0)])
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

    # A segment that stays on the high range under the 350 mA setting's 1.5 mA asks for the low
    # range back; one sample over, or a segment that ends on the low range, does not. The cases run
    # in turn on one machine, so a segment over the limit must not hold back the next.
    samples = 300
    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    # 100 nA a code, so 10000 is 1 mA and 20000 is 2 mA.
    probe.uc.mem_write(probe.sym["high_range_calibration"],
                       struct.pack("<4H7IB3x", 0, 65535, 0, 0, 0, 6553500, 0, 0, 100 << 16, 0, 0, 2))
    for segment, (label, currents, ranges, expected) in enumerate((
            ("one sample over", [10000] * (samples - 1) + [20000], [0] * samples, 0),
            ("under the limit, after one over", [10000] * samples, [0] * samples, 1),
            ("ends on the low range", [10000] * samples, [0] * (samples - 1) + [65535], 0),
            ("under the limit again", [10000] * samples, [0] * samples, 1))):
        put("energy_range_retry_pending", 0, 1)
        words = [word for current, state in zip(currents, ranges)
                 for word in (result(1, 32768), result(2, current), result(3, state))]
        feed(words + [0] * (1024 - len(words)), segment * 1024)
        put("energy_segments", segment)
        probe.call("energy_monitor_process_segment")
        c.check(get("energy_range_retry_pending", 1) == expected, f"low-range re-arm, {label}")
    probe.uc.hook_del(hook)
    c.note("a high-range segment under the low range's limit re-arms it; one over or a low-range end does not")

    probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
    adc.active_reads = -1
    value = probe.call("energy_monitor_pause")
    c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC and value == 0 and adc.resets == 0,
            "stuck ADC did not fail without discarding pending data")
    probe.uc.hook_del(hook)
    for fault in ("IRQ error", "pending error", "FIFO overflow"):
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        words = [result(1, 32768), result(2, 1), result(3, 0)] + [0] * 1021
        feed(words)
        probe.call("energy_monitor_process_segment")
        put("energy_segments", 1)
        written(1024)
        total([32768])
        if fault == "IRQ error":
            put("energy_dma_error", 1, 1)
        elif fault == "pending error":
            dma.set(0x40, ENERGY_MASK)
        else:
            adc.page[0x14:0x18] = u32(2)
        reply = probe.command(bytes([0x90, 1]))
        c.check(len(reply) == 21 and reply[2] == 7, f"{fault}: latest hid capture failure")
        reply = probe.command(bytes([0x90, 3]))
        c.check(len(reply) == 19 and reply[2] == 7 and int.from_bytes(reply[3:11], "little") == (power_pw(32768) * 4194304 >> 32) // 1000 and
                int.from_bytes(reply[11:19], "little") == 1, f"{fault}: total status/layout lost retained data")
        feed(words, 1024)
        probe.call("energy_monitor_process_segment")
        total([32768])
        reply = probe.command(bytes([0x90, 2]))
        c.check(reply == bytes([0x90, 0, 7]), f"{fault}: stop hid capture failure")
        c.check(probe.command(bytes([0x90, 1]))[2] == 7 and probe.command(bytes([0x90, 3]))[2] == 7,
                f"{fault}: stop cleared sticky status")
        reply = probe.command(bytes([0x90, 0]) + u32(0))
        c.check(reply[2] == 4 and probe.command(bytes([0x90, 3]))[2] == 7,
                f"{fault}: failed start cleared sticky status")
        local("requested_rate_hz", 1024)
        c.check(probe.call("energy_capture_start", 1) == 1, f"{fault}: new-session recovery failed")
        total([])
        c.check(probe.command(bytes([0x90, 1]))[2] == 2 and probe.command(bytes([0x90, 3]))[2] == 2,
                f"{fault}: successful start did not clear failure")
        probe.uc.hook_del(hook)
    c.note("energy partial/full/pending segments, FIFO tail, split tuples, Q32 carry and session resume/reset")
    from energy_lifecycle import check_sequences
    check_sequences(c, setup, result)


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
        delay = probe.sym["calibration_delay"]

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

    plans = {350: ({22, 26}, {26, 23, 29}), 50: ({22}, {22, 26, 23})}
    for setting_ma, (low_loads, high_loads) in plans.items():
        probe, gpio, circuit, adc, ok, tables = run(setting_ma)
        c.check(ok, f"{setting_ma} mA: calibration failed on nominal parts")
        c.check(tables[True][3] == len(low_loads) + 1 and tables[False][3] == len(high_loads),
                f"{setting_ma} mA: points {tables[True][3]} low, {tables[False][3]} high")
        loads_seen = {low: {pin for q, loads in adc.states if q == low for pin in Circuit.LOADS if loads >> pin & 1}
                      for low in (1, 0)}
        c.check(loads_seen[1] == low_loads and loads_seen[0] == high_loads,
                f"{setting_ma} mA: loads by latch state {loads_seen}")
        c.check(all((words[1] >> 12) & 7 == 7 and (words[3] >> 12) & 7 == 7 for words in adc.software_commands),
                f"{setting_ma} mA: a calibration conversion averaged fewer than 128 times")
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
