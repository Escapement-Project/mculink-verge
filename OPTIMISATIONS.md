# Optimisation opportunities

Found while building, to be spent at the bench once the probe works. Nothing here is urgent and
nothing here should be done before the firmware is correct.

## How these are measured now

`tools/cycles.py` runs the built image on an emulated Cortex-M33, drives it through the real DAP
command layer against a target that always acknowledges, and counts what executes between one wire
edge and the next.

```
uv run tools/cycles.py --calibrate              # recover the clock constants
uv run tools/cycles.py --wire cjtag --clock 10  # cycles per TMSC packet
```

**What is fact and what is model.** Instruction counts, delay-loop iterations and wire clocks are
facts about the image. Cycles are a model: one cycle per store, two per load, two per taken branch,
one otherwise, all overridable on the command line. Nothing below has been seen on an analyser.

The instrument agrees with the one bench measurement that can check it. It says a 10 MHz request
and a 14 MHz request compile to the same path, and the stock sweep measured 469.1 and 475.4 KiB/s
at those two settings.

Baseline, this build, SWD read, 16-word block, 144 MHz core:

| asked | on the wire | |
| --- | --- | --- |
| 1 MHz | 0.93 MHz | 93% |
| 5 MHz | 3.56 MHz | 71% |
| 10 MHz | 6.41 MHz | 64% |
| 14 MHz | 6.41 MHz | 46% |
| 20 MHz | 8.74 MHz | 44% |
| 50 MHz | 13.79 MHz | 28% |

## 1. IO_PORT_WRITE_CYCLES says 2 where the code costs 5

The headline. One constant, no code change, and it is wrong by more than a factor of two.

`DAP.c` models a half period as `IO_PORT_WRITE_CYCLES + clock_delay * DELAY_SLOW_CYCLES` and picks
`clock_delay` for a requested frequency from that model. Fitting the real image gives

```
cycles per bit = 10.47 + 3.00 * delay iterations per bit
```

so `DELAY_SLOW_CYCLES` at 3 is exactly right and `IO_PORT_WRITE_CYCLES` should be about 5, not 2.

It costs twice. The delay computed for a request is longer than it should be. And `Set_Clock_Delay`
switches to the delay-free path at `(CPU_CLOCK/2) / IO_PORT_WRITE_CYCLES`, which is 36 MHz on this
build instead of 14.4 MHz, so every request between those two numbers is stuck paying for a pacing
loop it does not need.

Setting it to 5, same build otherwise:

| asked | today | at 5 | |
| --- | --- | --- | --- |
| 1 MHz | 0.93 | 0.97 | +4% |
| 5 MHz | 3.56 | 4.18 | +17% |
| 10 MHz | 6.41 | 8.74 | +36% |
| 14 MHz | 6.41 | 8.74 | +36% |
| 20 MHz | 8.74 | 13.79 | +58% |
| 25 MHz | 8.74 | 13.79 | +58% |

**Do not ship it on the model alone.** If the real fixed cost is below five cycles a half period,
the probe clocks faster than it was asked to, which is the one failure mode a debugger cannot
diagnose. One analyser capture of the period at two delay settings solves for the real store cost;
re-run `--calibrate` with that figure and set the constant to what it says.

## 2. The requested clock quantises, so sweep by delay and not by megahertz

10 MHz and 14 MHz produce byte-identical paths, as do 20, 25 and 30. A sweep in round megahertz
measures the same thing several times and misses the steps. Step the requested clock so that
`clock_delay` changes each time; `--calibrate` prints the column to steer by.

The corollary is the cheapest lever on this list and needs no firmware change at all: **ask for more
than you want.** 50 MHz requested gives 13.79 MHz on the wire where 14 MHz requested gives 6.41.
Until item 1 lands, that is a host-side setting worth two-thirds of the probe's speed.

## 3. Three GPIO stores per bit, where two would do

The data bit, the falling clock edge and the rising clock edge are three separate stores. The data
and the falling edge could be one masked `MPIN` write covering both P0_0 and P0_2, since `MPIN`
writes only unmasked bits and both pins are on port 0.

Predicts: one store in three off a floor of 10.44 cycles per bit, so about 10% on the delay-free
path and proportionally less wherever pacing dominates. Worth doing after item 1, not before:
below the switch point the delay loop is most of the bit.

## 4. The OScan1 frames pay two pacing delays they do not need

`JTAG_DP.c`'s cycle macros call `PIN_DELAY()` around what, on the two-pin path, are no-op clock
calls — the real clocking and pacing happen inside `cJTAG_Frame`. Eight delays per frame where six
are wanted.

Measured, cJTAG read at 10 MHz requested: 27.70 cycles per TMSC packet now, 22.18 with the macro
delays suppressed. **20%.** On the delay-free path it is worth nothing, because `PIN_DELAY_FAST` is
already empty and there is no delay left to remove.

An earlier version of this file predicted this would roughly halve the frame time. It does not.

The fix is two `#define`s in `src/cjtag_jtag_dp.c`, next to the ones that already shadow the clock
calls:

```c
#define PIN_DELAY_SLOW(d) ((void)(d))
#define PIN_DELAY_FAST() ((void)0)
```

## 5. Two-pin costs 3.7 times four-pin per JTAG clock

Measured on the same 16-word block: 700 JTAG clocks four-wire against 2100 TMSC packets two-pin,
which is the 3x the format requires and nothing wasted. The rest is per-packet overhead — 27.70
cycles a packet against 22.45 a four-wire clock at 10 MHz requested, so 83 cycles per JTAG clock
against 22.5.

Item 4 takes that 3.7 down to 3.0, which is the floor. After it there is nothing left in the framing
and the remaining work is in the bit path, items 1 and 3.

## 6. The buffer direction pin toggles every frame

`cJTAG_Frame` releases TMSC for the TDO packet and takes it back afterwards: four of the thirteen
GPIO writes a frame makes. Both are needed for correctness — the target drives TDO whatever we do,
so the buffer has to be off — but a sequence that captures no TDO still pays them.
`JTAG_SEQUENCE_TDO` says when the host does not want it. The third packet must still be clocked,
because the target counts packets.

Predicts: four writes a frame down to two, so roughly 15% of the frame's stores, on sequences that
want no TDO — most of a TAP walk.

**Unproven and the riskiest item here.** If the target drives TDO regardless, not releasing means
contention on the wire. Measure before keeping.

## 7. DAP packet size above 512

512 is the USB 2.0 ceiling for a high-speed **bulk endpoint**, and it is not tunable. It is not the
ceiling for a DAP packet: `DAP_Info` reports packet size as a 16-bit value, a bulk transfer spans as
many endpoint packets as it needs, and probe-rs sizes its buffer from the reported value
(`packet_buffer_len` is `max_packet_size + 1`) rather than from the descriptor.

Predicts: **at most 10%**, probably half that. The stock capture says the wire is busy 90% of the
time at 14 MHz, with about 52 us of gap between 480-byte bursts; a 4096-byte read spends 416 us of
its 8.49 ms not clocking. Doubling the packet size recovers about half of that.

Worth more once the bit loop is faster, since the same gap is a larger share of a shorter transfer.
Worth nothing if done first.

Two caveats. It deviates from stock, which reports 512, so a host that hardcodes rather than reading
`DAP_Info` would break — check pyOCD and OpenOCD before relying on it. And it costs RAM: two buffers
at `DAP_PACKET_SIZE` here plus whatever `DAP.c` holds.

## 8. Compiler flags are not a throughput lever here

Recorded because it is the obvious thing to try and it does not work. The four builds, SWD read at
50 MHz requested:

| | text | cycles/bit |
| --- | --- | --- |
| -Os | 35,020 | 10.60 |
| -Os + LTO | 30,444 | 10.59 |
| -O3 | 43,676 | 10.51 |
| -O3 + LTO | 46,980 | 10.44 |

1.5% across a 54% size range. ARM's macros already unroll the bit path by hand and what is left is
GPIO stores, which no optimiser can remove. The build ships at -O3 with LTO anyway: the flash is
there, and the command layer and USB plumbing are not bit-bound even if the wire is.

If flash ever matters, `-Os` with LTO is the better corner of that table.

## 9. Measure the delay-free path on hardware, before any of this

Every figure from the bench is from the paced path, because 14 MHz was the highest clock the target
would attach at and the switch was above it. The emulator says the delay-free path runs at 13.79 MHz
read and 16.34 MHz write for a 20 MHz request once item 1 lands. Those are the numbers that decide
whether anything else here is worth doing.

First measurement at the bench: a target that will attach above the switch point, and the same sweep
the stock baseline was taken with.
