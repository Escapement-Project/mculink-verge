# SWO trace source for a CC13x2/CC26x2 LaunchPad

`itm_counter.c` streams an incrementing 32-bit counter through ITM port 0 on DIO16 (JTAG TDO).
`itm_check.py` verifies each captured word. Debug the target over cJTAG: JTAG needs DIO16 for
TDO.

Build and load, from this directory, with probe-rs built from the cJTAG branch of
https://github.com/CordlessCoder/probe-rs (`wip-cmsisdap-cjtag`):

    make
    probe-rs download --chip CC1312R --protocol cjtag build/itm_counter.elf
    probe-rs reset --chip CC1312R --protocol cjtag

After a reset over JTAG the boot ROM halts in boot (TRM 6.6) until a debugger halts and resumes the
core. A gdb attach and detach does that:

    probe-rs gdb --chip CC1312R --protocol cjtag &
    arm-none-eabi-gdb -batch -ex 'target remote :1337' -ex detach

The image preserves the last flash sector and its existing CCFG.

## Control block, `swo_ctl` at 0x20000000

| Offset | Field | |
| --- | --- | --- |
| 0x00 | `magic` | 0x304D5449 once running |
| 0x04 | `acpr` | written by the host: baud = 48 MHz / (acpr + 1); 47 (1 MHz) at boot |
| 0x08 | `gap` | written by the host: busy-wait iterations between words, 0 for back to back |
| 0x0C | `next` | the counter value of the next packet |
| 0x10 | `configs` | times the trace path has been set up |

A probe-rs detach clears DEMCR. The image notices and sets the trace path up again, so a write to
`acpr` takes effect when `probe-rs write` detaches:

    probe-rs write --chip CC1312R --protocol cjtag b32 0x20000004 5    # 8 MHz

## Checking a capture

    uv run tools/dap.py swo --baud 8000000 --seconds 3 --out capture.bin   # from the repo root
    uv run itm_check.py bytes capture.bin     # bytes the probe returned
    uv run itm_check.py line digital_5.bin    # a Saleae binary export of the line

`dap.py swo` needs no debug connection: the image streams on its own. Set `--baud` to match the
target's `acpr` value.

The checker reports the bit time from back-to-back frames, the framing errors, and each counter gap
with the number of words missing. The first and last frames of an export are usually incomplete.
