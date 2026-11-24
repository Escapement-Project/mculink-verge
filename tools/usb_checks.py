"""Run USB callbacks and command tasks from the linked images; stub endpoint submission only."""

from cycles import Model, RETURN_MAGIC, SCRATCH, STACK_TOP, build, dap_setup, u16, u32
from energy_checks import machine
from unicorn import UC_HOOK_CODE
from unicorn.arm_const import UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2, UC_ARM_REG_R3


TUSB_SPEED_FULL, TUSB_SPEED_HIGH = 0, 2


class Usb:
    def __init__(self, elf0, elf1, symbols0, symbols1, fields):
        self.cpu0 = machine(elf0, symbols0)
        self.cpu1 = machine(elf1, symbols1)
        self.fields = fields
        self.sent = []
        # CMSIS-DAP's replies as `sent` has them, with the interface each went out on.
        self.sent_on = []
        self.configured = True
        self.tx_attempts = 0
        # Replies on the aux interface's vendor instance, apart from CMSIS-DAP's, and the buffers its
        # OUT endpoint was armed into.
        self.aux_index = None
        self.aux_configured = False
        self.aux_sent = []
        self.aux_arms = []
        # Aux IN as the controller sees it: a write while one is in flight would be refused.
        self.aux_in_flight = False
        self.aux_overlaps = 0
        self.accept_tx = True
        # What tud_speed_get answers.
        self.speed = TUSB_SPEED_HIGH
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
            elif at == self.cpu0.sym["tud_speed_get"]:
                result = self.speed
            elif at == self.cpu0.sym["tud_vendor_tx_cb"]:
                if uc.reg_read(UC_ARM_REG_R0) == self.aux_index:
                    self.aux_in_flight = False
                return
            elif at == self.cpu0.sym["tud_vendor_n_read_xfer_buf"]:
                self.aux_arms.append((uc.reg_read(UC_ARM_REG_R0), uc.reg_read(UC_ARM_REG_R1)))
                result = 1
            elif at == self.cpu0.sym["tud_vendor_n_write_xfer"]:
                index, address, length = (uc.reg_read(r) for r in (UC_ARM_REG_R0, UC_ARM_REG_R1, UC_ARM_REG_R2))
                self.tx_attempts += 1
                accepted = self.accept_tx and (self.aux_configured if index == self.aux_index else self.configured)
                if accepted and index == self.aux_index:
                    self.aux_overlaps += int(self.aux_in_flight)
                    self.aux_in_flight = True
                if accepted:
                    (self.aux_sent if index == self.aux_index else self.sent).append(bytes(uc.mem_read(address, length)))
                    if index != self.aux_index:
                        self.sent_on.append(("bulk", self.sent[-1]))
                result = length if accepted else 0
            elif at == self.cpu0.sym["tud_hid_n_report"]:
                address, length = uc.reg_read(UC_ARM_REG_R2), uc.reg_read(UC_ARM_REG_R3)
                self.tx_attempts += 1
                if self.accept_tx and self.configured:
                    self.sent.append(bytes(uc.mem_read(address, length)))
                    self.sent_on.append(("HID", self.sent[-1]))
                result = int(self.accept_tx and self.configured)
            else:
                return
            uc.reg_write(UC_ARM_REG_R0, result)
            uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

        self.hook = self.cpu0.uc.hook_add(UC_HOOK_CODE, endpoints)
        self.cpu0.uc.ctl_flush_tb()
        self.put("transfer_abort", self.cpu1.sym["DAP_TransferAbort"])
        # As dap_shared_init leaves it, until a configuration sets the speed's.
        self.put("bulk_packet_size", 512, 2)

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
        c.check(all(usb.get(f"{ring}_len", 2 * slots) == 0 for ring in ("response", "hid_response")),
                "stale request published a reply")
    c.check(bytes(usb.cpu1.uc.mem_read(symbols1["DAP_Data"][0], 1)) == b"\x00", "stale Connect executed")

    # Production selection must merge HID and bulk in arrival order, each reply in its own ring.
    usb.receive(b"\x02\x01", True)
    usb.receive(b"\x03")
    for seq, ring in ((0, "hid_response"), (1, "response")):
        out = usb.get(f"{ring}_tail", 1)
        c.check(usb.dispatch() == 1, "fresh command did not dispatch")
        c.check(usb.get(f"{ring}_tail", 1) == (out + 1) % slots, f"the reply did not go to the {ring} ring")
        c.check(int.from_bytes(usb.cpu0.uc.mem_read(usb.shared[0] + fields[f"{ring}_seq"] + out * 4, 4), "little") == seq,
                "HID/bulk arrival order changed")

    # A configuration change must release a pending old reply without submitting it to USB.
    usb.call0("tud_mount_cb")
    usb.call0("send_head")
    c.check(not usb.sent and all(usb.get(f"{ring}_head", 1) == usb.get(f"{ring}_tail", 1)
                                 for ring in ("response", "hid_response")),
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
            dap_setup(transfer)
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


def check_full_speed(c, elf0, elf1, symbols0, symbols1, fields):
    """A configuration at full speed: DAP_Info gives the 64-byte packet, and a batch stops at it."""
    usb = Usb(elf0, elf1, symbols0, symbols1, fields)
    usb.speed = TUSB_SPEED_FULL
    usb.call0("tud_mount_cb")
    usb.receive(b"\x00\xff")
    usb.dispatch()
    usb.call0("send_head")
    c.check(usb.sent == [b"\x00\x02\x40\x00"], f"full speed: DAP_Info packet size {[m.hex() for m in usb.sent]}")
    usb.call0("tud_vendor_tx_cb", 0, len(usb.sent[-1]) if usb.sent else 0)
    usb.receive(bytes([0x7F, 20]) + b"\x00\xff" * 20)
    usb.dispatch()
    usb.call0("send_head")
    reply = usb.sent[-1] if len(usb.sent) == 2 else b""
    c.check(reply == bytes([0x7F, 15]) + b"\x00\x02\x40\x00" * 15,
            f"full speed: a batch of 20 DAP_Info answered {reply[:8].hex()}, {len(reply)} bytes, wanted 15 in 62")
    usb.call0("tud_vendor_tx_cb", 0, len(reply))
    usb.speed = TUSB_SPEED_HIGH
    usb.call0("tud_mount_cb")
    usb.receive(b"\x00\xff")
    usb.dispatch()
    usb.call0("send_head")
    c.check(usb.sent[2:] == [b"\x00\x02\x00\x02"], f"high speed after full: DAP_Info {[m.hex() for m in usb.sent[2:]]}")
    c.note("full speed: DAP_Info packet size 64, a batch stops at 64 bytes, 512 again at high speed")


def check_rechirp(c, elf0, elf1, symbols0, symbols1, fields):
    """A bus reset that leaves the probe at full speed takes it off the bus for 300 ms and back, at
    most three times per attach; one at high speed, or any other event, does nothing
    (src/usb_speed.c)."""
    devcmdstat, bus_reset_end, sof = 0x40094000, 2, 4
    usb = Usb(elf0, elf1, symbols0, symbols1, fields)
    calls = []

    def pull_up(uc, at, size, data):
        calls.append("off" if at == usb.cpu0.sym["tud_disconnect"] else "on")
        uc.reg_write(UC_ARM_REG_R0, 1)
        uc.reg_write(UC_ARM_REG_PC, uc.reg_read(UC_ARM_REG_LR))

    for name in ("tud_disconnect", "tud_connect"):
        entry = usb.cpu0.sym[name]
        usb.cpu0.uc.hook_add(UC_HOOK_CODE, pull_up, begin=entry, end=entry)
    usb.cpu0.uc.ctl_flush_tb()

    def event(speed, kind=bus_reset_end):
        usb.cpu0.uc.mem_write(devcmdstat, u32(speed << 22))
        usb.call0("tud_event_hook_cb", 1, kind, 1)

    def task(ms):
        usb.cpu0.uc.mem_write(usb.cpu0.sym["system_ticks"], u32(ms))
        usb.call0("usb_speed_task")

    def waiting():
        """What usb_speed_waiting answers; LTO inlines it into CPU 0's idle check."""
        return bytes(usb.cpu0.uc.mem_read(usb.cpu0.sym["down"], 1)) != b"\x00"

    event(2)
    task(1000)
    event(1, sof)
    task(1001)
    c.check(calls == [], f"a high-speed reset or a SOF moved the pull-up: {calls}")
    now = 2000
    for attempt in range(1, 5):
        retry = attempt <= 3
        event(1)
        task(now)
        c.check(calls[-1:] == (["off"] if retry else ["on"]) and waiting() == retry,
                f"full-speed reset {attempt}: pull-up {calls}, waiting {waiting()}")
        # A reset the controller reports while the probe is off the bus is not the host's.
        event(1)
        task(now + 299)
        c.check(calls[-1:] == (["off"] if retry else ["on"]), f"full-speed reset {attempt}: back on before 300 ms: {calls}")
        task(now + 300)
        task(now + 301)
        wanted = ["off", "on"] * min(attempt, 3)
        c.check(calls == wanted and not waiting(),
                f"full-speed reset {attempt}: pull-up {calls}, wanted {wanted}")
        now += 1000
    c.note("re-chirp: a full-speed reset leaves the bus for 300 ms with the loop kept awake, three times after power-up;"
           " high speed, other events and resets while off the bus left alone")


def descriptor_endpoints(config):
    """(address, transfer type, wMaxPacketSize) of each endpoint in a configuration descriptor."""
    found, i = [], 0
    while i + 1 < len(config) and config[i] > 0:
        if config[i + 1] == 5:
            found.append((config[i + 2], config[i + 3] & 3, int.from_bytes(config[i + 4:i + 6], "little")))
        i += config[i]
    return found


def check_descriptors(c, elf0, elf1, symbols0, symbols1, fields):
    """Bulk endpoints are 512 bytes at high speed and 64 at full speed (USB 2.0 5.8.3), the other-speed
    configuration is the other speed's configuration under its own type, and the qualifier says
    what the device descriptor says."""
    for board in (0, 1):
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        usb.put("board", board, 1)
        usb.put("jumpers", 0, 1)

        def read(entry, *args):
            at = usb.call0(entry, *args)
            total = int.from_bytes(usb.cpu0.uc.mem_read(at + 2, 2), "little")
            return bytes(usb.cpu0.uc.mem_read(at, total))

        configs, others = {}, {}
        for speed in (TUSB_SPEED_HIGH, TUSB_SPEED_FULL):
            usb.speed = speed
            configs[speed] = read("tud_descriptor_configuration_cb", 0)
            others[speed] = read("tud_descriptor_other_speed_configuration_cb", 0)
        for speed, other, name, bulk in ((TUSB_SPEED_HIGH, TUSB_SPEED_FULL, "high", 512),
                                         (TUSB_SPEED_FULL, TUSB_SPEED_HIGH, "full", 64)):
            config = configs[speed]
            sizes = [size for _, kind, size in descriptor_endpoints(config) if kind == 2]
            c.check(config[1] == 2 and len(sizes) >= 2 and all(size == bulk for size in sizes),
                    f"board {board} at {name} speed: bulk endpoints {sizes}, wanted {bulk}")
            described = others[other]
            c.check(described[1] == 7 and described[2:] == config[2:],
                    f"board {board}: the other-speed configuration asked at the other speed is not the {name}-speed one")
        device = bytes(usb.cpu0.uc.mem_read(usb.call0("tud_descriptor_device_cb"), 18))
        qualifier = bytes(usb.cpu0.uc.mem_read(usb.call0("tud_descriptor_device_qualifier_cb"), 10))
        c.check(qualifier[:2] == b"\x0a\x06" and qualifier[2:8] == device[2:8] and qualifier[8] == device[17]
                and qualifier[9] == 0, f"board {board}: qualifier {qualifier.hex()} against device {device.hex()}")
    c.note("descriptors: bulk 512 at high speed and 64 at full on both boards; other-speed configurations and the qualifier agree")


def check_interfaces_apart(c, elf0, elf1, symbols0, symbols1, fields, slots):
    """A host that leaves one CMSIS-DAP interface with replies owed must not stop a host on the other.
    Nothing polls the abandoned IN endpoint again (Linux reads an HID one only while its node is
    open), so the reply offered there stays in flight for good, with the rest queued behind it."""
    # Past `slots`, the gone interface's next command waits for reply room that never comes, as a new
    # host's would if it sent before reading what the last one left.
    for gone_hid, owed in ((gone_hid, owed) for gone_hid in (True, False) for owed in range(1, slots + 2)):
        gone, live = ("HID", "bulk") if gone_hid else ("bulk", "HID")
        ring = "hid_response" if gone_hid else "response"
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)

        def held():
            return (usb.get(f"{ring}_tail", 1), usb.get(f"{ring}_seq", 4 * slots),
                    [transport for transport, _ in usb.sent_on].count(gone))

        for sent in range(owed):
            if sent == slots:
                untouched = held()
            usb.receive(b"\x00\xff", gone_hid)
            usb.dispatch()
            usb.call0("send_head")
        if owed <= slots:
            untouched = held()
        expected = bytes([0, 1, slots]) + (b"" if gone_hid else bytes(61))
        answered = 0
        for _ in range(2 * slots):
            before = len(usb.sent_on)
            usb.receive(b"\x00\xfe", not gone_hid)
            usb.dispatch()
            usb.call0("send_head")
            replies = [reply for transport, reply in usb.sent_on[before:] if transport == live]
            if replies != [expected]:
                break
            answered += 1
            if gone_hid:
                usb.call0("tud_vendor_tx_cb", 0, len(expected))
            else:
                usb.call0("tud_hid_report_complete_cb", 0, SCRATCH, 64)
        c.check(answered == 2 * slots, f"{owed} {gone} replies nobody reads stopped {live}: "
                                       f"{answered} of {2 * slots} commands answered")
        c.check(held() == untouched, f"{owed} {gone} replies nobody reads: their ring changed once full")
    c.note("a gone host's unread replies on either CMSIS-DAP interface leave the other answering")


def check_completion_chains(c, elf0, elf1, symbols0, symbols1, fields):
    """A reply's completion sends the next one waiting on its transport, without the loop coming
    round (src/main.c)."""
    for hid in (False, True):
        name = "HID" if hid else "bulk"
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        for _ in range(2):
            usb.receive(b"\x00\xfe", hid)
            usb.dispatch()
        usb.call0("send_head")
        c.check([transport for transport, _ in usb.sent_on] == [name], f"{name}: the first reply did not go")
        if hid:
            usb.call0("tud_hid_report_complete_cb", 0, SCRATCH, 64)
        else:
            usb.call0("tud_vendor_tx_cb", 0, 3)
        c.check([transport for transport, _ in usb.sent_on] == [name, name],
                f"{name}: the first reply's completion did not send the next")
    c.note("a bulk or HID reply's completion sends the next one waiting")


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
        ring, in_flight = ("hid_response", "hid_reply_in_flight") if hid else ("response", "bulk_reply_in_flight")
        c.check(usb.get(f"{ring}_head", 1) == usb.get(f"{ring}_tail", 1) and
                all(usb.cpu0.uc.mem_read(usb.shared[0] + fields[f"{ring}_len"] + slot * 2, 2) == b"\x00\x00"
                    for slot in range(slots)), f"unconfigured {name}: CPU 0 retained completed replies")
        c.check(usb.tx_attempts == 0, f"unconfigured {name}: CPU 0 retried a closed endpoint {usb.tx_attempts} times")
        c.check(usb.cpu0.uc.mem_read(usb.cpu0.sym[in_flight], 1) == b"\x00",
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
        abandoned = usb.get(f"{ring}_head", 1)
        usb.configured = False
        usb.call0("send_head")
        c.check(usb.cpu0.uc.mem_read(usb.cpu0.sym[in_flight], 1) == b"\x00" and
                usb.get(f"{ring}_head", 1) == (abandoned + 1) % slots and
                usb.cpu0.uc.mem_read(usb.shared[0] + fields[f"{ring}_len"] + abandoned * 2, 2) == b"\x00\x00",
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
    instance. Instance 1 is the plain board or the Pro with CMSIS-DAP; instance 0 either with its
    SWD jumper."""
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

    def board(usb, kind):
        usb.put("board", kind, 1)  # board_straps.h: 0 the plain board, 1 the Pro
        usb.put("jumpers", 0, 1)
        usb.cpu1.uc.mem_write(usb.shared[0], bytes(usb.cpu0.uc.mem_read(*usb.shared)))
        for name in ("board_debug_pins_init", "dio_init"):
            usb.cpu1.call(name, timeout_insns=1_000_000)
        usb.cpu0.uc.mem_write(usb.shared[0], bytes(usb.cpu1.uc.mem_read(*usb.shared)))
        return usb

    def pro(usb):
        return board(usb, 1)

    def plain(usb):
        return board(usb, 0)

    # The commands it carries answer as on CMSIS-DAP, size checks included.
    carried = (b"\x82", b"\x8e\x00\x00\x00", b"\x8e\x08\x08\x08", b"\x8e\x01\x01\x01", b"\x8e\x08\x00\x00", b"\x8e\x00",
               b"\x8f\x02", b"\x8f\x01", b"\x8f", b"\x90\x01", b"\x90\x03", b"\x90\x04\x00", b"\x90\x04\x02", b"\x90\x02",
               b"\x90", b"\x90\x00\x01", b"\x94\x01\x00\x00\x00\x02hi", b"\x94\x01\x00\x00\x00\x03hi", b"\x94",
               b"\x94\x01\x00\x00\x00\x20" + bytes(32))
    for usb in (fresh(), plain(fresh()), pro(fresh())):
        for payload in carried:
            c.check(ask(usb, payload) == bulk(usb, payload), f"aux reply to {payload.hex()} differs from CMSIS-DAP's")
    # Anything else gets [0xff] and does not run.
    for payload in (b"\x00\x01", b"\x02\x02", b"\x07", b"\x83\x00", b"\x85\x01", b"\x91\x00\x00\x00\x00\x00\x00\x00"):
        c.check(ask(usb, payload) == b"\xff", f"aux ran {payload.hex()}")
    c.check(bytes(usb.cpu1.uc.mem_read(connect, 1)) == b"\x00", "an aux DAP_Connect connected")

    # Aux drives the DIO pins themselves, the Pro's and J8: CMSIS-DAP sees what aux left, and the
    # other way round.
    for name, usb in (("Pro", pro(fresh())), ("plain", plain(fresh()))):
        driven = ask(usb, b"\x8e\x08\x08\x08")
        c.check(driven[:4] == b"\x8e\x00\x08\x08" and bulk(usb, b"\x8e\x00\x00\x00")[2:4] == b"\x08\x08",
                f"{name}: aux 0x8E driving DIO3 high answered {driven.hex()}, and CMSIS-DAP did not see it")
        bulk(usb, b"\x8e\x08\x00\x00")
        c.check(ask(usb, b"\x8e\x00\x00\x00")[2:4] == b"\x00\x00",
                f"{name}: aux did not see CMSIS-DAP return DIO3 to an input")

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


CTIMER4_TC = 0x4002A008


def check_aux_stream(c, elf0, elf1, symbols0, symbols1, fields, layout):
    """The stream on Verge Aux, CPU 0's side: 0x90 modes 7 and 10, which CPU 0 answers itself once CPU 1
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
        field(usb, "switch_weight_q16", 9786)
        field(usb, "rearm_between_pairs", 1, 1)
        field(usb, "previous_known", 1, 1)
        field(usb, "previous_na", 6_700_000)
        field(usb, "stream.clock_hz", 150_000_000)
        field(usb, "stream.period", 150_000)
        field(usb, "stream.next_sequence", 42)
        field(usb, "decoder", 41, 4, 12)

    def publish(usb, message):
        """A message as the capture's owner publishes one."""
        written = field(usb, "stream.written")
        at = written % 16
        usb.cpu0.uc.mem_write(symbols0["energy_stream_slots"][0] + 512 * at, message)
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
            (0, 1, 0, b"\x90\x0a\x00", b"\x90\x00\x03"), (1, 0, 0, b"\x90\x0a\x00", b"\x90\x00\x01"),
            (1, 1, 1, b"\x90\x0a\x00", b"\x90\x00\x07"), (1, 1, 0, b"\x90\x0a", b"\xff"),
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

    # A DIO stream, mode 15, where the pin interrupts are and energy capture is not: its reply, then
    # a format 5 header for the pins asked, after anything queued is dropped; mode 7 ends it with
    # reason 1 and no last pair. With energy capture, or without the interrupts, it is refused, and a
    # request without its mask is invalid.
    from dio_edges_checks import edge_statics, j8_static
    edges = edge_statics(symbols0)
    j8 = j8_static(symbols0, edges)
    for energy, present, payload, want in ((0, 1, b"\x90\x0f\x05", b"\x90\x00\x00"), (1, 1, b"\x90\x0f\x05", b"\x90\x00\x03"),
                                           (0, 0, b"\x90\x0f\x05", b"\x90\x00\x03"), (0, 1, b"\x90\x0f", b"\xff")):
        usb = fresh()
        usb.put("energy_available", energy, 1)
        usb.cpu0.uc.mem_write(edges["present"], bytes([present]))
        usb.cpu0.uc.mem_write(j8, b"\x01")
        # An earlier stream's header and end, never sent: the new stream's header must come alone.
        publish(usb, b"\xa1\x05" + bytes(8))
        publish(usb, b"\xa2\x01" + bytes(12))
        arrive(usb, payload)
        c.check(usb.dispatch() == 0, f"aux {payload.hex()} went to CPU 1")
        sent = drain(usb)
        if want != b"\x90\x00\x00":
            c.check(sent == [want], f"DIO stream {payload.hex()} with energy {energy}, present {present}: {[m.hex() for m in sent]}")
            continue
        header = b"\xa1\x05" + (150_000_000).to_bytes(4, "little") + b"\x05\x00" + (7).to_bytes(2, "little")
        c.check(sent == [want, header], f"DIO stream start: {[m.hex() for m in sent]}")
        # The end says when: 1 ms on the stamping timer after the header's read.
        usb.cpu0.uc.mem_write(0x4002A008, (150_000).to_bytes(4, "little"))
        arrive(usb, b"\x90\x07")
        sent = drain(usb)
        end = b"\xa2\x01" + bytes(4) + (1_000_000).to_bytes(8, "little")
        c.check(sent == [b"\x90\x00\x00", end], f"DIO stream end: {[m.hex() for m in sent]}")

    # A protected attach discards A's pending header and binds B's identity to the first new header.
    token = bytes(range(16)) + (2).to_bytes(8, "little")
    stale = bytes(range(16)) + (1).to_bytes(8, "little")
    usb = fresh()
    capture(usb)
    usb.cpu0.uc.mem_write(usb.energy + layout["capture_id"], token)
    publish(usb, b"\xa1\x01" + bytes(66))
    publish(usb, b"\xa0" + bytes(15))
    arrive(usb, b"\x90\x0a\x00")
    sent = drain(usb)
    c.check(len(sent) == 2 and sent[0] == b"\x90\x00\x00" and len(sent[1]) == 103
            and sent[1][:2] == b"\xa1\x04" and sent[1][72:96] == token,
            f"protected attach boundary: {[m.hex() for m in sent]}")
    # An attach's header, which CPU 0 builds, says whether the capture holds the high range (0x90
    # mode 14): the flags at 98 in format 4.
    for held in (False, True):
        attached = fresh()
        capture(attached)
        attached.cpu0.uc.mem_write(attached.energy + layout["capture_id"], token)
        field(attached, "high_range_only", int(held), 1)
        arrive(attached, b"\x90\x0a\x00")
        header = [m for m in drain(attached) if m[:1] == b"\xa1"]
        c.check(len(header) == 1 and len(header[0]) == 103 and bool(header[0][98] & 4) == held,
                f"an attach's header with the high range {'held' if held else 'free'}: {[m.hex() for m in header]}")
    for candidate, result in ((stale, 8), (bytes(24), 8), (token, 0), (token, 0)):
        before = bytes(usb.cpu0.uc.mem_read(usb.energy, symbols0["energy_integration"][1]))
        arrive(usb, b"\x90\x0d" + candidate)
        sent = drain(usb)
        c.check(sent[:1] == [bytes([0x90, 0, result])], f"conditional stream end {candidate.hex()}: {sent}")
        if result == 8:
            c.check(bytes(usb.cpu0.uc.mem_read(usb.energy, len(before))) == before,
                    "stale stream end changed capture state")
    for sequence in (1, 3):
        field(usb, "capture_id_sequence", sequence)
        arrive(usb, b"\x90\x0d" + token)
        c.check(drain(usb) == [] and usb.call0("aux_has_work") == 0, "partial identity did not wait")
        field(usb, "capture_id_sequence", sequence + 1)
        c.check(drain(usb)[:1] == [b"\x90\x00\x00"], "published identity did not resume stop")
    usb.put("energy_dma_error", 1, 1)
    usb.put("energy_control_ack", 1)
    for candidate, result in ((stale, 8), (token, 0)):
        arrive(usb, b"\x90\x0d" + candidate)
        c.check(drain(usb) == [bytes([0x90, 0, result])], "conditional stream cleanup hung after fault")

    # CPU1 may publish a nonstream replacement after quiet release. An odd sequence prevents
    # a mixed token from authorizing mode13 at any instruction in its snapshot/comparison.
    a = bytes([0x11]) * 16 + (1).to_bytes(8, "little")
    b = bytes([0x22]) * 16 + (2).to_bytes(8, "little")
    mixed = a[:12] + b[12:]
    usb = fresh()
    capture(usb)
    usb.cpu0.uc.mem_write(usb.energy + layout["capture_id"], a)
    arrive(usb, b"\x90\x0d" + mixed)
    # Every function on mode 13's path that the compiler left out of line, so the sweep covers the
    # same instructions however it inlined them.
    path = [symbols0[name] for name in ("aux_task", "stream_command", "stream_waits", "stream_owned_by_cpu1",
                                        "energy_identity_matches") if name in symbols0]

    def hook_path(uc, callback):
        return [uc.hook_add(UC_HOOK_CODE, callback, begin=start, end=start + size - 1) for start, size in path]

    trace = []
    trace_hooks = hook_path(usb.cpu0.uc, lambda uc, at, size, data: trace.append(at))
    usb.cpu0.uc.ctl_flush_tb()
    drain(usb)
    for hook in trace_hooks:
        usb.cpu0.uc.hook_del(hook)
    for cut in range(len(trace)):
        usb = fresh()
        capture(usb)
        usb.cpu0.uc.mem_write(usb.energy + layout["capture_id"], a)
        arrive(usb, b"\x90\x0d" + mixed)
        step = 0
        def publish_identity(uc, at, size, data):
            nonlocal step
            if step == cut:
                field(usb, "capture_id_sequence", 1)
                uc.mem_write(usb.energy + layout["capture_id"], mixed)
            if step == cut + 2:
                uc.mem_write(usb.energy + layout["capture_id"], b)
                field(usb, "capture_id_sequence", 2)
            step += 1
        interleave = hook_path(usb.cpu0.uc, publish_identity)
        usb.cpu0.uc.ctl_flush_tb()
        sent = drain(usb)
        for hook in interleave:
            usb.cpu0.uc.hook_del(hook)
        usb.cpu0.uc.mem_write(usb.energy + layout["capture_id"], b)
        field(usb, "capture_id_sequence", 2)
        sent += drain(usb)
        c.check(sent == [b"\x90\x00\x08"], f"partial identity at instruction {cut} accepted {sent}")

    # Mode 10 on a running capture: the reply, then the header from the next pair, then pairs.
    usb = fresh()
    capture(usb)
    arrive(usb, b"\x90\x0a\x00")
    sent = drain(usb)
    c.check(len(sent) == 2 and sent[0] == b"\x90\x00\x00" and len(sent[1]) == 103 and sent[1][:2] == b"\xa1\x04",
            f"mode 10 sent {[m[:8].hex() for m in sent]}")
    if len(sent) == 2 and len(sent[1]) == 103:
        h = sent[1]
        c.check(int.from_bytes(h[2:6], "little") == 150_000_000 and int.from_bytes(h[6:10], "little") == 150_000
                and h[10] == 2 and int.from_bytes(h[11:13], "little") == 3000
                and int.from_bytes(h[13:17], "little") == 42 and h[17] == 0, f"mode 10 header {h[:18].hex()}")
        # Step 5's fields, after the identity, with the current of the last pair integrated.
        c.check(int.from_bytes(h[96:98], "little") == 9786 and h[98] == 3 and int.from_bytes(h[99:103], "little") == 6_700_000,
                f"mode 10 header's step 5 fields {h[96:].hex()}")
    publish(usb, b"\xa0\x00" + bytes(14))
    c.check(drain(usb) == [b"\xa0\x00" + bytes(14)], "pairs after the header were not sent")
    # At full speed a message of whole packets, 48 pairs in 256 bytes, needs a zero-length packet to
    # end it. At high speed no message is a whole packet.
    for speed, zlp in ((TUSB_SPEED_FULL, [b""]), (TUSB_SPEED_HIGH, [])):
        usb = fresh()
        usb.speed = speed
        usb.call0("tud_mount_cb")
        capture(usb)
        arrive(usb, b"\x90\x0a\x00")
        drain(usb)
        whole, short = b"\xa0\x00" + bytes(254), b"\xa0\x00" + bytes(14)
        publish(usb, whole)
        publish(usb, short)
        sent = drain(usb)
        c.check(sent == [whole] + zlp + [short] and usb.aux_overlaps == 0,
                f"aux at speed {speed}: a 256-byte message then a short one went as {[len(m) for m in sent]},"
                f" {usb.aux_overlaps} started while one was in flight")
    # Mode 7: the reply, then the end, with the last pair integrated.
    arrive(usb, b"\x90\x07")
    sent = drain(usb)
    c.check(sent == [b"\x90\x00\x00", b"\xa2\x01" + (41).to_bytes(4, "little")], f"mode 7 sent {[m.hex() for m in sent]}")

    # CPU 1 holds the capture state for good when nothing runs, from boot and after a stop: modes 7 and
    # 10 answer without it. Inside a command, around a supply read, the capture runs and they wait,
    # without CPU 0 spinning; so they do while a start has not yet given its timeline.
    for mode, want in ((b"\x90\x0a\x00", b"\x90\x00\x01"), (b"\x90\x07", b"\x90\x00\x00")):
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
        arrive(usb, b"\x90\x0a\x00")
        c.check(drain(usb) == [] and usb.call0("aux_has_work") == 0,
                f"mode 10 ran while CPU 1 held the running capture's state (quiet {quiet}, pending {pending})")
        usb.put("energy_control_ack", 2)
        field(usb, "stream.timeline_pending", 0, 1)
        c.check(drain(usb)[:1] == [b"\x90\x00\x00"], "mode 10 did not run once CPU 1 let go")

    # A failure that left CPU 0 owning the state: its loop ends the stream.
    usb = fresh()
    capture(usb)
    arrive(usb, b"\x90\x0a\x00")
    drain(usb)
    usb.put("energy_dma_error", 1, 1)
    c.check(usb.call0("aux_has_work") == 1, "a failed capture's stream left no work for CPU 0's loop")
    sent = drain(usb)
    c.check(sent == [b"\xa2\x04" + (41).to_bytes(4, "little")], f"a failed capture's stream ended with {[m.hex() for m in sent]}")

    # Markers (0x94): CPU 1 records one while a stream runs, stamped by CTimer 4, and CPU 0's loop
    # sends it once it has waited 10 ms. CPU 1 reaches the capture state in CPU 0's SRAM.
    energy_size = symbols0["energy_integration"][1]

    def mark(usb, payload, stamp=1000):
        for cpu in (usb.cpu0, usb.cpu1):
            cpu.uc.mem_write(CTIMER4_TC, stamp.to_bytes(4, "little"))
        usb.cpu1.uc.mem_write(usb.energy, bytes(usb.cpu0.uc.mem_read(usb.energy, energy_size)))
        arrive(usb, payload)
        ran = usb.dispatch()
        usb.cpu0.uc.mem_write(usb.energy, bytes(usb.cpu1.uc.mem_read(usb.energy, energy_size)))
        sent = drain(usb)
        c.check(ran == 1, f"aux {payload.hex()} ran {ran} times")
        return sent

    usb = fresh()
    capture(usb)
    arrive(usb, b"\x90\x0a\x00")
    drain(usb)
    sent = mark(usb, b"\x94\x07\x00\x00\x00\x02hi")
    c.check(sent == [b"\x94\x00\x00"], f"a marker during a stream answered {[m.hex() for m in sent]}")
    usb.cpu0.uc.mem_write(CTIMER4_TC, (1000 + 1_499_999).to_bytes(4, "little"))
    c.check(drain(usb) == [] and usb.call0("aux_has_work") == 0, "a marker left before it had waited 10 ms")
    usb.cpu0.uc.mem_write(CTIMER4_TC, (1000 + 1_500_000).to_bytes(4, "little"))
    c.check(usb.call0("aux_has_work") == 1, "a marker that waited 10 ms left no work for CPU 0's loop")
    # 1,000 counts of 150 MHz from the timeline's origin: 6,666 ns, rounded down.
    want = b"\xa4\x00\x01" + (6_666).to_bytes(8, "little") + (7).to_bytes(4, "little") + b"\x02hi"
    sent = drain(usb)
    c.check(sent == [want], f"a marker that waited 10 ms was sent as {[m.hex() for m in sent]}")
    for available, running, streaming, payload, want in (
            (1, 1, False, b"\x94\x01\x00\x00\x00\x00", b"\x94\x00\x02"),
            (1, 0, False, b"\x94\x01\x00\x00\x00\x00", b"\x94\x00\x01"),
            (0, 1, False, b"\x94\x01\x00\x00\x00\x00", b"\x94\xff"),
            (1, 1, True, b"\x94\x01\x00\x00\x00\x21" + bytes(33), b"\x94\xff"),
            (1, 1, True, b"\x94\x01\x00\x00\x00\x03hi", b"\xff")):
        usb = fresh()
        capture(usb)
        if streaming:
            arrive(usb, b"\x90\x0a\x00")
            drain(usb)
        usb.put("energy_available", available, 1)
        field(usb, "running", running, 1)
        sent = mark(usb, payload)
        c.check(sent == [want], f"marker {payload[:7].hex()} with available {available}, running {running}, "
                f"streaming {streaming}: {[m.hex() for m in sent]}")

    # After a configuration, messages are a gone host's until a header this configuration asked for:
    # one it did not, from a request that ran across the reset, is dropped too. Unconfigured, all are.
    usb = fresh()
    capture(usb)
    arrive(usb, b"\x90\x0a\x00")
    c.check(len(drain(usb)) == 2, "mode 10 did not send its reply and header")
    usb.call0("tud_mount_cb")
    publish(usb, b"\xa0\x00" + bytes(14))
    publish(usb, b"\xa1" + bytes(67))
    publish(usb, b"\xa0\x01" + bytes(14))
    c.check(drain(usb) == [], "after a configuration, a header nobody asked for was sent")
    # The gone host's stream, its pairs and the end mode 10 gives it, go too; the new header passes.
    arrive(usb, b"\x90\x0a\x00")
    publish(usb, b"\xa0\x02" + bytes(14))
    sent = drain(usb)
    c.check([m[:2] for m in sent] == [b"\x90\x00", b"\xa1\x04"],
            f"after a configuration, mode 10 sent {[m[:2].hex() for m in sent]}")
    publish(usb, b"\xa0\x03" + bytes(14))
    c.check([m[:2] for m in drain(usb)] == [b"\xa0\x03"], "pairs after the new header were not sent")
    usb.aux_configured = False
    publish(usb, b"\xa0\x02" + bytes(14))
    c.check(drain(usb) == [] and field(usb, "stream.sent") == field(usb, "stream.written"),
            "messages kept while the interface was unconfigured")

    # A mode 9 request out: messages wait until its reply has gone.
    usb = fresh()
    usb.call0("tud_mount_cb")
    publish(usb, b"\xa1" + bytes(102))
    arrive(usb, b"\x90\x09" + (1000).to_bytes(4, "little") + b"\x00")
    c.check(drain(usb) == [], "a stream message went before the mode 9 reply")
    usb.dispatch()
    sent = drain(usb)
    c.check(len(sent) == 2 and sent[0][:2] == b"\x90\x00" and sent[1][0] == 0xA1,
            f"mode 9 then its header sent {[m[:4].hex() for m in sent]}")
    c.note("stream on Verge Aux: modes 10 and 7 on CPU 0 with each result, waiting for CPU 1, reply before header,"
           " end after mode 7, gone hosts' and unconfigured messages dropped, mode 9's reply first, markers from CPU 1"
           " sent after 10 ms with each refusal")
