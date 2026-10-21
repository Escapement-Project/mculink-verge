# mculink-verge

CMSIS-DAP firmware for the NXP MCU-Link, with IEEE 1149.7 two-pin cJTAG support that the stock
firmware does not have.

## Limits

The fastest each transport runs, on the MCU-Link and the MCU-Link Pro alike, as of 2026-10-21,
beside NXP's stock firmware on the same probe.
This table is the record of where the firmware stands: a change that moves one of these limits
updates it in the same commit, so the history of this table is the history of the optimisations.

| Transport | Ours | Ours depends on | NXP's stock V3.172 |
| --- | --- | --- | --- |
| SWD writes | 50 MHz | Nothing | 10.7 MHz |
| SWD reads | 37.5 MHz | Nothing | 9.6 MHz |
| JTAG, four wires (reads and writes) | 30 MHz | The target's TDO delay | Not measured yet |
| cJTAG, two pins (reads and writes) | 50 MHz per wire packet, 8.3 MHz per JTAG clock | The target's TDO delay | Not supported |
| Console (VCOM) | 19.2 Mbaud | Nothing | 3 Mbaud or more; its limit not measured yet |
| SWO | 24 Mbaud, UART encoding only | Nothing | Not measured yet |
| USB | High speed, 512-byte packets, 3 commands held | Nothing | 512-byte packets, 8 commands reported |

The core runs at 150 MHz, and every clock is a whole number of its cycles. A host that asks for a
clock gets it, or the next slower step: 30 MHz is 5 cycles, 18.75 MHz is 8. A request above the
fastest gets the fastest.

- **SWD.** A write clock takes 3 core cycles and a read clock 4. At 50 MHz asked, a transfer's
  request and write data run at 50 MHz and its read data at 37.5. The target's reply delay does
  not slow either.
- **JTAG, four wires.** The probe has to sample TDO after the target drives it, so a slow target
  lengthens the clock. The probe measures that delay when a host connects. Up to 8 core cycles
  (53 ns) the clock runs at 30 MHz, at 9 or 10 cycles at 25 MHz, and past that each cycle of delay
  adds one to the clock: 18.75 MHz at 12, 12.5 at 16. A CC1312R1 measures 7 after a pin reset
  and 11 after a system reset.
- **cJTAG, two pins.** The wire carries packets at up to 50 MHz, and three packets make one JTAG
  clock. That frame takes 18 core cycles at the CC1312R1's delay of 8, or 8.3 MHz. A target with
  a longer delay adds a cycle to the frame for each cycle of delay.
- **Console.** A console loops back clean at 19.2 Mbaud, and so do both of the Pro's at once.
  That is the fastest rate the UART's clock can make. Above 10 Mbaud is past NXP's limit (below).
- **Stock.** Its SWD figures are the data bits on the wire from 25 MHz asked, where they stop
  rising; at 10 MHz asked it runs at 6.4. From 25 MHz asked it also misreads acknowledges now and
  then, and with commands queued it fails at 50; up to 20 MHz asked it was clean, at 9.1 MHz
  reads and 9.4 writes. Its console figure is both of the Pro's consoles at once, the fastest
  tried. Its USB figures are what its `DAP_Info` says, not what it was seen to hold.
- **SWO.** Captured over `DAP_SWO_Data` in UART (NRZ) encoding up to 24 Mbaud, which is 2.4 MB/s,
  with both of the Pro's consoles running at 19.2 Mbaud alongside. A faster request is clamped to
  24 Mbaud. 24 Mbaud is the fastest the bench's trace source sends, so the probe's own limit is
  not found yet. There is no Manchester encoding and no streaming endpoint. The buffer holds
  7 KiB.

### Past NXP's limits

Some of these rates are faster than NXP specifies for the part. They ran clean on the bench, and
the host asks for the rate, so whoever runs the probe decides whether to use them or stay slower.

| Feature | NXP's limit | Ours past it |
| --- | --- | --- |
| Console | The UART is specified to 10 Mbit/s (LPC55S6x data sheet, 7.26.3.4) | Above 10 Mbaud: 12, 16 and 19.2 Mbaud ran clean |
| SWO | The same UART, to 10 Mbit/s | Above 10 Mbaud: 12, 16 and 24 Mbaud ran clean |
| SWO | The UART's clock divider is specified to 100 MHz (UM11126, 4.5.49) | Above 20.1 Mbaud, where nothing inside the divider's limit comes within 0.5% of the rate: 24 Mbaud ran clean with it at 120 MHz |

Where the figures come from: SWD from the wire on the bench (mculink-bench's README, "The wire").
JTAG and cJTAG from the clock length the firmware reports for each delay (vendor 0x8A, run in the
emulator); four-wire's 30 MHz is also measured on the bench (CJTAGGEN.md). The console from
loopbacks on the bench (BENCH.md §6). SWO from captures on the bench (BENCH.md §7). Stock's SWD
from the wire on the bench (mculink-stock-notes' `wire.md`, and mculink-bench's README), the rest
as BENCH.md §6 and mculink-bench's `results/throughput/2026-10-21-stock.json` record it.

## Build

```
cmake -B build -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc . && ninja -C build
```

Produces `build/mculink_cjtag.{elf,hex,bin}`. `-DLTO=OFF` and `-DCMAKE_BUILD_TYPE=MinSizeRel` exist
for bisecting a miscompilation.

Clone with `--recurse-submodules`: TinyUSB supplies the toolchain file, startup code, linker script
and USB driver, and ARM's CMSIS-DAP supplies the protocol.

## Check

These checks need no hardware. An emulated Cortex-M33 runs the built image, driven through the
real DAP command layer against a modelled target. What the probe measured on real targets is in
`BENCH.md`.

```
ninja -C build check                      # the gate, against what was just built
uv run tools/cycles.py --check            # the same thing by hand
uv run tools/cycles.py --clock 10 50      # throughput, wire period, where the time goes
uv run tools/cycles.py --explain          # name the code in the longest gap between edges
uv run tools/equiv.py <ref.elf> <new.elf> # hold a rewrite to a reference build, case by case
uv run tools/image.py               # the USB descriptors and the serial number
uv run tools/cycles.py --table     # KiB/s for every rate a host can ask for
uv run tools/window.py             # disassemble the gap that repeats once a word
```

`ninja check` runs `tools/image.py` as well, which asks what the built image says about itself:

- the USB descriptors, held to each other, because a configuration descriptor states a total that
  has to match the bytes after it and nothing in the build checks that;
- the clocks the board support leaves off, by running the boot-time setup against the register
  model and asking the clock driver what came out;
- the cycle counter behind `TIMESTAMP_GET`, which stays dormant until two bits are set;
- every command in the parity table, which has to answer rather than reject;
- that no request can make a reply land past the response buffer, with the canary placed by the
  buffer size in the image rather than by a number repeated in the tool;
- what `DAP_Info` reports, since probe-rs sizes its buffers from it;
- the baud rates SWO and the console can actually reach, against a bound tighter than the
  driver's, because a rate it merely accepts can still be too far out for a link;
- and the serial number, against a programmed unique id and an unprogrammed one.

Each of those is a bench failure that looks like something else: a device that does not appear, a
console that stays silent, a probe that stops answering for ninety seconds, a probe that resets
under a host asking for more than it can hold.

The wire gate requires seven things of a build:

- every transfer answers with the right data;
- a block reply carries the word count that was asked for, at one word and at a full packet as
  well as in the middle, since that count is derived from the response pointer rather than kept;
- the wire event stream matches `tools/baseline/` at both clock paths;
- the cJTAG activation puts the same edges on the wire it always has;
- so do the sequence commands a host uses to reset a line or walk a TAP by hand;
- the two-pin path emits no transition while the clock is high, which on a 1149.7 wire is an
  escape edge and the reason this firmware exists;
- and no wire clocks faster than the host asked for, beyond what
  `tools/baseline/known-overshoot.txt` already records.

`--bless` rewrites the baseline.

The escape hatches are not exercised by any of that, and they do break: inlining a transfer once
hid an uninitialised-variable warning that only the non-LTO builds could see. Build all four
before trusting a change.

```
for t in Release MinSizeRel; do for l in ON OFF; do
  cmake -B build-$t-$l -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc -DCMAKE_BUILD_TYPE=$t -DLTO=$l .
  ninja -C build-$t-$l && uv run tools/cycles.py build-$t-$l/mculink_cjtag_core1.elf --check
done; done
```

The transfer paths are generated at runtime by default. `-DSWD_CODEGEN=OFF -DJTAG_CODEGEN=OFF`
builds ARM's paths alone, and the check holds that build to `tools/baseline-armwire/` instead. It
finds ARM's paths clocking SWD and both JTAG wires faster than asked at 10 and 20 MHz, and
four-wire JTAG at 50 MHz too. That directory's `known-overshoot.txt` records them; see
OPTIMISATIONS.md.

`tools/equiv.py` is the heavier check, for replacing code rather than tuning it. It drives two
builds over 536 SWD block cases, 188 four-wire JTAG cases, 128 two-pin cases and, with
`--command transfer`, 360 single-register cases -- every acknowledge a target can give, every
turnaround length and chain layout, with and without the data phase and idle cycles, all sixteen
address patterns, block sizes from one word to a full packet, and the match values, match masks and
timestamps only `DAP_Transfer` reaches. It requires identical wire events and identical replies
from both.

The last two of those were added after the code they would have had to check was already written:
the suite used two request values, and the packet parity is taken over exactly the bits those two
fix; and every block case sent two words, while the loops derive the reply's word count from the
response pointer. Both gaps were found by asking what a change could break that nothing here
would notice.

## Jumpers

The function jumpers work as they do with NXP's firmware. Each one turns a function off. The probe
reads them once when it starts, so fit or remove one with the probe unplugged.

| function | plain MCU-Link | MCU-Link Pro | with the jumper fitted |
| --- | --- | --- | --- |
| VCOM | J4 | J14 | no target serial port. The Pro's second one, on J26, stays |
| SWD | J5 | J13 | no CMSIS-DAP interface, so no debugger can use the probe |
| USB-SIO | | J15 | nothing yet: this firmware has no USB-SIO bridge |

A function that is off is left out of the USB configuration, not shown as an interface that does
nothing. The interfaces after it are numbered down. CMSIS-DAP v2 is always interface 0 when it is
there, so the probe selector `probe-rs list` prints still works. A selector you wrote for the
CMSIS-DAP v1 interface, such as `1fc9:0143-3:<serial>`, stops matching once the VCOM jumper
moves that interface to 1.

With the SWD jumper fitted the probe still restarts into its bootloader when asked over USB, so
it can still be updated.

## Releases

`.github/workflows/release.yml` publishes a release when a tag starting with `v` is pushed. It
attaches three files in the layout verge, the update tool, reads: the image as an S-record, its
checksum, and a minisign signature whose trusted comment names the file and the tag. verge's
README says what it checks. The workflow has never run, and cannot until this repository is
public.

It has three jobs, so that the secret key never sits on a machine that ran code the build fetched:

- `build` makes the image and its checksum, and runs `ninja -C build check` on it. It has no
  secret. It runs in an Arch Linux container, which has the compiler the bench uses. The
  container is pinned by digest, and its packages come from Arch's archive for one day. The
  compiler changes only when a commit moves both.
- `sign` first checks that the `release` environment has a required reviewer, and stops if it has
  none. Then it signs the image. It is the only job that sees the key.
- `publish` attaches the three files to the tag's release, creating the release if there is none.
  It has no secret either.

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

## Where to read next

- `DESIGN.md` -- why this exists, the board, and where every reference
  lives.
- `BENCH.md` -- the reference to keep open at the bench, and what the probe measured on real
  targets.
- mculink-bench and mculink-stock-notes, beside this repository: throughput and the wire clock
  against stock, and what stock does as seen from outside.
- `TIMING.md` -- the timing rules the generated paths are placed by, each from a bench measurement.
- `CODEGEN.md` and `CJTAGGEN.md` -- how the SWD and JTAG paths are generated at runtime.
- `TIMERGEN.md` -- SWD reads at 50 MHz with the clock made by a timer: built, measured and removed
  (kept on branch `swd-timer-reads`).
- `PRO.md` -- MCU-Link Pro support: what runs on the Pro today, what is still to come, and how each
  step was checked.
- `PACING.md` -- how ARM's paths pace the wire clock, and what is wrong with it. Only the build with
  generation off still uses them.
- `OPTIMISATIONS.md` -- what was done to ARM's paths for throughput, and what was tried and
  rejected. Its figures predate the bench.
- `AGENTS.md` -- the rules for changing this code.
- verge, the flash and update tool, as a command line and a window, is a repository of its own:
  https://github.com/Escapement-Project/verge.
