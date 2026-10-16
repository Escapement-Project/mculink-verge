# mculink-cjtag-fw

CMSIS-DAP firmware for the NXP MCU-Link, with IEEE 1149.7 two-pin cJTAG support that the stock
firmware does not have.

## Build

```
cmake -B build -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc . && ninja -C build
```

Produces `build/mculink_cjtag.{elf,hex,bin}`. `-DLTO=OFF` and `-DCMAKE_BUILD_TYPE=MinSizeRel` exist
for bisecting a miscompilation.

Clone with `--recurse-submodules`: TinyUSB supplies the toolchain file, startup code, linker script
and USB driver, and ARM's CMSIS-DAP supplies the protocol.

## Check

Nothing here has run on hardware. What stands in for that is an emulated Cortex-M33 running the
built image, driven through the real DAP command layer against a modelled target.

```
ninja -C build check                      # the gate, against what was just built
uv run tools/cycles.py --check            # the same thing by hand
uv run tools/cycles.py --clock 10 50      # throughput, wire period, where the time goes
uv run tools/cycles.py --explain          # name the code in the longest gap between edges
uv run tools/equiv.py <ref.elf> <new.elf> # hold a rewrite to a reference build, case by case
uv run tools/image.py               # the USB descriptors and the serial number
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
  ninja -C build-$t-$l && uv run tools/cycles.py build-$t-$l/mculink_cjtag.elf --check
done; done
```

Two of the four report the two-pin path clocking 3% fast at 10 MHz. That is known and recorded;
see OPTIMISATIONS.md.

`tools/equiv.py` is the heavier check, for replacing code rather than tuning it. It drives two
builds over 528 SWD block cases, 180 four-wire JTAG cases, 120 two-pin cases and, with
`--command transfer`, 300 single-register cases -- every acknowledge a target can give, every
turnaround length and chain layout, with and without the data phase and idle cycles, and the match
values, match masks and timestamps only `DAP_Transfer` reaches. It requires identical wire events
and identical replies from both.

## Where to read next

- `DESIGN.md` -- why this exists, the pin map, and the open risks.
- `OPTIMISATIONS.md` -- what was done for throughput, what was tried and rejected with its numbers,
  and what is left.
