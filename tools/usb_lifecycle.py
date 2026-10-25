"""Exercise serial interleavings of production USB callbacks and both cores' queue tasks."""

from collections import deque
import random

from cycles import SCRATCH
from usb_checks import Usb


def check_sequences(c, elf0, elf1, symbols0, symbols1, fields, slots):
    for seed in (31, 997):
        rng = random.Random(seed)
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        requests, replies = deque(), deque()
        generation = 0
        seq = 0xfffffff0
        usb.put("next_request_seq", seq)
        usb.put("first_live_seq", seq)

        def dispatch():
            available = bool(requests) and len(replies) < slots
            result = usb.dispatch()
            c.check(bool(result) == available, f"USB lifecycle seed={seed}: dispatch ownership")
            if available:
                number, epoch, hid, answer = requests.popleft()
                if epoch == generation:
                    replies.append((number, epoch, hid, answer))

        def send(accept):
            usb.accept_tx = accept
            before = len(usb.sent)
            usb.call0("send_head")
            if not replies:
                c.check(len(usb.sent) == before, "empty reply queue submitted data")
            elif replies[0][1] != generation:
                replies.popleft()
                c.check(len(usb.sent) == before, "stale reply reached the endpoint")
            elif accept:
                _, _, hid, expected = replies.popleft()
                c.check(usb.sent[before:] == [expected], f"USB lifecycle seed={seed}: reply bytes/order")
                # Completion may immediately retry the next head. Refuse that submission so
                # each step has one accepted transfer, including failed-submission recovery.
                usb.accept_tx = False
                if hid:
                    usb.call0("tud_hid_report_complete_cb", 0, SCRATCH, 64)
                else:
                    usb.call0("tud_vendor_tx_cb", 0, len(expected))
                if replies and replies[0][1] != generation:
                    replies.popleft()
            else:
                c.check(len(usb.sent) == before, "refused submission released data")

        for turn in range(160):
            action = rng.randrange(5)
            if action <= 1 and len(requests) + len(replies) < slots:
                hid = bool(rng.randrange(2))
                payload, answer = rng.choice(((b"\x00\xff", b"\x00\x02" + (b"\x40\x00" if hid else b"\x00\x02")),
                                              (b"\x7e", b"\xff"), (b"\x11", b"\xff"),
                                              (b"\x02\x01", b"\x02\x01"), (b"\x03", b"\x03\x00")))
                usb.receive(payload, hid)
                requests.append((seq, generation, hid, answer + bytes(64 - len(answer)) if hid else answer))
                seq = (seq + 1) & 0xffffffff
            elif action == 2:
                dispatch()
            elif action == 3:
                send(bool(rng.randrange(2)))
            else:
                generation += 1
                usb.call0("tud_mount_cb")
                c.check(usb.get("first_live_seq") == seq, "configuration generation differs from arrival oracle")
            published = sum(usb.cpu0.uc.mem_read(usb.shared[0] + fields["response_len"] + slot * 2, 2) != b"\x00\x00"
                            for slot in range(slots))
            c.check(published == len(replies), f"USB lifecycle seed={seed} turn={turn}: lost/reused reply slot")
        for _ in range(slots * 3):
            dispatch()
            send(True)
        c.check(not requests and not replies, f"USB lifecycle seed={seed}: final queues did not drain")
    c.note("USB lifecycle sequences: two seeds, 160 actions, HID/bulk, generation wrap, refused submissions and slot reuse")
