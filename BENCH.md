# Bench guide

The firmware runs on hardware and beats the stock image. This document is the reference to keep
open at the bench, and the plan for what is still untested.

Three parts: **Reference** is what you will need to look up quickly, starting with where the bench
stands today. **The plan** is the ordered list of what to test and what each result unlocks, with
steps 1 to 5 now carrying what they measured. The sections after it record what the bench settled
and what it is still guessing.

The emulator's cost model has been calibrated against the wire, so it can be trusted for ranking
again -- within the 3.6% it is still out over a whole transfer. Nothing in `OPTIMISATIONS.md` has
been re-derived against it yet.

---

# Reference

## Where the bench stands, end of 2026-10-16

Both halves of the plan's first five steps are done and the firmware beats stock. What is flashed
on `CJ6B19D4E7` is build `bb44d28`: the bit paths in SRAM and the core at 150 MHz. **Every commit
since is docs and tools only, so the code on the unit is HEAD's code** -- rebuilding would change
only the build string, and nothing needs reflashing to carry on measuring.

| | at 10 MHz asked |
| --- | --- |
| stock | 22.0 cycles a data bit, 494 KiB/s |
| ours, before | 22/20/34/54 cycles, 269 KiB/s |
| ours, now | 15.0 cycles uniform, ~562 KiB/s |


**Steps 6 to 10 are untouched**: the console, SWO, cJTAG activation on a CC13xx, the two-pin data
read, and the HID parity question. The open optimisation work includes moving
the two-pin block paths to SRAM the way the SWD ones went.


## Hazards, before anything else

- **`DAP_TransferAbort` answers nothing.** Reading for a reply after sending it stalls the probe's
  bulk endpoint, and every later command times out. `usb.core.Device.reset()` clears it without
  unplugging and does not disturb a halted target.
- **The analyser can be driven from a script.** Captures start, stop and export
  without a pair of hands. Export the raw digital CSV and
  read it with `tools/clockscope.py`.
- **Run `ninja -C build check` before flashing.** It refuses a build that clocks any wire faster
  than the host asked for, beyond the two cases already recorded in
  `tools/baseline/known-overshoot.txt`. Two of the four build configurations fail it, so the one
  you flash matters.

## Flashing, ISP mode and getting back to stock

A unit enters ISP mode with the **ISP jumper fitted** before it is plugged in. The ROM bootloader
samples the ISP pins at reset, so the jumper decides what comes up: bootloader with it, firmware
without it.

| state | VID:PID |
| --- | --- |
| ISP mode, bootloader | `1fc9:0021` |
| ISP mode, MCU-Link mini | `1fc9:0022` |
| running as a probe | `1fc9:0143` |

A recovery script watches for anything arriving as `1fc9:0021` and flashes it with
LinkServer's stock image, using `rblhost` from `/opt/linkserver/MCU-LINK/bin/` and the newest
`probe_firmware/MCU-LINK_CMSIS-DAP_V*.s19`. Read its docstring before using it: it deliberately
does **not** reset a flashed unit, because with the jumper still fitted a reset would return it to
the bootloader and flash it again. The unit stays in the bootloader until unplugged. `rblhost`
addresses a probe by VID:PID alone, so **only one unit may be in ISP mode at a time**.

That script is also the recovery path from a bad image of ours, so the sequence is: fit the jumper,
plug in, flash, unplug, remove the jumper, plug in.

`uv run tools/flash.py` writes our image the same way and verifies it; `--check` says what every
attached unit is running without touching any of them. **Each reflash costs a pair of hands**,
which is the one step in a measure-change-measure loop that cannot be scripted. A vendor command
that re-enters the ROM bootloader would remove it, and the jumper stays the
recovery route for an image that will not enumerate.

**Reading flash back to verify stops at the last fully programmed page.** Erased flash on this
part has no valid ECC, so a read that runs past the image fails with `FLASH Driver: ECC Error` and
drops the bootloader off the bus. Round the length down to a multiple of 512.

**`P0_5` is both LED1 and ISP1.** A unit that keeps coming up as `1fc9:0021` and never runs our
firmware has the jumper still on. That is the first thing to check when a freshly flashed probe
does not appear, and it is more likely than anything in the failure table below.

## Build and flash

```
cmake -B build -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc .
ninja -C build                  # build/mculink_cjtag.{elf,hex,bin}
ninja -C build check            # the gate: wire, descriptors, reply bounds
```

The shipping configuration is the default, `-O3` with link-time optimisation, 229,124 bytes of
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
| `0x82` | identify. Magic, format version, capability flags, build string. Stock answers a bare `82` with no payload, which is the discriminator. |
| `0x83 0x01` | turn OScan1 framing on |
| `0x83 0x00` | turn it off |

`0x82` is read-only, so a host may send it blindly to any probe.

Everything the probe says about itself. In a listing it reads
`Escapement Project MCU-Link Verge CMSIS-DAP (CJF2551AD5)`:

| | value | where |
| --- | --- | --- |
| USB VID:PID | `0x1FC9:0x0143` | `src/usb_descriptors.c:20` — the same pair stock uses |
| bcdDevice | `0x0100` | `src/usb_descriptors.c:34`, never changed |
| manufacturer, `DAP_Info` 0x01 | `Escapement Project` | `src/DAP_config.h`. Stock says `NXP Semiconductors` |
| product, `DAP_Info` 0x02 | `MCU-Link Verge CMSIS-DAP` | same. Stock says `MCU-Link CMSIS-DAP V3.172` |
| serial, `DAP_Info` 0x03 | `CJ` + 8 hex of CRC32 over the unique id, or `CJNOUUID` | `src/ser_num.c:24` |
| `DAP_Info` 0x04 | `2.1.2` | ARM's CMSIS-DAP protocol version from the pinned submodule, not ours |
| `DAP_Info` 0x05..0x08 | nothing | `TARGET_FIXED` is 0, so the four target strings are compiled out. They are for a probe soldered to a known target; this one has a connector |
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

## The two units on the bench

Each has its own MSPM0L1306, and each is wired to its own analyser channel group. The mapping was
established by driving a clock burst on one unit at a time and seeing which channels moved.

| serial | running | analyser | nRESET | SWDIO | SWCLK | its target |
| --- | --- | --- | --- | --- | --- | --- |
| `CJ6B19D4E7` (was `07HW51KCTNRZQ`) | ours | D0-D2 | D0 | D1 | D2 | alive |
| `QK7RM2VTX8D41` | stock | D4-D6 | D4 | D5 | D6 | recovered, left halted |

**A unit's serial changes when it is flashed with ours**, from NXP's 13-character string to `CJ`
plus 8 hex of a CRC over the unique id. `uv run tools/flash.py --check` says what every attached
unit is running without touching any of them.

A stock unit calls itself `MCU-LINK (r0FB) CMSIS-DAP V3.172` and reports firmware `3.172`,
protocol `2.1.1`, 512-byte packets, eight of them, and a 150 MHz test domain timer. The serial is
NXP's 13-character string; ours replaces it with `CJ` plus 8 hex, so **the serial itself says which
firmware a unit is running** and `tools/dap.py list` is enough to tell them apart.

Stock's own vendor commands: `0x80` answers `00 01 0f` after the echoed id
and `0x81` answers `00 01`. Neither is documented and neither is needed by any host here.

## J8, the marker header

The four-pin 50-mil header beside the MCU, and the only pins on the board nothing else uses. No
buffer, no series resistor and no ground pin, so the analyser takes its ground from the debug
connector.

| J8 pin | MCU pin | signal |
| --- | --- | --- |
| 1 | 4 | `PIO1_0` |
| 2 | 39 | `PIO1_1` |
| 3 | 41 | `PIO1_2` |
| 4 | 42 | `PIO1_3` |

**Pin 1 is the leftmost pad with the board the right way up**, USB to the left and the 10-pin debug
connector to the right. Settled by driving one pin at a time and watching the analyser, not by
reading the silkscreen. `uv run tools/dap.py j8 --walk` repeats it in twelve seconds.

The firmware drives all four together while a host command runs, which is one store and makes the
header orientation-free. `--mask` takes pins from the marker so a single one can be held.

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
| LED1 | P0_5 | also `ISP1`: the pin the ROM bootloader samples at reset, and what the jumper pulls |

Releasing SWDIO means clearing `TMS_SWDIO_TXEN` as well as making the pin an input. The buffer
keeps driving the wire otherwise, and releasing the wire is the whole of OScan1.

## Failure modes, and what each looks like

| symptom | first suspect |
| --- | --- |
| comes up as `1fc9:0021`, not as a probe | the ISP jumper is still fitted. Check this first |
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
| `uv run tools/equiv.py <ref> <new>` | 1,212 differential cases against a reference build, or one toolchain against the other |
| `uv run tools/dap.py j8` | drives the four-pin header beside the MCU. `--walk` names the pads |
| `uv run tools/dap.py limit` | every achievable wire rate, checking the data at each |
| `uv run tools/dap.py reboot --bootloader` | into ISP mode with no jumper and no hands |

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

**Measured 2026-10-16 against stock. `DELAY_SLOW_CYCLES` is confirmed; the store cost is not.**

Drive `DAP_SWJ_Sequence` (command `0x12`) at several requested rates and measure the period on
TCK:

```
uv run tools/dap.py --serial <serial> sweep --rates 50000000,10000000,5000000
uv run tools/clockscope.py <dir>/digital.csv --channel <swclk> --cpu-hz 150000000
```

`sweep` leaves a silent gap between rates so the groups can be told apart without a trigger, and
`clockscope.py` takes the period across a whole 256-clock burst rather than between two edges.
That matters: one edge pair at 100 MS/s quantises to 10 ns, a third of a period at the top of the
range, and 255 periods of the same burst brings it to 0.04 ns.

What stock gave, both units agreeing to 0.01 ns over 20 bursts each:

| asked | period | wire | `clock_delay` |
| --- | --- | --- | --- |
| 50 MHz | 127.48 ns | 7.844 MHz | 1 |
| 10 MHz | 167.49 ns | 5.971 MHz | 2 |
| 5 MHz | 287.49 ns | 3.478 MHz | 5 |

### What it settles

**`DELAY_SLOW_CYCLES` is exactly 3.** The marginal cost of one delay iteration is 3.001 and 3.000
cycles across the two intervals. This is the constant the six-cycle step structure rests on, so
the steps in `PACING.md` are real and the specialisation work below is worth doing.

**The core clock of a stock unit is 150.0 MHz**, from three independent pairs: 149.96, 149.99 and
150.00 MHz. That agrees with what stock reports for `DAP_Info` 0xF1, and with the CMSIS-DAP
example's `BOARD_BootClockRUN`, which attaches PLL0 at 150 MHz to `MAIN_CLK`.

**Ours is 144 MHz and that is correct.** We build TinyUSB's board file instead, whose
`BOARD_BootClockPLL150M` sets PLL0 to 150 MHz but then attaches **PLL1** at 144 MHz to `MAIN_CLK`
and sets `SystemCoreClock` to 144000000. Two board files, two clocks, and the one in
`src/DAP_config.h` matches the one we compile. The 150 MHz measured here is not evidence against
it.

### What it does not settle, and why the plan was wrong about that

**`DAP_SWJ_Sequence` does not isolate the store cost.** `SWJ_Sequence` at
`external/CMSIS-DAP/Firmware/Source/SW_DP.c` is a software loop around each clock -- byte refill,
bit test, shift, two counters -- and it is compiled before the `#undef PIN_DELAY` at `:264`, so it
always takes the paced copy and never the delay-free one. The fitted non-delay cost is 6.561,
6.562 and 6.562 cycles per half-bit across the three points, flat to three decimals and nothing
like the 2 that `IO_PORT_WRITE_CYCLES` declares. The extra 4.5 cycles are that loop.

So the differences give the delay cost and the core clock, which is most of the value. The
intercept gives the cost of a command we do not care about. **The store cost needs the transfer
path**, which means step 5: a `DAP_Transfer` block at a known clock, where the per-bit cost is the
unrolled code the emulator actually models.

### The store cost, measured on the transfer path instead

A block read gives what `DAP_SWJ_Sequence` cannot. Capture SWCLK while probe-rs reads a RAM block,
and read the capture as a distribution rather than an average:

```
probe-rs read --chip MSPM0L1306 --probe 1fc9:0143:<serial> --speed 10000 b32 0x20000000 512
uv run tools/clockscope.py <dir>/digital.csv --channel 2 --histogram --cpu-hz 150000000
```

A transfer holds several periods at once, because a read bit, a write bit and the bookkeeping
between words cost different numbers of I/O operations. `--histogram` averages each run of clocks
that share a period, so the data bits separate out from the rest and each one is recovered far
finer than the 10 ns sample period.

Stock, reading 512 words at three requested clocks:

| asked | `clock_delay` | period | wire | cycles at 150 MHz |
| --- | --- | --- | --- | --- |
| 1 MHz | 25 | 1066.774 ns | 0.937 MHz | 160.016 |
| 5 MHz | 5 | 266.774 ns | 3.748 MHz | 40.016 |
| 10 MHz | 2 | 146.774 ns | 6.813 MHz | 22.016 |

`PIN_DELAY_SLOW` is a `subs` and a `bne`, so n iterations cost 3n-1 cycles rather than 3n. Taking
that out leaves **6.008 cycles of non-delay work per half-bit, identical at all three points**
across a twelve-fold range of delay. `IO_PORT_WRITE_CYCLES` declares 2.

So ARM's arithmetic understates stock's own per-bit cost by a factor of three, and a stock probe
lands at 68% of a 10 MHz request, 75% of 5 MHz and 94% of 1 MHz. The one bench number that existed
before this session -- 0.94 MHz for a 1 MHz request -- is reproduced exactly.

**That constant may be about right for us.** Our bit paths are unrolled, and the emulator puts a
SWD data bit at 5 cycles, which is 2.5 per half-bit against the 2 declared. Stock pays 6.
The comparison to make after flashing is ours at the same three rates.

### The target sets the ceiling, not the probe

The MSPM0L1306 on this bench connects at 1, 5 and 10 MHz and fails to connect at 20, 30 and
50 MHz. **Step 5 cannot sweep above 10 MHz here**, so the steps the specialisation work aims at
sit above anything this target will answer. Sizing that work needs either a faster target or a
capture with no target attached at all, which `tools/dap.py sweep` already does.

A failed connect still clocks the wire for five seconds while probe-rs retries, and those groups
are not worth reading: the retry changes the requested rate, so the period in them is not the one
asked for.

### What this unlocks

**Compile-time delay specialisation, worth up to about 2x in the 13-29 MHz band.** The achievable
wire period goes up in steps of six cycles, because the delay is a counted loop costing three
cycles per iteration and there are two delays per bit. That three is now measured rather than
assumed.

The fix is to compile the fast bit path five or six times with 0, 1, 2 ... `nop`s baked into
`PIN_DELAY`, and have `Set_Clock_Delay` pick the copy, exactly as `fast_clock` already picks
between two copies today. That fills in the gap, costs a `nop` rather than a branch per bit, and
the dispatch is once per transfer and already paid. It costs a couple of KB of flash per copy,
which is affordable.

Choosing the right copy for a requested clock still needs the store cost, so it waits on step 5
rather than on this capture. `PACING.md` has the full trace through the source.

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

## What the bench measured, 2026-10-16

The firmware runs. It also loses to stock on the wire, by about half, for a reason the emulator
cannot see.

### Bring-up, steps 1 to 3: all clear

The image boots, enumerates as `MCU-Link Verge CMSIS-DAP`, and answers vendor `0x82` with magic
`CJTAG`, format 2, flags 0x01 and its build string. The serial came back `CJ6B19D4E7` rather than
`CJNOUUID`, so **the unique-id read at `0x0009_FC70` works on silicon** -- the single most likely
thing to stop the device appearing at all. Every `DAP_Info` field matches what `tools/image.py`
predicted. `rblhost` read all 229,464 bytes back and they are identical to the built image.

SWD works against the MSPM0L1306: RAM and flash read correctly, and the words match what a stock
probe read from the same addresses.

It connects at 20 MHz requested where stock does not, and **that is not an advantage.** The target's
ceiling is on the wire, somewhere between 7 and 9 MHz: at 20 MHz asked stock puts 9.375 MHz on the
wire and fails, while we put 6.86 MHz and succeed. Both firmwares meet the same limit at different
requested rates, because ours is slower.

### The transfer path is half the speed of stock, and the flash is why

Measured with the same command on both probes, each against its own live target:
`tools/dap.py block` reads a DP register with `DAP_TransferBlock` 3,840 times. Data-bit period,
averaged over every transfer:

| asked | stock, 150 MHz core | ours, 144 MHz core | stock KiB/s | ours KiB/s |
| --- | --- | --- | --- | --- |
| 1 MHz | 160.0 cycles, every bit | 147, 147, 159, 171 | 77.2 | 71.7 |
| 5 MHz | 40.0, every bit | 33, 33, 45, 57 | 290.8 | 221.3 |
| 10 MHz | 22.0, every bit | 22, 20, 34, 54 | 494.3 | 269.4 |
| 20 MHz | 16.0, every bit | 22, 16, 23, 23 | 640.0 | 382.5 |

Stock holds one period for the whole data phase, to the cycle, at every setting -- exactly
`2 * (6 + PIN_DELAY_SLOW(delay))`, so its fixed cost is 6.0 cycles per half-bit and ARM's model
describes it perfectly. **Ours repeats a four-bit cycle** and does it to within a few nanoseconds
over thousands of transfers.

Our best bit equals stock's. Two of every four stall.

### It is the flash, and the stall is an exact number of wait states

Take each four-bit group's cheapest bit as the baseline and the extra cost is

| asked | extra cycles across the group |
| --- | --- |
| 1 MHz | 0.1, 0.0, **12.1**, **24.1** |
| 5 MHz | 0.0, 0.1, **12.0**, **24.0** |

`CLOCK_SetFLASHAccessCyclesForFreq` puts 130-150 MHz in one bracket at **11 wait states**, so one
flash access that the prefetcher did not cover costs 12 core cycles. The two slow bits in every
group pay exactly one and exactly two of them.

The emulator sees none of this: it puts 65% of the transfer at a single 17-cycle period and has no
four-bit structure anywhere, so the instruction stream is uniform and the stalls are the memory
system fetching it. Three things agree:

- **Our own compact loop does not stall.** `DAP_SWJ_Sequence` is ARM's small loop rather than our
  unrolled code, and in the same image on the same part at the same delay setting it clocks its
  bits at 23.25 cycles, uniform to 0.02 ns, where our unrolled transfer needs 32.52 with the
  pattern. Same flash, same wait states, different code size.
- **Stock's loop does not stall either**, which is why its bits are exactly integer.
- The penalty is a fixed 12 and 24 cycles whatever the delay, so it is not scaled by anything in
  the pacing.

**The unrolling is the cause.** It bought 10.7 KB of straight-line code per bit path and 120 KB of
flash overall, and on this part the prefetcher cannot stay ahead of it. This is the item the rest
of this document listed as the one thing the emulator could never size; it is now the largest
effect measured, and it points the other way from every ranking in `OPTIMISATIONS.md`.

The two things to try are running the bit paths from SRAM, which needs a linker script of our own,
and simply unrolling less. Both are cheap. Neither can be ranked from emulation.

### Fixed: the bit paths now run from SRAM

Moving the four SWD entry points to `.ramfunc` removed the stall completely, and the firmware now
beats stock at every rate this target will answer.

| asked | data bit before | after | stock | KiB/s before | after | stock |
| --- | --- | --- | --- | --- | --- | --- |
| 1 MHz | 147, 147, 159, 171 | **147.0 every bit** | 160.0 | 71.7 | 78.9 | 77.2 |
| 5 MHz | 33, 33, 45, 57 | **33.0 every bit** | 40.0 | 221.3 | 305.5 | 290.8 |
| 10 MHz | 22, 20, 34, 54 | **15.0 every bit** | 22.0 | 269.4 | 562.6 | 494.3 |

The four-bit pattern is gone: every data bit at every setting is now one value, with the whole
28-bit span measuring within a nanosecond of itself. Solving each setting for the non-delay cost
gives **2.5 cycles per half-bit against stock's 6.0**, the same figure at all three, which is the
unrolling finally doing what it was written to do. 2.09x our own previous throughput at 10 MHz,
and 1.14x stock.

It also confirms the emulator was right about the instruction stream all along. It predicted a
5-cycle data bit at the delay-free setting, which is 2.5 a half-bit; the flash was the entire
discrepancy.

**20 MHz requested now fails to connect**, and that is the fix working rather than a regression.
Without the stalls a 20 MHz request puts about 16 MHz on the wire, well past the 7-9 MHz this
MSPM0L1306 will answer. Stock fails there for the same reason. 10 MHz requested is 9.6 MHz on the
wire and is the most this target will take.

What is not yet moved: the two-pin and four-wire JTAG block paths, which are the same unrolled
shape and will have the same stall. `cjtag_BlockImpl` and `cjtagf_BlockImpl` are 67 KB together
against the 79 KB already in SRAM and 112 KB still free.

### The cost model is now calibrated against the wire

With the stall gone the emulator can be checked instruction by instruction, and it was wrong in
one place. A conditional branch that **falls through** was charged a full cycle; it is free on this
part. The 14-instruction data bit the model priced at 17 cycles measures exactly 15.00, and its two
fall-through `bne`s are the whole difference. There are two of them per wire clock, one per delay
invocation, which is why the error was a constant 2.5 cycles per clock at every requested rate.

| | before | after | measured |
| --- | --- | --- | --- |
| 1 MHz asked | 151.02 | 148.89 | 149.54 |
| 5 MHz | 37.02 | 34.89 | 35.51 |
| 10 MHz | 19.02 | 16.89 | 17.51 |

Error over a whole transfer goes from 13.4% to 3.6%, and the **data bit is now exact**. The
remaining 0.63 cycles per clock is an underestimate concentrated in the once-per-word bookkeeping,
unattributed: fitting the five constants over these three rates prefers a cheaper load and a
charged fall-through, but that combination prices the data bit at 16 against a measured 15.00, and
the data bit is the number measured to a hundredth over thousands of transfers. A load of 2 with a
free fall-through is the only candidate that satisfies it, so the fit was not allowed to override
it. Separating the rest needs a write block, whose instruction mix differs; `tools/dap.py block`
only reads today.

**It found a real overshoot.** The model now says SWD and the two-pin path clock faster than asked
at 10 and 20 MHz, where before it said SWD at 10 MHz was comfortably under. The analyser agrees:
five bit positions measured 10.28 to 10.30 MHz for a 10 MHz request. Those are recorded in
`tools/baseline/known-overshoot.txt` as known rather than new, because they are a property the code
always had and the calibration merely made visible.

### Taken: the core now runs at 150 MHz

PLL0 was already configured and locked at 150 MHz by the board file and simply not attached;
TinyUSB puts PLL1's 144 MHz on `MAIN_CLK` so that MAIN_CLK/3 is the 48 MHz a full-speed USB
controller needs, and this firmware is on the high-speed port and builds no host controller.

Measured, and every prediction landed:

| asked | data bit at 144 | at 150 | change | why |
| --- | --- | --- | --- | --- |
| 1 MHz | 1021.1 ns | 1020.25 ns, 153.04 cycles | +0.1% | `Set_Clock_Delay` went from 24 iterations to 25, and the extra six cycles cancel the faster clock |
| 5 MHz | 229.3 ns | 220.08 ns, 33.01 cycles | +4.2% | same delay, faster clock |
| 10 MHz | 104.2 ns | 100.04 ns, 15.01 cycles | +4.2% | same delay, faster clock |

The core clock itself is confirmed at **150.00 MHz**, solved from a bare clock sweep: the delay 2
to delay 5 interval spans exactly 18 cycles over 120.00 ns.

**End to end it barely shows, and that is the useful part.** Five runs at 10 MHz spread 553 to
570 KiB/s, so the 4.2% is inside the noise. Of a 3,840-transfer block read, 17.67 ms is wire time
and 26.70 ms is the whole thing: **the wire is 66% of it and USB and the host are the other 34%.**
A wire-only gain is worth about 2.8% from here. Anything further on the bit paths buys less than
its own size until the USB side moves, which is what the 512-byte packet question below is about.

### Where a block read's time actually goes, measured on one timeline

A marker pin on J8 goes high while the firmware runs a host command and low otherwise, so the gap
between commands and the clock are in the same capture. Before this the gap was a residual: total
time from the host, wire time from the analyser, the difference called USB. Two runs and a
subtraction, and the subtraction absorbed everything that was not either one.

60 `DAP_TransferBlock` commands of 64 transfers at 10 MHz asked, 100 MS/s on SWCLK and the marker.
Every block clocked exactly 5,888 edges, which is 64 x 46 with no retries.

| | per block | share |
| --- | --- | --- |
| the 32 data bits and the request | 270.4 us | 62.7% |
| turnarounds, parity and the per-word bookkeeping | 70.6 us | 16.4% |
| firmware with the clock idle | 3.2 us | 0.7% |
| USB and the host | 84.7 us | 19.6% |

**USB and the host are 19.6%, not the 34% this document carried.** The old figure came from
subtracting a separately measured wire time from a separately measured total, and it was wrong by
enough to misrank the work: the wire is 79% of a block read, so the bit paths are worth more than
the packet size question, not less.

### The per-transfer overhead is four clock positions, and two of them are most of it

Folding all 2,943 intervals in a block by position in the 46-clock transfer, over 60 blocks, so
each number is 3,840 samples:

| clock | cycles | what it is |
| --- | --- | --- |
| 7 | 27.01 | turnaround after the request |
| 11 | 27.01 | turnaround after the acknowledge |
| 44 | 51.88 | parity |
| 45 | 66.07 | the trailing turnaround and the per-word bookkeeping |
| everything else | 14.00 to 18.00 | request, acknowledge and data bits |

Against a 15-cycle baseline those four cost 112 extra cycles on a 690-cycle transfer, which is the
16.4% above. **Clocks 44 and 45 are 89 of those 112.** That is the once-per-word bookkeeping this
document recorded as an unattributed underestimate in the cost model; it is now located and
measured. It is also a larger target than anything left in the data phase, where 32 bits already
run at 15.00 cycles dead.

### The overshoot is five named clock positions, not a tendency

Clocks 5, 9, 10, 12 and 13 measure 14.00 cycles against the 15.00 a 10 MHz request should give:
**10.72 MHz on the wire, 7.2% over what the host asked for.** The data bits from clock 14 on are
15.00 to a hundredth, and clocks 1 to 4 are 16.00, so the transfer straddles the requested rate
rather than sitting on it.

The fastest position is what a marginal target breaks on, so 10.72 MHz is the number that matters
for the overshoot, not the 9.975 MHz average of the data phase. This supersedes the earlier 10.28 to
10.30 MHz figure, which was the same effect measured without folding by position.

### The per-word bookkeeping, and the one register that decided it

The four clocks that are not data bits cost 112 cycles a word over a 15-cycle baseline. Two
changes took 36.5 of them, and the first attempt at the second one lost, which is the part worth
keeping.

**Say what the turnaround period can be.** `DAP_SWD_Configure` stores `(value & 3) + 1` and
`DAP_Setup` stores 1, so the field is 1 to 4. Read as a plain byte the compiler has to allow 0 to
255 and spends a zero test and a three-way dispatch per turnaround discovering otherwise. One
`__builtin_unreachable` removes both. Worth 14.9 cycles a word.

**Peel the last read out of the block loop.** It is the only transfer that can use a different
request, so leaving it inside makes the packet request loop-variant and GCC re-derives five
shifted bits and spills them on every word. Peeling removes ten instructions a word.

**The peel has to keep its tail out of line, and that is the whole lesson.** Peeled with the
transfer inlined a second time it came out worse:

| build | data bit | the four overhead clocks | wire a block |
| --- | --- | --- | --- |
| before either change | 15.107 | 112.0 cycles | 343.55 us |
| peeled, tail inlined | 16.139 | 87.2 | 348.84 us |
| peeled, tail inlined, 16-byte aligned | 16.138 | 87.3 | 348.84 us |
| turnaround stated | 15.073 | 97.2 | 336.33 us |
| turnaround stated, peeled, tail out of line | 15.121 | 75.6 | 330.43 us |

The inlined peel saved 24.8 cycles of bookkeeping and **cost one cycle on every one of the 32 data
bits**, which is 32. Disassembling a data bit in each build gives the same eight instructions in
the same order and only the registers differ: a second inlined transfer takes one more register
and the data phase pays for it 32 times a word. Out of line it costs a call once a block instead.

**Nothing upstream catches this.** The emulator counts instructions and sees no change. The gate
passes it. All 1,212 differential cases pass it. Forcing the functions to a 16-byte boundary moved
the figure by 0.00 cycles, so it is not alignment. The only instrument that separates a 2% win
from a 1.5% loss is flashing it and folding a capture by position in the transfer.

So the rule for this file: **a change may remove work from the bookkeeping but must not take a
register from the data phase.** 480 of a transfer's cycles are in the data phase and 112 in the
bookkeeping, so the exchange rate is bad.

End to end the two changes took a 64-word block read from 585 to 602 KiB/s, and the wire from
343.6 to 330.4 us.

### gcc against clang, same source, same hardware

`-DTOOLCHAIN=clang` builds and passes the whole gate, and `tools/equiv.py` says the two images are
identical on the wire across all 852 cases. So the difference is codegen alone.

| | gcc | clang |
| --- | --- | --- |
| data bit | 15.119 cycles | 15.899 |
| clock 7, turnaround | 20.03 | 19.02 |
| clock 11, turnaround | 20.07 | 22.07 |
| clock 44, parity | 42.10 | **18.95** |
| clock 45, bookkeeping | 52.48 | 64.41 |
| the four together, over a 15-cycle baseline | 74.7 | **64.5** |
| wire a block | 330.01 us | 335.10 us |
| end to end | 602 KiB/s | 580 KiB/s |
| image | 238 KB | 166 KB |

**gcc wins the data phase and clang wins the bookkeeping.** gcc's bit is 0.78 cycles cheaper,
which over 32 bits is 25 cycles and decides the result.

Clang's clock 44 looks like a 23-cycle lead and is not: it pays 12 of that back at clock 45, so
the two together are 94.6 cycles for gcc against 83.4 for clang. The pair is one unit of work --
the parity, the acknowledge check, the word store and the loop -- and the compilers divide it
differently across the last two clocks. **10 cycles a word is the real difference**, and it is
worth having, but reading clock 44 on its own overstates it by more than double.

One hint already closed a larger gap. `PIN_DELAY` expanded to a read of `DAP_Data.clock_delay`, so
the paced copies loaded it twice a bit. GCC hoists that into a register by itself and clang did
not, costing clang two cycles on every bit. Naming it in a local took clang's modelled SWD read
from 653 to 783 KiB/s and left gcc's output unchanged.

**The emulator ranked these the wrong way round.** It put clang ahead at 783 against gcc's 767,
and hardware puts gcc ahead. Same blind spot as the rejected peel: it counts instructions, and
whatever makes clang's data bit 0.78 cycles dearer is not in the count.

### The USB side: the probe held one packet and told the host eight

`tools/dap.py block --pipeline N` keeps N commands in flight instead of waiting for each reply.
Measured on a 64-word block read at 10 MHz asked:

| commands in flight | end to end |
| --- | --- |
| 1 | 581 KiB/s |
| 2 | 646 |
| 3 | 646 |
| 4 | times out |

**A second command in flight is worth 11%**, and it is worth it for a reason nothing on the wire
can fix: with one in flight the wire sits idle while the host notices the reply and sends the next
request. The third adds nothing, and the fourth does not work.

`DAP_Info` said eight packets, copied from ARM's reference. It is now 2, which is what the stack
can actually hold: one packet in the endpoint buffer and one in the class buffer.

**TinyUSB's vendor class is a byte stream, not a packet interface**, and that is the whole reason.
`tud_vendor_read` drains whatever the receive buffer holds, so a buffer sized for two commands
hands both to one read and the second is answered by nobody. Raising both buffers to eight packets
was tried and made it worse: the host times out from three in flight upward and throughput falls
as 1/N. A buffer of exactly one packet is what keeps CMSIS-DAP's one-command-per-packet framing
true, so `CFG_TUD_VENDOR_RX_BUFSIZE` and `TX_BUFSIZE` must stay at 512.

Going deeper than two needs the replies to leave as framed transfers, which means driving the bulk
endpoints directly instead of through the class FIFO. That is the remaining USB work.

The command loop also waits for the transmit side to drain before starting the next command.
`tud_vendor_write` writes what fits and returns the count, and nothing was reading it, so a full
buffer truncated a reply silently.

## What the bench has sized, and what is left

- **Running the bit paths from SRAM.** Done; it was the biggest item there was and it doubled the
  block read. It needed no linker script of our own: `.ramfunc` is already in the board support's
  script and already copied by its startup code, which the earlier note here got wrong.
- **A DAP packet above 512 bytes.** Worth less than this document used to claim. The marker puts
  USB and the host at 19.6% of a block read, not 34%, so the ceiling on this is a fifth rather
  than a third, and two commands in flight has already taken 11% of it. It deviates from stock, so
  check pyOCD and OpenOCD before changing what `DAP_Info` reports.
- **Framed replies, so more than two packets can be in flight.** Needs the bulk endpoints driven
  directly rather than through TinyUSB's vendor FIFO, which cannot keep a packet boundary.
- **Clocks 44 and 45 of every transfer**, 51.9 and 66.1 cycles against a 15-cycle data bit. 89
  extra cycles once per word and 12.9% of a transfer, but one attempt at them already came out
  worse by disturbing the data phase. See the rejected change above before trying again.
- **The two-pin and four-wire JTAG block paths are still in flash** and will have the same stall
  the SWD paths had. `cjtag_BlockImpl` and `cjtagf_BlockImpl` are 67 KB together, against 79 KB
  already in SRAM and 112 KB still free.
- **The onboard LED says nothing.** `LED_CONNECTED_OUT` and `LED_RUNNING_OUT` are stubs and
  `DAP_HostStatus` does nothing. Deferred until the probe is functionally complete. It has
  the two constraints that matter, that `P0_5` is also `ISP1` and that a GPIO store is one cycle of
  a fifteen-cycle data bit, so this belongs in the dispatch layer and not in a bit path.
- **Where the wire actually stops working** is still a guess from two points: 9.6 MHz works on
  this MSPM0L1306 and 16 MHz does not. `tools/dap.py block` is already the right instrument --
  idempotent, no access port, no target state -- so sweeping the clock and counting errors would
  replace the guess with a number. Read DPIDR rather than RDBUFF so a wrong word is detectable and
  not just a bad acknowledge, and enumerate the achievable rates from the delay arithmetic rather
  than sweeping requested Hz, because the steps are coarse.
