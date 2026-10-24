"""Console recovery checks for image.py, using the linked DMA IRQ and console task."""

import struct

from energy_checks import Dma, SWO_MASK, machine
from cycles import RETURN_MAGIC, u32
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
                    c.check(rx_stride == 16 and tx_stride == 16, "console state fixture layout changed")
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
