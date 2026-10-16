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
| SWD read | 10 MHz | 538 | 614 | +14% |
| SWD write | 10 MHz | 547 | 645 | +18% |
| SWD read | 50 MHz | 1161 | 1624 | +40% |
| SWD write | 50 MHz | 1387 | 1862 | +34% |
| JTAG read | 10 MHz | 616 | 665 | +8% |
| JTAG write | 10 MHz | 621 | 713 | +15% |
| JTAG read | 50 MHz | 1355 | 1639 | +21% |
| JTAG write | 50 MHz | 1630 | 2047 | +26% |
| two-pin read | 10 MHz | 166 | 208 | +25% |
| two-pin write | 10 MHz | 177 | 215 | +21% |
| two-pin read | 50 MHz | 283 | 631 | +123% |
| two-pin write | 50 MHz | 290 | 603 | +108% |

The bar is cleared on wire time at every setting, by 3.4x at the top. **Wire time is not the same
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

### Make the SWD data phase cost two operations a bit, not four

ARM's loop carries a running parity, shifts the word down and ors each bit into the top. The parity
is only ever used one bit wide at the end, so it can come off the finished word; and with the phase
unrolled each bit goes straight to its own position, which the barrel shifter folds into the or.

A read bit goes from 7 cycles to 5 and a write bit from 5 to 4, against 3 for a packet bit, which
is the floor. 64-word blocks gain 14% to 41%. The shortest period on the wire is unchanged, so the
probe still never clocks faster than it was asked to.

`src/swd_dp.c` owns `SWD_Transfer`; SW_DP.c still compiles unchanged and still supplies the two
sequence commands. `tools/equiv.py` holds it to ARM's behaviour across 528 cases.

### Give the two-pin path a copy without the pacing loop

Asked for 50 MHz it delivered 16, because `cJTAG_Frame` paced with the slow primitive whatever the
host asked for and above the switch point that is still a loop iteration per half period.
JTAG_DP.c is now built three times rather than twice -- four-wire, two-pin paced, two-pin not --
and the choice is made once per call.

It has to be made there. Putting the branch inside the frame costs a cycle in the paced copy, which
is enough to take a 10 MHz request from 9.60 MHz to 10.29 and past what it asked for. Three
arrangements were tried, including keeping the delay-free variant out of line, and all three moved
that cycle.

Reads go from 283 KiB/s to 631 above the switch point, with the fastest edge at 48 MHz against the
50 asked. Below it the extra copy costs 2% to 3%, and SWD and four-wire JTAG lose up to 2% to the
same code growth. Text grows 11 KB.

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

### The same rewrite on four-wire JTAG

What works for SWD does not work here, and only measurement says so. Unrolling JTAG's read phase
the same way takes a read bit from 6 cycles to 5 as predicted, and **loses 5% to 10% overall**:
the gap between one transfer and the next grows from 63 cycles to 97, more than the data phase
saves.

The difference from SWD is that ARM already unrolls SWD's packet phase by hand, so that function
was large and its register pressure already paid for. JTAG's was not, and unrolling 31 reads keeps
the accumulator live across all of them.

Measured with the chain at one device and at two, with the bypass bit before and after the data, so
the branches a rewritten data phase is least likely to reach were covered.

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

## A correctness finding that outranks all of this

**Four-wire JTAG clocks the target faster than the host asked for.** The emulator now reports the
shortest period in a transfer, which is the only one that can break anything, and compares it with
the request:

| asked | SWD | JTAG | two-pin |
| --- | --- | --- | --- |
| 0.5 MHz | 0.49 | 0.50 | 0.49 |
| 1 MHz | 0.98 | 0.99 | 0.98 |
| 2 MHz | 1.92 | 1.95 | 1.92 |
| 5 MHz | 4.36 | 4.50 | 4.36 |
| 10 MHz | 9.60 | **10.29** | 9.60 |
| 20 MHz | 16.00 | 18.00 | 16.00 |
| 50 MHz | 48.00 | **72.00** | 48.00 |

SWD and the two-pin path are within the request at every setting, and their numbers are identical
because both pace with the same `clock_delay` and both spend three stores on a bit or a packet. JTAG is 3% over
at 10 MHz and 44% over at 50.

The cause is structural rather than a slip. `Set_Clock_Delay` models a bit as
`2 * (IO_PORT_WRITE_CYCLES + delay * DELAY_SLOW_CYCLES)` with one constant for every protocol, and
a SWD bit really is three stores while a bare `JTAG_CYCLE_TCK` is two. The constant is calibrated
for three. ARM's reference has the same shape.

**This is a decision, not a fix, because both ways out cost throughput.** Measured:

- `DELAY_FAST_CYCLES = 1` puts a NOP in each half of the delay-free path. It fixes the 50 MHz case,
  leaves 10 MHz and 30 MHz still over, and costs SWD **22%** at the top end, because SWD shares the
  same delay.
- Overriding `PIN_DELAY_FAST` for the four-wire copy of JTAG_DP.c alone fixes everything above the
  switch point and leaves SWD untouched, at about **22% of JTAG's top-end throughput**. The 3%
  overshoot below the switch point stays.
- Doing nothing keeps the throughput and leaves a probe that will overdrive a target asked for a
  clock near its limit.

Nothing has been changed. A 3% overshoot is inside any target's margin; 44% is not, and the setting
that produces it is one the bench has never reached.

## The pacing has no margin, and the compiler decides which side of the line it lands on

A 10 MHz request needs a period of at least 14.4 cycles at 144 MHz. The firmware emits 15 or 14
depending on nothing more than optimisation flags. Running the gate against all four build
configurations:

| | text | two-pin at 10 MHz |
| --- | --- | --- |
| -O3, LTO (ships) | 66,672 | 9.60 MHz, within |
| -O3, no LTO | 59,472 | **10.29 MHz, over** |
| -Os, LTO | 33,128 | **10.29 MHz, over** |
| -Os, no LTO | 37,600 | 9.60 MHz, within |

The build that ships is on the right side of it, and the two documented escape hatches are not
reliably. Nothing is wrong with the arithmetic: `Set_Clock_Delay` rounds the delay up and the
model is sound. What is missing is margin. A period that lands within one cycle of the limit is
decided by instruction scheduling, and the frame's cost is not something `Set_Clock_Delay` knows
about -- the two-pin path borrows `clock_delay` without ever telling anyone what a packet costs.

The fix is to give the two-pin frame a pacing calculation of its own, with a cycle of headroom,
rather than borrowing one calibrated for a four-wire bit. Until then, **check a build before
flashing it**: `ninja -C build check` is the whole test.

This is the same shape as the four-wire JTAG overshoot above, and the same root cause: one clock
model serving paths that do not cost the same.

## Left, ranked

### 1. A seventh of a read is still spent between transfers

Measured at 50 MHz asked: around 70 cycles of dead wire between one transfer and the next, 13% of a
read. `--explain` names it: the tail and head of the transfer function, and the block loop that
`DAP_ProcessCommand` inlined. The block loop stores the result a byte at a time and re-reads the
retry count per word; the transfer tail checks parity, walks the turnaround, re-drives SWDIO and
restores nine registers.

Letting the optimiser inline the transfer into the loop does not touch it, and the JTAG result
above says making the function bigger makes this worse. What is left is making the loop itself
cheaper, in our own copy of `DAP_SWD_TransferBlock`.

Predicts: most of 13%, and the risk that it goes the way the JTAG rewrite went.

### 2. The read bit is two cycles above the floor

A packet bit is 3 cycles, a data read bit is now 5. The two are a byte load from the GPIO, which
cannot be avoided, and the or that places the bit. There is nothing obvious left; recorded so the
floor is written down.

### 3. Nothing is left in the pacing itself

Recorded so it is not tried again. `DELAY_SLOW_CYCLES` is 3 and the measurement agrees to two
decimal places, so the delay moves in 3-cycle steps and a finer quantum looks like free speed. It
is not. What the delay has to satisfy is the *shortest* period in a transfer, and at every setting
that period already lands just under the clock asked for: 9.60 MHz against 10, 48.00 against 50.
The firmware is already as fast as the request allows on its fastest bit.

The gap between that and what a transfer actually averages is entirely items 1 and 2 -- the slow
classes costing more than the fast one. There is no third thing.

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

Predicts: **at most 10%**, probably half that, and all of it on the host side. The firmware's own
per-command cost is already negligible: a 255-word block runs at 1647 KiB/s against 1639 for a
127-word one, so doubling what fits in a packet is worth 0.5% of wire time. Everything else a
larger packet could buy is USB round trips, which the emulator does not model.

Worth more once the wire is faster, since the same gap is a larger share of a shorter transfer.

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
