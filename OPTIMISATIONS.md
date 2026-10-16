# Optimisation opportunities

What has been done, what was tried and rejected, and what is left.

## How these are measured

`tools/cycles.py` runs the built image on an emulated Cortex-M33, drives it through the real DAP
command layer against a target that always acknowledges, and counts what executes between one wire
edge and the next.

```
uv run tools/cycles.py --clock 10 50 --words 64        # throughput and where the time goes
uv run tools/cycles.py --explain                       # name the code in the longest gap
uv run tools/cycles.py --trace DIR                     # the wire event stream, to diff
uv run tools/cycles.py --calibrate                     # recover ARM's two clock constants
```

**Fact and model.** Instruction counts, wire clocks, pacing iterations and the wire event stream
are facts about the image. Cycles are a model: one per store, two per load, two per taken branch,
one otherwise, all overridable. Flash wait states, the USB round trip and bus contention are not
modelled, so every throughput figure here is a ceiling.

**It agrees with the bench where the bench can check it.** Asked for 1 MHz, it says the wire runs
at 0.94 MHz; the analyser measured 0.94 MHz on the stock image. It says a 10 MHz request and a
14 MHz request compile to the same path; the stock sweep measured 469.1 and 475.4 KiB/s at those
two settings.

**Every change below was checked against the wire event stream**, which records each clock edge,
each transition, each sample and each release. A change that leaves all six traced scenarios
byte-identical cannot have altered the protocol.

## Where this stands against the bar

Stock measured 476 KiB/s read and 502 KiB/s write at its best setting. KiB/s of wire time, 64-word
block, before and after tonight:

| | asked | before | after | |
| --- | --- | --- | --- | --- |
| SWD read | 10 MHz | 538 | 575 | +7% |
| SWD write | 10 MHz | 547 | 621 | +14% |
| SWD read | 50 MHz | 1161 | 1358 | +17% |
| SWD write | 50 MHz | 1387 | 1679 | +21% |
| JTAG read | 50 MHz | 1355 | 1681 | +24% |
| JTAG write | 50 MHz | 1630 | 2079 | +28% |
| cJTAG read | 10 MHz | 166 | 213 | +28% |
| cJTAG write | 10 MHz | 177 | 217 | +23% |

The bar is cleared on wire time at every setting, by 2.9x at the top. **Wire time is not the same
as throughput**: the stock capture put the USB gap at about 10% of a transfer at 14 MHz, and a
target that will not attach above 14 MHz makes the top of this table unreachable. The bench decides.

## Done

### Unroll the bit paths

ARM unrolls the packet phases of a transfer by hand and leaves the 32-bit data phases as loops, so
a data bit carried a loop branch and an induction variable that a packet bit did not: 22 cycles
against 16 at the same setting. `-funroll-loops` on SW_DP.c, JTAG_DP.c and our second copy of
JTAG_DP.c closes most of that.

SWD +7% to +21%, JTAG +9% to +30%, cJTAG almost nothing because its cost is framing rather than
loop overhead. Text grows 10.7 KB. Wire identical.

### Stop pacing the OScan1 frames twice

`JTAG_DP.c` wraps every clock in two `PIN_DELAY` calls. On the two-pin path the clock calls they
surround are no-ops, because `cJTAG_Frame` does the real clocking and its own pacing, so a frame
paid eight delays where six are wanted.

cJTAG +23% to +32% wherever the clock is paced, +5% where it is not. The image gets smaller. Wire
identical.

## Tried and rejected

Recorded because each one is the obvious next thing to try.

### Fusing the data bit into the falling clock edge

The idea: stage the data bit, then write it and the falling edge as one masked `MPIN` store, three
stores a bit down to two. Measured: **2% to 15% slower.** The staged value is a static the compiler
must keep in memory, which costs more than the store it saves, and `MPIN` is no cheaper than
`SET`/`CLR`. It also changes the idle level the probe leaves on SWDIO, and the shortest period on a
write dropped to 2 cycles, which is 72 MHz.

### Letting the optimiser inline the transfer into the block loop

A third of the time at a high clock is spent between transfers rather than clocking, and half of
that looked like call overhead. Raising GCC's inlining parameters until the transfer was inlined
cost 33 KB of text and bought **nothing**: the gap went from 78 cycles to 77, and the write path
got slower.

### Raising IO_PORT_WRITE_CYCLES

**This was recommended by an earlier version of this file, with a predicted 36% to 58%, and it is
wrong and unsafe.** The reasoning rested on a per-bit cost averaged over a whole transfer. Measuring
the wire period directly shows a transfer is not one period but several: at 10 MHz asked, the
packet bits run at 16 cycles, the data write bits at 17 and the data read bits at 19.

ARM's constant is calibrated against the *fastest* class, and for that class it is right: a packet
bit at 16 cycles is exactly the `2 * (IO_PORT_WRITE_CYCLES + delay * DELAY_SLOW_CYCLES)` the model
predicts. Raising the constant shortens the computed delay for every class, so the fastest class
would then exceed the clock the host asked for. It would also drop the switch to the delay-free
path from 36 MHz to 14.4 MHz, and that path emits 3-cycle periods, which is 48 MHz.

The real asymmetry is that the read bit costs three cycles more than the packet bit, and the fix
for that is to make the read bit cheaper, not to let the clock run away.

## Left, ranked

### 1. A seventh of a read is spent between transfers

Measured at 50 MHz asked: 78 cycles of dead wire between one transfer and the next, 13% of a read
and 15% of a write. `--explain` names it: 32 instructions in the tail and head of `SWD_TransferFast`
and 26 in the block loop that `DAP_ProcessCommand` inlined.

The block loop stores the result a byte at a time and re-reads the retry count per word. The
transfer tail checks parity, walks the turnaround, re-drives SWDIO and restores nine registers.
None of it is in a macro, so none of it can be reached the way the cJTAG framing was: it needs our
own copy of ARM's `DAP_SWD_TransferBlock` and `SWD_Transfer`.

Predicts: most of 13% to 15% at a high clock, 6% at 10 MHz. Against that, it is the first change
that would stop ARM's sources compiling as they ship.

### 2. The read data bit costs three cycles more than it needs

At the delay-free setting a packet bit is 3 cycles, a data write bit 5 and a data read bit 7. The
read bit pays a two-cycle byte load it cannot avoid and three ALU operations it can: `parity += bit`,
`val >>= 1`, `val |= bit << 31`.

Predicts: 7 cycles to 5 is 29% of the data phase, which is 36 of a transfer's 46 clocks. Same
objection as item 1: it means replacing ARM's loop body.

### 3. The pacing quantum straddles the clocks a host asks for

`DELAY_SLOW_CYCLES` is 3 and measurement agrees exactly, so the delay can only move in 3-cycle
steps per half period. At 10 MHz asked, one step too few gives 11.1 MHz, which is faster than the
host asked for, and one step too many gives 7.6 MHz. There is nothing in between.

A finer quantum, a short sled of `nop`s entered by a computed branch, would let the firmware land
just under the request instead of well under it. Predicts up to 27% at the settings where the
quantum straddles, nothing at the settings where it does not. The hazard is that the sled must
never overshoot, and the only instrument that can confirm that is the analyser.

### 4. The buffer direction pin toggles every cJTAG frame

`cJTAG_Frame` releases TMSC for the TDO packet and takes it back afterwards: four of the thirteen
GPIO writes a frame makes. Both are needed for correctness, but a sequence that captures no TDO
still pays them, and `JTAG_SEQUENCE_TDO` says when the host does not want it. The third packet must
still be clocked, because the target counts packets.

Predicts: four writes a frame down to two on TDO-less sequences, most of a TAP walk.

**Unproven and the riskiest item here.** If the target drives TDO regardless, not releasing means
contention. Measure before keeping.

### 5. DAP packet size above 512

512 is the USB 2.0 ceiling for a high-speed **bulk endpoint**, and it is not tunable. It is not the
ceiling for a DAP packet: `DAP_Info` reports packet size as a 16-bit value, a bulk transfer spans as
many endpoint packets as it needs, and probe-rs sizes its buffer from the reported value
(`packet_buffer_len` is `max_packet_size + 1`) rather than from the descriptor.

Predicts: **at most 10%**, probably half that, and nothing the emulator can confirm because the
host round trip is not modelled. Worth more once the wire is faster, since the same gap is a larger
share of a shorter transfer.

Two caveats. It deviates from stock, which reports 512, so a host that hardcodes rather than reading
`DAP_Info` would break: check pyOCD and OpenOCD first. And it costs RAM.

### 6. Run the bit paths from SRAM

Unrolling put another 10.7 KB into the straight-line bit paths, and they run from flash behind a
144 MHz wait-state setting. The emulator has no flash model, so this is the one item here it cannot
size at all. Cheap to try at the bench, cannot make the wire slower, and would need a linker script
of our own rather than the board support's.

## Before anything else, at the bench

Every figure the probe has ever been measured by is from the paced path, because 14 MHz was the
highest clock the target would attach at and the switch was above it. Get a target that will attach
above the switch point and run the sweep the stock baseline was taken with. If the delay-free path
does not deliver what the emulator says, nothing else on this list is worth doing until that is
understood.
