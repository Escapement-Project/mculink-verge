# /// script
# requires-python = ">=3.11"
# dependencies = ["unicorn", "capstone"]
# ///
"""Reproducible stateful mutation fuzzing of the linked command dispatcher.

Run directly for a longer campaign, or use the bounded seeds included in image.py. The corpus
avoids target power, reboot and calibration commands, and takes only the Pro commands that read;
no USB device is opened.
"""

import argparse
import random

from cycles import Model, RETURN_MAGIC, SCRATCH, build, u16, u32
from unicorn.arm_const import UC_ARM_REG_PC


# The read-only Pro commands, as the plain board answers them: DIO refused, energy unavailable with
# every field zero, the SPI and I2C bridges unavailable.
PRO_READS = ((b"\x8e\x00\x00\x00", bytes.fromhex("8eff0000000000")),
             (b"\x90\x01", b"\x90\x00\x03" + bytes(18)),
             (b"\x90\x03", b"\x90\x00\x03" + bytes(16)),
             (b"\x90\x04\x00", b"\x90\x00\x03" + bytes(25)),
             (b"\x90\x04\x01", b"\x90\x00\x03" + bytes(25)),
             (b"\x91" + bytes(7), b"\x91\x02\x00\x00"),
             (b"\x92" + bytes(9), b"\x92\x02\x00\x00"))


def campaign(c, elf, symbols, *, seed, steps, capacity):
    rng = random.Random(seed)
    probe = build(elf, "swd", True, Model())
    probe.sym.update({name: address for name, (address, _) in symbols.items()})
    state, state_size = symbols["DAP_Data"]
    response = SCRATCH + 0x1000
    history = []

    def execute(payload, *, rejected=False, expected=None):
        history.append(payload.hex())
        context = f"seed={seed} capacity={capacity} step={len(history)} last={history[-8:]}"
        before = bytes(probe.uc.mem_read(state, state_size))
        request = b"\xa5" * 32 + payload + b"\x5a" * 32
        probe.uc.mem_write(SCRATCH, request)
        probe.uc.mem_write(response - 32, b"\xee" * (capacity + 64))
        packed = probe.call("DAP_ExecuteCommandSized", SCRATCH + 32, len(payload), capacity, response,
                            timeout_insns=1_000_000)
        assert probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC, f"dispatcher did not return: {context}"
        produced = packed & 0xffff
        assert produced <= capacity, f"reply length: {context}"
        assert bytes(probe.uc.mem_read(response - 32, 32)) == b"\xee" * 32, f"reply underflow: {context}"
        assert bytes(probe.uc.mem_read(response + capacity, 32)) == b"\xee" * 32, f"reply overflow: {context}"
        assert bytes(probe.uc.mem_read(SCRATCH, len(request))) == request, f"request modified: {context}"
        reply = bytes(probe.uc.mem_read(response, produced))
        if rejected:
            assert reply == b"\xff", f"malformed request accepted: {context}"
            assert bytes(probe.uc.mem_read(state, state_size)) == before, f"rejection changed state: {context}"
        if expected is not None:
            assert reply == expected, f"reply {reply.hex()} != {expected.hex()}: {context}"
        return reply

    execute(b"\x02\x01", expected=b"\x02\x01")
    execute(b"\x11" + u32(10_000_000), expected=b"\x11\x00")
    execute(b"\x04\x00" + u16(0) + u16(0), expected=b"\x04\x00")
    expected_port = 1
    for _ in range(steps):
        choice = rng.randrange(8)
        if choice == 0:
            expected_port = rng.randrange(3)
            if expected_port == 0:
                execute(b"\x03", expected=b"\x03\x00")
            else:
                execute(bytes([2, expected_port]), expected=bytes([2, expected_port]))
        elif choice == 1:
            templates = [b"\x11" + u32(rng.choice((1_000_000, 5_000_000, 10_000_000))),
                         b"\x04\x00" + u16(0) + u16(0), b"\x13\x00", b"\x15\x02\x04\x05",
                         b"\x05\x00\x01\x00" + u32(rng.getrandbits(32)),
                         b"\x06\x00\x01\x00\x00" + u32(rng.getrandbits(32)),
                         b"\x1d\x01\x08\xaa", b"\x14\x01\x08\xaa"]
            payload = rng.choice(templates)
            execute(payload[:rng.randrange(1, len(payload))], rejected=True)
        elif choice == 2:
            # Count mutations deliberately require more input than is present.
            execute(rng.choice((b"\x05\x00\xff\x00", b"\x06\x00\xff\xff\x00",
                                b"\x1d\xff\x00", b"\x14\xff\x00", b"\x15\xff\x04")), rejected=True)
        elif choice == 3:
            prefix_count = rng.randrange(1, 6)
            prefix = b"\x02\x01" * prefix_count
            # The rejected clock and the valid Connect behind it must not execute.
            execute(bytes([0x7f, prefix_count + 2]) + prefix + b"\x11\x02\x02",
                    expected=bytes([0x7f, prefix_count]) + prefix)
            expected_port = 1
        elif choice == 4:
            execute(bytes([0x7f, 2, 2, 1, 3]), expected=b"\x7f\x02\x02\x01\x03\x00")
            expected_port = 0
        elif choice == 5:
            execute(rng.choice((b"\x15\x01\x04", b"\x05\x00\x01\x02",
                                b"\x06\x00\x02\x00\x02", b"\x13\x00")))
        elif choice == 6:
            # Alone, and in a batch whose next command has to be read from the right place.
            payload, reply = rng.choice(PRO_READS)
            if rng.random() < 0.5:
                execute(payload, expected=reply)
            else:
                execute(bytes([0x7f, 2]) + payload + b"\x02\x01", expected=bytes([0x7f, 2]) + reply + b"\x02\x01")
                expected_port = 1
        else:
            execute(bytes([rng.choice((0x20, 0x40, 0x7e, 0xa0, 0xfe))]) + rng.randbytes(rng.randrange(16)),
                    expected=b"\xff")
        assert bytes(probe.uc.mem_read(state, 1)) == bytes([expected_port]), f"port changed: seed={seed} history={history[-8:]}"
        execute(b"\x00\xff", expected=b"\x00\x02" + u16(capacity))
    c.note(f"dispatcher mutation sequence: seed={seed}, {steps} steps, {capacity}-byte transport, recovery after every step")


VENDOR_MODES = (0, 1, 2, 3, 4, 5, 6, 7, 8, 0x10, 0x80, 0xE0, 0xFF)


def check_vendor_plans(c, elf, symbols, boards):
    """Every vendor id from 0x80 to 0x9F, with each mode byte above: the reply fits the size
    checker's plan (src/dap_packet.h), and the handler reads as much of the request as the plan
    says. A handler the checker lacks is planned as one byte each way, so it writes its reply past
    a full transport, and in a batch the next command is read from the wrong place.

    The plan's reply size is the smallest capacity the dispatcher accepts. `boards` maps a name to
    a setup that takes the machine."""
    length = 64
    requests = 0
    response = SCRATCH + 0x1000

    def run(probe, entry, request, capacity):
        probe.uc.mem_write(SCRATCH, request)
        probe.uc.mem_write(response - 32, b"\xee" * (capacity + 64))
        args = (SCRATCH, length, capacity, response) if entry == "DAP_ExecuteCommandSized" else (SCRATCH, response)
        packed = probe.call(entry, *args)
        returned = probe.uc.reg_read(UC_ARM_REG_PC) == RETURN_MAGIC
        past = len(bytes(probe.uc.mem_read(response + capacity, 32)).rstrip(b"\xee"))
        return returned, packed >> 16, packed & 0xffff, past

    for name, setup in boards.items():
        probe = build(elf, "swd", True, Model())
        probe.sym.update({symbol: address for symbol, (address, _) in symbols.items()})
        setup(probe)
        if "lab_sram" in probe.sym:
            # The code lab's LAB_RUN calls whatever the host left in the arena, at offset 0 here:
            # a `bx lr`, so it returns (src/code_lab.c).
            probe.uc.mem_write(probe.sym["lab_sram"], b"\x70\x47")
        for command in range(0x80, 0xA0):
            for mode in VENDOR_MODES:
                request = bytes([command, mode]) + bytes(length - 2)
                label = f"{name} board, vendor {command:#04x} mode {mode:#04x}"
                requests += 1
                low, high = 1, 512
                if run(probe, "DAP_ExecuteCommandSized", request, high)[1] == length:
                    continue  # the plan refuses this request at any capacity
                while low < high:
                    middle = (low + high) // 2
                    if run(probe, "DAP_ExecuteCommandSized", request, middle)[1] != length:
                        high = middle
                    else:
                        low = middle + 1
                returned, planned_in, produced, past = run(probe, "DAP_ExecuteCommandSized", request, low)
                if not c.check(returned, f"{label}: did not return"):
                    continue
                c.check(produced <= low and not past,
                        f"{label}: a {produced}-byte reply, {past} bytes written past the plan's {low}")
                returned, handled_in, produced, past = run(probe, "DAP_ProcessCommand", request, low)
                c.check(returned and handled_in == planned_in,
                        f"{label}: the handler took {handled_in} request bytes, the plan {planned_in}")
    c.note(f"vendor ids 0x80-0x9F: {requests} requests on {len(boards)} boards; each reply within the size"
           f" checker's plan, each handler's request length the plan's")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("elf")
    parser.add_argument("--seed", type=int, action="append", help="repeat for independent reproducible campaigns")
    parser.add_argument("--steps", type=int, default=2000)
    args = parser.parse_args()
    from image import Checker, symbols_with_size
    checker = Checker()
    symbols = symbols_with_size(args.elf)
    for seed in args.seed or (23,):
        for capacity in (64, 512):
            campaign(checker, args.elf, symbols, seed=seed, steps=args.steps, capacity=capacity)
    print("\n".join(checker.notes))


if __name__ == "__main__":
    main()
