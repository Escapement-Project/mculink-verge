# mculink-verge

Open CMSIS-DAP firmware for the NXP MCU-Link and MCU-Link Pro. It supports SWD, JTAG and IEEE 1149.7
cJTAG in OScan1, OScan2, OScan5 and OScan6 formats. Stock firmware does not support cJTAG.

It runs on the standalone boards, not on MCU-Links built into evaluation boards. Both provide SWO
capture and one USB serial port. The Pro adds a second serial port, four GPIO pins, SPI and I2C
bridges, switched target power and energy measurement.
The [capability guide](CAPABILITIES.md) lists board conditions and the
[host interface](HOST-INTERFACE.md) documents the supported vendor commands.

## Limits

These figures were measured or reported on 2026-10-20 and 2026-10-21, on older builds than this
source. The SWD rows show the build measured beside stock V3.172 on 2026-10-20, and mculink-bench's
[notes for that date](https://github.com/Escapement-Project/mculink-bench/blob/master/results/throughput/2026-10-20.md)
record the builds and methods.

| Transport | Ours | Evidence | Ours depends on | NXP's stock V3.172 |
| --- | --- | --- | --- | --- |
| SWD writes | 50 MHz | Bench, 2026-10-20 | Nothing | 10.7 MHz |
| SWD reads | 37.5 MHz | Bench, 2026-10-20 | The Pro's energy capture: 30 MHz while it runs (bench, 2026-10-29) | 9.6 MHz |
| JTAG (reads and writes) | 30 MHz | Bench and firmware timing report | The target's TDO delay; the Pro's energy capture (emulator, 2026-10-29) | Not measured yet |
| cJTAG (reads and writes) | 50 MHz per wire packet; per JTAG clock in Shift, 7.9 MHz in OScan1 and OScan2, 9.4 MHz in OScan5 and OScan6 | Firmware timing report; target-dependent | The target's TDO delay | Not supported |
| Console (VCOM) | 19.2 Mbaud | Bench loopback | Nothing | 3 Mbaud or more; its limit not measured yet |
| SWO | 30 Mbaud, UART encoding only | Bench capture | Nothing | Not measured yet |
| USB | High speed, 512-byte packets, 3 commands held | Firmware report | The host's port: 64-byte packets on a full-speed one (bench, 2026-11-03) | 512-byte packets, 8 commands reported |

The core runs at 150 MHz. Each wire period is a whole number of core cycles, so the firmware
rounds a requested rate down to the next available step: 30 MHz is 5 cycles, 18.75 MHz is 8.
A request above the maximum gets the maximum. A request below 9 Hz gets `DAP_ERROR`, and the clock
stays as it was: its period would be more than 2^24 core cycles, longer than the generated paths
run.

- **SWD.** A write clock takes 3 core cycles and a read clock 4. At a requested 50 MHz, a transfer's
  request and write data run at 50 MHz and its read data at 37.5 MHz. The target's reply delay does
  not slow either. On the Pro, energy capture slows reads. While it runs, a read clock takes at
  least 5 cycles (30 MHz), or 6 for some reply delays. The capture's DMA can delay the probe's
  read of SWDIO by two cycles, and a 4-cycle clock leaves no room for that.
- **JTAG.** The probe has to sample TDO after the target drives it, so a slow target
  lengthens the clock. The probe measures that delay when a host connects. Up to 8 core cycles
  (53 ns) the clock runs at 30 MHz, at 9 or 10 cycles at 25 MHz, and past that each cycle of delay
  adds one to the clock: 18.75 MHz at 12, 12.5 at 16. A CC1312R1 measures 7 after a pin reset
  and 11 after a system reset. On the Pro, energy capture can make these clocks up to two cycles
  longer while it runs, because its DMA can delay the probe's read of TDO by two cycles. Scans run
  at 25 MHz at 6 or 7 cycles of delay, 21.4 MHz at 8 and 18.75 MHz at 9 or 10.
- **cJTAG.** The wire carries packets at up to 50 MHz, and three packets make one JTAG
  clock. The probe samples TDO a cycle past the delay it measures, and each cycle adds one to the
  frame. A CC1312R1 measures 8 in OScan1, so that frame takes 19 core cycles, or 7.9 MHz (the
  generator's frame lengths, 2026-10-26). In OScan2, a clock outside Shift-DR and Shift-IR is a
  single packet. Through probe-rs that reads a CC1312R1 4 to 7% faster than OScan1. In OScan5, a
  clock in Shift-DR and Shift-IR is two packets, and the last one leaves with an escape. A CC1312R1
  measures 7 there, and that frame takes 16 core cycles, or 9.4 MHz. dap.py reads a CC1312R1 27%
  faster in OScan5 than in OScan1 at 12.5 MHz, 15% faster at 25 MHz and 6% faster at 50 MHz, where
  USB starts to set the rate. OScan6 sends a clock outside Shift as one packet and a clock in Shift
  as OScan5 does. On a CC1312R1 it reads level with OScan5 at 12.5 MHz and slower from 25 MHz, where
  the target answers WAIT more often to the shorter frames. The host selects one of the formats both
  the probe and the target offer, and `verge cjtag --formats` limits the probe's offer until it
  restarts.
- **Console.** Loopback passed at 19.2 Mbaud, including both Pro ports running together.
  This is the UART clock's highest rate. Rates above 10 Mbaud exceed NXP's specification (below).
- **Stock.** Its SWD figures are measured data-bit rates at requests of 25 MHz and above, where
  they stop increasing. At a requested 10 MHz, reads run at 6.4 MHz. At requests of 25 MHz and
  above it misreads some acknowledgements; queued commands fail at 50 MHz. Requests up to
  20 MHz passed, with data rates of 9.1 MHz for reads and 9.4 MHz for writes.
  Its console figure is the fastest tested with both Pro ports active.
  Its USB figures are what `DAP_Info` reports; we did not measure how many commands it holds.
- **SWO.** Captured over `DAP_SWO_Data` in UART (NRZ) encoding up to 30 Mbaud, which is 3 MB/s.
  This is the UART clock's highest rate, and a faster request gets it. The
  standalone MCU-Link captured 30 Mbaud with its console running at 19.2 Mbaud. The Pro
  captured up to 24 Mbaud with both consoles at 19.2 Mbaud; no trace source on the bench sends
  faster. There is no Manchester encoding and no streaming endpoint. The buffer
  holds 7 KiB.

### Past NXP's limits

Some rates exceed NXP's specifications. They passed the tests below. The host asks for the rate,
so whoever runs the probe decides whether to use them.

| Feature | NXP's limit | Ours past it |
| --- | --- | --- |
| Console | The UART is specified to 10 Mbit/s (LPC55S6x data sheet, 7.26.3.4) | Above 10 Mbaud: 12, 16 and 19.2 Mbaud ran clean |
| SWO | The same UART, to 10 Mbit/s | Above 10 Mbaud: 12, 16, 24 and 30 Mbaud ran clean |
| SWO | The UART's clock divider is specified to 100 MHz (UM11126, 4.5.49) | Above 20.1 Mbaud, where nothing inside the divider's limit comes within 0.5% of the rate: 21.4, 24, 25 and 30 Mbaud ran clean with it at 107 to 150 MHz |

SWD rates come from wire captures in mculink-bench's `results/wireclock/2026-10-21.md`. JTAG and
cJTAG rates come from the firmware's timing report (vendor 0x8A, run in the emulator); JTAG at
30 MHz was also measured on hardware (CJTAGGEN.md). Console loopbacks and SWO captures are
recorded in MEASUREMENTS.md. Stock SWD captures are in mculink-stock-notes' `wire.md` and
mculink-bench's `results/wireclock/2026-10-21.md`. Other stock figures are in MEASUREMENTS.md under
"The console" and in mculink-bench's `results/throughput/2026-10-21-stock.json`.

## Build

```
cmake -B build -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc . && ninja -C build
```

Produces `build/mculink_cjtag.{elf,hex,bin}`. `-DLTO=OFF` and `-DCMAKE_BUILD_TYPE=MinSizeRel` exist
for bisecting a miscompilation.

Clone with `--recurse-submodules`: TinyUSB supplies the toolchain file, startup code, linker script
and USB driver, and ARM's CMSIS-DAP supplies the protocol. Before the first build, fetch the NXP
SDK files TinyUSB's LPC55 support uses:

```
uv run python external/tinyusb/tools/get_deps.py lpc55
```

## Check

These checks need no hardware. An emulated Cortex-M33 runs the image's DAP command layer against
a modelled target. Hardware results are in `MEASUREMENTS.md`.

```
ninja -C build check                      # the gate, against what was just built
uv run tools/cycles.py --check            # the same thing by hand
uv run tools/cycles.py --engine --clock 10 50  # throughput, wire period, where the time goes
uv run tools/cycles.py --explain          # name the code in the longest gap between edges
uv run tools/equiv.py <ref.elf> <new.elf> # hold a rewrite to a reference build, case by case
uv run tools/image.py               # the USB descriptors and the serial number
uv run tools/cycles.py --engine --table  # SWD KiB/s for every rate a host can ask for
uv run tools/window.py             # disassemble the gap that repeats once a word
```

`ninja check` also runs `tools/image.py` to check:

- USB descriptor lengths and consistency;
- peripheral clocks after boot-time setup, using the register model and clock driver;
- cycle-counter setup for `TIMESTAMP_GET`;
- replies to every command in the parity table;
- response-buffer bounds, with a canary placed using the buffer size from the image;
- `DAP_Info` values used by probe-rs to size its buffers;
- achievable SWO and console baud rates, with a tighter tolerance than the driver accepts;
- serial numbers derived from programmed and unprogrammed unique IDs.

These checks cover failures observed on hardware: USB enumeration failures, silent consoles,
long response stalls and resets caused by requests larger than the probe can hold.

The wire gate requires six things of a build:

- every transfer answers with the right data;
- block replies carry the requested word count, at one word, a full packet and intermediate sizes;
- the wire event stream matches `tools/baseline/` at both clock paths;
- line-reset and TAP sequence commands preserve their wire events;
- the cJTAG path emits no unintended transition while the clock is high, which IEEE 1149.7
  interprets as an escape;
- no wire clocks faster than requested, beyond what
  `tools/baseline/known-overshoot.txt` already records.

`--bless` rewrites the baseline.

Also check Release and MinSizeRel with LTO on and off. An inlined transfer once hid an
uninitialised-variable warning that appeared only without LTO.

```
for t in Release MinSizeRel; do for l in ON OFF; do
  cmake -B build-$t-$l -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc -DCMAKE_BUILD_TYPE=$t -DLTO=$l .
  ninja -C build-$t-$l && uv run tools/cycles.py build-$t-$l/mculink_cjtag_core1.elf --check
done; done
```

The transfer paths are generated at runtime, and a product image carries no other transfer path.
`-DSWD_CODEGEN=OFF -DJTAG_CODEGEN=OFF` builds ARM's paths alone, the reference the generated ones
are held to, marked `+armwire`; CMake refuses one option off without the other. The check holds that
build to `tools/baseline-armwire/` instead. It finds ARM's paths clocking SWD, JTAG and cJTAG faster
than requested at 10 and 20 MHz, and JTAG at 50 MHz too. That directory's `known-overshoot.txt`
records them. ARM's code computes one delay for every wire from a cost model of an SWD bit. A JTAG
clock makes one store fewer and runs fast, most of all at 50 MHz, and SWD and the cJTAG frame land
within a cycle of the request.

Use `tools/equiv.py` to compare a replacement implementation with a reference build. It drives two
builds over 536 SWD block cases, 188 JTAG cases, 128 cJTAG cases in each format and, with
`--command transfer`, 360 single-register cases. They cover every target acknowledgement, every
turnaround length and chain layout, with and without the data phase and idle cycles, all sixteen
address patterns, block sizes from one word to a full packet, and the match values, match masks and
timestamps only `DAP_Transfer` reaches. It requires identical wire events and identical replies
from both. `--wire all` runs every matrix for every wire, and the gate runs that for each compiler.

Address-pattern and word-count coverage were added after the implementation. The original suite
used two request values that left parity inputs fixed. It also sent two words in every block
case, missing errors in counts derived from the response pointer.

## Jumpers

The function jumpers work as they do with NXP's firmware. Each one turns a function off. The probe
reads them once when it starts, so fit or remove one with the probe unplugged.

| function | plain MCU-Link | MCU-Link Pro | with the jumper fitted |
| --- | --- | --- | --- |
| VCOM | J4 | J14 | no target serial port. The Pro's second one, on J26, stays |
| SWD | J5 | J13 | no CMSIS-DAP interface, so no debugger can use the probe. The Pro keeps Verge Aux |
| USB-SIO | | J15 | disables the DIO, SPI and I2C bridges; the DIO pins on J19 stay inputs |

A function that is off is left out of the USB configuration, not shown as an interface that does
nothing. The interfaces after it are numbered down. CMSIS-DAP v2 is always interface 0 when it is
there, so the probe selector printed by `probe-rs list` still works. A selector for the
CMSIS-DAP v1 interface, such as `1fc9:0143-3:<serial>`, stops matching once the VCOM jumper
moves that interface to 1.

With the SWD jumper fitted the probe still restarts into its bootloader on a USB request, so
it can still be updated.

## The Pro

[CAPABILITIES.md](CAPABILITIES.md) says what each of the Pro's features needs, and
[HOST-INTERFACE.md](HOST-INTERFACE.md) has their commands.

- **DIO pins:** four GPIO pins on J19. `verge dio` reads and drives them.
- **SPI and I2C bridges:** vendor commands `0x91` and `0x92`.
- **Target supply:** switched by vendor command `0x8F`. `verge power` controls it.
- **Verge Aux:** a second vendor interface that carries the DIO, supply, energy and marker
  commands, so a tool can use them while a debugger holds CMSIS-DAP.

## Releases

`.github/workflows/release.yml` publishes a release when a tag starting with `v` is pushed. It
attaches three files in the layout verge, the update tool, reads: the image as an S-record, its
checksum, and a minisign signature whose trusted comment names the file and the tag. verge's
README says what it checks. `LICENSE`, `THIRD-PARTY-NOTICES.md` and newlib's licence files go with
them.

Build and signing run in separate jobs, so the secret key never sits on a machine that ran code
the build fetched:

- `build` makes the image and its checksum, and runs `ninja -C build check` on it. It has no
  secret. It runs in an Arch Linux container with the compiler used for the gate's baselines.
  The container is pinned by digest, and its packages come from a dated Arch archive. The
  compiler changes only when a commit moves both.
- `sign` first checks that the `release` environment has a required reviewer, and stops if it has
  none. Then it signs the image. It is the only job that sees the key.
- `publish` attaches the three files to the tag's release, creating the release if there is none.
  It has no signing key.

To make the key pair:

```
minisign -G -W -p verge.pub -s verge.key
```

`-W` leaves the secret key without a password, which the workflow needs. Keep `verge.key` out of
every repository. The second line of `verge.pub` is verge's `RELEASE_KEY`.

The repository needs these settings before the first tag:

- **The `release` environment** (Settings, Environments). At least one required reviewer, who
  approves each signing. The environment secret `MINISIGN_SECRET_KEY`, holding the whole of
  `verge.key`; keep it out of the repository's own secrets, where any job could read it. Without
  it the workflow fails rather than publish an unsigned image. Optionally the variable
  `MINISIGN_PUBLIC_KEY`, the second line of `verge.pub`; when it is set, the signature is checked
  before it leaves the signing job. And deployments limited to tags matching `v*`, so that no
  other ref reaches the key.
- **A tag ruleset** (Settings, Rules, Rulesets) on `refs/tags/v*`. Restrict creation, updates and
  deletion, with only the maintainers on the bypass list. A tag's own copy of the workflow decides
  what the signing job runs, so whoever can push a `v*` tag can change it. The reviewer is the last
  check: approve only a tag you know.

To make the files by hand instead:

```
arm-none-eabi-objcopy -O srec build/mculink_cjtag.elf mculink-verge-v0.3.0.s19
sha256sum mculink-verge-v0.3.0.s19 > mculink-verge-v0.3.0.s19.sha256
minisign -S -s verge.key -m mculink-verge-v0.3.0.s19 -t "file:mculink-verge-v0.3.0.s19 tag:v0.3.0"
```

## Licence

Apache-2.0, in `LICENSE`. Files changed from Arm's CMSIS-DAP carry Arm's notice and say what was
changed. Two files adapted from NXP's SDK drivers, `src/vcom_line.h` and `src/energy_identity.c`,
carry NXP's notice and stay under its BSD-3-Clause licence. The image also contains TinyUSB (MIT),
NXP's SDK drivers (BSD-3-Clause) and newlib-nano: `THIRD-PARTY-NOTICES.md` has their notices.

## Not done yet

- On the Pro: the analog trace input and the LPC804 (BOARDS.md).
- SWO over a streaming endpoint, from the ring already in USB SRAM.
- A cheaper read-latency measurement at connect. In OScan2 and OScan5 at 25 and 50 MHz it costs
  each session 10 to 15 ms.
- The generated scan engine as a loop rather than 32 near-identical copies, which would shrink
  the generated code at slow clocks.

## Where to read next

- `ARCHITECTURE.md`: what each core runs, and the path from each USB interface to its feature.
- `DESIGN.md`: design rationale, cJTAG support in ARM's code, and the board.
- `BENCH.md`: reading the target's supply, bench hazards, the marker header and failure modes.
- `MEASUREMENTS.md`: what the bench measured, newest first.
- [mculink-bench](https://github.com/Escapement-Project/mculink-bench) and
  [mculink-stock-notes](https://github.com/Escapement-Project/mculink-stock-notes): throughput and
  the wire clock against stock, and what stock does as seen from outside.
- `TIMING.md`: measured timing rules for the generated paths.
- `CODEGEN.md` and `CJTAGGEN.md`: runtime generation of SWD and JTAG paths.
- `BOARDS.md`: both boards' pins and straps, and the Pro's circuits.
- `BUSES.md`: which code uses which bus port, and when.
- `AGENTS.md`: rules for changing this code.
- [verge](https://github.com/Escapement-Project/verge): the CLI and GUI for firmware updates.
