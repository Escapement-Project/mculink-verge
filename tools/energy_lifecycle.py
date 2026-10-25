"""Longer linked-image capture sequences with an independent sample/energy oracle."""

import random

from cycles import RETURN_MAGIC
from unicorn.arm_const import UC_ARM_REG_PC


def check_sequences(c, setup, result):
    for seed in (19, 811):
        rng = random.Random(seed)
        probe, dma, adc, put, get, local, written, feed, pause, total, hook = setup()
        session_codes = []
        for turn in range(32):
            context = f"energy lifecycle seed={seed} turn={turn}"
            # Resume restarts the DMA position, while the IRQ completion counter remains global.
            if turn:
                c.check(probe.command(b"\x86\x01\x00")[1] == 0 and
                        bytes(probe.uc.mem_read(probe.sym["capture_running"], 1)) == b"\x01",
                        f"{context}: sense/resume failed")
            count = rng.randrange(1, 310)
            codes = [rng.randrange(1, 65536) for _ in range(count)]
            words = []
            for code in codes:
                words.extend((result(1, code), result(2, 1), result(3, 0), 0, 0, 0, 0, 0, 0))
            partial = rng.randrange(3)
            next_code = rng.randrange(1, 65536)
            extra = [result(1, next_code), result(2, 1), result(3, 0)]
            words.extend(extra[:partial])
            pending = rng.choice((False, True))
            processed = 0
            for segment in range(len(words) // 1024):
                feed(words[segment * 1024:(segment + 1) * 1024], segment * 1024)
                if pending and segment == len(words) // 1024 - 1:
                    continue
                probe.call("energy_monitor_process_segment")
                put("energy_segments", get("energy_segments") + 1)
                processed += 1024
            tail = len(words) // 1024 * 1024
            feed(words[tail:], tail)
            written(len(words), pending=pending and tail != 0)
            accounted = min(count, (processed + 6) // 9)
            total(session_codes + codes[:accounted])
            # Repeated observations must not consume the partial segment.
            for _ in range(rng.randrange(1, 4)):
                reply = probe.command(b"\x90\x01")
                c.check(reply[2] == 0, f"{context}: latest failed")
                total(session_codes + codes[:accounted])
            finish_fifo = partial and rng.choice((False, True))
            adc.fifo = extra[partial:] if finish_fifo else []
            expected = codes + ([next_code] if finish_fifo else [])
            pause()
            session_codes.extend(expected)
            total(session_codes)
            pause()
            total(session_codes)
            reply = probe.command(b"\x90\x03")
            c.check(reply[2] == 0 and int.from_bytes(reply[11:19], "little") == len(session_codes),
                    f"{context}: retained total reply differs")

            if turn % 7 == 6:
                # A fault freezes the old total across observations, stop and a rejected start.
                put("energy_dma_error", 1, 1)
                for payload in (b"\x90\x01", b"\x90\x03", b"\x90\x02"):
                    c.check(probe.command(payload)[2] == 7, f"{context}: sticky fault lost")
                    total(session_codes)
                c.check(probe.command(b"\x90\x00\x00\x00\x00\x00")[2] == 4,
                        f"{context}: bad-rate start accepted")
                total(session_codes)
                local("requested_rate_hz", 1024)
                c.check(probe.call("energy_capture_start", 1) == 1, f"{context}: recovery start failed")
                session_codes = []
                total(session_codes)
                written(0)
                pause()
            elif turn % 5 == 4:
                # A successful new session clears totals only after ownership release succeeds.
                c.check(probe.call("energy_capture_start", 1) == 1, f"{context}: new session failed")
                session_codes = []
                total(session_codes)
                written(0)
                pause()
            c.check(probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, f"{context}: call did not return")
        probe.uc.hook_del(hook)
    c.note("energy lifecycle sequences: two seeds, 32 capture/pause/resume rounds, pending segments, FIFO tails and fault recovery")
