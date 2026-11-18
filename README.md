# mculink-verge

Open CMSIS-DAP firmware for NXP's MCU-Link and MCU-Link Pro debug probes, named Verge. It speaks
SWD, JTAG and cJTAG (IEEE 1149.7) in OScan1, OScan2, OScan5 and OScan6, captures SWO and runs the
probes' serial ports. Both probes get four DIO pins, and Verge Aux, a second interface for tools
to use beside a debugger. On the Pro it adds SPI and I2C bridges, a switched target supply and
energy capture. It runs on the standalone boards; an MCU-Link built into an evaluation board is not
supported.

[Verge's page](https://escapement-project.github.io/verge/) covers using it. This file covers
building, checking and releasing the firmware, and "Where to read next", at the end, lists the
other docs.

## Build

You need git, CMake 3.20 or later, Ninja, uv, and the Arm GNU toolchain: arm-none-eabi-gcc with its
binutils and newlib. The gate also needs the host's gcc and clang, with lld, clang-tidy and LLVM's
tools, and qemu-system-arm. Making a release by hand needs minisign.

Clone with `--recurse-submodules`: TinyUSB supplies the toolchain file, startup code, linker script
and USB driver, and ARM's CMSIS-DAP supplies the protocol. Then fetch the NXP SDK files TinyUSB's
LPC55 support uses:

```
uv run python external/tinyusb/tools/get_deps.py lpc55
```

Then build:

```
cmake -B build -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc .
ninja -C build
```

Produces `build/mculink_cjtag.{elf,hex,bin}`. `-DLTO=OFF` and `-DCMAKE_BUILD_TYPE=MinSizeRel` exist
for bisecting a miscompilation.

Build from a git checkout. The build string comes from `git describe`, and outside a checkout it is
`unknown`.

As it configures, TinyUSB's board support prints CMake deprecation warnings and
`IPO_SUPPORTED="NO"`. LTO is still on; `-DLTO=OFF` turns it off. The link warns that `_dvmd_tls.o`
and `sbrk.o`, from the toolchain's own libraries, have no `.note.GNU-stack` section.

## Check

These checks need no hardware. An emulated Cortex-M33 runs the image's DAP command layer against
a modelled target. Hardware results are in `MEASUREMENTS.md`.

```text
# The gate: host checks, then gcc and clang images
uv run tools/gate.py
# One image's checks, against what was just built
ninja -C build check
# The wire half of it, by hand
uv run tools/cycles.py --check
# Throughput, wire period, where the time goes
uv run tools/cycles.py --clock 10 50
# Name the code in the longest gap between edges
uv run tools/cycles.py --explain
# Hold a rewrite to a reference build, case by case
uv run tools/equiv.py <ref.elf> <new.elf>
# The USB descriptors and the serial number
uv run tools/image.py
# KiB/s on each wire for every rate a host can ask for
uv run tools/cycles.py --table
# Disassemble the gap that repeats once a word
uv run tools/window.py
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

Also check Release and MinSizeRel with LTO on and off: inlining changes which warnings appear.

```
for t in Release MinSizeRel; do for l in ON OFF; do
  cmake -B build-$t-$l -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc \
    -DCMAKE_BUILD_TYPE=$t -DLTO=$l .
  ninja -C build-$t-$l && \
    uv run tools/cycles.py build-$t-$l/mculink_cjtag_core1.elf --check
done; done
```

The transfer paths are generated at runtime, and a product image carries no other transfer path.
`-DSWD_CODEGEN=OFF -DJTAG_CODEGEN=OFF` builds ARM's paths alone, the reference the generated ones
are held to, marked `+armwire`; CMake refuses one option off without the other. The check holds that
build to `tools/baseline-armwire/` instead. It finds ARM's paths clocking SWD, JTAG and cJTAG faster
than requested at 10 and 20 MHz, and JTAG at 50 MHz too. That directory's `known-overshoot.txt`
records them. ARM's code computes one delay for every wire from a cost model of an SWD bit. A JTAG
clock makes one store fewer and runs fast, most of all at 50 MHz, and SWD and the cJTAG frame land
within a cycle of the request. The gate holds that build's cJTAG frames to `tools/jtagcheck.py`'s
wire rules with `--no-clock-rules`. `tools/equiv.py` compares levels, and a frame that drives TMSC
into the target's TDO packet still has the right ones.

Use `tools/equiv.py` to compare a replacement implementation with a reference build. It drives two
builds over 536 SWD block cases, 188 JTAG cases, 128 cJTAG cases in each format and, with
`--command transfer`, 360 single-register cases. They cover every target acknowledgement, every
turnaround length and chain layout, with and without the data phase and idle cycles, all sixteen
address patterns, block sizes from one word to a full packet, and the match values, match masks and
timestamps only `DAP_Transfer` reaches. It requires identical wire events and identical replies
from both. `--wire all` runs every matrix for every wire, and the gate runs that for each compiler.

## When a build misbehaves

| symptom | first suspect |
| --- | --- |
| comes up as `1fc9:0021`, not as a probe | the ISP jumper is still fitted. Check this first |
| device does not enumerate at all | the unique-id read at `0x0009_FC70` in `src/ser_num.c`. If that region faults it faults inside `GetSerialNum`, before USB comes up |
| enumerates, console silent | `FRO_HF` or its divider not running. `board_clocks_init` starts both; a Flexcomm with no clock reports no error and never reaches a baud rate |
| enumerates, SWO silent | same clock, and NXP's driver also waits on a status bit |
| probe stops answering for about ninety seconds | `DAP_Delay(0)`, ARM's pacing loop counting down from 2³². Guarded in `src/dap_dispatch.c`, so seeing this means the guard is not in the image you flashed |
| every command times out after one | a stalled bulk endpoint from `DAP_TransferAbort`. `usb.core.Device.reset()` |
| attaches but the target behaves oddly at a high clock | the probe may be clocking faster than asked. A product image never does, since the gate refuses it; a `+armwire` build can ("Check", above) |

## Releases

`.github/workflows/release.yml` publishes a release when a tag starting with `v` is pushed. It
attaches three files in the layout verge, the update tool, reads: the image as an S-record, its
checksum, and a minisign signature whose trusted comment names the file and the tag. verge's
[Updating and
recovery](https://github.com/Escapement-Project/verge/blob/HEAD/docs/updating.md#what-a-release-carries)
says what it checks. `LICENSE`, `THIRD-PARTY-NOTICES.md` and newlib's licence files go with them.

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
arm-none-eabi-objcopy -O srec build/mculink_cjtag.elf \
    mculink-verge-v0.3.0.s19
sha256sum mculink-verge-v0.3.0.s19 > mculink-verge-v0.3.0.s19.sha256
minisign -S -s verge.key -m mculink-verge-v0.3.0.s19 \
    -t "file:mculink-verge-v0.3.0.s19 tag:v0.3.0"
```

### The board declaration

An image declares the boards it runs on in a 16-byte record that starts with a magic value. The
firmware places it after CPU 0's vector table, and verge looks at every 4-byte-aligned address in
the image. Fields are little-endian:

| offset | size | field |
| --- | --- | --- |
| 0 | 8 | the ASCII bytes `VERGEIMG`, with no terminator |
| 8 | 1 | format, 1 |
| 9 | 1 | the record's length in bytes, 16 at format 1 |
| 10 | 2 | zero |
| 12 | 4 | boards: bit n set means the image runs on board n |

The board numbers are the ones Verge's identification reply uses: 0 for the standalone MCU-Link, 1
for the MCU-Link Pro and 2 for an on-board MCU-Link. verge ignores board bits it does not know.
Later formats add fields at the end, read only in the formats that define them. verge refuses two
declarations in one image, format 0, a length under 16, and a declaration that runs past its part of
the image. `verge inspect IMAGE` shows the record.

## Licence

Apache-2.0, in `LICENSE`. Files changed from Arm's CMSIS-DAP carry Arm's notice and say what was
changed. Two files adapted from NXP's SDK drivers, `src/vcom_line.h` and `src/energy_identity.c`,
carry NXP's notice and stay under its BSD-3-Clause licence. The image also contains TinyUSB (MIT),
NXP's SDK drivers (BSD-3-Clause) and newlib-nano: `THIRD-PARTY-NOTICES.md` has their notices.

## Not done yet

- On the Pro: the analog trace input and the LPC804 (BOARDS.md).
- SWO over a streaming endpoint, from the ring already in USB SRAM.
- A cheaper read-latency measurement at connect. On a CC1312R1 it costs a JTAG connection 3.4 to
  5.8 ms, and a cJTAG one 2.9 to 4.5 ms.
- The generated scan engine as a loop rather than 32 near-identical copies, which would shrink
  the generated code at slow clocks.

## Where to read next

For using the probe and for host authors:

- [Wires and clocks](docs/wires.md): which hosts drive each wire, the clock you get for the one you
  ask for, and how fast each wire runs beside NXP's firmware.
- [Trace and serial ports](docs/trace-and-serial.md): SWO capture and the target's serial ports.
- [Probes and jumpers](docs/probes.md): what each probe has, and what each jumper turns off.
- [The Pro](docs/pro.md): the target supply, the DIO pins, the SPI and I2C bridges, and Verge Aux.
- [Host interface](docs/host-interface.md): the vendor commands, for tools that drive the probe.
- [cJTAG targets](docs/cjtag-targets.md): bringing a TI cJTAG target up from a host.

How the firmware works, in the order to read it:

- `ARCHITECTURE.md`: what each core runs, and the path from each USB interface to its feature.
- `CONCURRENCY.md`: the state the cores, interrupts and DMA share, and what orders each access.
- `BUSES.md`: which code uses which bus port, and when.
- `TIMING.md`: the timing rules the SWD generator places instructions by.
- `CODEGEN.md`: generating the SWD, JTAG and cJTAG transfers at run time.
- `CJTAG.md`: cJTAG as the firmware uses it.
- `BOARDS.md`: both boards' pins and straps, and the Pro's circuits.
- `STACK.md`: where each core's stack sits, and how an overflow is caught.

And beside them:

- `MEASUREMENTS.md`: the measurement log, newest first.
- `BENCH.md`: the bench the firmware is checked on.
- `AGENTS.md`: rules for changing this code.
- [mculink-bench](https://github.com/Escapement-Project/mculink-bench) and
  [mculink-stock-notes](https://github.com/Escapement-Project/mculink-stock-notes): throughput and
  the wire clock against stock, and what stock does as seen from outside.
- [verge](https://github.com/Escapement-Project/verge): the CLI and GUI for firmware updates.
