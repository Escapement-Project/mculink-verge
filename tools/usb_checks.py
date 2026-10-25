"""Run USB callbacks and command tasks from the linked images; stub endpoint submission only."""

from cycles import Model, RETURN_MAGIC, SCRATCH, STACK_TOP, build, u16, u32
from energy_checks import machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3


class Usb:
    def __init__(self, elf0, elf1, symbols0, symbols1, fields):
        self.cpu0 = machine(elf0, symbols0)
        self.cpu1 = machine(elf1, symbols1)
        self.fields = fields
        self.sent = []
        self.accept_tx = True
        self.shared = symbols0["dap_shared"]
        self.staging = symbols0["dap_stage"]
        assert self.shared == symbols1["dap_shared"]
        assert self.staging == symbols1["dap_stage"]

        def endpoints(uc, at, size, data):
            if at == self.cpu0.sym["tud_vendor_n_mounted"]:
                result = 0  # Arming another OUT transfer belongs to TinyUSB.
            elif at == self.cpu0.sym["tud_vendor_n_write_xfer"]:
                address, length = uc.reg_read(UC_ARM_REG_R1), uc.reg_read(UC_ARM_REG_R2)
                if self.accept_tx:
                    self.sent.append(bytes(uc.mem_read(address, length)))
                result = length if self.accept_tx else 0
            elif at == self.cpu0.sym["tud_hid_n_report"]:
                address, length = uc.reg_read(UC_ARM_REG_R2), uc.reg_read(UC_ARM_REG_R3)
                if self.accept_tx:
                    self.sent.append(bytes(uc.mem_read(address, length)))
                result = int(self.accept_tx)
            else:
                return
            uc.reg_write(UC_ARM_REG_R0, result)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

        self.hook = self.cpu0.uc.hook_add(UC_HOOK_CODE, endpoints)
        self.cpu0.uc.ctl_flush_tb()
        self.put("transfer_abort", self.cpu1.sym["DAP_TransferAbort"])

    def put(self, field, value, size=4):
        self.cpu0.uc.mem_write(self.shared[0] + self.fields[field], value.to_bytes(size, "little"))

    def get(self, field, size=4):
        return int.from_bytes(self.cpu0.uc.mem_read(self.shared[0] + self.fields[field], size), "little")

    def dispatch(self):
        for address, length in (self.shared, self.staging):
            self.cpu1.uc.mem_write(address, bytes(self.cpu0.uc.mem_read(address, length)))
        result = self.cpu1.call("dap_task", timeout_insns=1_000_000)
        assert self.cpu1.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, "dap_task exceeded its instruction budget"
        for address, length in (self.shared, self.staging):
            self.cpu0.uc.mem_write(address, bytes(self.cpu1.uc.mem_read(address, length)))
        return result

    def call0(self, name, *args):
        result = self.cpu0.call(name, *args, timeout_insns=1_000_000)
        assert self.cpu0.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, f"{name} exceeded its instruction budget"
        return result

    def receive(self, payload, hid=False):
        self.cpu0.uc.mem_write(SCRATCH, payload)
        if hid:
            self.cpu0.uc.mem_write(STACK_TOP, u32(len(payload)))
            self.call0("tud_hid_set_report_cb", 0, 0, 2, SCRATCH)
        elif payload[0] == 7:
            self.call0("tud_vendor_rx_cb", 0, SCRATCH, len(payload))
        else:
            slot = self.get("request_tail", 1)
            address = self.shared[0] + self.fields["request"] + slot * 512
            self.put("rx_armed_slot", slot, 1)
            self.cpu0.uc.mem_write(address, payload)
            self.call0("tud_vendor_rx_cb", 0, address, len(payload))

def check_transport(c, elf0, elf1, symbols0, symbols1, fields, slots):
    for hid in (False, True):
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        # Fill the ordinary queue before sending abort through the callback.
        for _ in range(slots):
            usb.receive(b"\x00\xff", hid)
        before = bytes(usb.cpu0.uc.mem_read(*usb.shared))
        abort = symbols1["DAP_TransferAbort"][0]
        usb.cpu0.uc.mem_write(abort, b"\x00")
        usb.receive(b"\x07", hid)
        c.check(bytes(usb.cpu0.uc.mem_read(abort, 1)) == b"\x01", "USB abort did not bypass the full queue")
        c.check(bytes(usb.cpu0.uc.mem_read(*usb.shared)) == before, "abort consumed or changed queued commands")
        # An unpublished/corrupt pointer must not write outside CPU1's RAM.
        for pointer in (0, 0x20000000, 0x2003e000, 0x40101000):
            usb.put("transfer_abort", pointer)
            snapshot = bytes(usb.cpu0.uc.mem_read(pointer, 1)) if pointer else b""
            usb.receive(b"\x07", hid)
            if pointer:
                c.check(bytes(usb.cpu0.uc.mem_read(pointer, 1)) == snapshot, "abort wrote through an out-of-range pointer")

    usb = Usb(elf0, elf1, symbols0, symbols1, fields)
    usb.put("next_request_seq", 0xfffffffe)
    usb.receive(b"\x02\x02")  # Would select JTAG if an old request ran.
    usb.receive(b"\x02\x02", True)
    usb.call0("tud_mount_cb")
    c.check(usb.get("first_live_seq") == 0, "USB configuration did not advance the host generation")
    for _ in range(2):
        c.check(usb.dispatch() == 1, "stale request did not release its queue slot")
        c.check(usb.get("response_len", 2) == 0, "stale request published a reply")
    c.check(bytes(usb.cpu1.uc.mem_read(symbols1["DAP_Data"][0], 1)) == b"\x00", "stale Connect executed")

    # Production selection must merge HID and bulk in arrival order.
    usb.receive(b"\x02\x01", True)
    usb.receive(b"\x03")
    for seq, transport in ((0, 1), (1, 0)):
        out = usb.get("response_tail", 1)
        c.check(usb.dispatch() == 1, "fresh command did not dispatch")
        c.check(int.from_bytes(usb.cpu0.uc.mem_read(usb.shared[0] + fields["response_seq"] + out * 4, 4), "little") == seq,
                "HID/bulk arrival order changed")
        c.check(bytes(usb.cpu0.uc.mem_read(usb.shared[0] + fields["response_transport"] + out, 1)) == bytes([transport]),
                "reply transport changed")

    # A configuration change must release a pending old reply without submitting it to USB.
    usb.call0("tud_mount_cb")
    for _ in range(2):
        usb.call0("send_head")
    c.check(not usb.sent and usb.get("response_head", 1) == usb.get("response_tail", 1),
            "old replies were submitted or retained after configuration")
    usb.receive(b"\x00\xff")
    usb.dispatch()
    usb.accept_tx = False
    usb.call0("send_head")
    c.check(not usb.sent, "rejected endpoint submission was recorded as sent")
    usb.accept_tx = True
    usb.call0("send_head")
    c.check(usb.sent == [b"\x00\x02\x00\x02"], "new host did not receive its own reply after a failed submission")
    usb.call0("tud_vendor_tx_cb", 0, 4)
    c.check(usb.get("response_head", 1) == usb.get("response_tail", 1), "completion did not release the reply")
    for hid in (False, True):
        counts = []
        for inject in (False, True):
            transfer = build(elf1, "swd", True, Model())
            transfer.sym.update({name: address for name, (address, _) in symbols1.items()})
            transfer.command(b"\x02\x01")
            transfer.command(b"\x11" + u32(10_000_000))
            transfer.command(b"\x04\x00" + u16(7) + u16(0))
            abort = transfer.sym["DAP_TransferAbort"]
            transfer.target.pattern = [0, 1, 0]  # WAIT on every SWD attempt.
            attempts = []
            restart = transfer.target.restart

            def waiting():
                restart()
                attempts.append(1)
                if inject and len(attempts) == 1:
                    usb.receive(b"\x07", hid)
                    transfer.uc.mem_write(abort, bytes(usb.cpu0.uc.mem_read(abort, 1)))

            transfer.target.restart = waiting
            reply = transfer.command(b"\x05\x00\x01\x02")
            c.check(reply == b"\x05\x00\x02", "WAIT/abort response changed")
            counts.append(len(attempts))
            transfer.target.restart = restart
            transfer.target.pattern = [1, 0, 0] + [0] * 33
            c.check(transfer.command(b"\x05\x00\x01\x02") == b"\x05\x01\x01" + bytes(4),
                    "next transfer did not clear the previous abort")
        c.check(counts == [8, 1], f"abort did not terminate SWD retries: {counts}")
    c.note("linked USB callbacks: full-queue abort, pointer guard, host-generation wrap, transport ordering and reply retry")
    c.note("HID/bulk callback abort stops a running SWD WAIT loop; the next transfer recovers")
