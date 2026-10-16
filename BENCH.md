# Bench guide

Everything in this repository is compile-time truth or emulation. Nothing has run on hardware.
This document is the reference to keep open during the first bench session, and the plan for what
that session should do.

Two halves: **Reference** is what you will need to look up quickly. **The plan** is the ordered
list of what to test and what each result unlocks.

---

# Reference

## Hazards, before anything else

- **`DAP_TransferAbort` answers nothing.** Reading for a reply after sending it stalls the probe's
  bulk endpoint, and every later command times out. `usb.core.Device.reset()` clears it without
  unplugging and does not disturb a halted target.
- **Analyser captures are manual.** Start and stop them yourself.
- **Run `ninja -C build check` before flashing.** It refuses a build that clocks any wire faster
  than the host asked for, beyond the two cases already recorded in
  `tools/baseline/known-overshoot.txt`. Two of the four build configurations fail it, so the one
  you flash matters.

## Resolve this before plugging anything in

**How does an MCU-Link enter ISP mode?** A recovery script flashes stock firmware to
any MCU-Link already in ISP mode, but nothing in this repository records how to get it there. That
script is the only recovery path from a bad image, so confirm it works end to end on a spare unit
before writing our firmware to anything you care about.

## Build and flash

```
cmake -B build -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc .
ninja -C build                  # build/mculink_cjtag.{elf,hex,bin}
ninja -C build check            # the gate: wire, descriptors, reply bounds
```

The shipping configuration is the default, `-O3` with link-time optimisation, 229,168 bytes of
text, 36% of the part's flash. Two thirds of that is flash deliberately traded for speed on the
two transfer commands. `-DLTO=OFF` and `-DCMAKE_BUILD_TYPE=Debug` exist for bisecting a miscompilation, and both
change which side of the two-pin pacing line the build lands on.

Stock firmware for recovery: LinkServer's `MCU-LINK_CMSIS-DAP_V3_172.s19`, flashed by
the recovery script.

## Telling our image from stock, from the host

The cleanest discriminator is a vendor command. CMSIS-DAP leaves `0x80`–`0x9F` to the probe, stock
answers only `0x80` and `0x81`, so anything above is free.

| command | meaning |
| --- | --- |
| `0x82` | identify. Magic, format version, capability flags, build string. Stock answers `ID_DAP_Invalid`, which is the discriminator. |
| `0x83 0x01` | turn OScan1 framing on |
| `0x83 0x00` | turn it off |

`0x82` is read-only, so a host may send it blindly to any probe.

Everything the probe says about itself:

| | value | where |
| --- | --- | --- |
| USB VID:PID | `0x1FC9:0x0143` | `src/usb_descriptors.c:20` — the same pair stock uses |
| bcdDevice | `0x0100` | `src/usb_descriptors.c:34`, never changed |
| manufacturer, `DAP_Info` 0x01 | `NXP Semiconductors` | `src/DAP_config.h:166` |
| product, `DAP_Info` 0x02 | `MCU-Link CMSIS-DAP cJTAG` | `src/DAP_config.h:167`. Stock says `MCU-Link CMSIS-DAP V3.172` |
| serial, `DAP_Info` 0x03 | `CJ` + 8 hex of CRC32 over the unique id, or `CJNOUUID` | `src/ser_num.c:24` |
| `DAP_Info` 0x04 | `2.1.2` | ARM's CMSIS-DAP protocol version from the pinned submodule, not ours |
| `DAP_Info` 0x05/0x06 | `NXP` / `Cortex-M` | `src/DAP_config.h:151` |
| `DAP_Info` 0x07/0x08 | `NXP` / `NXP board` | `src/DAP_config.h:153` |
| `DAP_Info` 0x09 | `git describe` of the build | ours, `src/DAP_config.h:276` |
| vendor `0x82` | `CJTAG`, format 2, capability flags, build string | `src/dap_vendor.c` |

`DAP_Info` also reports 512-byte packets, eight of them, and the capabilities SWD, JTAG, SWO UART,
atomic commands and timestamps.

**To tell two builds apart, read the build string.** `DAP_Info` 0x09 answers it to any
CMSIS-DAP host, and vendor `0x82` answers it along with the capability flags. It is `git describe
--always --dirty --tags`, regenerated on every build, so a `-dirty` suffix means the image came
from uncommitted work and nothing in the repository identifies it. That matters here: two of the
four build configurations fail the pacing gate, so knowing which one is on a unit is the
difference between a measurement and a guess.

The vendor `0x82` reply, after the echoed id:

```
[0..4]  "CJTAG"                magic
[5]     format version         2
[6]     capability flags       bit 0 OScan1 supported, bit 1 OScan1 active now
[7]     build string length n
[8..]   build string, n bytes, not terminated
```

Check the magic to know it is us, the flags to know what may be used, the string to know which
image. A flag is how a later feature arrives without every host learning a new version.

## Pin map

| signal | pin | note |
| --- | --- | --- |
| TCK/SWCLK | P0_0 | |
| TDI | P0_1 | |
| TMS/SWDIO | P0_2 | the two-pin wire |
| TDO/SWO | P0_3 | also `FC3_RXD` for SWO, so only one configuration is live at a time |
| nRESET | P0_19 | |
| RESET_TXEN | P0_13 | buffer direction |
| TMS_SWDIO_TXEN | P0_28 | buffer direction, **load-bearing for OScan1** |
| target console RXD / TXD | P0_24 / P0_25 | Flexcomm 0 |
| LED1 | P0_5 | |

Releasing SWDIO means clearing `TMS_SWDIO_TXEN` as well as making the pin an input. The buffer
keeps driving the wire otherwise, and releasing the wire is the whole of OScan1.

## Failure modes, and what each looks like

| symptom | first suspect |
| --- | --- |
| device does not enumerate at all | the unique-id read at `0x0009_FC70` in `src/ser_num.c`, which is untested on hardware. If that region faults it faults inside `GetSerialNum`, before USB comes up |
| enumerates, console silent | `FRO_HF` or its divider not running. `board_clocks_init` starts both; a Flexcomm with no clock reports no error and never reaches a baud rate |
| enumerates, SWO silent | same clock, and NXP's driver also waits on a status bit |
| probe stops answering for about ninety seconds | `DAP_Delay(0)`, ARM's pacing loop counting down from 2³². Guarded in `src/dap_dispatch.c`, so seeing this means the guard is not in the image you flashed |
| every command times out after one | a stalled bulk endpoint from `DAP_TransferAbort`. `usb.core.Device.reset()` |
| attaches but the target behaves oddly at a high clock | the probe may be clocking faster than asked. See `PACING.md` |

## Tools and where they live

| | |
| --- | --- |
| `uv run tools/cycles.py` | cycles per bit by emulation. `--table` prints the rate for every clock a host can ask for; `--calibrate` solves the cost model from a measured period |
| `uv run tools/window.py` | disassembles the gap that repeats once a word |
| `uv run tools/image.py` | what the image says about itself: descriptors, clocks, commands, reply bounds, serial |
| `uv run tools/equiv.py <ref> <new>` | 1,212 differential cases against a reference build |

Host side: probe-rs on branch `wip-cmsisdap-cjtag`, currently at `815fb048c`. The cJTAG host
control is there; the two-pin data read is not finished.

## What the emulator predicts, so you can check it

KiB/s of wire time over a 64-word block. Regenerate with `uv run tools/cycles.py --table --words 64`.

| asked | SWD read | SWD write | JTAG read | JTAG write | two-pin read | two-pin write |
| --- | --- | --- | --- | --- | --- | --- |
| 5 MHz | 326 | 338 | 361 | 380 | 117 | 119 |
| 10 MHz | 635 | 682 | 684 | 757 | 217 | 223 |
| 16 MHz | 930 | 1033 | 975 | 1131 | 304 | 315 |
| 40 MHz | 1776 | 2152 | 1800 | 2463 | 648 | 639 |

Single-register transfers are a different command and move independently:
`uv run tools/cycles.py --transfer --words 16` gives 569/613 at 10 MHz and 1439/1705 at 50.

Stock measured 476 read and 502 write at its best setting. Wire time is not throughput: the stock
capture put the USB gap at about 10% of a transfer at 14 MHz, so treat the table as a ceiling.

Requests between those rows land on the row below them. `PACING.md` explains why.

---

# The plan

Ordered so that each step depends only on the ones above it. The fourth is the one that matters
most, and it is cheap, so do not let the session end without it.

## 1. Flash and enumerate

Flash `build/mculink_cjtag.bin`, having run the gate first. Check the device appears, and check
the serial string.

Proves the image boots, the clocks come up, and the unique-id read does not fault. **Everything
below depends on this**, and the serial read is the single most likely thing to stop the device
appearing at all.

If it does not enumerate, the fastest bisection is to flash a build with `ser_num.c` returning a
constant.

## 2. Identity

Send vendor command `0x82` and read back `"CJTAG"`. Read `DAP_Info` for the packet size, the
capabilities and the version strings.

Proves you are talking to our image rather than stock, which matters because every later
measurement is a comparison against stock and the two are easy to confuse. Confirms the
descriptors that `tools/image.py` has only ever checked against themselves.

## 3. SWD attach

`probe-rs info` and then an attach against a known-good target. The halted MSPM0L1306 is the
safest, given it will not survive a reset.

Proves the SWD bit path, the acknowledge handling, and the whole command layer on real silicon.
Unlocks every measurement below.

## 4. The clock calibration capture

**The highest-value measurement in this list.** The emulator's cost model has four constants —
one cycle for a store, one for a GPIO store, two for a load, two for a taken branch — and they are
command-line arguments that nobody has checked against silicon.

Measure, rather than calibrate in one step. Drive `DAP_SWJ_Sequence` (command `0x12`), which is a
bare clocked bit with no transfer around it: no packet, no acknowledge, no turnaround, no
bookkeeping. It is the only thing on the wire whose cycles-per-bit is the quantity ARM's clock
arithmetic is actually about. Put the analyser on TCK and record the period at three requested
clocks:

| request | what it isolates |
| --- | --- |
| 50 MHz | the delay-free path. Period here is the fixed per-bit cost, which gives the store cost |
| 10 MHz | one delay iteration per half-bit |
| 5 MHz | two. The difference from 10 MHz is twice `DELAY_SLOW_CYCLES`, which gives the branch cost |

Then put the measured costs back into the model and re-run the predictions:

```
uv run tools/cycles.py --gpio-store-cycles N --branch-cycles M --wire swd --clock 10 50
uv run tools/cycles.py --calibrate --gpio-store-cycles N --branch-cycles M
```

`--calibrate` fits a line through emulated points and recovers what the image implies for
`IO_PORT_WRITE_CYCLES` and `DELAY_SLOW_CYCLES`. It does not read the analyser. Its value is as the
comparison once the constants are right: if it still disagrees with what `src/DAP_config.h`
declares, ARM's arithmetic is wrong for this part and every requested clock is off by the same
proportion.

What it settles, all at once:

- **Whether the JTAG overshoot and the two-pin margin are real.** Both come from arithmetic over a
  cost model nobody has checked. If a GPIO store costs two cycles on this part rather than one,
  neither exists and nothing changes.
- **Whether every number in `OPTIMISATIONS.md` is trustworthy.** The whole file is emulation. One
  disagreement here and the rankings need re-deriving.
- **The NOP-specialisation work**, which is the largest throughput item left — see below.

The one data point that exists agrees with the model: a 1 MHz request measured 0.94 MHz on stock,
and the emulator says 0.94. That is the slowest setting, where the delay loop dominates and the
store cost barely shows, so it does not constrain the thing in question.

### What this unlocks

**Compile-time delay specialisation, worth up to about 2x in the 13–29 MHz band.** The achievable
wire period goes up in steps of six cycles, because the delay is a counted loop costing three
cycles per iteration and there are two delays per bit. Measured periods are 5, 11, 17, 23 cycles
and nothing between, so a target good for 20 MHz runs at 13.1.

The fix is to compile the fast bit path five or six times with 0, 1, 2 … `nop`s baked into
`PIN_DELAY`, and have `Set_Clock_Delay` pick the copy, exactly as `fast_clock` already picks
between two copies today. That fills in the gap, costs a `nop` rather than a branch per bit, and
the dispatch is once per transfer and already paid. It costs a couple of KB of flash per copy,
which is affordable.

Choosing the right copy for a requested clock needs the cost model to be right, which is why this
waits on the capture. `PACING.md` has the full trace through the source.

## 5. Throughput against stock

Run `swd-throughput` against our image and against stock, at several requested clocks,
and compare with the table above. Count packets rather than wall time where the instrument allows.

Proves or breaks the central claim of the project. Also measures the USB gap, which the emulator
does not model at all and which decides how much of the wire-time advantage survives.

Sweep the requested clock across the steps — 8, 10, 14, 16, 20, 30, 40, 50 MHz — rather than
picking one. The steps are the prediction most worth checking, because they are what the
specialisation work above is aimed at.

## 6. The console

Open the CDC port and talk to a target's UART at a few baud rates, including 921600 and above.

Proves `vcom_init` took Flexcomm 0 back from the board support, and that the fractional divider
reset actually fixed the 2.4% error at a megabaud. Unlocks the integrated UART
from probe-rs.

## 7. SWO

Ask for SWO at 8 MHz and below, and capture.

The emulator cannot reach this path at all: with a real clock NXP's driver waits on a status bit
no register model sets, so the four SWO commands only answer in emulation because the clock is
off. Nothing currently says SWO works. One thing is settled in advance — `SWO_UART_MAX_BAUDRATE`
is 8 MHz rather than ARM's 9, because 9 is 3.03% away from anything a 96 MHz Flexcomm can produce
and the driver refuses it.

## 8. cJTAG activation on a CC13xx, with the analyser

Send `0x83 0x01`, drive the activation, and capture. Decode with
`find_oscan.py` and compare against the working J-Link reference capture
`cc13xx-oscan1-working-2026-10-15.sal`.

Proves the claim the whole firmware exists for: that the wire never moves while the clock rests
high, which on a 1149.7 wire is an escape edge and is exactly what the stock firmware gets wrong.
The gate already refuses a build that emits one in emulation, so a disagreement here means the
emulator's pin model is wrong rather than the code.

Check the activation is 17 clocks and matches the J-Link's edge for edge.

## 9. The two-pin data read

Finish the two-pin read against a real CC13xx: scan the IDCODE over OScan1 and read a memory word.

The point of the project. Unlocks the probe-rs host side, and after that the question of whether
this goes upstream.

## 10. Parity questions

Whether stock answers DAP over HID needs a stock unit and a v1 host. It decides whether
our HID interface is parity or surplus.

## Two things only the bench can size

- **Running the bit paths from SRAM.** Unrolling put 10.7 KB of straight-line code in flash behind
  a 144 MHz wait-state setting. The emulator has no flash model, so this is the one item in
  `OPTIMISATIONS.md` it cannot size at all. Cheap to try, cannot make the wire slower, needs a
  linker script of our own rather than the board support's.
- **A DAP packet above 512 bytes.** At most 10% and all of it USB round trips, which the emulator
  does not model. It deviates from stock, so check pyOCD and OpenOCD before changing what
  `DAP_Info` reports.
