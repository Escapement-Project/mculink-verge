# swd-throughput

How fast is a probe's SWD, end to end through probe-rs, and **which part of it is slow**.

```
cargo run --release --manifest-path tools/swd-throughput/Cargo.toml -- \
    --probe 1fc9:0143 --chip MSPM0L1306 --csv sweep.csv
```

`tools/throughput.py` runs it as one half of a firmware comparison and keeps the result in
`bench/throughput/`. probe-rs is pinned to a revision of the fork that carries this firmware's
CMSIS-DAP changes. A probe-rs change moves these numbers, so move the pin on purpose.

## Why it is not one number

"Slower" has two shapes, and a single throughput figure cannot tell them apart:

- **Per byte.** The probe's bit loop, and the SWD clock it runs at.
- **Per transfer.** Command dispatch, the USB round trip, whatever the firmware does before the
  first clock edge. Paid in full whether the transfer carries 4 bytes or 4096.

So every arm runs at several transfer sizes and the times are fit to `a + b * bytes`. `a` is the
fixed cost of a transfer and `1/b` is the ceiling a large one approaches. A firmware change that
reorders the bit loop moves `1/b`; one that adds a vendor command check moves `a`.

Sweeping the SWD clock on top of that says which of the two is binding. **Throughput that stops
rising with the clock is bound by something off the wire**, and a slower bit loop would then cost
nothing measurable at all. That is the question worth answering before touching a probe's firmware.

## Arms

`read32`, `write32`, `read8`, `write8` at each size and clock, plus `rt`: single-word reads timed
one at a time. Nothing can be batched in `rt`, so it is one round trip per word and it is the
figure most sensitive to per-command overhead.

The `of wire` column compares the fitted ceiling against an idealised SWD wire at the clock the
probe actually took — 46 clocks a word, being the 8-bit request, the turnarounds, the acknowledge,
32 data bits and parity. A probe near 100% there is limited by the wire and nothing else can be.

## What it does to the target

The core is halted (pass `--no-halt` to leave it running, which puts the target's own bus traffic
into the measurement). The region under test is read first and written back before exit, so the
write arms do not leave the part's RAM clobbered.

**Transfers must fit the RAM region they start in**, and this is checked rather than trusted: a
read past the end of RAM does not fail loudly, it returns words and times whatever the bus does
with an unmapped address. On a part with 4 KiB of SRAM the largest size is 4096, which leaves the
fit short of leverage — read the intercept with that in mind, and prefer a part with more RAM when
the per-byte figure is the one in question.

## Reading the result

Quote the clock the probe reported, not the one asked for. A probe that can only divide its own
clock will round down, and charging it for the difference makes every figure look worse than it is;
the tool warns when the two differ and uses the reported one in the wire comparison.
