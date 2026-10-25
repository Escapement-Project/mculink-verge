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
                                  ("energy_control_ack", ack, 4), ("energy_accumulator_nj", 12345, 8),
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
        c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_accumulator_nj"], 8)) == struct.pack("<Q", 12345),
                f"{label}: old total lost")
        c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_dma_error"], 1)) == b"\x01",
                f"{label}: failure not latched")
        if label == "absent ack":
            reply = cpu1.command(bytes([0x90, 0]) + u32(1000))
            c.check(len(reply) == 7 and reply[2] == 7, "start did not report missing ownership")
            reply = cpu1.command(bytes([0x90, 2]))
            c.check(len(reply) == 3 and reply[2] == 7, "stop did not report missing ownership")
            c.check(bytes(cpu1.uc.mem_read(base + offsets1["energy_accumulator_nj"], 8)) == struct.pack("<Q", 12345),
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
        calibration = struct.pack("<4H7IB3x", 0, 65535, 0, 0, 1000, 1000, 0, 0, 0, 0, 0, 2)
        for name in ("current_calibration_high", "current_calibration_low"):
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
            expected_q32 = sum((v * 6600 // 65536) * 1000 * 4194304 for v in voltage_codes)
            c.check(get("energy_accumulator_pairs", 8) == len(voltage_codes), "tail sample count mismatch")
            c.check(get("energy_accumulator_nj", 8) == expected_q32 >> 32 and
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
        if "target_sense_convert_pair_for_calibration" in symbols:
            probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
            local("capture_running", 0)
            local("requested_rate_hz", 0)
            value = probe.call("target_sense_convert_pair_for_calibration", rate, SCRATCH, SCRATCH + 4)
            c.check(value == 0 and probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "ADC calibration pair failed")
            check_commands(adc.software_commands[-1], next(avg for limit, avg in limits if rate <= limit), True)
            c.check(int.from_bytes(adc.page[0xA0:0xA4], "little") == 0x01000000,
                    "ADC calibration is not software trigger 0 starting at command 1")
            c.check(bytes(probe.uc.mem_read(SCRATCH, 2)) == b"\x45\x23" and
                    bytes(probe.uc.mem_read(SCRATCH + 4, 2)) == b"\x34\x12", "ADC calibration pair lost its results")
            probe.uc.hook_del(hook)
    c.note("ADC monitor command registers at averaging boundaries use the actual timer rate")
    c.note("ADC calibration command/trigger checks " +
           ("passed" if "target_sense_convert_pair_for_calibration" in symbols else "not run: entry inlined"))

    if c.check("current_calibrate" in symbols, "calibration entry missing from rate checks"):
        for requested, expected in ((1901, 1900), (7601, 7600), (15201, 15199), (29198, 29197),
                                    (54795, 54794), (97561, 97560), (100000, 100000)):
            probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
            local("capture_running", 0)
            seen = []

            def calibration(uc, at, size, data):
                if at == probe.sym["current_calibrate"]:
                    seen.append(uc.reg_read(UC_ARM_REG_R0))
                    uc.reg_write(UC_ARM_REG_R0, 0)
                    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

            call_hook = probe.uc.hook_add(UC_HOOK_CODE, calibration)
            probe.uc.ctl_flush_tb()
            reply = probe.command(bytes([0x90, 0]) + u32(requested))
            c.check(seen == [expected], f"calibration rate {seen} for request {requested}, expected {expected}")
            c.check(reply == bytes([0x90, 0, 6]) + bytes(4), f"mock calibration failure reply: {reply.hex()}")
            probe.uc.hook_del(call_hook)
            probe.uc.hook_del(hook)
    c.note("energy start passes the effective timer rate to calibration at averaging boundaries")

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
        c.check(len(reply) == 19 and reply[2] == 7 and int.from_bytes(reply[3:11], "little") == 3222 and
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
