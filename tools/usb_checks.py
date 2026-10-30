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
        self.configured = True
        self.tx_attempts = 0
        # Replies on the aux interface's vendor instance, apart from CMSIS-DAP's, and the buffers its
        # OUT endpoint was armed into.
        self.aux_index = None
        self.aux_configured = False
        self.aux_sent = []
        self.aux_arms = []
        self.accept_tx = True
        self.shared = symbols0["dap_shared"]
        self.staging = symbols0["dap_stage"]
        assert self.shared == symbols1["dap_shared"]
        assert self.staging == symbols1["dap_stage"]

        def endpoints(uc, at, size, data):
            if at == self.cpu0.sym["tud_vendor_n_mounted"]:
                index = uc.reg_read(UC_ARM_REG_R0)
                result = int(self.aux_configured if index == self.aux_index else self.configured)
            elif at == self.cpu0.sym["tud_mounted"]:
                result = int(self.configured)
            elif at == self.cpu0.sym["tud_vendor_n_read_xfer_buf"]:
                self.aux_arms.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))
                result = 1
            elif at == self.cpu0.sym["tud_vendor_n_write_xfer"]:
                index, address, length = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
                self.tx_attempts += 1
                accepted = self.accept_tx and (self.aux_configured if index == self.aux_index else self.configured)
                if accepted:
                    (self.aux_sent if index == self.aux_index else self.sent).append(bytes(uc.mem_read(address, length)))
                result = length if accepted else 0
            elif at == self.cpu0.sym["tud_hid_n_report"]:
                address, length = uc.reg_read(UC_ARM_REG_R2), uc.reg_read(UC_ARM_REG_R3)
                self.tx_attempts += 1
                if self.accept_tx and self.configured:
                    self.sent.append(bytes(uc.mem_read(address, length)))
                result = int(self.accept_tx and self.configured)
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


def check_unconfigured(c, elf0, elf1, symbols0, symbols1, fields, slots):
    for hid in (False, True):
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        for _ in range(slots):
            usb.receive(b"\x00\xff", hid)
        usb.configured = False
        # CPU 1 finishes commands while USB has lost its configuration, before the next mount.
        for _ in range(slots):
            usb.dispatch()
        for _ in range(slots * 2):
            usb.call0("send_head")
        name = "HID" if hid else "bulk"
        c.check(usb.get("response_head", 1) == usb.get("response_tail", 1) and
                all(usb.cpu0.uc.mem_read(usb.shared[0] + fields["response_len"] + slot * 2, 2) == b"\x00\x00"
                    for slot in range(slots)), f"unconfigured {name}: CPU 0 retained completed replies")
        c.check(usb.tx_attempts == 0, f"unconfigured {name}: CPU 0 retried a closed endpoint {usb.tx_attempts} times")
        c.check(usb.cpu0.uc.mem_read(usb.cpu0.sym["response_in_flight"], 1) == b"\x00",
                f"unconfigured {name}: reply ownership stayed busy")
        usb.configured = True
        usb.call0("tud_mount_cb")
        for _ in range(slots):
            usb.call0("send_head")
        usb.receive(b"\x00\xff", hid)
        usb.dispatch()
        usb.call0("send_head")
        expected = b"\x00\x02" + (b"\x40\x00" + bytes(60) if hid else b"\x00\x02")
        c.check(usb.sent == [expected], f"unconfigured {name}: new host's first reply changed")
        # A bus reset also abandons accepted IN transfers without a completion callback.
        abandoned = usb.get("response_head", 1)
        usb.configured = False
        usb.call0("send_head")
        c.check(usb.cpu0.uc.mem_read(usb.cpu0.sym["response_in_flight"], 1) == b"\x00" and
                usb.get("response_head", 1) == (abandoned + 1) % slots and
                usb.cpu0.uc.mem_read(usb.shared[0] + fields["response_len"] + abandoned * 2, 2) == b"\x00\x00",
                f"unconfigured {name}: abandoned IN transfer retained its slot")
        usb.configured = True
        usb.call0("tud_mount_cb")
        usb.receive(b"\x00\xff", hid)
        usb.dispatch()
        usb.call0("send_head")
        c.check(usb.sent == [expected, expected], f"unconfigured {name}: abandoned IN transfer blocked the next host")
    c.note("unconfigured HID/bulk replies: late CPU 1 publication releases every slot without endpoint retries; mount recovers")


def check_aux(c, elf0, elf1, symbols0, symbols1, fields):
    """Verge Aux beside CMSIS-DAP: CPU 0 hands each aux request to CPU 1 in dap_stage.aux, CPU 1 runs
    it ahead of the queue with CMSIS-DAP's own dispatch, and CPU 0 sends the reply on the aux
    instance. Instance 1 is the Pro with CMSIS-DAP; instance 0 the Pro with its SWD jumper."""
    connect = symbols1["DAP_Data"][0]

    def fresh(present=True, instance=1):
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        usb.call0("aux_init", int(present), instance)
        usb.aux_index = instance
        usb.aux_configured = True
        return usb

    def arrive(usb, payload):
        buffer = usb.cpu0.sym["aux_out"]
        usb.cpu0.uc.mem_write(buffer, payload)
        usb.call0("tud_vendor_rx_cb", usb.aux_index, buffer, len(payload))

    def ask(usb, payload):
        before = len(usb.aux_sent)
        arrive(usb, payload)
        c.check(usb.dispatch() == 1, f"aux request {payload.hex()} was not run")
        usb.call0("aux_task")
        replies = usb.aux_sent[before:]
        c.check(len(replies) == 1, f"aux request {payload.hex()} sent {len(replies)} replies")
        if replies:
            usb.call0("tud_vendor_tx_cb", usb.aux_index, len(replies[0]))
        return replies[0] if replies else b""

    def bulk(usb, payload):
        before = len(usb.sent)
        usb.receive(payload)
        usb.dispatch()
        usb.call0("send_head")
        reply = usb.sent[before] if len(usb.sent) > before else b""
        usb.call0("tud_vendor_tx_cb", 0, len(reply))
        return reply

    def pro(usb):
        usb.put("board", 1, 1)  # board_straps.h: the Pro
        usb.put("jumpers", 0, 1)
        usb.cpu1.uc.mem_write(usb.shared[0], bytes(usb.cpu0.uc.mem_read(*usb.shared)))
        for name in ("board_debug_pins_init", "dio_init"):
            usb.cpu1.call(name, timeout_insns=1_000_000)
        usb.cpu0.uc.mem_write(usb.shared[0], bytes(usb.cpu1.uc.mem_read(*usb.shared)))
        return usb

    # The commands it carries answer as on CMSIS-DAP, size checks included.
    carried = (b"\x82", b"\x8e\x00\x00\x00", b"\x8e\x08\x08\x08", b"\x8e\x01\x01\x01", b"\x8e\x08\x00\x00", b"\x8e\x00",
               b"\x8f\x02", b"\x8f\x01", b"\x8f", b"\x90\x01", b"\x90\x03", b"\x90\x04\x00", b"\x90\x04\x02", b"\x90\x02",
               b"\x90", b"\x90\x00\x01")
    for usb in (fresh(), pro(fresh())):
        for payload in carried:
            c.check(ask(usb, payload) == bulk(usb, payload), f"aux reply to {payload.hex()} differs from CMSIS-DAP's")
    # Anything else gets [0xff] and does not run.
    for payload in (b"\x00\x01", b"\x02\x02", b"\x07", b"\x83\x00", b"\x85\x01", b"\x91\x00\x00\x00\x00\x00\x00\x00"):
        c.check(ask(usb, payload) == b"\xff", f"aux ran {payload.hex()}")
    c.check(bytes(usb.cpu1.uc.mem_read(connect, 1)) == b"\x00", "an aux DAP_Connect connected")

    # Aux drives the Pro's DIO pins themselves: CMSIS-DAP sees what aux left, and the other way round.
    usb = pro(fresh())
    driven = ask(usb, b"\x8e\x08\x08\x08")
    c.check(driven[:4] == b"\x8e\x00\x08\x08" and bulk(usb, b"\x8e\x00\x00\x00")[2:4] == b"\x08\x08",
            f"aux 0x8E driving DIO3 high answered {driven.hex()}, and CMSIS-DAP did not see it")
    bulk(usb, b"\x8e\x08\x00\x00")
    c.check(ask(usb, b"\x8e\x00\x00\x00")[2:4] == b"\x00\x00", "aux did not see CMSIS-DAP return DIO3 to an input")

    # OUT is armed into aux_out once, and again only once the reply has left the slot.
    usb = fresh()
    usb.call0("aux_task")
    usb.call0("aux_task")
    c.check(usb.aux_arms == [(1, usb.cpu0.sym["aux_out"])], f"aux OUT armed {usb.aux_arms}, wanted once into aux_out")
    arrive(usb, b"\x8f\x02")
    usb.call0("aux_task")
    c.check(len(usb.aux_arms) == 1, "aux OUT armed while CPU 1 held a request")
    usb.dispatch()
    usb.call0("aux_task")
    c.check(len(usb.aux_sent) == 1 and len(usb.aux_arms) == 2, "aux OUT not armed again once the reply left the slot")

    # Ahead of the queue: one dispatch runs the aux request and leaves the queued command.
    usb = fresh()
    usb.receive(b"\x02\x02")
    arrive(usb, b"\x8f\x02")
    c.check(usb.dispatch() == 1 and usb.get("response_len", 2) == 0, "aux request waited behind the queue")
    usb.call0("aux_task")
    c.check(len(usb.aux_sent) == 1, "aux reply not sent")
    c.check(usb.dispatch() == 1 and usb.get("response_len", 2) != 0, "queued command lost behind an aux request")

    # A packet into a full slot is dropped rather than overwriting the request CPU 1 holds.
    usb = fresh()
    arrive(usb, b"\x8f\x02")
    arrive(usb, b"\x82")
    usb.dispatch()
    usb.call0("aux_task")
    c.check(len(usb.aux_sent) == 1 and usb.aux_sent[0][:1] == b"\x8f", "a request arriving into a full slot was run")
    c.check(usb.dispatch() == 0, "a dropped aux request left work for CPU 1")

    # A reply waits for the one on its way, and the two leave in request order.
    usb = fresh()
    arrive(usb, b"\x8f\x02")
    usb.dispatch()
    usb.call0("aux_task")
    arrive(usb, b"\x82")
    usb.dispatch()
    usb.call0("aux_task")
    c.check(len(usb.aux_sent) == 1, "a second reply went while the first was on its way")
    usb.call0("tud_vendor_tx_cb", 1, len(usb.aux_sent[0]))
    c.check([r[:1] for r in usb.aux_sent] == [b"\x8f", b"\x82"], "aux replies left out of order")

    # A refused submission is tried again.
    usb = fresh()
    usb.accept_tx = False
    arrive(usb, b"\x8f\x02")
    usb.dispatch()
    usb.call0("aux_task")
    usb.accept_tx = True
    c.check(not usb.aux_sent, "refused aux submission recorded as sent")
    usb.call0("aux_task")
    c.check(len(usb.aux_sent) == 1, "refused aux reply was not sent again")

    # Gone hosts: a reply to a request from before a new configuration, one that arrives while the
    # interface is unconfigured, and a new configuration while a reply is on its way.
    usb = fresh()
    arrive(usb, b"\x8f\x02")
    usb.call0("tud_mount_cb")
    usb.dispatch()
    usb.call0("aux_task")
    c.check(not usb.aux_sent, "aux reply to a gone host was sent")
    c.check(ask(usb, b"\x82")[:1] == b"\x82", "aux did not answer after a new configuration")
    usb = fresh()
    arrive(usb, b"\x8f\x02")
    usb.aux_configured = False
    usb.dispatch()
    usb.call0("aux_task")
    c.check(not usb.aux_sent, "aux reply submitted while the interface was unconfigured")
    c.check(usb.call0("aux_has_work") == 0, "CPU 0 kept aux work while unconfigured, so it would not sleep")
    usb.aux_configured = True
    usb.call0("tud_mount_cb")
    c.check(ask(usb, b"\x82")[:1] == b"\x82", "aux did not answer once configured again")
    usb = fresh()
    arrive(usb, b"\x8f\x02")
    usb.dispatch()
    usb.call0("aux_task")
    usb.call0("tud_mount_cb")
    c.check(ask(usb, b"\x82")[:1] == b"\x82", "a reply on its way at a new configuration stopped aux for good")

    # The Pro with its SWD jumper: Verge Aux is vendor instance 0, and its callbacks are aux's.
    usb = fresh(instance=0)
    c.check(ask(usb, b"\x8f\x02") == bytes([0x8f]) + bulk(fresh(instance=1), b"\x8f\x02")[1:],
            "aux at vendor instance 0 did not answer")

    # Without the interface, nothing reaches the slot through its instance number.
    usb = fresh(present=False)
    arrive(usb, b"\x8f\x02")
    c.check(usb.dispatch() == 0, "a request reached CPU 1 through an absent aux interface")
    c.note("Verge Aux: CMSIS-DAP-identical replies, [0xff] for the rest, OUT armed only with the slot free, ahead "
           "of the queue, in order, retry, gone hosts (new configuration, unconfigured, reply in flight), instance 0")


def check_aux_stream(c, elf0, elf1, symbols0, symbols1, fields, layout):
    """The stream on Verge Aux, CPU 0's side: 0x90 modes 6 and 7, which CPU 0 answers itself once CPU 1
    holds none of the capture state, and the messages it sends after replies."""
    instance = 1

    def fresh():
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        usb.call0("aux_init", 1, instance)
        usb.aux_index = instance
        usb.aux_configured = True
        energy = usb.cpu0.sym["energy_integration"]
        usb.put("energy_integration", energy)
        usb.energy = energy
        return usb

    def field(usb, name, value=None, size=4, offset=0):
        at = usb.energy + layout[name] + offset
        if value is None:
            return int.from_bytes(usb.cpu0.uc.mem_read(at, size), "little")
        usb.cpu0.uc.mem_write(at, value.to_bytes(size, "little"))
        return value

    def capture(usb):
        """A capture CPU 1 started at 1,000 pairs a second, 41 pairs integrated."""
        usb.put("energy_available", 1, 1)
        field(usb, "running", 1, 1)
        field(usb, "setting", 2, 1)
        field(usb, "full_scale_mv", 3000)
        field(usb, "previous_low", 0, 1)
        field(usb, "stream.clock_hz", 96_000_000)
        field(usb, "stream.period", 96_000)
        field(usb, "stream.next_sequence", 42)
        field(usb, "decoder", 41, 4, 12)

    def publish(usb, message):
        """A message as the capture's owner publishes one."""
        written = field(usb, "stream.written")
        at = written % 16
        usb.cpu0.uc.mem_write(usb.energy + layout["stream.slot"] + 512 * at, message)
        field(usb, "stream.length", len(message), 2, 2 * at)
        field(usb, "stream.written", written + 1)

    def arrive(usb, payload):
        buffer = usb.cpu0.sym["aux_out"]
        usb.cpu0.uc.mem_write(buffer, payload)
        usb.call0("tud_vendor_rx_cb", instance, buffer, len(payload))

    def drain(usb):
        """Everything aux_task sends, each transfer completing before the next. A completion's
        callback sends the next one itself."""
        start = done = len(usb.aux_sent)
        usb.call0("aux_task")
        while done < len(usb.aux_sent) and done - start < 32:
            usb.call0("tud_vendor_tx_cb", instance, len(usb.aux_sent[done]))
            done += 1
            usb.call0("aux_task")
        return usb.aux_sent[start:]

    for available, running, error, payload, want in (
            (0, 1, 0, b"\x90\x06\x00", b"\x90\x00\x03"), (1, 0, 0, b"\x90\x06\x00", b"\x90\x00\x01"),
            (1, 1, 1, b"\x90\x06\x00", b"\x90\x00\x07"), (1, 1, 0, b"\x90\x06", b"\xff"),
            (1, 1, 0, b"\x90\x07", b"\x90\x00\x00")):
        usb = fresh()
        capture(usb)
        usb.put("energy_available", available, 1)
        field(usb, "running", running, 1)
        usb.put("energy_dma_error", error, 1)
        arrive(usb, payload)
        c.check(usb.dispatch() == 0, f"aux {payload.hex()} went to CPU 1")
        sent = drain(usb)
        c.check(sent == [want], f"aux {payload.hex()} with available {available}, running {running}, error {error}: {[m.hex() for m in sent]}")

    # Mode 6 on a running capture: the reply, then the header from the next pair, then pairs.
    usb = fresh()
    capture(usb)
    arrive(usb, b"\x90\x06\x00")
    sent = drain(usb)
    c.check(len(sent) == 2 and sent[0] == b"\x90\x00\x00" and len(sent[1]) == 68 and sent[1][:2] == b"\xa1\x01",
            f"mode 6 sent {[m[:8].hex() for m in sent]}")
    if len(sent) == 2 and len(sent[1]) == 68:
        h = sent[1]
        c.check(int.from_bytes(h[2:6], "little") == 1000 and h[6] == 2 and int.from_bytes(h[7:9], "little") == 3000
                and int.from_bytes(h[9:13], "little") == 42 and h[13] == 0, f"mode 6 header {h[:14].hex()}")
    publish(usb, b"\xa0\x00" + bytes(14))
    c.check(drain(usb) == [b"\xa0\x00" + bytes(14)], "pairs after the header were not sent")
    # Mode 7: the reply, then the end, with the last pair integrated.
    arrive(usb, b"\x90\x07")
    sent = drain(usb)
    c.check(sent == [b"\x90\x00\x00", b"\xa2\x01" + (41).to_bytes(4, "little")], f"mode 7 sent {[m.hex() for m in sent]}")

    # CPU 1 holds the capture state for good when nothing runs, from boot and after a stop: modes 6 and
    # 7 answer without it. Inside a command, around a supply read, the capture runs and they wait,
    # without CPU 0 spinning; so they do while a start has not yet given its timeline.
    for mode, want in ((b"\x90\x06\x00", b"\x90\x00\x01"), (b"\x90\x07", b"\x90\x00\x00")):
        usb = fresh()
        capture(usb)
        field(usb, "running", 0, 1)
        usb.put("energy_control_ack", 1)
        arrive(usb, mode)
        c.check(drain(usb) == [want], f"{mode.hex()} with the capture stopped and CPU 1 holding its state")
    for quiet, pending in ((1, 0), (0, 1)):
        usb = fresh()
        capture(usb)
        usb.put("energy_control_ack", quiet)
        field(usb, "stream.timeline_pending", pending, 1)
        arrive(usb, b"\x90\x06\x00")
        c.check(drain(usb) == [] and usb.call0("aux_has_work") == 0,
                f"mode 6 ran while CPU 1 held the running capture's state (quiet {quiet}, pending {pending})")
        usb.put("energy_control_ack", 2)
        field(usb, "stream.timeline_pending", 0, 1)
        c.check(drain(usb)[:1] == [b"\x90\x00\x00"], "mode 6 did not run once CPU 1 let go")

    # A failure that left CPU 0 owning the state: its loop ends the stream.
    usb = fresh()
    capture(usb)
    arrive(usb, b"\x90\x06\x00")
    drain(usb)
    usb.put("energy_dma_error", 1, 1)
    c.check(usb.call0("aux_has_work") == 1, "a failed capture's stream left no work for CPU 0's loop")
    sent = drain(usb)
    c.check(sent == [b"\xa2\x04" + (41).to_bytes(4, "little")], f"a failed capture's stream ended with {[m.hex() for m in sent]}")

    # After a configuration, messages are a gone host's until a header this configuration asked for:
    # one it did not, from a request that ran across the reset, is dropped too. Unconfigured, all are.
    usb = fresh()
    capture(usb)
    arrive(usb, b"\x90\x06\x00")
    c.check(len(drain(usb)) == 2, "mode 6 did not send its reply and header")
    usb.call0("tud_mount_cb")
    publish(usb, b"\xa0\x00" + bytes(14))
    publish(usb, b"\xa1" + bytes(67))
    publish(usb, b"\xa0\x01" + bytes(14))
    c.check(drain(usb) == [], "after a configuration, a header nobody asked for was sent")
    # The gone host's stream, its pairs and the end mode 6 gives it, go too; the new header passes.
    arrive(usb, b"\x90\x06\x00")
    publish(usb, b"\xa0\x02" + bytes(14))
    sent = drain(usb)
    c.check([m[:2] for m in sent] == [b"\x90\x00", b"\xa1\x01"],
            f"after a configuration, mode 6 sent {[m[:2].hex() for m in sent]}")
    publish(usb, b"\xa0\x03" + bytes(14))
    c.check([m[:2] for m in drain(usb)] == [b"\xa0\x03"], "pairs after the new header were not sent")
    usb.aux_configured = False
    publish(usb, b"\xa0\x02" + bytes(14))
    c.check(drain(usb) == [] and field(usb, "stream.sent") == field(usb, "stream.written"),
            "messages kept while the interface was unconfigured")

    # A mode 5 request out: messages wait until its reply has gone.
    usb = fresh()
    usb.call0("tud_mount_cb")
    publish(usb, b"\xa1" + bytes(67))
    arrive(usb, b"\x90\x05" + (1000).to_bytes(4, "little") + b"\x00")
    c.check(drain(usb) == [], "a stream message went before the mode 5 reply")
    usb.dispatch()
    sent = drain(usb)
    c.check(len(sent) == 2 and sent[0][:2] == b"\x90\x00" and sent[1][0] == 0xA1,
            f"mode 5 then its header sent {[m[:4].hex() for m in sent]}")
    c.note("stream on Verge Aux: modes 6 and 7 on CPU 0 with each result, waiting for CPU 1, reply before header,"
           " end after mode 7, gone hosts' and unconfigured messages dropped, mode 5's reply first")
