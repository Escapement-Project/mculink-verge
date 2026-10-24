"""Energy DMA ownership checks executed by image.py against both linked images."""

import struct

from cycles import Machine, Model, RETURN_MAGIC, SCRATCH, Target, u32
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_PC, UC_ARM_REG_PRIMASK

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
        for name, value, size in (("capture_running", 1, 1), ("sample_phase", 2, 1)):
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
                bytes(cpu1.uc.mem_read(cpu1.sym["sample_phase"], 1)) == b"\x02",
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
