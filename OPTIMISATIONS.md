# Optimisation opportunities

What has been done, what was tried and rejected, and what is left.

## How these are measured

`tools/cycles.py` runs the built image on an emulated Cortex-M33, drives it through the real DAP
command layer against a target that always acknowledges, and counts what executes between one wire
edge and the next.

Two of the changes below came off `tools/window.py`, which groups every gap by length and
disassembles one of the length asked for. The longest gap is the first word of a block and is paid
once; the one that repeats is what costs. Neither change was visible in a disassembly of the
function, because the optimiser hoists this code to the top of a loop body thousands of
instructions long.

```
uv run tools/cycles.py --clock 10 50 --words 64        # throughput and where the time goes
uv run tools/cycles.py --explain                       # name the code in the longest gap
uv run tools/cycles.py --table                        # KiB/s for every rate a host can ask for
uv run tools/window.py                                # disassemble the gap that repeats a word
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

**A change under about 1.5% on four-wire JTAG is not attributable to itself.** Three edits that
did not touch the JTAG bit paths moved its 50 MHz figures by up to 1.3%, in both directions: the
emulation is deterministic, so these are real differences in the generated code, but they are
link-time register allocation landing differently rather than the change doing anything. SWD has
not shown this. Attribute a small JTAG move from the disassembly or not at all.

Stock measured 476 KiB/s read and 502 KiB/s write at its best setting. KiB/s of wire time, 64-word
block, before and after tonight:

| | asked | before | after | |
| --- | --- | --- | --- | --- |
| SWD read | 10 MHz | 538 | 635 | +18% |
| SWD write | 10 MHz | 547 | 682 | +25% |
| SWD read | 50 MHz | 1161 | 1776 | +53% |
| SWD write | 50 MHz | 1387 | 2152 | +55% |
| JTAG read | 10 MHz | 616 | 685 | +11% |
| JTAG write | 10 MHz | 621 | 759 | +22% |
| JTAG read | 50 MHz | 1355 | 1799 | +33% |
| JTAG write | 50 MHz | 1630 | 2462 | +51% |
| two-pin read | 10 MHz | 166 | 213 | +28% |
| two-pin write | 10 MHz | 177 | 218 | +23% |
| two-pin read | 50 MHz | 283 | 647 | +129% |
| two-pin write | 50 MHz | 290 | 616 | +112% |

Text grows from 46,980 bytes to 108,736, which is 17% of the part's flash.

The bar is cleared on wire time at every setting, by 4.3x at the top. **Wire time is not the same
as throughput**: the stock capture put the USB gap at about 10% of a transfer at 14 MHz, and a
target that will not attach above 14 MHz makes the top of this table unreachable. The bench decides.

## What a host actually gets for the rate it asks

The clock delay is a whole number of loop iterations, so the wire rate goes up in steps and most
requests land on a step below them. KiB/s of wire time, 64-word block, from
`uv run tools/cycles.py --table --words 64`:

| asked | SWD read | SWD write | JTAG read | JTAG write | two-pin read | two-pin write |
| --- | --- | --- | --- | --- | --- | --- |
| 1 MHz | 80 | 80 | 90 | 92 | 30 | 30 |
| 2 MHz | 153 | 155 | 172 | 176 | 56 | 57 |
| 3 MHz | 219 | 224 | 245 | 254 | 80 | 80 |
| 5 MHz | 326 | 338 | 361 | 381 | 116 | 117 |
| 8 MHz | 483 | 509 | 527 | 570 | 166 | 169 |
| 10 MHz | 635 | 682 | 685 | 759 | 213 | 218 |
| 12 MHz | 635 | 682 | 685 | 759 | 213 | 218 |
| 14 MHz | 635 | 682 | 685 | 759 | 213 | 218 |
| 16 MHz | 930 | 1033 | 977 | 1135 | 295 | 304 |
| 20 MHz | 930 | 1033 | 977 | 1135 | 295 | 304 |
| 25 MHz | 930 | 1033 | 977 | 1135 | 295 | 304 |
| 30 MHz | 930 | 1033 | 977 | 1135 | 295 | 304 |
| 40 MHz | 1776 | 2152 | 1799 | 2462 | 647 | 616 |
| 50 MHz | 1776 | 2152 | 1799 | 2462 | 647 | 616 |

Above 8 MHz there are three settings and nothing in between, which `PACING.md` explains and
proposes a fix for. 10, 12 and 14 are the same setting;
16 through 30 are the same setting; 40 and 50 are the delay-free path. **Ask for 16 rather than 20
or 30, and 40 rather than 50**: the wire is identical and the request says what the probe will do.

The steps are where the bench should look. A target that stops at 14 MHz gets 635 KiB/s and one
good for 16 gets 930, a 46% difference for 2 MHz of margin, and 30 to 40 is another 91%. Which
step a target tolerates decides more than anything left in this file.

One oddity, and it is not the one it looks like. On the two-pin path the delay-free setting makes
writes slower than reads, 616 against 647, where every other setting and every other wire has
writes ahead. It is not that the write clocks more slowly: both put 7860 clocks on the wire, and
the write holds a 3-cycle period for most of a transfer against the read's 7. The write runs 2,619
more instructions for the same clocks, so the extra is between the edges rather than on them, and
its bit cost is lopsided -- 1,344 bits at 9 cycles where the read has 2,866 at 7.

The driven bit is nine instructions against the sampled bit's six. The clock stores and the
buffer-direction store are common to both. What the write adds is masking the data bit out of the
word, inverting it for the nTDI packet, and one more store. The mask and the inversion are the
frame format. The extra store goes to `staged_tdi`, the file static
`PIN_TDI_OUT` is shadowed to, which the frame reads back.

**Why that store is there is unsettled, and the evidence cuts both ways.** The static is file-local
and its address never escapes, so nothing obliges it to live in memory. But the disassembly loads
its address in `cjtag_ReadIDCode`, `cjtag_WriteAbort` and `cjtagf_Sequence` as well as in the
transfer, and those are small functions under no register pressure at all. So it is not simply the
allocator spilling in the big loop, and a local may not help.

It is 1,344 of the 52,109 instructions a two-pin write block runs, so 2.6% is the ceiling on
removing it, not a prediction.

And removing it is not cheap. The static exists because the shadowed pin layer is a set of macros
that take no arguments: `PIN_TDI_OUT(bit)` has nowhere to put the bit but a variable with static
storage, and `PIN_SWCLK_TCK_SET()` has no argument to read it back through. Making it a local
means giving every enclosing function a declaration, and those functions are ARM's `JTAG_DP.c`,
compiled unchanged three times. That property is what keeps the three copies from drifting, and it
is worth more than 2.6%.

It is also the setting with no pacing margin to begin with, so measure the period distribution
before changing anything there.

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

### Pay for a transfer's registers once a block, not once a word

ARM's block command calls the transfer per word, and a transfer saves eight or nine registers on
entry, restores them on exit and reloads the GPIO addresses and pin masks each time. At the
delay-free setting that was 60 to 80 cycles of dead wire between one transfer and the next, a fifth
of a read.

`src/jtag_block.h` holds ARM's block loop, transcribed once, and it is expanded next to each
transfer it drives: SWD in `src/swd_dp.c`, four wires in `src/jtag_dp_block.c`, and the two OScan1
copies in `src/cjtag_jtag_dp.c`. `src/dap_dispatch.c` owns the two command entry points and routes
a block command to the right one, sending everything else to ARM's processing unchanged.

SWD gains 4% on reads and 15% on writes at the top end, four-wire JTAG 11% and 19%, two-pin 2%.
The dead stretch between SWD writes halves, from 62 cycles to 34. Text grows 42 KB.

### Store a read word in one go

ARM writes the four bytes of a read word separately. GCC merges them back into a single word store
-- and rebuilds the word from its own bytes first, four extracts and four `bfi`s, ten instructions
to copy a register, once per word. `__builtin_memcpy` of the whole word takes one store.

SWD reads gain 3% at 50 MHz and 1% at 10, and the per-word gap drops from 65 cycles to 52. Writes
do not move, which is the check: a write puts nothing in the response. Four-wire JTAG loses 0.4%, and not to its own
store: putting ARM's four bytes back in `jtag_block.h` alone leaves all four JTAG numbers
unchanged and the image 8 bytes larger, so the loss arrives from the SWD file through link-time
optimisation. Taken for the SWD gain. Text falls 184 bytes.

### Take a read block's word count from the response pointer

One word is four bytes, so the count the reply carries is the distance the response pointer moved.
ARM keeps a separate counter, and the unrolled data phase leaves it nowhere but the stack: a load,
an add and a store per word. Deriving it where the read branch leaves, at a label the two failure
paths jump to as well, costs one divide once.

SWD reads gain 1.3% at 50 MHz and 0.5% at 10. The same change in the JTAG loop gains 1.3% on
writes and loses 0.3% on reads, which is the allocation moving rather than the read path doing
more. Together the image is 404 bytes smaller.

### Carry the packet parity in the request

The packet parity is taken over the four address bits, and ARM sums them inside the transfer, so a
block recomputed the same parity for every word. Bit 6 of a request is unused by CMSIS-DAP: the
caller puts the parity there once, and the transfer writes it with one more shift of a register it
is already holding. SWD gains 0.6% on reads and 0.3% on writes at 50 MHz, for 36 bytes.

The differential suite could not have caught this being wrong. Its 300 cases used two request
values, `0x0F` and `0x0D`, and the parity depends on exactly the four bits those two fix. A
sixth case now sends all sixteen address patterns in one command, and an inverted parity fails it.

## Tried and rejected

Recorded because each one is the obvious next thing to try.

### Hoisting the per-word loads out of DAP_Data

The transfer reloads `DAP_Data.transfer.retry_count` and `DAP_Data.transfer.idle_cycles` for every
word, and only `DAP_TransferConfigure` changes either, so neither can move during a block. Both
were hoisted and both bought nothing.

The retry count, loaded once a block: SWD unchanged, JTAG reads up 0.6%, JTAG writes down 1.3%,
image size identical. The idle cycles, passed into the transfer so the block loads them once:
SWD reads unchanged at 1776, writes 2152 to 2151, image 12 bytes smaller.

Two experiments, one conclusion. **The per-word loads are not what the gap is made of.** What
costs is the spilling, and these values were being rematerialised or reloaded because the unrolled
data phase wants every register, not because the compiler could not prove them invariant. Anything
that does not reduce register pressure will keep landing here.

Attacking the spilling directly does not work either, at least not this way. The packet's four
address bits are four independent shifts of the request, hoisted to the top of the loop body and
spilled; writing them as a serial chain through one scratch variable, which cannot be hoisted the
same way, produces a **byte-identical image**. Value numbering puts the four shifts back.

### Hoisting the packet request out of the block loop

The eight packet bits are the same for every word of a block, and the compiler was recomputing the
four shifts and the parity once a word and spilling them. Building the byte once and shifting bits
out of it inside the transfer is **1.9% slower on reads, 0.8% on writes, and 5,332 bytes larger**:
1743 to 1710 KiB/s and 2145 to 2127 at 50 MHz. The built packet has to stay live across the whole
transfer, where a shift of the request was free wherever the register already was.

### `restrict` on the block pointers

The response is a `uint8_t*`, so a store through it could in principle be a store to `DAP_Data`,
and the loop reloads the retry count and the idle cycles every word. Marking both pointers
`restrict` -- which is true, they are separate buffers -- **changes not one instruction**, on any
of the three wires. The reloads have another cause. Dropped rather than kept as documentation,
since it buys nothing and reads like it does.

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

**`PACING.md` traces all of this through the source** and is the document to hand someone who is
going to work on it. What follows is the measurement and the decision.

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

### 1. What is left between transfers

About 46 cycles on a SWD read at the delay-free setting, 15% of the time, down from 65.

What is in those 46, counted off `tools/window.py`: seven shifting the packet's address bits out
of the request and spilling them, five on the loop counter, three storing the response pointer,
which lives on the stack, three loading and testing the idle-cycle count, and the rest the tail of
one transfer and the head of the next.

**All of it is one problem.** The unrolled data phase wants every register, so the loop's own state
is spilled and its invariant parts are rematerialised rather than kept. Six attempts have now
landed on that wall from different directions, and every one is recorded above: hoisting the packet
(slower, and 5 KB larger), `restrict` on the block pointers (no change), hoisting the retry count
(no change), hoisting the idle cycles (no change), a serial shift chain that cannot be hoisted
(byte-identical image), and before tonight the inlining parameters (33 KB for one cycle) and the
allocator's own knobs (`-fsched-pressure` and `-fno-schedule-insns2` change nothing,
`-fira-algorithm=priority` costs 2%).

Four-wire JTAG is the same wall with one more spill on it. Its gap is 51 cycles, and alongside the
same response pointer and loop counter it reloads the **request value itself** from the stack and
decomposes it into four values once a word. On SWD that value stays in a register. So the JTAG
loop is a notch further over the edge, which is also why its numbers move by up to 1.3% when
something else in the image changes.

**Anything that does not reduce register pressure will land here too.** The one lever that would
reduce it is shortening the unrolled data phase, and that has now been measured: four passes of
eight bits, with a rotate to reassemble the word, costs **4.4% of a SWD read at 10 MHz and 1.0% at
50, for 2,056 more bytes**. It is correct -- the gate passes -- and it is simply a bad trade. The
loop overhead and the rotate cost more than the freed registers return.

So the gap stays. Seven attempts, all recorded, and the thing they have in common is that none of
them made the wire shorter.

### 2. DAP_Transfer spends a quarter of its time not clocking

The block command is now the fast one and the single-register command is not. Measured at the
delay-free setting, 16 reads through `DAP_Transfer`: **121 cycles between one transfer and the
next, 26% of the command**, against 65 cycles and 19% for the same reads through a block. At
10 MHz it is 133 cycles and 11%.

This is not the throughput path -- a host reads and writes memory in blocks -- but it is most of
what an attach is made of, and of anything that touches one register at a time.

**Built, measured and reverted.** ARM's `DAP_SWD_Transfer` extracted the same way the block was,
with the transfer inlined into its loop:

| | asked | before | after | |
| --- | --- | --- | --- | --- |
| read | 10 MHz | 561 | 577 | +3% |
| write | 10 MHz | 599 | 616 | +3% |
| read | 50 MHz | 1365 | 1482 | +9% |
| write | 50 MHz | 1611 | 1759 | +9% |

**It is correct**, and that is not a guess: `uv run tools/equiv.py --command transfer` drives it
against ARM's over 300 cases -- every acknowledge, both turnaround lengths, with and without idle
cycles, and the paths a block never reaches: a match value that hits, one that misses and burns
its retries, and a timestamped transfer. All 300 are identical on the wire and in the reply, as
are the gate's nine traces and the 828 block cases.

What it costs is flash, and that is the only reason it is not here. Two versions were built:

| inlined | text | 10 MHz read / write | 50 MHz read / write |
| --- | --- | --- | --- |
| none, as it ships | 108,772 | 561 / 599 | 1365 / 1611 |
| the five per-word calls | 146,528 | +1% / +2% | +5% / +8% |
| all ten calls | 164,468 | +3% / +3% | +9% / +9% |

Half as much flash again for 9% at a clock the bench has never reached, or a third as much again
for 5%. Neither is worth taking for the 1% to 3% available at the clocks it has.

The recipe, because it is mechanical and worth redoing rather than rewriting. Extract
`DAP_SWD_Transfer` from DAP.c, rename it, and change the one call it makes to the speed-specific
transfer. Two things do not survive being put in a macro: its five `#if (TIMESTAMP_CLOCK != 0U)`
blocks, which have to be resolved and then guarded by a `_Static_assert`, and its `//` comments,
which swallow the line continuation and have to become `/* */`. Then route `ID_DAP_Transfer` for
the SWD port in `src/dap_dispatch.c` the way the block is routed.

### 3. The read bit is two cycles above the floor

A packet bit is 3 cycles, a data read bit is now 5. The two are a byte load from the GPIO, which
cannot be avoided, and the or that places the bit. There is nothing obvious left; recorded so the
floor is written down.

### 4. Nothing is left in the pacing itself

Recorded so it is not tried again. `DELAY_SLOW_CYCLES` is 3 and the measurement agrees to two
decimal places, so the delay moves in 3-cycle steps and a finer quantum looks like free speed. It
is not. What the delay has to satisfy is the *shortest* period in a transfer, and at every setting
that period already lands just under the clock asked for: 9.60 MHz against 10, 48.00 against 50.
The firmware is already as fast as the request allows on its fastest bit.

The gap between that and what a transfer actually averages is entirely items 1 and 2 -- the slow
classes costing more than the fast one. There is no third thing.

### 5. The two-pin transfer is not inlined into its block loop

**The best unclaimed item here, and it was invisible until `tools/window.py` existed.** The two-pin
per-word gap is 73 cycles against SWD's 46, and the middle of it is this:

```
uxtb   r0, r0
add    sp, #0x1c
pop.w  {r4, r5, r6, r7, r8, sb, sl, fp, pc}
```

A nine-register epilogue and a return, once a word. `src/jtag_block.h` exists to pay that once a
block, and it does for SWD and four-wire JTAG -- neither of their gaps contains a `pop`. The
two-pin transfer is bigger, so the compiler declined to inline it and the block loop calls it
instead.

Predicts most of the difference between a 73-cycle gap and a 46-cycle one, which is around 15% of
a two-pin block.

Two attempts, and the flash is why neither landed.

`-finline-limit=2000` on the two cJTAG translation units produces a **byte-identical image**:
link-time optimisation decides inlining at link time and never sees a per-file compile flag.

`__attribute__((flatten))` on the block function does work, and covers both copies at once --
`cjtag_jtag_dp_fast.c` is an include of the paced file with different defines, so one define
reaches both: **+1.9% and +2.3% at 10 MHz, +0.2% and +3.9% at 50, for 64,564 more bytes.** That is 59%
more image for at most 4% of the slowest wire, and `flatten` is why -- it inlines transitively,
not just the transfer. The surgical version would be `always_inline` on the transfer only, and
that transfer is ARM's `JTAG_DP.c`, compiled unchanged three times.

So the 27 cycles are real and reachable, and nothing cheap reaches them. Same shape as the
`DAP_Transfer` rewrite above: correct, measured, and rejected on flash.

### 6. The buffer direction pin toggles every cJTAG frame

`cJTAG_Frame` releases TMSC for the TDO packet and takes it back afterwards: four of the thirteen
GPIO writes a frame makes. Both are needed for correctness, but a sequence that captures no TDO
still pays them, and `JTAG_SEQUENCE_TDO` says when the host does not want it. The third packet must
still be clocked, because the target counts packets.

Predicts: four writes a frame down to two on TDO-less sequences, most of a TAP walk.

**Unproven and the riskiest item here.** If the target drives TDO regardless, not releasing means
contention. Measure before keeping.

### 7. DAP packet size above 512

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

### 8. Run the bit paths from SRAM

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
