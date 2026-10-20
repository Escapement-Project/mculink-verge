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

**SWD is finished.** Through probe-rs against the same target, ours reads at 593 KiB/s and writes
at 612, against stock's 473 and 501: 1.26x and 1.22x. The wire is 92% of a block read and the
block command itself is as efficient as a raw CMSIS-DAP script's, so what is left belongs to the
host rather than the probe.

**Two-pin works on hardware, 2026-10-17.** On a CC1312R1 LaunchPad our probe reads the ICEPick
IDCODE and, through the ICEPick's routing, the Cortex-M4's CPUID over TCKC and TMSC alone, with
TDI and TDO flat. Block reads of the boot ROM are correct from 1 to 25 MHz requested and fail at
37.5. The host drives TI's own activation (the XDS110 routine, with the 200 us power-up wait
TI requires) using standard commands, and vendor `0x83 0x02` turns on our framing; the
firmware's own activation (`0x83 0x01`, OpenOCD's) is for adapters that power up offline and
does not work on this part. The wire rules, the activation and the generator plan are in
`CJTAGGEN.md`. Four-wire JTAG is still unmeasured on
hardware.

**nRESET reaches targets since c62226b.** Before it, the reset buffer's direction pin sat in I2C
open-drain mode, so no reset from this firmware ever reached a target.

The onboard LED is done and checked by eye: see "The LED: a timer's job" below. The probe also
reads the target's supply and J6 pin 9's detect line now: `tools/dap.py target`, measured in all
three states under "The probe can measure the target" below.

**Physical state at the end of the session.** Our unit, `CJ6B19D4E7`, runs `cd56325`, and its
10-pin cable is unplugged from its MSPM0L1306. The analyser is still on the SWD-era map: D0-D2 on
our probe, D3 our J8 marker, D4-D6 stock's. The JTAG plan below moves it.

## Next session: cJTAG on the CC13xx, then four-wire through it

**Done 2026-10-17 for two-pin; see the top of this file.**

**Decided 2026-10-16: two-pin first, on the CC13xx boards.** The pair is already instrumented, and
the J-Link is a known-good probe on the same wires, so every capture of ours has a working one to
diff against. That is plan steps 8 and 9 below.

**Four-wire JTAG comes after, on the same part.** probe-rs's `ctag_to_jtag` switches a CC13xx out
of its power-up two-pin mode into four-pin, and the board routes TDI and TDO (the J-Link reference
captures have both on the analyser). Once the two-pin path is trusted, four-wire is the same TAP
behind the same activation with two more wires live.

**The NUCLEO-U575ZI-Q is set aside for JTAG**: TDI needs two solder bridges changed, and the
board is not to be permanently modified. It stays useful as the fast SWD target, which needs
jumpers only (JP1 on, JP2 off, JP6 [7-8] with USB-C power): see "The STM32U575 is that faster
target".

**Analyser, at the target**: the four-wire map below serves both halves. D1 TMSC, D2 TCKC, D3 our
marker, D4 TDI and D5 TDO, which stay flat throughout a two-pin run and are the check that it
really is two-pin. `find_oscan.py` and `la_tap.py` take channel numbers, so the reference
captures' D0-D3 order does not need copying.

## Four-wire JTAG: harness, taps, hazards, and the Nucleo

**One target, moved between the two probes: the NUCLEO-U575ZI-Q** (MB1549), the board the embassy
`examples/stm32u575` crate runs on. probe-rs knows the chip as `STM32U575ZI`. It is a
Cortex-M33 behind an SWJ-DP, so it answers four-wire JTAG out of reset: TMS on PA13, TCK PA14, TDI PA15, TDO PB3, and NRST.
nTRST (PB4) has no pin on J6 or on the Nucleo's debug header and stays on its internal pull-up.
The MSPM0L1306 pair stays out of this.

**J6 carries all of four-wire JTAG.** It is the standard 10-pin Cortex Debug layout: 1 VREF,
2 TMS, 4 TCK, 6 TDO, 8 TDI, 10 nRESET, 3/5/9 ground. Pin 7 is `IF_ISPEN`, NXP's target-ISP line,
where the standard has a key. Leave it unconnected at the target. There is no nTRST, so the TAP is
reset through TMS.

**The target needs its own supply; the probe cannot power it.** J6 pin 1 is a sense input. A
voltage follower (U6) buffers it, and the buffered level becomes the reference for every level
shifter on the debug lines. The only path from the probe's 3V3 to pin 1 is R16, 33 kOhm, which
gives the shifters a defined reference when nothing is plugged in and collapses under any real
load. That is why a target hung off it stayed unpowered. No other connector carries power either:
J7 is RX, TX and ground. The one path is a hand-wired mod from J2's unpopulated pad 1, which is the
probe's own 3V3 rail. That rail has a 700 mA regulator the probe's own MCU and USB PHY also draw
on, with no current limit and no switch, so a target fault resets the probe. The MCU-Link Pro
supplies a target by design (its J6 to enable, J5 for 3.3 or 1.8 V). So the target powers itself,
and the Nucleo's CN5 already puts its VDD on pin 1. With pin 1 unwired the shifters have no
reference, and the probe drives nothing at all.

**The probe can measure the target, and tell an empty connector from a dead target.** The buffered
level goes through a 2K/2K divider (R22/R23) to `DBGIF_VREF`, an analog input on PIO0_31, so the
MCU reads half the target voltage. The buffer exists so the divider's ~0.8 mA comes from the probe
and not the target. J6 pin 9 is `IF_DETECT`, through 2K to `DBGIF_DETECT` on PIO0_22, which needs
the MCU's pull-up. A target connector that grounds pin 9 pulls it low. Together:

| state | J6 pin 1 | pin 9 | measured on the MSPM0 bench board |
| --- | --- | --- | --- |
| nothing connected | ~3.3 V, through R16 | open | 3.276 V, open |
| target connected and powered | the target's VDD | grounded | 3.268 V, grounded |
| target connected and unpowered | wherever the dead board pulls R16 | grounded | 1.084 V, grounded |

**Empty and powered differ by 8 mV here**, so pin 9 is what separates them, as predicted. The
empty reading is also the chain's own error: pin 1 then sits at the probe's 3V3 through an unloaded
R16, and the ADC's reference is that same rail, so it should read exactly full-rail. It reads 0.7%
low, which is divider, buffer and converter together. Take readings as good to about 1%.

**An unpowered target does not read zero.** This MSPM0 board, dead, loads R16 like roughly 16K,
so pin 1 sits at 1.08 V. That is unmistakable against a 3.3 V supply, but the level depends on the
board. Against a 1.2 V target a dead board and a live one could read the same.

A 3.3 V target and an empty connector read the same voltage. Nothing loads the sense node but the
buffer's input, so the pull-up delivers the probe's own 3V3 almost exactly, within the divider's
tolerance of any real regulator. What differs is source impedance, 33 kOhm against well under an
ohm, and the board has no switchable load to measure it with. SJ1 in its 2-3 position would put
the divider straight on pin 1, and then an empty connector would read ~0.36 V. That is a solder
change, and it loads every target with ~0.8 mA. So presence comes from pin 9, or from the target
answering an IDCODE or DPIDR read. The connector standard names pin 9 GNDDetect and has the target
ground it, so a board with the standard 10-pin header usually does it already; check continuity on
an unfamiliar one. **With flying leads, ground pin 9 in the harness**, or the detect pin says
nothing. **Ours reads both**: vendor 0x86, `tools/dap.py target`. The ADC
channel was not in any document here and one third-party source contradicted the obvious guess,
so it was found on the board: `dap.py target --sweep` with the target unpowered and then powered.
ADC0 3A went 542 to 1634 mV at the pin, half the supply. No other channel moved by more than
174 mV. Five repeat reads agree to 2 codes in 65536. The reading is only as absolute as the
probe's own 3V3, which VDDA and VREFP are tied to.

**Adding code that never runs cost 0.06% of block throughput** (126 words: 702.8 to 702.3 KiB/s,
three runs each). The ADC only wakes when asked, so this is placement in flash, not work. Treat
anything under about 0.1% as inside the floor for any change to the image, unless placement is
controlled for.

**Make the swap one connector.** One harness stays on the target and only its J6 end moves
between probes, so both are measured through the same wires.

**The Nucleo's CN5 takes a straight 10-pin cable.** It is a 10-pin 1.27 mm header fitted on a
14-pin STDC14 footprint, middle ten positions only, and its MIPI10 pinout is J6's: 1 VDD, 2 TMS,
4 TCK, 6 TDO, 8 TDI, 10 NRST, 3/5 ground, 7 not connected, 9 GNDDetect through R17 (100R) to
ground. Pin 7 being open suits our `IF_ISPEN`. From UM2861 Rev 10 and schematic MB1549 C-05.

**The board needs two solder bridges changed before TDI works.**
- **SB39 closed.** It is open by default, so CN5 pin 8 reaches nothing: PA15 is not wired to the
  debug header at all.
- **SB42 opened.** It is fitted by default and puts PA15 on `UCPD_CC1`: the TCPP01-M12's CC1 pin
  and C62, 330 pF to ground. Leave it and TDI drives 330 pF. UM2861 Table 13 gives SB42 off with
  SB39 on as the T_JTDI configuration. USB-C power delivery on CC1 goes; nothing here uses it.
- Both are on the bottom side, a stacked pair beside the CN8 Zio connector (UM2861 Figure 5). SB39
  is the open one.
- SB44, PB3 to CN5's TDO, is fitted by default. Leave it.

Without the rework the IDCODE still reads, since it needs no TDI after Test-Logic-Reset and PA15
pulls itself up. Nothing past that does, because every IR load comes in on TDI.

**Jumpers: JP1 on, JP2 off.** JP1 holds the onboard STLINK-V3E in reset, so its PF9 and PH6, which
drive PA13 and PA14 through 47R, go high-impedance. JP2 disconnects its NRST. Its level shifter U9
(SN74LVC2T45 with DIR tied low) only ever reads the target's TMS and TDO, so it loads them and never
drives them. T_JTDI goes only to CN5 and the ESD array.

**JP1 on also cuts the default power.** JP6 [1-2] takes 5 V from the ST-LINK's USB port through
U2, a TPS2051C. Its enable comes from the ST-LINK after USB enumeration, with R9 (10K) holding it
low, so with the ST-LINK in reset it stays off. Use JP6 [7-8] and a cable into the user USB-C port
CN15, good for up to 1 A. JP6 [9-10] also skips the switch, but UM2861 forbids it from a computer
port. That reading comes from the schematic and is unconfirmed: LD5, the green 5 V LED, lighting
with JP1 on and only CN1 plugged would prove it wrong.

**Tap the analyser at the target, not the probes.** The target is the one thing both probes
share, so one set of clips serves both, and the probe-side taps stop meaning anything once the
target moves:

| channel | signal | on the Nucleo | was |
| --- | --- | --- | --- |
| D0 | nRESET | CN8 pin 5, Zio | our probe's nRESET |
| D1 | TMS | CN11 pin 13, morpho | our probe's SWDIO |
| D2 | TCK | CN11 pin 15, morpho | our probe's SWCLK |
| D3 | marker | our J8 pin 4 | unchanged |
| D4 | TDI | CN11 pin 17, morpho | stock's nRESET |
| D5 | TDO | CN7 pin 15, Zio D23 | stock's SWDIO |
| D6, D7 | free | | stock's SWCLK and J8 |

Ground on CN11 pin 19 or 20, or CN8 pin 11 or 13. The Zio connectors are fitted sockets. The morpho
headers ship unsoldered, so CN11 needs a 1x4 strip on pins 13, 15, 17 and 19. PB3 runs to CN7 with
no bridge in the way. These taps hang off traces that exist anyway, so they add only the clip.

D2 stays the clock and D3 the marker, so the fold and `clockscope.py --channel 2` carry over.
Timing captures take D1, D2 and D3 at 100 MS/s, exactly as the SWD marker captures did. Protocol
decodes take D1, D2, D4 and D5 at 50 MS/s, the rate the cJTAG reference capture used on four
channels. Stock never drives J8, so a stock capture has the clock and no marker.

**Hazards specific to this target.**
- The Nucleo's STLINK-V3E shares PA13, PA14, PB3 and NRST. JP1 on and JP2 off isolate it, as
  above; skip that and it fights the probe.
- Do not set TZEN or raise RDP. Level 1 only comes back with a mass erase, and level 2 never does.
  Nothing here asks for either, but a flash tool's option-byte page can.
- Ask probe-rs for JTAG explicitly. An SWJ-DP starts in JTAG, and a host that defaults to SWD
  switches it.
- Put the probe in every capture and CSV name. With the target moving, the filename is the only
  record of which probe produced it. Alternate the probes run by run (ABAB) so drift lands on both.

**Check the target before every attach**, with the harness on whichever probe runs ours: `dap.py
target` should say about 3.3 V with pin 9 grounded. Well under that means the Nucleo is
unpowered (check LD5), and 3.3 V with pin 9 open means the harness is not plugged in.

**Tooling to write first; none of it needs the bench.**
1. `dap.py scan`: count the TAPs on four-wire JTAG and read their IDCODEs. Older STM32s put a
   boundary-scan TAP in series with the debug TAP, so the chain is an open question. Settle it
   before anything is configured from it.
2. `dap.py block --wire jtag`: `DAP_JTAG_Configure` from the scan, then the same DP-read block.
   `dap.py limit` wants the same option.
3. `swd-throughput`: a `--protocol` flag. `WireProtocol::Swd` is hardcoded at
   `src/main.rs:294`.
4. The fold: a JTAG transfer is not 46 clocks, so the period becomes a parameter. Derive it from
   the TAP path in a capture rather than from arithmetic.

**Then measure, in the order the SWD work found things.** Run each step on both probes unless it
says otherwise:
1. The chain scan, and a probe-rs connect over JTAG.
2. A data bit's cycles, folded by clock position, against `tools/cycles.py --table`. Ours only:
   stock has no marker.
3. Whether the SRAM move (`ecaec8a`) did for four-wire JTAG what it did for SWD.
4. The per-transfer overhead positions, and the rate ladder with `dap.py limit`.
5. End to end through probe-rs, ABAB.

The two-pin half (plan steps 8 and 9) comes first; see the section above.

## Where the bench stood before that

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
read, and the HID parity question. (SWO since done: §7.) The open optimisation work includes moving
the two-pin block paths to SRAM the way the SWD ones went.


## Hazards, before anything else

- **`DAP_TransferAbort` answers nothing.** Reading for a reply after sending it stalls the probe's
  bulk endpoint, and every later command times out. `usb.core.Device.reset()` clears it without
  unplugging and does not disturb a halted target.
- **The analyser can be driven from a script.** Captures start, stop and export
  without a pair of hands. Export the raw digital CSV and
  read it with `tools/clockscope.py`.
- **Run `ninja -C build check` before flashing.** It refuses a build that clocks any wire faster
  than the host asked for, beyond what `tools/baseline/known-overshoot.txt` records: nothing for
  the default build. ARM's paths alone (`+armwire`) have theirs in `tools/baseline-armwire/`.

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
| `0x83 0x01` | drive OpenOCD's activation, then frame as OScan1 |
| `0x83 0x02` | frame as OScan1 after an activation the host drove; the CC13xx needs this one |
| `0x83 0x00` | stop framing |

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
[6]     capability flags       bit 0 OScan1 supported, bit 1 OScan1 active now,
                               bit 2 target sense, bit 3 generated SWD, bit 4 0x83 mode 2
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

**The three 2-pin jumpers beside each other are ISP (J3), VCOM (J4) and SWD (J5).** The board's
silkscreen labels J4 "VCCM". It is the virtual COM port: shorted, it pulls `HW_VER_6` low, and
firmware that reads that pin turns the USB-to-UART bridge off. J5 does the same for SWD through
`HW_VER_7`. **Our firmware reads neither pin**, so on ours both jumpers do nothing.

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
50 MHz with the compiled bit paths. (Later measured: that is the probe's sampling. Placed by the
timing model, the same part reads and writes clean at 37.5 MHz; see "The cost model now times the
bus".) **Step 5 cannot sweep above 10 MHz here**, so the steps the specialisation work aims at
sit above anything this target will answer. Sizing that work needs either a faster target or a
capture with no target attached at all, which `tools/dap.py sweep` already does.

**The STM32U575 is that faster target.** Its datasheet (DS13737 Rev 10, Tables 152 and 153) gives
SWCLK up to 66.5 MHz and TCK up to 38 MHz at 2.7 to 3.6 V. Both figures come from
characterisation at 30 pF, so `dap.py limit` through the real harness is the number to trust.
It also has 786 KB of SRAM against the L1306's 4 KiB, which lifts the transfer-size cap on
`swd-throughput`. SWD needs none of the TDI rework. The nRF54L family is no help here: the
nRF54L15 datasheet (v0.7) caps SWDCLK at 8 MHz, the same band this MSPM0 fails in. The nRF54LM20's
own figure was not checked.

**Measured 2026-10-16, ours against the U575 over CN5: clean to 10.00 MHz, failing at 16.67.**
`dap.py limit` checks every word at every rate the firmware has. Delay 2 (10.00 MHz on the wire)
and below read 3,200 DPIDRs each with no bad word. Delay 1 (16.67 MHz) fails, and so does the
delay-free path at 30 MHz. There is nothing between 10 and 16.67 on the ladder. The U575 does
better than the MSPM0, which failed at 9.375 MHz under stock, but nowhere near its datasheet.

**The wire is clean; the probe misreads it.** Captured at our J6 (SWDIO and SWCLK, 100 MS/s,
`mculink-ours-u575-swd-10-vs-16mhz.sal`) and decoded by sampling at each
rising edge, as the target does. Every read at both settings carries OK and `0x0BE12477`,
including the one the probe reported as acknowledge 3. At the connector the target's bit
arrives 15 ns after the rising edge that launched it (10 or 20 ns at this resolution, mean 15.3
at 10 MHz and 14.9 at the faster setting), so the cable and the U575 together spend 15 ns.

The faster setting is not 16.67 MHz on the wire for a block: data bits run at 60 and 70 ns
(mean 67.5), acknowledge bits at 60 and 80, and the period at 10 MHz is 100 ns throughout.

Acknowledge 3 is an OK (1,0,0) seen one bit behind: the turnaround's high level, then 1 and 0.

**Only one function misreads, and it does every time.** At the faster setting `DAP_Transfer` read
19,200 DPIDRs without a fault, and every block-size test gave the same answer, 200 runs each:

| block | words returned, acknowledge |
| --- | --- |
| 1 word | 0, 3 |
| 2 words | 1, 3 |
| 4 words | 3, 3 |
| 64 words | 63, 3 |

The block command peels its last read into `SWD_TransferTailSlow`, an out-of-line copy of the
transfer, and that copy misreads its acknowledge on every call. The loop that does the other 63
never does. At 10 MHz all four sizes come back whole. The tail's acknowledge reads are the same
instructions as the loop's, in the same order and the same sizes, using the same GPIO aliases,
at a different address.

**It is not instruction alignment, and it is not the out-of-line function.** Two rebuilds, each
run through the same block-size test, 200 runs a size:

- A `nop` at the top of the tail shifted it by a halfword, flipping the 4-byte alignment of every
  instruction in it, and the compiler re-laid the neighbours too. Still 200 of 200 failing.
- Forcing the tail inline into the block function, so the last read is code in the loop's own
  function again, the peel undone. Still 200 of 200 failing, and the loop's reads still clean.

So the failure goes with the block's last read, whatever code carries it.

**On the wire the last read's clock is a cycle off.** Averaged over a capture of 14,977 one-word
blocks and 4,025 sixty-four-word blocks (`mculink-ours-u575-tail-vs-loop.sal`),
which washes out the analyser's 10 ns steps because its phase differs from read to read:

| | high after ack0's rise | low before ack1's rise | target's ack1 edge after the rise |
| --- | --- | --- | --- |
| last read, 1-word blocks | 21.8 ns | 38.2 ns | 17.6 ns |
| last read, 64-word blocks | 21.7 | 38.4 | 17.8 |
| the loop's reads | 27.9 | 32.2 | 17.1 |

Standard errors are 0.01 to 0.15 ns. The period is the same 60 ns and the target answers the
same, but in the last read the falling edge comes 6 ns, one core cycle, sooner after the rising
edge. Its instructions are the loop's, so one of the two clock stores reaches the pin a cycle
off from where it does in the loop. That one cycle is the difference between every read passing
and every read failing, which puts the loop's own margin at this setting under a cycle. A
comparison made earlier from a single transfer pointed the same way and was wrongly retracted as
quantisation: the averages bear it out.

**An earlier reading here, that the probe's own pins and buffers take ~45 ns, was wrong.** It
was a model fitted to one failure, and the two results above break it.

**Measured with vendor 0x87 (`dap.py sample`): the next edge's bit reaches the sample about five
cycles after the store that raised the clock.** At a 333 ns period, 1,000 reads at each position:
production's place before the edge and every position up to 2 filler cycles after it read
correctly, and from 3 on every read comes back one bit ahead, acknowledge 4. No position in
between gave a mixture. The filler times as exactly one cycle a step from a fill of 4 up (0 to 4
costs 5), and the period does not move with the sample. A computed branch of about two cycles
sits between the store and the filler, so the edge is near five cycles, ~35 ns, after the store
issues. In the loop, the sample before the next edge comes about eight cycles after this one at
the faster setting, so the loop has two or three cycles in hand. The tail runs the same
instructions and does not.

**Measured with both shapes of vendor 0x87 (`7d0808e`): production samples at the early end of a
window about a period wide, and leaves most of it unused.** 1,000 reads a position, fill 24, a
260-280 ns period, saved as `sample-u575-lead-and-after-7d0808e.txt`:

- **Lead shape, production's own pattern** (rising edge, filler, falling-edge store, production's
  delay loop, sample): every high phase from 0 to 24 cycles reads correctly. Even the shortest,
  about five or six cycles from the rising-edge store to the sample, already sees the bit that
  edge launched.
- **After shape** (sample in the high phase): correct up to 3 filler cycles past the edge, one bit
  ahead from 4. With operands reloaded through a struct the edge sat between 2 and 3, so it
  moves by a cycle with the code around it.
- A zero-length filler run is one cycle cheaper than the trend in both shapes, so position 0 sits
  a cycle earlier than its number.

So a bit is readable from about five or six cycles after the store that launches it until about
as long after the next one. Production samples a cycle before the next rising edge, at the
window's early end: one or two cycles in at the 16.67 MHz setting, which the block tail's
one-cycle shift used up, with five or six cycles of the window behind the sample unused.
**Prediction, not yet tested:** sampling a few cycles after the rising-edge store would sit near
the middle at that setting, and keep about two cycles either side even at the delay-free step's
five-cycle period. How much room there is past the edge depends on the target's round trip, which
was 15 ns here and which a closer or faster target shortens.

**Sampling after the edge fixes the 16.67 MHz setting (`6072ae9`, option `SWD_SAMPLE_AFTER_EDGE`,
off by default).** The delayed path samples one instruction after its rising-edge store, with
each transfer masked. The delay-free path is unchanged, because with no delay its low phase
would shrink to a cycle. A build with the option names itself `+after-edge`.

| | default | after-edge |
| --- | --- | --- |
| block-size test at 16.67 MHz, 200 runs a size | the last read fails every time | all whole |
| `DAP_Transfer` at 16.67 MHz | 19,200 clean | 19,200 clean |
| `dap.py limit` | clean to 10.00 | clean to 16.67; 128,000 reads at each of 16.67 and 10 |
| block read, 126 words, depth 2, 16.67 MHz | fails | 973 KiB/s |
| block read, same, 10 MHz, two rounds of three, alternated | 701.6-701.8 | 697.9-698.0 |

The 0.53% at 10 MHz is about four cycles a transfer, which is what saving, setting and
restoring PRIMASK costs. In `cycles.py --check` every SWD trace keeps its event count, and the
only change is that each sample now follows a rising edge rather than a falling one. Turning
the option on by default means re-blessing those five traces.

**What holds it back from the default.** A transfer stays masked for its whole length: 3 us at
16.67 MHz, 46 us at 1 MHz, longer below. SWO receives by interrupt into a 16-entry FIFO
(`RTE_USART3_DMA_EN 0`), which fills in about 20 us at its 8 MHz maximum. So SWO streamed fast
alongside slow SWD would drop bytes, where today it does not. (No longer: SWO is captured by DMA
since 2026-10-19, which a mask cannot hold up.) VCOM is polled from the main loop
and is not affected. The right shape is after the edge with a mask at the fast settings and
before the edge, unmasked, at the slow ones. That is a second copy of the delayed path, and as
unrolled C it does not fit in SRAM. Generating the bit paths makes it free, and
modelling the pin round trip in `cycles.py` comes first.

Four-wire JTAG reads TDO, which changes on the falling edge, before the rising one: half a period
rather than a whole one.

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

Done 2026-10-19, after a fix: it had never received a byte.

Open the CDC port and talk to a target's UART at a few baud rates, including 921600 and above.

Proves `vcom_init` took Flexcomm 0 back from the board support, and that the fractional divider
reset actually fixed the 2.4% error at a megabaud. Unlocks the integrated UART
from probe-rs.

## 7. SWO

Ask for SWO at 8 MHz and below, and capture.

**Done 2026-10-19, on the dual-core image's DMA capture (`src/swo.c`).** Target:
LAUNCHXL-CC1312R1 over two-pin cJTAG, running `tools/swo_target/itm_counter.c`, which sends a
counter on ITM port 0 out of DIO16 (TDO) as fast as the port takes it, so every byte is checkable
and the target drops nothing. The analyser on the line read 1.0000 and 7.9999 Mbaud from
back-to-back frames, saturated, counter unbroken.

| through | rate | captured | counter |
| --- | --- | --- | --- |
| `dap.py swo`, 2 s | 1 MHz | 200,008 bytes, 100.0 KB/s | 40,000 words, no break |
| `dap.py swo`, 3 s, twice | 8 MHz | 2,400,064 and 2,400,049 bytes, 800.0 KB/s | 480,011 and 480,009 words, no break |
| `probe-rs itm swo`, debug build, 0.5 s | 1 MHz | 10,067 words after the decoder locked | no break |
| `probe-rs itm swo`, release build, 3 s | 8 MHz | 117,871 words decoded | one three-word blip, no reply lost |

`dap.py` polls with DAP_SWO_Data back to back: about 14,000 polls a second, each under the
508-byte maximum, so the host keeps ahead of the line. probe-rs decodes and prints every packet,
which in a debug build manages about 70 KB/s; behind that the probe overruns, says so, and the
stream resumes after the gap. probe-rs ignores the overrun bit and fails a read on the stream-error
bit, which a capture that starts mid-frame sets once.

The emulator still cannot run capture: CPU 0's clock setup never runs there, so Flexcomm 3 reads
0 Hz and SWO_Baudrate answers 0. The ring and rate arithmetic is tested on the host instead
(`tools/swo_ring_test.py`, in the gate).

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
`tools/baseline-armwire/known-overshoot.txt` as known rather than new, because they are a
property the code always had and the calibration merely made visible.

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

### Taken: TinyUSB's non-FIFO vendor mode, which keeps packet boundaries

A buffer size of zero selects it. A received packet arrives at `tud_vendor_rx_cb` with its own
length, and `tud_vendor_write` becomes one `usbd_edpt_xfer`. The command loop holds two request
slots and arms the receive endpoint only while one is free, so a command it cannot place is
refused by NAK rather than dropped.

| | FIFO mode | non-FIFO, two slots |
| --- | --- | --- |
| 1 command in flight | 581 KiB/s | 608 |
| 2 in flight | 646 | 669 |
| 4 in flight | times out | 568, no data lost |

### Through probe-rs, which is the host that matters

Everything above is `tools/dap.py`, which reads a DP register and touches no access port. The
numbers a user sees come through probe-rs, and `swd-throughput` measures those. At 10 MHz
asked, fitted ceiling over 64 to 4096 byte transfers:

| | stock | ours |
| --- | --- | --- |
| read32 | 469 KiB/s | 593 |
| write32 | 503 | 612 |

1.26x on reads and 1.22x on writes, against the same target through the same harness.

**probe-rs already pipelines, and it sizes the pipeline from what the probe reports minus one.**
The spare packet is where `DAP_TransferAbort` has to land: an abort exists to stop a transfer that
is running now, so it has to overtake the queue rather than join it. A probe advertising two
therefore gets a pipeline of one and no overlap at all, which is why `DAP_PACKET_COUNT` is three.
Measured, three runs each: reads 568 to 593 KiB/s and writes 584 to 612, every run of the deeper
pipeline ahead of every run of the shallower one.

**Our firmware was not honouring that contract.** The abort was queued like any other command, so
the flag was set only once the transfer it was meant to abort had finished. ARM's own MCU-Link
example sets it in the endpoint callback and never queues it; `tud_vendor_rx_cb` now does the
same.

### The fair comparison: probe-rs against the script, same instrument

The 11% between `dap.py` and probe-rs was not a like-for-like figure, so both were captured with
the marker and folded the same way. `dap.py` was given probe-rs's own block size, 126 words.

| | `dap.py`, 2 in flight | probe-rs, steady state |
| --- | --- | --- |
| wire | 94.6% | 82.0% |
| other commands | -- | 2.7% |
| firmware, clock idle | 0.5% | 0.1% |
| USB and the host | 4.9% | 15.4% |
| clocks a block command | 5,796 | 5,842 |
| wire a block command | 660.78 us | 661.39 us |
| gap after a block command | 34.98 us | 36.82 us |

**The block command is the same in both, to within a cycle.** 661.39 against 660.78 microseconds
of wire, and 5,842 clocks against 5,796 -- the 46 extra are exactly one more 46-clock transfer,
which is the posted read an access port needs and the DP read does not. Nothing in the firmware or
in probe-rs's block path is leaving anything there.

**What differs is how many commands a read is made of.** probe-rs issues 0.69 other commands for
every block command, and every command costs a round trip of about 40 microseconds whatever it
carries. That is where the throughput goes: 1.69 round trips per block of data instead of one.

So the remaining work is in probe-rs and not in the probe: fewer commands per block of data, not
faster commands. The largest gap seen was 2.1 ms against a 40 us median, so something occasionally
stalls far longer than the rest and is worth finding first.

Captures: `mculink-dap126-marker-2026-10-16.sal` and
`mculink-probers-marker-2026-10-16.sal`.

### What limits the pipeline, and why two in flight is the number

The depth a host can use is the number of request slots in `src/main.c`, set by `-DREQUEST_SLOTS`
and reported to the host as `DAP_PACKET_COUNT`. Built with 2, 4 and 8 slots and swept:

| host depth | 2 slots | 4 slots | 8 slots |
| --- | --- | --- | --- |
| 1 | 608 KiB/s | 612 | 632 |
| 2 | 669 | 669 | 669 |
| 4 | 568 | 669 | 669 |
| 8 | host times out | host times out | 667 |
| 12 | | | host times out |

**Throughput saturates at two in flight and stays there.** Four and eight slots measure the same 669 KiB/s
as two. The wire is 92% of a block read, so the link only has to stay a little ahead of it, and
two commands is enough for that.

**Nothing above the slot count is the probe misbehaving.** With no free slot it NAKs, which is the
correct thing to do and loses nothing. What fails is a host that blocks on a write while refusing
to read replies, which deadlocks against its own backpressure -- `tools/dap.py` does exactly that,
and so would anything using synchronous bulk transfers. The probe recovers the moment the host
drains: a depth-8 run against two slots times out, and the next depth-2 run measures 669.5.

So the knob stays at two, which is honest and saturating. It is worth re-measuring against a
target that answers far above 10 MHz, where the wire would stop being 92% of a block read and the
link could become the limit again.

### Pipelining costs wire time, and it is still worth it

The same image measured at both depths, so only the host's behaviour differs:

| | 1 in flight | 2 in flight |
| --- | --- | --- |
| data bit | 15.120 cycles | 15.763 |
| wire a block | 330.03 us | 342.13 us |
| USB and the host | 61.01 us, 15.5% | 26.55 us, 7.1% |
| end to end | 632 KiB/s | 669 |

**A command in flight makes every data bit longer.** The pacing is a counted loop, so anything
that interrupts one simply makes the bit longer, and 0.64 cycles a bit is 12 us on a 64-word
block. It buys 34 us of idle, so it wins by a wide margin, but it is not free and the emulator
cannot see it.

**Masking the USB interrupt around a block was tried and reverted.** It recovered a third of the
interference -- 15.763 to 15.557 cycles, 342.13 to 338.95 us -- and moved the end-to-end figure
from 669.0 to 670.3, which is inside the noise. The other two thirds are not the interrupt, most
likely the USB controller's DMA competing with instruction fetch for the same SRAM. Against that
it would have left USB unserviced for the length of a block, which at 1 MHz and 127 words is
5.8 ms. Not worth it for 1% of wire time and nothing measurable.

The command loop also never truncates a reply now. `tud_vendor_write` writes what fits and
returns the count, and nothing was reading it.

### The LED: a timer's job, and what it costs the wire

**Stock fades the LED when nothing is using it.**

**Ours does the same with CTIMER3 and adds activity.** Breathing when nothing is using the probe,
blinking while commands arrive, fully lit when a host has said it is connected and is idle. The
period interrupt runs the policy and a command pays one byte store. `tools/image.py` runs the
interrupt against the image and checks all three, including that one command is visible whatever
tick it lands on; two mutations of the policy were confirmed to fail it.

Measured with `dap.py block --words 126 --blocks 400 --pipeline 2` at 10 MHz, three runs a build,
no spread within any build:

| build | KiB/s | against no LED |
| --- | --- | --- |
| no LED | 703.1 | |
| polled from the main loop, the first version | 701.6 | -0.21% |
| CTIMER3 PWM, 1 kHz tick | 702.5 | -0.09% |
| CTIMER3 PWM, 488 Hz tick, the one kept | 702.8 | -0.04% |
| 488 Hz with the handler in `.ramfunc` | 701.9 | -0.17% |

- **The polled version cost 1.5 us a command**, several times what its instructions explain. It
  ran on the path between one command and the next, which is exactly where the next one waits.
  Flash fetch misses are the likely reason and were not tested, because the timer design removes
  the call from that path whatever the cause.
- **The PWM's cost is its interrupt rate.** Each tick lands wherever the core is, usually in a
  transfer, and stretches it. Halving the rate recovered 0.23 KiB/s against a predicted 0.3.
  `LED_PERIOD` in `src/led.c` is the knob.
- **Moving the handler into SRAM made it worse.** It went in at the head of `.ramfunc`, shifted
  every bit path by 116 bytes and cost 0.11%. `src/hot.h` now says so.

**Confirmed by eye** on the bench unit with `tools/dap.py led`, which walks it through breathing,
blinking and lit and says which is due.

### The cost model now times the bus, and reproduces every SWD result so far

**A code lab, vendor 0x88.** A build with `-DCODE_LAB=ON` runs a function the host writes into
RAM, with interrupts masked. The function reads the cycle counter around the code under test, so
the call itself never enters the measurement. `tools/lab.py` assembles a case, runs it on the
probe and in the emulator at the same address, and compares the two. A lab build resets itself if
a function faults.

**Fetch from SRAM shares a bus with every GPIO access; fetch from SRAMX does not.** The bit paths
run from SRAM at 0x2000_0000, which the core fetches over S-AHB, the port its loads and stores
also use. SRAMX at 0x0400_0000 is fetched over C-AHB. The same code in each, every repeat
identical:

| case | SRAM | SRAMX |
| --- | --- | --- |
| 16 GPIO stores, 32-bit encodings | 32 cycles | 16 + 2 |
| 16 GPIO loads, 32-bit encodings | 32 | 16 + 2 |
| 16 stack loads, 16-bit encodings | 24 | 16 + 2 |
| 16 `mov.w` | 16 or 17, by alignment | 16 |

From SRAM every cycle carries one bus transaction, an instruction word or a data access. From
SRAMX the two run side by side. The +2 is the cycle counter's own read waiting for the last
access to finish. NOPs pair up and cost half a cycle each, so they are no use as timing filler.

**The pin round trip is seven cycles against the U575, and exact.** A DPIDR read generated as
straight-line code in SRAMX, one instruction a cycle, with the sample swept across the clock
period:

- A bit is readable when the sample load issues 7 to P + 6 cycles after the store that raised
  the clock, where P is the period. The read comes back one bit ahead at 7 cycles past its own
  edge and one bit behind at 6 cycles past the previous one, so both ends give the same number.
- No position gave a mixture of results.
- Without a target, a load sees the probe's own SWDIO store 3 cycles later. The target, its
  cable and the two buffers make up the other 4.

The window is a whole period wide at every rate. At the delay-free step's five-cycle period a
sample nine cycles after its edge would have two cycles either side. That sample comes after the
next rising edge, which a generator can place and compiled C cannot.

**The emulator now models fetch and the bus.** `tools/m33.py` decides when each instruction
issues, from its fetch, the instruction before it and the bus slots data has already taken. It is
fitted against the lab: 494 of 516 cases exact. `cycles.py` runs on it on request. A modelled SWD
target launches each bit on a rising edge and makes it readable 7 cycles later. Every read records
how far it sits from both ends of its window.

**With nothing fitted to them, it reproduces every SWD result the bench has:**

- the generated read sweep, all 70 positions, including which way each failure goes;
- vendor 0x87, at every position of both shapes, and its after-shape period to within a cycle
  over 32 clocks. The old model was two cycles a clock out;
- the block-size table at 16.67 MHz on the default build: the last read fails at every size, with
  acknowledge 3, and the loop's reads pass;
- the same for both variants tried on the bench, the tail shifted by a halfword and the tail
  inlined;
- the after-edge build reading clean at 16.67 MHz, and the delay-free step failing in both.

It also measures the tail's clock as the averaged capture did: three cycles high and six low
after ack0's rising edge, against four and five in the loop. Varying the parameters the fit does
not pin down leaves the result unchanged.

**Why the block's last read fails.** In the loop the rising-edge store's data access takes the
bus just as the next instruction word is due. The fetch waits a cycle, and that idle cycle
separates the rising and falling edges. The tail runs the same instructions at a different word
alignment and does not wait. Its falling edge comes a cycle sooner, the sample lands 6 cycles
after the edge, and it reads the bit before. The loop's own reads sit exactly at the window's
edge, so the loop passes by no margin at all.

**Two predictions, tested on the bench:**

- On the default build at 16.67 MHz, every write through `DAP_Transfer` misreads its acknowledge
  as 3, and reads pass. Confirmed, 50 of 50 in each of five orderings of reads and writes.
- `DAP_Transfer` reads at that rate sample data bits 1 and 2 a cycle early, which DPIDR's bits
  would hide. Wrong: the access port's TAR, written with values that would expose it, read back
  clean. So the model is a cycle out in that stretch of compiled code from SRAM.

**Refitted on code timed in isolation.** Timing each case between filler, so the closing
cycle-counter read waits on nothing in the body, removed most of what the first fit had got
wrong. From SRAMX the model now matches all 692 such cases at four alignments: every instruction a
cycle, loads and stores back to back, a computed value ready two cycles after its load, every
taken branch two cycles plus one for a straddling target. One more rule came out of it: an access
through the core's private bus, where the cycle counter is, waits two cycles behind an access over
S-AHB, and the reverse. From SRAM, straight-line code is still a cycle out at one alignment and
loops drift by up to a cycle an iteration.

**The wire agrees.** SWCLK trains from SRAMX, built from every GPIO store form (SET and CLR, NOT,
byte, word, and SET with a byte), span exactly their issue cycles. Every form reaches the pin at
the same delay, to a tenth of a cycle. The analyser cannot resolve pulses of one or two cycles.

**Generated reads run at 37.5 MHz, and the model predicts every one.** A read generated with each
sample placed a set number of cycles after the edge that launched its bit, and stored to a buffer
by the next free cycle, needs four instructions a clock. At every period from 4 to 8 cycles,
37.5 to 18.75 MHz, and every sample position, 45 configurations, the bench and the model agree on
the verdict and on the cycle count. The window's edges are included.

**Generated writes run at 50 MHz.** With the SWDIO store anywhere from one cycle before its rising
edge to one cycle after the previous one, every write landed, at every period from 3 to 10
cycles. Setup and hold at the target are each under a cycle.

**The MSPM0L1306 runs at 37.5 MHz too: its ceiling was ours.** On the second unit, now running
the lab build, the L1306 shows the same seven-cycle window start as the U575, at every period from
8 to 30 cycles. The scheduler, placing samples with it, ran clean against the L1306 at every period
from 4 to 40 cycles. "The target sets the ceiling, not the probe", above, is wrong for this part:
it fails above 10 MHz with today's firmware because of where the firmware samples.

**A scheduler that places everything from the model runs at every period.** Given a period and a
list of transfers, a prototype places every edge, SWDIO store and sample by the model alone, with
each sample at the middle of its window. Two writes and five reads, acknowledges, data and parity
all checked, ran clean at every period from 4 to 40 cycles, 37.5 to 3.75 MHz, ten times each. The
cycle count the probe measured equalled the model's at all 37 periods. A rate is then any whole
number of cycles: the furthest a request falls short is 20% at the top of the range, against 56%
today, and nothing runs faster than asked.

TIMING.md has the rules these give a generator. CODEGEN.md is the context for implementing it.

### The transfers are generated at run time, and the wire runs at every period from three cycles

**What it is.** A build with `SWD_CODEGEN` on, the default since 2026-10-20, writes its SWD
transfer code into SRAMX whenever the host changes the clock, the turnaround, the data phase or
the idle cycles. Every edge, data store and sample sits at a cycle the timing model chose. ARM's
command logic stays in C around it: posted reads, the RDBUFF reads and WAIT retries.
`src/swd_gen.c` has the details and CODEGEN.md the reasoning.

**The host now gets the rate it asks for, to one cycle.** The period is the shortest whole number
of cycles that is no faster than the request. Clocks the probe drives go down to 3 cycles, 50 MHz.
Clocks the target drives go down to 4, 37.5 MHz, because a read needs a sample and its use as well
as the two edges.

**It holds to ARM's wire.** `tools/equiv.py --ignore-samples` passes all 536 block cases at 50,
37.5, 25, 12.5, 10, 1 and 0.1 MHz, and all 360 single-register cases at 50, 37.5, 10 and 1 MHz.
Only where the samples sit differs: a sample can trail the edge that launched its bit by two clocks.

**Every period ran clean on the MSPM0L1306.** `lab/gensweep.py` reads DPIDR in blocks and writes
CTRL/STAT, checking 18 bits of each write. It found no error at any period from 3 to 40 cycles,
or at 3, 2, 1 and 0.1 MHz. probe-rs wrote random words to SRAM and read them back correctly at
every clock from 0.1 to 50 MHz.

**Throughput through probe-rs, 64 KB of flash, same probe and same part:**

| asked | generated | previous firmware |
| --- | --- | --- |
| 50 MHz | 1407 KiB/s | fails |
| 37.5 MHz | 1312 | fails |
| 25 MHz | 1172 | fails |
| 15 MHz | 830 | fails |
| 10 MHz | 629 | 577 |
| 5 MHz | 360 | 324 |
| 1 MHz | 80 | 79 |

The previous firmware fails above 10 MHz on this part because it samples too close to the edge,
which the section above found. At 37.5 MHz the wire is idle about half the time. A running block
reads a word in 210 cycles, which is 2.7 MB/s, and probe-rs gets 1.3. USB and the host take the
rest.

**The masked port register reaches the pin when CLR does.** The generator lowers the clock and
sets the data bit in one store to that register, which no earlier measurement covered. On the
analyser at the target, over 2.4 million clocks at P = 20, the low phases of clocks that fall that
way and of clocks that fall on CLR differ by 0.17 ns.

**Reads stop at 37.5 MHz while the core makes the clock.** A read clock is four instructions and
the core pairs none of them: the corpus timed a 16-bit load or store against a 16-bit add, in both
orders, at a cycle each.

**A timer can make the clock instead, and then reads run at 50 MHz.** SWCLK's pin is also
CTimer 0's match output, and in PWM mode that timer counts the core's own clock. It holds its
phase to the cycle at every period tried, from 2 to 10 cycles. With the timer making the
target's clocks, the core only loads and keeps each bit. A lab prototype read DPIDR correctly
2,000 times out of 2,000 with those clocks at 50 MHz, and the analyser shows them at exactly that
rate. Stopping the timer is the hard part. A store to any of its registers holds the core up four
cycles, which at 50 MHz always swallows one bit's sampling window, so the prototype stops the
timer with a DMA write instead, triggered by a second timer. None of this is in the generator
yet; `timer-swclk-2026-10-16.txt` has the measurements.

### The USB path at 50 MHz, and what a perfect host gets

At 50 MHz the wire stopped being most of a block read. probe-rs read SRAM at 1,397 KiB/s, half
of what the wire allows, so the question became how much of the rest is the probe.

**`tools/dap.py block --queue N` is the instrument.** It queues commands through libusb's
asynchronous API, so the probe never waits on the host. A queue of 2 already saturates, deeper
queues change nothing, and the host's CPU is 17% busy at 126 words: what it measures is the
probe and the link. 50 MHz, DP reads, queue 8, three runs each, identical to 0.1%:

| build | 1 word | 32 words | 126 words |
| --- | --- | --- | --- |
| generated transfers, as before | 22,965 commands/s | 1,349 KiB/s | 2,067 KiB/s |
| + memcpy a block at a time | 23,093 | 1,429 | 2,242 |
| + TinyUSB in SRAM | 30,399 | 1,618 | 2,351 |
| clang, same, buffers aligned | 33,476 | 1,686 | 2,383 |

The shipping configuration (ARM's C paths, 10 MHz) gains from TinyUSB in SRAM too: 25,235 to
35,103 one-word commands a second, 717 to 729 KiB/s at 126 words.

**Where a command's time went, from the marker** (126-word blocks, before → after the two changes):

| | before | after |
| --- | --- | --- |
| clocks running | 186.1 µs | 186.5 µs |
| before the first clock | 6.1 | 6.0 |
| after the last clock | 1.8 | 1.8 |
| gap to the next command | 45.5 | 14.6 |
| period | 239.6 | 208.9 |

The generator schedules 170.5 µs of clocking for 126 reads; the other 16 µs are interrupts, which
run between transfers because the transfers mask them. Folding the gaps between transfers shows
about 2.4 per command at 4.4 µs each from SRAM, 6 µs from flash. A one-word command spends 14 µs
before its first clock, mostly those same interrupts.

**newlib-nano's memcpy was 8 cycles a byte.** It is built for size and copies a byte per
iteration, and every reply passes through it into the endpoint buffer: 26 µs for a 507-byte
reply. Ours (`src/mem.S`) moves
32 bytes per ldm/stm pair. It saved 18.5 µs of the predicted 24.

**clang left the reply buffer on a halfword**, so its replies still went through the byte path
and its 126-word figure was 7% under gcc's until the buffers were aligned.

**What is left, per 126-word command:** 14.6 µs between commands (dispatch, TinyUSB's task, the
main loop), 6 µs before the first clock (command dispatch, still in flash), and about 10 µs of
interrupts inside the block. At 50 MHz that is 15% of the period. The copy into the endpoint
buffer can go entirely: the high-speed controller reaches only USB RAM, so the slots and the reply
would have to live there and the bulk endpoints be driven without TinyUSB's vendor class.

**probe-rs, against the same probe, is a separate gap.** Within a run of blocks it keeps the probe
as busy as the perfect host does (39 µs between blocks against 45 for dap.py on the old build).
But every 1 KB, where the access port's address wraps, it sends two small commands and waits for
each reply before continuing: about 170 µs of round trips per 1 KB against 470 µs of blocks.
That, not the probe, is most of the difference between 1,397 and the 2,000 the probe gave a
script.

Tools: `qsweep.sh`, `marker_gaps.py`, `isr_gaps.py` and the sweeps, with the
captures `usb-*.sal` beside them.

### DAP_Transfer ran from flash in the generated build

A host sends anything shorter than a block as one DAP_Transfer: an access under 1 KB, the address
write that opens each 1 KB window, every register poke during an attach. In the generated build
that command ran ARM's loop from flash, and each transfer re-checked the generator's
configuration with a memcmp from flash. `dap.py block --command transfer` times the same DP reads
both ways (50 MHz, 16, 64 and 120 words a command, synchronous, per word from the slope):

| | DAP_TransferBlock | DAP_Transfer |
| --- | --- | --- |
| before (`84100db`) | 1.64 µs/word | 7.24 µs/word |
| loop in SRAM, configuration checked once per command (`1a82cd5`) | 1.68 | 2.60 |

The prediction was 2 to 3 µs. Through probe-rs (`swd-throughput`, same probe-rs build),
a 256-byte read at 50 MHz went from 562 µs to 251, and 64 bytes from 188 to 104; reads of 1 KB and
up did not move. The fitted fixed cost per probe-rs call fell from 205 µs to 71, so most of what
looked like a probe-rs cost was this. The default build never had it: its DAP_Transfer was in
`.ramfunc` all along, which is why the generated build was slower than it below 1 KB.

The gate missed it because it measures blocks. It also hid the next problem: once
`swd_gen_ready` had one caller, clang inlined it, and the gate, which recognised a generating
image by that function's symbol, judged the clang image against the default build's baselines.

## What the bench has sized, and what is left

Ranked, with the items here that are since done marked as such, in BACKLOG.md.

- **MCU-Link Pro support.** Stock runs one image on every LPC55S69 MCU-Link, the Pro
  included, and picks features at runtime.
  The goal is one faster image for both boards, with the Pro's extras (second VCOM, SPI/I2C/GPIO
  bridge, power measurement, target supply) reachable from probe-rs rather than only from
  MCUXpresso IDE.
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
- **Make the generated SWD paths the default.** Done 2026-10-20, with the JTAG paths, once the
  MSPM0L1306 had run the SWD ones with its access port answering WAIT.
- **Measure the read latency per target.** Done on all three wires: the probe measures it
  at connect where the
  default leaves under two cycles, SWD at 37.5 and 50 MHz, four-wire from 12.5 MHz and two-pin from
  15 MHz on the CC1312R1.
- **Generate the JTAG and two-pin paths the same way.** They are 80 KB of SRAM and are still
  paced by ARM's delay loop.
