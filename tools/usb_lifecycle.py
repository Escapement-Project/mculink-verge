"""Exercise serial interleavings of production USB callbacks and both cores' queue tasks."""

from collections import deque
import random

from cycles import SCRATCH
from usb_checks import Usb


def check_sequences(c, elf0, elf1, symbols0, symbols1, fields, slots):
    for seed in (31, 997):
        rng = random.Random(seed)
        usb = Usb(elf0, elf1, symbols0, symbols1, fields)
        # Requests in arrival order, and each transport's replies in the order CPU 1 ran them.
        requests, replies = deque(), {False: deque(), True: deque()}
        generation = 0
        seq = 0xfffffff0
        usb.put("next_request_seq", seq)
        usb.put("first_live_seq", seq)

        def dispatch():
            # The oldest command whose transport has a free reply slot; each transport in order.
            chosen, seen = None, set()
            for request in requests:
                if request[2] not in seen:
                    seen.add(request[2])
                    if len(replies[request[2]]) < slots:
                        chosen = request
                        break
            result = usb.dispatch()
            c.check(bool(result) == (chosen is not None), f"USB lifecycle seed={seed}: dispatch ownership")
            if chosen is not None:
                requests.remove(chosen)
                if chosen[1] == generation:
                    replies[chosen[2]].append(chosen)

        def send(accept):
            usb.accept_tx = accept
            before = len(usb.sent_on)
            usb.call0("send_head")
            expected = []
            for hid in (False, True):
                queue = replies[hid]
                if queue and queue[0][1] != generation:
                    queue.popleft()  # released unsent
                elif queue and accept:
                    expected.append(("HID" if hid else "bulk", queue[0][3]))
            c.check(usb.sent_on[before:] == expected, f"USB lifecycle seed={seed}: reply bytes/order")
            # Completion may immediately retry the next head. Refuse that submission so each step
            # has one accepted transfer per transport, including failed-submission recovery.
            usb.accept_tx = False
            for transport, reply in expected:
                hid = transport == "HID"
                replies[hid].popleft()
                if hid:
                    usb.call0("tud_hid_report_complete_cb", 0, SCRATCH, 64)
                else:
                    usb.call0("tud_vendor_tx_cb", 0, len(reply))
                if replies[hid] and replies[hid][0][1] != generation:
                    replies[hid].popleft()

        for turn in range(160):
            action = rng.randrange(5)
            hid = bool(rng.randrange(2))
            # Bounded by the request ring alone, so a reply ring can fill with commands waiting behind it.
            if action <= 1 and sum(request[2] == hid for request in requests) < slots:
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
            else:  # also when the chosen transport is full: a host resets with work outstanding
                generation += 1
                usb.call0("tud_mount_cb")
                c.check(usb.get("first_live_seq") == seq, "configuration generation differs from arrival oracle")
            published = sum(usb.cpu0.uc.mem_read(usb.shared[0] + fields[ring] + slot * 2, 2) != b"\x00\x00"
                            for ring in ("response_len", "hid_response_len") for slot in range(slots))
            c.check(published == len(replies[False]) + len(replies[True]),
                    f"USB lifecycle seed={seed} turn={turn}: lost/reused reply slot")
        for _ in range(slots * 6):
            dispatch()
            send(True)
        c.check(not requests and not replies[False] and not replies[True],
                f"USB lifecycle seed={seed}: final queues did not drain")
    c.note("USB lifecycle sequences: two seeds, 160 actions, HID and bulk each with full request and reply rings, "
           "generation wrap, refused submissions and slot reuse")
