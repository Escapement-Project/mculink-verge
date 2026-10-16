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
```

The gate requires six things of a build: every transfer answers with the right data, the wire
event stream matches `tools/baseline/` at both clock paths, the two-pin path emits no transition
while the clock is high, and no wire clocks faster than the host asked for beyond what
`tools/baseline/known-overshoot.txt` already records, and the cJTAG activation sequence puts the
same edges on the wire it always has, and the sequence commands a host uses to reset a line or
walk a TAP by hand put out what they always have. `--bless` rewrites the baseline.

`tools/equiv.py` is the heavier check, for replacing code rather than tuning it: it drives two
builds over 528 SWD cases, 180 four-wire JTAG cases and 120 two-pin cases -- every acknowledge a
target can give, every turnaround length and chain layout, with and without the data phase and idle
cycles -- and requires identical wire events and identical replies from both.

## Where to read next

- `DESIGN.md` -- why this exists, the pin map, and the open risks.
- `OPTIMISATIONS.md` -- what was done for throughput, what was tried and rejected with its numbers,
  and what is left.
