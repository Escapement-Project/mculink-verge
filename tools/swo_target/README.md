# SWO trace source for a CC13x2/CC26x2 LaunchPad

`itm_counter.c` streams an incrementing 32-bit counter through ITM port 0, out of DIO16 (JTAG TDO),
so a capture can be checked word by word with `itm_check.py`. Debug over two-pin cJTAG only: four-wire
JTAG takes DIO16 back as TDO.

Build and load, from this directory and the probe-rs branch with cJTAG (`wip-cmsisdap-cjtag`):

    make
    probe-rs download --chip CC1312R --protocol cjtag build/itm_counter.elf
    probe-rs reset --chip CC1312R --protocol cjtag

After a reset over JTAG the boot ROM halts in boot (TRM 6.6) until a debugger halts and resumes the
core; a gdb attach and detach does it:

    probe-rs gdb --chip CC1312R --protocol cjtag &
    arm-none-eabi-gdb -batch -ex 'target remote :1337' -ex detach

The image leaves the last flash sector alone, so the CCFG already on the part stays.

## Control block, `swo_ctl` at 0x20000000

| Offset | Field | |
| --- | --- | --- |
| 0x00 | `magic` | 0x304D5449 once running |
| 0x04 | `acpr` | written by the host: baud = 48 MHz / (acpr + 1); 47 (1 MHz) at boot |
| 0x08 | `gap` | written by the host: busy-wait iterations between words, 0 for back to back |
| 0x0C | `next` | the counter value of the next packet |
| 0x10 | `configs` | times the trace path has been set up |

A probe-rs detach writes DEMCR to 0, and the image sets the trace path up again when it sees that,
so each `probe-rs write` to `acpr` takes effect as the command exits:

    probe-rs write --chip CC1312R --protocol cjtag b32 0x20000004 5    # 8 MHz

## Checking a capture

    uv run tools/dap.py swo --baud 8000000 --seconds 3 --out capture.bin   # from the repo root
    uv run itm_check.py bytes capture.bin     # bytes the probe returned
    uv run itm_check.py line digital_5.bin    # a Saleae binary export of the line

`dap.py swo` needs no debug connection: the image streams on its own, and the target's baud is
whatever `acpr` says, so set both ends to the same rate.

It prints the bit time measured from back-to-back frames, framing errors (the first and last frame
of an export are usually cut), and every break in the counter with the words lost.
