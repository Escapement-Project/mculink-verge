"""Console recovery checks for image.py, using the linked DMA IRQ and console task."""

import struct

import itertools

from energy_checks import Dma, ENERGY_MASK, SWO_MASK, machine
from cycles import RETURN_MAGIC, SCRATCH, u32
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2


def check_dma(c, elf, symbols, offsets):
    for physical in ((0,), (1,), (0, 1)):
        for instance, flexcomm in enumerate(physical):
            for receive_error in (False, True):
                for simultaneous in (False, True):
                    probe = machine(elf, symbols)
                    dma = Dma(probe)
                    base = probe.sym["dap_shared"]
                    rx_stride = symbols["vcom_rx"][1] // 2
                    tx_stride = symbols["vcom_tx"][1] // 2
                    uart_stride = symbols["vcom"][1] // 2
                    c.check(rx_stride == 16 and tx_stride == 20, "console state fixture layout changed")
                    probe.uc.mem_write(probe.sym["vcom_count"], u32(len(physical)))
                    for i, port in enumerate(physical):
                        probe.uc.mem_write(probe.sym["vcom"] + i * uart_stride,
                                           struct.pack("<2I", 0x40086000 + port * 4096, 96000000))
                        probe.uc.mem_write(probe.sym["vcom_rx"] + i * rx_stride,
                                           struct.pack("<3IB3x", 4 + port * 2, 77, 3, 0))
                        tx = bytearray(tx_stride)
                        tx[:4] = u32(5 + port * 2)
                        tx[4] = int(not receive_error and i == instance)
                        probe.uc.mem_write(probe.sym["vcom_tx"] + i * tx_stride, bytes(tx))
                    rx = probe.sym["vcom_rx"] + instance * rx_stride
                    tx = probe.sym["vcom_tx"] + instance * tx_stride
                    rx_channel = 4 + flexcomm * 2
                    tx_channel = rx_channel + 1
                    failed_channel = rx_channel if receive_error else tx_channel
                    mask = 1 << failed_channel
                    pending = {}
                    room = {}
                    reads = []
                    writes = []

                    def stub(uc, at, size, data):
                        itf = uc.reg_read(UC_ARM_REG_R0)
                        if at == probe.sym["tud_cdc_n_available"]:
                            result = len(pending.get(itf, b""))
                        elif at == probe.sym["tud_cdc_n_read"]:
                            payload = pending.pop(itf, b"")[:uc.reg_read(UC_ARM_REG_R2)]
                            uc.mem_write(uc.reg_read(UC_ARM_REG_R1), payload)
                            reads.append((itf, payload))
                            result = len(payload)
                        elif at == probe.sym["tud_cdc_n_write_available"]:
                            result = room.get(itf, 0)
                        elif at == probe.sym["tud_cdc_n_write"]:
                            count = uc.reg_read(UC_ARM_REG_R2)
                            writes.append((itf, bytes(uc.mem_read(uc.reg_read(UC_ARM_REG_R1), count))))
                            room[itf] = 0
                            result = count
                        elif at == probe.sym["tud_cdc_n_write_flush"]:
                            result = 0
                        else:
                            return
                        uc.reg_write(UC_ARM_REG_R0, result)
                        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

                    hook = probe.uc.hook_add(UC_HOOK_CODE, stub)
                    probe.uc.ctl_flush_tb()

                    def word(address):
                        return int.from_bytes(probe.uc.mem_read(address, 4), "little")

                    dma.set(0x20, 0xf0)
                    dma.set(0x48, 0xf0)
                    dma.set(0x38, mask)
                    dma.set(0x40, mask)
                    dma.set(0x58, SWO_MASK | (mask if simultaneous else 0))
                    probe.call("DMA0_IRQHandler")
                    c.check(word(base + offsets["swo_segments"]) == 1, "console fault blocked SWO completion")
                    c.check(dma.get(0x20) & mask == 0 and dma.get(0x48) & mask == 0,
                            "failed console DMA channel stayed enabled")
                    c.check(probe.call("vcom_ready") == 1, "console error did not wake recovery")
                    if receive_error:
                        c.check(word(rx + 8) == 3, "failed RX completion advanced its old baseline")
                    else:
                        c.check(bytes(probe.uc.mem_read(tx + 4, 1)) == b"\x01",
                                "failed TX released its buffer before DMA was idle")
                        pending[instance] = b"next"
                    if len(physical) == 2:
                        pending[1 - instance] = b"other"
                    before = len(dma.writes)
                    probe.call("vcom_task")
                    c.check(not any(offset == 0x78 and value & mask for offset, value in dma.writes[before:]),
                            "busy console DMA was aborted/reused")
                    c.check(instance in pending if not receive_error else True,
                            "failed TX consumed a new buffer while still busy")
                    if len(physical) == 2:
                        c.check((1 - instance, b"other") in reads, "failed console blocked the other console")
                    dma.set(0x38, 0)
                    dma.set(0x58, SWO_MASK)
                    probe.call("vcom_task")
                    c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "console recovery did not return")
                    c.check(dma.get(0x58) == SWO_MASK, "console recovery cleared another channel's completion")
                    if receive_error:
                        c.check(word(rx + 4) == 0 and word(rx + 8) == 0, "RX recovery kept a stale ring baseline")
                        dma.set(0x58, 1 << rx_channel)
                        probe.call("DMA0_IRQHandler")
                        c.check(word(rx + 8) == 1, "recovered RX stopped counting completions")
                        ring = probe.sym["vcom_rx_ring"] + instance * 4096
                        probe.uc.mem_write(ring, b"new")
                        room[instance] = 3
                        probe.call("vcom_task")
                        c.check((instance, b"new") in writes and word(rx + 4) == 3,
                                "recovered RX did not forward from its new baseline")
                    else:
                        c.check((instance, b"next") in reads, "recovered TX did not accept the next buffer")
                        entry = base + offsets["dma_table"] + tx_channel * 16
                        expected = probe.sym["vcom_tx_buffer"] + instance * 512 + 3
                        c.check(word(entry + 4) == expected and
                                (dma.get(0x408 + tx_channel * 16) >> 16) & 0x3ff == 3,
                                "recovered TX descriptor did not use only the new buffer")
                        dma.set(0x58, 1 << tx_channel)
                        probe.call("DMA0_IRQHandler")
                        c.check(bytes(probe.uc.mem_read(tx + 4, 1)) == b"\x00", "recovered TX stayed busy")
                        count = len(reads)
                        probe.call("vcom_task")
                        c.check(len(reads) == count, "TX recovery replayed a buffer")
                    probe.uc.hook_del(hook)
    c.note("console ERRINT/INTA recovery, idle ownership, next transfers, second-console isolation and renumbering")


# A console's entry in src/vcom.c's `vcom`: the UART, its clock now, the two sources, its pins.
VCOM_BYTES = 20
# SYSCON's clock selectors (fsl_clock.c CLOCK_AttachClk) and Flexcomm 0's index among them.
SYSCON_CLKSEL = 0x40000260
FLEXCOMM_CLKSEL0 = 20


def check_line(c, elf, symbols):
    for physical in ((0,), (1,), (0, 1)):
        for instance, port in enumerate(physical):
            probe = machine(elf, symbols)
            dma = Dma(probe)
            probe.uc.mem_write(probe.sym["vcom_count"], u32(len(physical)))
            for i, p in enumerate(physical):
                # The UART, the clock it runs from, FRO_HF_DIV and the main clock, and its pins.
                pins = probe.sym["vcom2_uart" if p else "vcom_uart"]
                probe.uc.mem_write(probe.sym["vcom"] + VCOM_BYTES * i,
                                   struct.pack("<5I", 0x40086000 + p * 4096, 96000000, 96000000, 150000000, pins))
                probe.uc.mem_write(probe.sym["vcom_rx"] + 16 * i, struct.pack("<3IB3x", 4 + p * 2, 77, 0, 0))
                probe.uc.mem_write(probe.sym["vcom_tx"] + 20 * i, u32(5 + p * 2) + bytes(16))
                probe.uc.mem_write(0x40086000 + p * 4096 + 8, u32(8))  # TXIDLE
            tx = probe.sym["vcom_tx"] + 20 * instance
            uart = 0x40086000 + port * 4096
            mask = 1 << (4 + port * 2)
            # Return no USB bytes; the test exercises the linked callback and task.
            def stub(uc, at, size, data):
                if at in usb_calls:
                    uc.reg_write(UC_ARM_REG_R0, 0)
                    uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))
            usb_calls = {probe.sym[n] for n in ("tud_cdc_n_available", "tud_cdc_n_write_available", "tud_cdc_n_write_flush")}
            hook = probe.uc.hook_add(UC_HOOK_CODE, stub)
            probe.uc.ctl_flush_tb()

            def request(baud, stop=0, parity=0, bits=8):
                probe.uc.mem_write(SCRATCH, struct.pack("<IBBB", baud, stop, parity, bits))
                probe.call("tud_cdc_line_coding_cb", instance, SCRATCH)

            def accepted(baud, stop=0, parity=0, bits=8):
                """Whether SET_LINE_CODING's data stage takes the coding or stalls."""
                probe.uc.mem_write(SCRATCH, struct.pack("<IBBB", baud, stop, parity, bits))
                return probe.call("tud_cdc_line_coding_request_cb", instance, SCRATCH) & 0xFF != 0

            def task():
                probe.call("vcom_task")
                c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "line coding blocked the console task")

            def word(address):
                return int.from_bytes(probe.uc.mem_read(address, 4), "little")

            probe.uc.mem_write(uart, u32(5))  # enabled, eight bits
            probe.uc.mem_write(uart + 0x20, u32(51))
            probe.uc.mem_write(uart + 0x28, u32(15))
            probe.uc.mem_write(uart + 0xe00, u32(0x30003))  # FIFO/DMA configuration sentinel
            initial = bytes(probe.uc.mem_read(uart, 4096))
            # SET_LINE_CODING stalls what neither clock makes within 3%, and takes the rest: 10 Mbaud
            # and 30 Mbaud only from the main clock.
            for args, wanted in (((115200,), True), ((19200000,), True), ((10000000,), True), ((30000000,), True),
                                 ((0,), False), ((1,), False), ((31000000,), False), ((0xffffffff,), False),
                                 ((115200, 1), False), ((115200, 0, 3), False), ((115200, 0, 0, 6), False)):
                c.check(accepted(*args) == wanted, f"console {instance}: SET_LINE_CODING {args} "
                                                   f"{'stalled' if wanted else 'taken'}")
            for args in ((0,), (1,), (31000000,), (0xffffffff,), (115200, 1), (115200, 0, 3), (115200, 0, 0, 6)):
                request(*args)
                task()
                c.check(bytes(probe.uc.mem_read(uart, 4096)) == initial, "rejected line coding changed UART registers")
                c.check(bytes(probe.uc.mem_read(tx + 6, 1)) == b"\x00", "rejected coding stayed pending")
            request(19200000, 2, 1, 7)
            probe.uc.mem_write(tx + 4, b"\x01")
            task()
            c.check(word(uart) == 5, "line coding interrupted an active TX DMA")
            probe.uc.mem_write(tx + 4, b"\x00")
            probe.uc.mem_write(uart + 8, u32(0))
            task()
            c.check(word(uart) == 5, "line coding changed before TX FIFO drained")
            probe.uc.mem_write(uart + 8, u32(8))
            dma.set(0x38, mask)
            task()
            c.check(word(uart) == 5 and dma.get(0x20) & mask == 0, "line coding did not wait for RX DMA")
            request(0)
            dma.set(0x38, 0)
            task()
            c.check(word(uart) == 0x71 and word(uart + 0x20) == 0 and word(uart + 0x28) == 4,
                    "valid line coding did not apply its format and divisors")
            c.check(word(uart + 0xe00) == 0x30003 and dma.get(0x20) & mask != 0,
                    "line coding reset FIFOs or left RX disabled")
            c.check(word(probe.sym["vcom_rx"] + 16 * instance + 4) == 77,
                    "line coding discarded the RX ring baseline")
            request(115200)
            task()
            c.check(word(uart) == 5 and word(uart + 0x20) == 51 and word(uart + 0x28) == 15,
                    "console could not return to its original coding")
            # 10 Mbaud runs from the main clock, 150 MHz / 15, and 115200 from FRO_HF_DIV again.
            selector = SYSCON_CLKSEL + 4 * (FLEXCOMM_CLKSEL0 + port)
            probe.uc.mem_write(selector, u32(7))
            request(10000000)
            task()
            chosen = word(selector)
            clock = word(probe.sym["vcom"] + VCOM_BYTES * instance + 4)
            c.check(word(uart + 0x20) == 0 and word(uart + 0x28) == 14 and chosen == 0 and clock == 150000000,
                    f"console {instance}: 10 Mbaud took BRG {word(uart + 0x20)}, OSR {word(uart + 0x28)},"
                    f" selector {chosen}, {clock} Hz; wanted the main clock / 15")
            request(115200)
            task()
            chosen = word(selector)
            clock = word(probe.sym["vcom"] + VCOM_BYTES * instance + 4)
            c.check(word(uart + 0x20) == 51 and word(uart + 0x28) == 15 and chosen == 3 and clock == 96000000,
                    f"console {instance}: back to 115200 took selector {chosen}, {clock} Hz; wanted FRO_HF_DIV")
            probe.uc.hook_del(hook)
    c.note("console line validation, SET_LINE_CODING stalled for what neither clock makes, the main clock for"
           " rates FRO_HF_DIV cannot make, TX drain, RX DMA quiescence, FIFO/ring preservation and renumbering")


INTA, INTB, ENABLESET, ENABLECLR, INTENCLR, ABORT = 0x58, 0x60, 0x20, 0x28, 0x50, 0x78
SETINTA, SETINTB, SWTRIG = 1 << 4, 1 << 5, 1 << 2


class WatchedDma(Dma):
    """Dma recording dap_shared.swo_lost as each write that clears a SWO flag lands."""

    def __init__(self, probe, lost_at):
        super().__init__(probe)
        self.probe, self.lost_at, self.lost_at_clear = probe, lost_at, []

    def write(self, uc, offset, size, value, data):
        if offset in (INTA, INTB) and value & SWO_MASK:
            self.lost_at_clear.append(self.probe.uc.mem_read(self.lost_at, 1)[0])
        super().write(uc, offset, size, value, data)


def check_ring_flags(c, elf, symbols, offsets):
    """CPU 0's DMA interrupt counts each ring's segments by alternating flags (src/dma_ring.h):
    the flag due counts, the other or both mark the count lost. SWO's is reported in
    dap_shared.swo_lost before the flags clear; a console's ring stops as a failed one does, and
    vcom_task starts it again with both flags clear and its descriptors alternating. A console
    reader that finds the next segment's flag up stops the ring itself and delivers nothing."""
    probe = machine(elf, symbols)
    base = probe.sym["dap_shared"]
    dma = WatchedDma(probe, base + offsets["swo_lost"])

    def word(address):
        return int.from_bytes(probe.uc.mem_read(address, 4), "little")

    def irq(a, b):
        dma.set(INTA, a)
        dma.set(INTB, b)
        dma.lost_at_clear.clear()
        probe.call("DMA0_IRQHandler")

    # Energy capture's segment is told only its own channel's flags, whatever other ring
    # completed alongside it.
    took = []

    def integrate(uc, at, size, data):
        took.append((uc.reg_read(UC_ARM_REG_R0) & 0xFF, uc.reg_read(UC_ARM_REG_R1) & 0xFF))

    hook = probe.uc.hook_add(UC_HOOK_CODE, integrate, begin=probe.sym["energy_integrate_segment"],
                             end=probe.sym["energy_integrate_segment"])
    probe.uc.ctl_flush_tb()
    for other in (SWO_MASK, 1 << 4):
        for energy_flag, other_flag in itertools.product((INTA, INTB), repeat=2):
            took.clear()
            irq(ENERGY_MASK * (energy_flag == INTA) | other * (other_flag == INTA),
                ENERGY_MASK * (energy_flag == INTB) | other * (other_flag == INTB))
            c.check(took == [(energy_flag == INTA, energy_flag == INTB)],
                    f"energy segment on {'INTA' if energy_flag == INTA else 'INTB'} beside channel"
                    f" {other.bit_length() - 1} on {'INTA' if other_flag == INTA else 'INTB'}: integration took {took}")
    probe.uc.hook_del(hook)
    probe.uc.ctl_flush_tb()

    segments, lost = base + offsets["swo_segments"], base + offsets["swo_lost"]
    for counted in (0, 1, 6, 0xFFFFFFFF):
        due, other = (INTB, INTA) if counted & 1 else (INTA, INTB)
        for took, name in (({due}, "due"), ({other}, "not due"), ({INTA, INTB}, "both")):
            probe.uc.mem_write(segments, u32(counted))
            probe.uc.mem_write(lost, b"\x00")
            irq(SWO_MASK if INTA in took else 0, SWO_MASK if INTB in took else 0)
            ok = name == "due"
            c.check(word(segments) == (counted + ok) & 0xFFFFFFFF and probe.uc.mem_read(lost, 1)[0] == (not ok),
                    f"SWO count {counted}, flag {name}: count {word(segments)}, lost {probe.uc.mem_read(lost, 1)[0]}")
            c.check(dma.get(INTA) & SWO_MASK == 0 and dma.get(INTB) & SWO_MASK == 0,
                    f"SWO count {counted}, flag {name}: a flag taken was left up")
            c.check(all(seen == (not ok) for seen in dma.lost_at_clear) and dma.lost_at_clear,
                    f"SWO count {counted}, flag {name}: swo_lost {dma.lost_at_clear} as the flags cleared")

    # A console: channel 4 receives, 5 sends.
    rx, tx = probe.sym["vcom_rx"], probe.sym["vcom_tx"]
    probe.uc.mem_write(probe.sym["vcom_count"], u32(1))
    probe.uc.mem_write(probe.sym["vcom"], struct.pack("<2I", 0x40086000, 96000000))
    probe.uc.mem_write(tx, u32(5) + bytes(16))
    mask = 1 << 4
    usb = {probe.sym[n] for n in ("tud_cdc_n_available", "tud_cdc_n_read", "tud_cdc_n_write_available",
                                  "tud_cdc_n_write", "tud_cdc_n_write_flush")}
    written = []
    preempt = []

    def stub(uc, at, size, data):
        if at in usb:
            if at == probe.sym["tud_cdc_n_write"]:
                written.append(uc.reg_read(UC_ARM_REG_R2))
            # send() asks between vcom_task's recover() and receive()'s read: the interrupt runs
            # there and finds the count lost, which leaves the flags clear and the count stale.
            if at == probe.sym["tud_cdc_n_available"] and preempt:
                preempt.clear()
                uc.mem_write(rx + 12, b"\x01")
                dma.set(ENABLESET, dma.get(ENABLESET) & ~mask)
                dma.set(INTA, 0)
                dma.set(INTB, 0)
            room = at == probe.sym["tud_cdc_n_write_available"]
            uc.reg_write(UC_ARM_REG_R0, 512 if room else 0)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    hook = probe.uc.hook_add(UC_HOOK_CODE, stub)
    probe.uc.ctl_flush_tb()

    def console(counted, read=0):
        probe.uc.mem_write(rx, struct.pack("<3IB3x", 4, read, counted, 0))
        dma.set(ENABLESET, mask)

    def failed():
        return probe.uc.mem_read(rx + 12, 1)[0]

    for counted in (0, 1, 2):
        due, other = (INTB, INTA) if counted & 1 else (INTA, INTB)
        for took, name in (({due}, "due"), ({other}, "not due"), ({INTA, INTB}, "both")):
            console(counted)
            irq(mask if INTA in took else 0, mask if INTB in took else 0)
            ok = name == "due"
            c.check(word(rx + 8) == counted + ok and failed() == (not ok),
                    f"console count {counted}, flag {name}: count {word(rx + 8)}, failed {failed()}")
            c.check(ok or dma.get(ENABLESET) & mask == 0, f"console count {counted}, flag {name}: ring left running")
            if not ok:
                c.check(probe.call("vcom_ready") == 1, f"console count {counted}, flag {name}: no wake to restart")
                before = len(dma.writes)
                probe.call("vcom_task")
                after = dma.writes[before:]
                c.check((ABORT, mask) in after and any(o == INTA and v & mask for o, v in after) and
                        any(o == INTB and v & mask for o, v in after),
                        f"console count {counted}, flag {name}: restart did not abort and clear both flags")
                c.check(word(rx + 8) == 0 and word(rx + 4) == 0 and failed() == 0,
                        f"console count {counted}, flag {name}: restart kept the old count or position")
                started = [v for o, v in after if o == 0x408 + 4 * 16]
                ring = probe.sym["vcom_rx_descriptors"]
                flags = [word(ring + 16 * k) & (SETINTA | SETINTB) for k in range(4)]
                c.check(started and started[-1] & (SWTRIG | SETINTA | SETINTB) == SWTRIG | SETINTA and
                        flags == [SETINTA, SETINTB, SETINTA, SETINTB],
                        f"console count {counted}, flag {name}: restarted ring's flags {started} {flags}")

    # A reader that finds the next segment's flag up before the interrupt runs. The reader is where
    # the position reads, so only the lost count can wake the loop.
    for counted in (0, 1):
        due, other = (INTB, INTA) if counted & 1 else (INTA, INTB)
        for pending in (False, True):
            console(counted, (counted + pending) * 1024)
            dma.set(due, mask if pending else 0)
            dma.set(other, mask)
            dma.set(0x408 + 4 * 16, 1023 << 16)
            c.check(probe.call("vcom_ready") == 1, f"console reader, count {counted}: overtaken ring did not wake")
            written.clear()
            probe.call("vcom_task")
            c.check(not written and failed() == 1 and dma.get(ENABLESET) & mask == 0,
                    f"console reader, count {counted}, pending {pending}: overtaken ring delivered {written}"
                    f" or kept running")
            dma.set(INTA, 0)
            dma.set(INTB, 0)

    # The interrupt finds the count lost after recover() and before the read: the stale position
    # is not delivered.
    console(2)
    dma.set(0x408 + 4 * 16, (1023 - 300) << 16)
    preempt.append(True)
    written.clear()
    probe.call("vcom_task")
    c.check(not preempt and not written and failed() == 1,
            f"console reader after the interrupt found the count lost: delivered {written}")
    probe.uc.hook_del(hook)
    c.note("ring flags: SWO and console counts by alternating flags, lost reported before the clear, console"
           " restart, an overtaken console reader and one the interrupt overtook")
