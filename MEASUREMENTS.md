# Measurements

What the bench measured, newest first. Each section says when it was measured and which transfer
paths it ran on. A product image generates its transfers at runtime (README.md, "Check"). Sections
on ARM's paths describe what the `+armwire` reference build still runs. BENCH.md has the bench
itself, and mculink-bench the comparisons with stock.

## SWO capture

Measured 2026-10-19 to 2026-10-21, on the dual-core image's DMA capture.

**Done 2026-10-19, on the dual-core image's DMA capture (`src/swo.c`).** Target:
LAUNCHXL-CC1312R1 over two-pin cJTAG, running `tools/swo_target/itm_counter.c`, which sends a
counter on ITM port 0 out of DIO16 (TDO) as fast as the port takes it, so every byte is checkable
and the target drops nothing. The analyser on the line read 1.0000 and 7.9999 Mbaud from
back-to-back frames, saturated, counter unbroken.

| through | rate | captured | counter |
| --- | --- | --- | --- |
| `dap.py swo`, 2 s | 1 MHz | 200,008 bytes, 100.0 KB/s | 40,000 words, no break |
| `dap.py swo`, 3 s, twice | 8 MHz | 2,400,064 and 2,400,049 bytes, 800.0 KB/s | 480,011 and 480,009 words, no break |
| `probe-rs itm swo`, debug build, 0.5 s | 1 MHz | 10,067 words after the decoder locked | no break |
| `probe-rs itm swo`, release build, 3 s | 8 MHz | 117,871 words decoded | one three-word blip, no reply lost |

`dap.py` polls with DAP_SWO_Data back to back: about 14,000 polls a second, each under the
508-byte maximum, so the host keeps ahead of the line. probe-rs decodes and prints every packet,
which in a debug build manages about 70 KB/s; behind that the probe overruns, says so, and the
stream resumes after the gap. probe-rs ignores the overrun bit and fails a read on the stream-error
bit, which a capture that starts mid-frame sets once.

**Above 8 MHz, 2026-10-21, through the Pro** (sweep images with a raised cap). Same target and `dap.py swo`; the Pro at 8 MHz first matched the
plain board's figures above (2,400,047 and 2,400,062 bytes, no break).

| Flexcomm 3 clock | rate | alone, 3 s twice | under console load, 15 s |
| --- | --- | --- | --- |
| 96 MHz, FRO_HF_DIV | 9.6 MHz | 960.0 KB/s, no break | |
| 96 MHz | 12 MHz | 1200.0 KB/s, no break | |
| 96 MHz | 16 MHz, oversampling 6 | 1600.0 KB/s, no break | 23,999,995 bytes, no break |
| 120 MHz, main clock / 1.25 | 8 MHz | 800.0 KB/s, no break | 11,999,917 bytes, no break |
| 120 MHz | 12 MHz | 1200.0 KB/s, no break | 17,999,934 bytes, no break |
| 120 MHz | 24 MHz, oversampling 5 | 2400.0 KB/s, no break; 30 s, 71,999,402 bytes, no break | 825 breaks, 4.8% lost |

Console load: both of the Pro's consoles looped back at once, one second's worth at 1, 2 and 3
Mbaud. The 24 MHz breaks all fall in the 2 and 3 Mbaud runs. The 120 MHz clock is over the
fractional divider's 100 MHz limit (UM11126 4.5.49), and every rate above 10 MHz is over the data
sheet's asynchronous USART maximum.

**At 24 MHz in the shipped image, 2026-10-21** (cap 24 MHz; `swo_uart_clock` in `src/swo_ring.h`
takes the 96 MHz clock when it makes the rate within 0.5%, otherwise the main clock through
Flexcomm 3's fractional divider, so of these rates only 24 comes from the divider). The sweep and
the rate switching below were repeated on the final image with the same results. Through the Pro: 24, 16, 12, 9.6, 8 and 1 MHz, 3 s twice each, every rate
answered exactly and every capture unbroken. 24 MHz while both consoles looped back at 3, 12 and
19.2 Mbaud, 12 s twice: 28,799,849 and 28,799,931 bytes, no break. One session moving 24, 8, 24,
12 MHz without a mode change: all unbroken; 30 MHz asked answers 24. probe-rs (`itm swo`, release
build, 3 s at 24 MHz from a 48 MHz trace clock): 76,278 packets decoded, its print loop the limit,
and the one stream error a capture starting mid-frame raises.

Through the plain MCU-Link, the LaunchPad moved to its debug connector: 24, 16, 12 and 8 MHz,
3 s twice each, unbroken; 24, 8, 24 MHz in one session, unbroken, and 30 MHz asked answers 24;
24 MHz for 10 s while its console looped back at 3, 12 and 19.2 Mbaud, 23,999,973 bytes, no break.
Its input buffer is the one part of the SWO path the two boards do not share.

**At 30 MHz, 2026-10-21, the Pro as the source.** The CC1312 stops at 24 MHz, so the Pro's own
CPU 0 sent the trace instead. The plain MCU-Link halted it through the Pro's J3 and ran an ITM
counter from SRAM: the same stream
as `itm_counter.c`, trace clock the 150 MHz main clock undivided, SWO on PIO0_10 to J3 pin 6, so
150 MHz / (ACPR + 1). A reset of the Pro brings its firmware back. The plain MCU-Link captured with
`dap.py swo`, 3 s twice each:

- On the shipped 24 MHz image: 1, 12.5, 15, 18.75 and 21.4 MHz unbroken. 25 MHz asked answers 24,
  and the 4% between the ends breaks the stream on every reply.
- On a sweep image with the cap at 30 MHz: 21.4, 25 and 30 MHz
  unbroken, 30 MHz at 9,000,675 and 9,000,009 bytes (3000 KB/s). 30 MHz for 20 s: 59,997,323
  bytes, 11,999,464 words, no break. 30 MHz for 20 s with the console looping back for 12 of them,
  a second each at 3, 6 and 12 Mbaud and then 19.2: 59,997,409 bytes, no break, and every console
  run identical. 37.5 MHz asked answers 30, with a stream error on every reply.
- On the shipped image: 30 MHz for 3 s, 9,000,468 bytes, no break; 40 MHz asked
  answers 30.

The analyser cannot resolve 30 MHz, about 3 samples a bit, so the evidence is the answered rate,
an unbroken counter and 3.0 MB/s together. 30 MHz is the fastest the UART's clock makes, so this
is the probe's ceiling. The Pro's input buffer has not seen anything above 24 MHz.

The emulator still cannot run capture: CPU 0's clock setup never runs there, so the 96 MHz clock
reads 0 Hz, SWO_Baudrate makes its rate from the main clock instead, and no capture is started. The
ring and rate arithmetic is tested on the host instead (`tools/swo_ring_test.py`, in the gate).

## The console

Measured 2026-10-19 to 2026-10-21.

Done 2026-10-19, after a fix: it had never received a byte.

Open the CDC port and talk to a target's UART at a few baud rates, including 921600 and above.

Proves `vcom_init` took Flexcomm 0 back from the board support, and that the fractional divider
reset actually fixed the 2.4% error at a megabaud.

Loopback, 300,000 bytes a run, a jumper across TX and RX (`tools/coverage.py console`, then one
console at a time):

| unit | consoles | clean at | lost |
| --- | --- | --- | --- |
| MCU-Link, 2026-10-19 | one (J7) | 9600, 115200, 921600, 1M, 2M baud | 18 to 62 bytes at 3 Mbaud |
| MCU-Link Pro, 2026-10-20 | each alone | 115200, 1M, 2M baud; VCOM also 3M | VCOM2 one byte at 3 Mbaud |
| MCU-Link Pro, 2026-10-20 | both at once | 1 Mbaud, twice | 2 to 4% at 2 Mbaud, 3 to 6% at 3 |

The losses are RX FIFO overruns: CPU 0's loop moves console bytes one at a time, and two consoles
share that loop. Stock V3.172 was clean with both of the Pro's at 3 Mbaud.

**On DMA, 2026-10-21:** each unit's consoles looped back at once, one
second's worth each (`tools/coverage.py console`, which now runs every console of the probe
together, sets any rate through Linux's termios2 where pyserial's path stops at 4 Mbaud, and times
each run to its last byte, so a refused rate cannot pass as a slower one).

| unit | consoles | clean at | through |
| --- | --- | --- | --- |
| MCU-Link Pro | both at once | 115200, 1M, 2M, 3M, 4M, 6M, 8M, 12M, 16M, 19.2M baud | 11.7 Mbaud of 12, 18.2 of 19.2 |
| MCU-Link | one (J7) | 115200, 3M, 12M, 19.2M baud | 12.0 Mbaud of 12, 18.9 of 19.2 |

19.2 Mbaud is the UART's fastest from its 96 MHz clock (oversampling 5). The same image started and
stopped SWO capture 20 times at 8 MHz while both consoles streamed at 3 and 12 Mbaud: every console
byte back, every capture unbroken. With a 24 MHz SWO sweep image ("SWO capture", above) and both
consoles run to 19.2 Mbaud during the capture, 20 s of SWO lost nothing; an earlier 15 s run lost
three gaps, 7,305 bytes, all after the consoles had finished.

## DAP_Transfer ran from flash in the generated build

Measured 2026-10-16, on the generated transfers.

A host sends anything shorter than a block as one DAP_Transfer: an access under 1 KB, the address
write that opens each 1 KB window, every register poke during an attach. In the generated build
that command ran ARM's loop from flash, and each transfer re-checked the generator's
configuration with a memcmp from flash. `dap.py block --command transfer` times the same DP reads
both ways (50 MHz, 16, 64 and 120 words a command, synchronous, per word from the slope):

| | DAP_TransferBlock | DAP_Transfer |
| --- | --- | --- |
| before | 1.64 µs/word | 7.24 µs/word |
| loop in SRAM, configuration checked once per command | 1.68 | 2.60 |

The prediction was 2 to 3 µs. Through probe-rs (`swd-throughput`, same probe-rs build),
a 256-byte read at 50 MHz went from 562 µs to 251, and 64 bytes from 188 to 104; reads of 1 KB and
up did not move. The fitted fixed cost per probe-rs call fell from 205 µs to 71, so most of what
looked like a probe-rs cost was this. The default build never had it: its DAP_Transfer was in
`.ramfunc` all along, which is why the generated build was slower than it below 1 KB.

The gate missed it because it measures blocks. It also hid the next problem: once
`swd_gen_ready` had one caller, clang inlined it, and the gate, which recognised a generating
image by that function's symbol, judged the clang image against the default build's baselines.

## The USB path at 50 MHz, and what a perfect host gets

Measured 2026-10-16, on the generated transfers.

At 50 MHz the wire stopped being most of a block read. probe-rs read SRAM at 1,397 KiB/s, half
of what the wire allows, so the question became how much of the rest is the probe.

**`tools/dap.py block --queue N` is the instrument.** It queues commands through libusb's
asynchronous API, so the probe never waits on the host. A queue of 2 already saturates, deeper
queues change nothing, and the host's CPU is 17% busy at 126 words: what it measures is the
probe and the link. 50 MHz, DP reads, queue 8, three runs each, identical to 0.1%:

| build | 1 word | 32 words | 126 words |
| --- | --- | --- | --- |
| generated transfers, as before | 22,965 commands/s | 1,349 KiB/s | 2,067 KiB/s |
| + memcpy a block at a time | 23,093 | 1,429 | 2,242 |
| + TinyUSB in SRAM | 30,399 | 1,618 | 2,351 |
| clang, same, buffers aligned | 33,476 | 1,686 | 2,383 |

The configuration that shipped then (ARM's C paths, 10 MHz) gains from TinyUSB in SRAM too: 25,235 to
35,103 one-word commands a second, 717 to 729 KiB/s at 126 words.

**Where a command's time went, from the marker** (126-word blocks, before → after the two changes):

| | before | after |
| --- | --- | --- |
| clocks running | 186.1 µs | 186.5 µs |
| before the first clock | 6.1 | 6.0 |
| after the last clock | 1.8 | 1.8 |
| gap to the next command | 45.5 | 14.6 |
| period | 239.6 | 208.9 |

The generator schedules 170.5 µs of clocking for 126 reads; the other 16 µs are interrupts, which
run between transfers because the transfers mask them. Folding the gaps between transfers shows
about 2.4 per command at 4.4 µs each from SRAM, 6 µs from flash. A one-word command spends 14 µs
before its first clock, mostly those same interrupts.

**newlib-nano's memcpy was 8 cycles a byte.** It is built for size and copies a byte per
iteration, and every reply passes through it into the endpoint buffer: 26 µs for a 507-byte
reply. Ours (`src/mem.S`) moves
32 bytes per ldm/stm pair. It saved 18.5 µs of the predicted 24.

**clang left the reply buffer on a halfword**, so its replies still went through the byte path
and its 126-word figure was 7% under gcc's until the buffers were aligned.

**What is left, per 126-word command:** 14.6 µs between commands (dispatch, TinyUSB's task, the
main loop), 6 µs before the first clock (command dispatch, still in flash), and about 10 µs of
interrupts inside the block. At 50 MHz that is 15% of the period. The copy into the endpoint
buffer can go entirely: the high-speed controller reaches only USB RAM, so the slots and the reply
would have to live there and the bulk endpoints be driven without TinyUSB's vendor class.

**probe-rs, against the same probe, is a separate gap.** Within a run of blocks it keeps the probe
as busy as the perfect host does (39 µs between blocks against 45 for dap.py on the old build).
But every 1 KB, where the access port's address wraps, it sends two small commands and waits for
each reply before continuing: about 170 µs of round trips per 1 KB against 470 µs of blocks.
That, not the probe, is most of the difference between 1,397 and the 2,000 the probe gave a
script.

## The transfers are generated at runtime, and the wire runs at every period from three cycles

Measured 2026-10-16, on the generated transfers, the default since 2026-10-20.

**What it is.** A build with `SWD_CODEGEN` on, the default since 2026-10-20, writes its SWD
transfer code into SRAMX whenever the host changes the clock, the turnaround, the data phase or
the idle cycles. Every edge, data store and sample sits at a cycle the timing model chose. ARM's
command logic stays in C around it: posted reads, the RDBUFF reads and WAIT retries.
`src/swd_gen.c` has the details and CODEGEN.md the reasoning.

**The host now gets the rate it asks for, to one cycle.** The period is the shortest whole number
of cycles that is no faster than the request. Clocks the probe drives go down to 3 cycles, 50 MHz.
Clocks the target drives go down to 4, 37.5 MHz, because a read needs a sample and its use as well
as the two edges.

**It holds to ARM's wire.** `tools/equiv.py --ignore-samples` passes all 536 block cases at 50,
37.5, 25, 12.5, 10, 1 and 0.1 MHz, and all 360 single-register cases at 50, 37.5, 10 and 1 MHz.
Only where the samples sit differs: a sample can trail the edge that launched its bit by two clocks.

**Every period ran clean on the MSPM0L1306.** A sweep that reads DPIDR in blocks and writes
CTRL/STAT, checking 18 bits of each write, found no error at any period from 3 to 40 cycles,
or at 3, 2, 1 and 0.1 MHz. probe-rs wrote random words to SRAM and read them back correctly at
every clock from 0.1 to 50 MHz.

**Throughput through probe-rs, 64 KB of flash, same probe and same part:**

| asked | generated | previous firmware |
| --- | --- | --- |
| 50 MHz | 1407 KiB/s | fails |
| 37.5 MHz | 1312 | fails |
| 25 MHz | 1172 | fails |
| 15 MHz | 830 | fails |
| 10 MHz | 629 | 577 |
| 5 MHz | 360 | 324 |
| 1 MHz | 80 | 79 |

The previous firmware fails above 10 MHz on this part because it samples too close to the edge,
which "The cost model times the bus" found. At 37.5 MHz the wire is idle about half the time. A
running block reads a word in 210 cycles, which is 2.7 MB/s, and probe-rs gets 1.3. USB and the host
take the rest.

**The masked port register reaches the pin when CLR does.** The generator lowers the clock and
sets the data bit in one store to that register, which no earlier measurement covered. On the
analyser at the target, over 2.4 million clocks at P = 20, the low phases of clocks that fall that
way and of clocks that fall on CLR differ by 0.17 ns.

**Reads stop at 37.5 MHz while the core makes the clock.** A read clock is four instructions and
the core pairs none of them: the corpus timed a 16-bit load or store against a 16-bit add, in both
orders, at a cycle each.

**A timer can make the clock instead, and then reads run at 50 MHz.** SWCLK's pin is also
CTimer 0's match output, and in PWM mode that timer counts the core's own clock. It holds its
phase to the cycle at every period tried, from 2 to 10 cycles. With the timer making the
target's clocks, the core only loads and keeps each bit. A lab prototype read DPIDR correctly
2,000 times out of 2,000 with those clocks at 50 MHz, and the analyser shows them at exactly that
rate. Stopping the timer is the hard part. A store to any of its registers holds the core up four
cycles, which at 50 MHz always swallows one bit's sampling window, so the prototype stops the
timer with a DMA write instead, triggered by a second timer. None of this is in the generator.

## The cost model times the bus, and reproduces every SWD result so far

Measured 2026-10-16, on the code lab build and ARM's paths.

**A code lab, vendor 0x88.** A build with `-DCODE_LAB=ON` runs a function the host writes into
RAM, with interrupts masked. The function reads the cycle counter around the code under test, so
the call itself never enters the measurement. `tools/lab.py` assembles a case, runs it on the
probe and in the emulator at the same address, and compares the two. A lab build resets itself if
a function faults.

**Fetch from SRAM shares a bus with every GPIO access; fetch from SRAMX does not.** The bit paths
run from SRAM at 0x2000_0000, which the core fetches over S-AHB, the port its loads and stores
also use. SRAMX at 0x0400_0000 is fetched over C-AHB. The same code in each, every repeat
identical:

| case | SRAM | SRAMX |
| --- | --- | --- |
| 16 GPIO stores, 32-bit encodings | 32 cycles | 16 + 2 |
| 16 GPIO loads, 32-bit encodings | 32 | 16 + 2 |
| 16 stack loads, 16-bit encodings | 24 | 16 + 2 |
| 16 `mov.w` | 16 or 17, by alignment | 16 |

From SRAM every cycle carries one bus transaction, an instruction word or a data access. From
SRAMX the two run side by side. The +2 is the cycle counter's own read waiting for the last
access to finish. NOPs pair up and cost half a cycle each, so they are no use as timing filler.

**The pin round trip is seven cycles against the U575, and exact.** A DPIDR read generated as
straight-line code in SRAMX, one instruction a cycle, with the sample swept across the clock
period:

- A bit is readable when the sample load issues 7 to P + 6 cycles after the store that raised
  the clock, where P is the period. The read comes back one bit ahead at 7 cycles past its own
  edge and one bit behind at 6 cycles past the previous one, so both ends give the same number.
- No position gave a mixture of results.
- Without a target, a load sees the probe's own SWDIO store 3 cycles later. The target, its
  cable and the two buffers make up the other 4.

The window is a whole period wide at every rate. At the delay-free step's five-cycle period a
sample nine cycles after its edge would have two cycles either side. That sample comes after the
next rising edge, which a generator can place and compiled C cannot.

**The emulator now models fetch and the bus.** `tools/m33.py` decides when each instruction
issues, from its fetch, the instruction before it and the bus slots data has already taken. It is
fitted against the lab: 494 of 516 cases exact. `cycles.py` runs on it on request. A modelled SWD
target launches each bit on a rising edge and makes it readable 7 cycles later. Every read records
how far it sits from both ends of its window.

**With nothing fitted to them, it reproduces every SWD result the bench has:**

- the generated read sweep, all 70 positions, including which way each failure goes;
- vendor 0x87, at every position of both shapes, and its after-shape period to within a cycle
  over 32 clocks. The old model was two cycles a clock out;
- the block-size table at 16.67 MHz on the default build: the last read fails at every size, with
  acknowledge 3, and the loop's reads pass;
- the same for both variants tried on the bench, the tail shifted by a halfword and the tail
  inlined;
- the after-edge build reading clean at 16.67 MHz, and the delay-free step failing in both.

It also measures the tail's clock as the averaged capture did: three cycles high and six low
after ack0's rising edge, against four and five in the loop. Varying the parameters the fit does
not pin down leaves the result unchanged.

**Why the block's last read fails.** In the loop the rising-edge store's data access takes the
bus just as the next instruction word is due. The fetch waits a cycle, and that idle cycle
separates the rising and falling edges. The tail runs the same instructions at a different word
alignment and does not wait. Its falling edge comes a cycle sooner, the sample lands 6 cycles
after the edge, and it reads the bit before. The loop's own reads sit exactly at the window's
edge, so the loop passes by no margin at all.

**Two predictions, tested on the bench:**

- On the default build at 16.67 MHz, every write through `DAP_Transfer` misreads its acknowledge
  as 3, and reads pass. Confirmed, 50 of 50 in each of five orderings of reads and writes.
- `DAP_Transfer` reads at that rate sample data bits 1 and 2 a cycle early, which DPIDR's bits
  would hide. Wrong: the access port's TAR, written with values that would expose it, read back
  clean. So the model is a cycle out in that stretch of compiled code from SRAM.

**Refitted on code timed in isolation.** Timing each case between filler, so the closing
cycle-counter read waits on nothing in the body, removed most of what the first fit had got
wrong. From SRAMX the model now matches all 692 such cases at four alignments: every instruction a
cycle, loads and stores back to back, a computed value ready two cycles after its load, every
taken branch two cycles plus one for a straddling target. One more rule came out of it: an access
through the core's private bus, where the cycle counter is, waits two cycles behind an access over
S-AHB, and the reverse. From SRAM, straight-line code is still a cycle out at one alignment and
loops drift by up to a cycle an iteration.

**The wire agrees.** SWCLK trains from SRAMX, built from every GPIO store form (SET and CLR, NOT,
byte, word, and SET with a byte), span exactly their issue cycles. Every form reaches the pin at
the same delay, to a tenth of a cycle. The analyser cannot resolve pulses of one or two cycles.

**Generated reads run at 37.5 MHz, and the model predicts every one.** A read generated with each
sample placed a set number of cycles after the edge that launched its bit, and stored to a buffer
by the next free cycle, needs four instructions a clock. At every period from 4 to 8 cycles,
37.5 to 18.75 MHz, and every sample position, 45 configurations, the bench and the model agree on
the verdict and on the cycle count. The window's edges are included.

**Generated writes run at 50 MHz.** With the SWDIO store anywhere from one cycle before its rising
edge to one cycle after the previous one, every write landed, at every period from 3 to 10
cycles. Setup and hold at the target are each under a cycle.

**The MSPM0L1306 runs at 37.5 MHz too: its ceiling was ours.** On the second unit, now running
the lab build, the L1306 shows the same seven-cycle window start as the U575, at every period from
8 to 30 cycles. The scheduler, placing samples with it, ran clean against the L1306 at every period
from 4 to 40 cycles. "The last read of a block at 16.67 MHz", below, assumed the target set this
part's ceiling. It does not: the part fails above 10 MHz on ARM's paths because of where they
sample.

**A scheduler that places everything from the model runs at every period.** Given a period and a
list of transfers, a prototype places every edge, SWDIO store and sample by the model alone, with
each sample at the middle of its window. Two writes and five reads, acknowledges, data and parity
all checked, ran clean at every period from 4 to 40 cycles, 37.5 to 3.75 MHz, ten times each. The
cycle count the probe measured equalled the model's at all 37 periods. A rate is then any whole
number of cycles: the furthest a request falls short is 20% at the top of the range, against 56%
today, and nothing runs faster than asked.

TIMING.md has the rules these give a generator. CODEGEN.md is the context for implementing it.

## The last read of a block at 16.67 MHz

Measured 2026-10-16, on ARM's paths.

The MSPM0L1306 on this bench connects at 1, 5 and 10 MHz and fails to connect at 20, 30 and 50 MHz
with the compiled bit paths. (Later measured: that is the probe's sampling. Placed by the timing
model, the same part reads and writes clean at 37.5 MHz; see "The cost model times the bus", above.)
A throughput sweep could not go above 10 MHz on it, so the faster settings needed either a faster
target or a capture with no target attached at all, which `tools/dap.py sweep` already does.

**The STM32U575 is that faster target.** Its datasheet (DS13737 Rev 10, Tables 152 and 153) gives
SWCLK up to 66.5 MHz and TCK up to 38 MHz at 2.7 to 3.6 V. Both figures come from
characterisation at 30 pF, so `dap.py limit` through the real harness is the number to trust.
It also has 786 KB of SRAM against the L1306's 4 KiB, which lifts the transfer-size cap on
`swd-throughput`. SWD needs none of the TDI rework. The nRF54L family is no help here: the
nRF54L15 datasheet (v0.7) caps SWDCLK at 8 MHz, the same band this MSPM0 fails in. The nRF54LM20's
own figure was not checked.

**Measured 2026-10-16, ours against the U575 over CN5: clean to 10.00 MHz, failing at 16.67.**
`dap.py limit` checks every word at every rate the firmware has. Delay 2 (10.00 MHz on the wire)
and below read 3,200 DPIDRs each with no bad word. Delay 1 (16.67 MHz) fails, and so does the
delay-free path at 30 MHz. There is nothing between 10 and 16.67 on the ladder. The U575 does
better than the MSPM0, which failed at 9.375 MHz under stock, but nowhere near its datasheet.

**The wire is clean; the probe misreads it.** Captured at our J6 (SWDIO and SWCLK, 100 MS/s) and decoded by sampling at each
rising edge, as the target does. Every read at both settings carries OK and `0x0BE12477`,
including the one the probe reported as acknowledge 3. At the connector the target's bit
arrives 15 ns after the rising edge that launched it (10 or 20 ns at this resolution, mean 15.3
at 10 MHz and 14.9 at the faster setting), so the cable and the U575 together spend 15 ns.

The faster setting is not 16.67 MHz on the wire for a block: data bits run at 60 and 70 ns
(mean 67.5), acknowledge bits at 60 and 80, and the period at 10 MHz is 100 ns throughout.

Acknowledge 3 is an OK (1,0,0) seen one bit behind: the turnaround's high level, then 1 and 0.

**Only one function misreads, and it does every time.** At the faster setting `DAP_Transfer` read
19,200 DPIDRs without a fault, and every block-size test gave the same answer, 200 runs each:

| block | words returned, acknowledge |
| --- | --- |
| 1 word | 0, 3 |
| 2 words | 1, 3 |
| 4 words | 3, 3 |
| 64 words | 63, 3 |

The block command peels its last read into `SWD_TransferTailSlow`, an out-of-line copy of the
transfer, and that copy misreads its acknowledge on every call. The loop that does the other 63
never does. At 10 MHz all four sizes come back whole. The tail's acknowledge reads are the same
instructions as the loop's, in the same order and the same sizes, using the same GPIO aliases,
at a different address.

**It is not instruction alignment, and it is not the out-of-line function.** Two rebuilds, each
run through the same block-size test, 200 runs a size:

- A `nop` at the top of the tail shifted it by a halfword, flipping the 4-byte alignment of every
  instruction in it, and the compiler re-laid the neighbours too. Still 200 of 200 failing.
- Forcing the tail inline into the block function, so the last read is code in the loop's own
  function again, the peel undone. Still 200 of 200 failing, and the loop's reads still clean.

So the failure goes with the block's last read, whatever code carries it.

**On the wire the last read's clock is a cycle off.** Averaged over a capture of 14,977 one-word
blocks and 4,025 sixty-four-word blocks, which washes out the analyser's 10 ns steps because its phase differs from read to read:

| | high after ack0's rise | low before ack1's rise | target's ack1 edge after the rise |
| --- | --- | --- | --- |
| last read, 1-word blocks | 21.8 ns | 38.2 ns | 17.6 ns |
| last read, 64-word blocks | 21.7 | 38.4 | 17.8 |
| the loop's reads | 27.9 | 32.2 | 17.1 |

Standard errors are 0.01 to 0.15 ns. The period is the same 60 ns and the target answers the
same, but in the last read the falling edge comes 6 ns, one core cycle, sooner after the rising
edge. Its instructions are the loop's, so one of the two clock stores reaches the pin a cycle
off from where it does in the loop. That one cycle is the difference between every read passing
and every read failing, which puts the loop's own margin at this setting under a cycle. A
comparison made earlier from a single transfer pointed the same way and was wrongly retracted as
quantisation: the averages bear it out.

**An earlier reading here, that the probe's own pins and buffers take ~45 ns, was wrong.** It
was a model fitted to one failure, and the two results above break it.

**Measured with vendor 0x87 (`dap.py sample`): the next edge's bit reaches the sample about five
cycles after the store that raised the clock.** At a 333 ns period, 1,000 reads at each position:
production's place before the edge and every position up to 2 filler cycles after it read
correctly, and from 3 on every read comes back one bit ahead, acknowledge 4. No position in
between gave a mixture. The filler times as exactly one cycle a step from a fill of 4 up (0 to 4
costs 5), and the period does not move with the sample. A computed branch of about two cycles
sits between the store and the filler, so the edge is near five cycles, ~35 ns, after the store
issues. In the loop, the sample before the next edge comes about eight cycles after this one at
the faster setting, so the loop has two or three cycles in hand. The tail runs the same
instructions and does not.

**Measured with both shapes of vendor 0x87: production samples at the early end of a
window about a period wide, and leaves most of it unused.** 1,000 reads a position, fill 24, a
260-280 ns period:

- **Lead shape, production's own pattern** (rising edge, filler, falling-edge store, production's
  delay loop, sample): every high phase from 0 to 24 cycles reads correctly. Even the shortest,
  about five or six cycles from the rising-edge store to the sample, already sees the bit that
  edge launched.
- **After shape** (sample in the high phase): correct up to 3 filler cycles past the edge, one bit
  ahead from 4. With operands reloaded through a struct the edge sat between 2 and 3, so it
  moves by a cycle with the code around it.
- A zero-length filler run is one cycle cheaper than the trend in both shapes, so position 0 sits
  a cycle earlier than its number.

So a bit is readable from about five or six cycles after the store that launches it until about
as long after the next one. Production samples a cycle before the next rising edge, at the
window's early end: one or two cycles in at the 16.67 MHz setting, which the block tail's
one-cycle shift used up, with five or six cycles of the window behind the sample unused.
**Prediction, not yet tested:** sampling a few cycles after the rising-edge store would sit near
the middle at that setting, and keep about two cycles either side even at the delay-free step's
five-cycle period. How much room there is past the edge depends on the target's round trip, which
was 15 ns here and which a closer or faster target shortens.

**Sampling after the edge fixes the 16.67 MHz setting (option `SWD_SAMPLE_AFTER_EDGE`, off by
default).** The delayed path samples one instruction after its rising-edge store, with
each transfer masked. The delay-free path is unchanged, because with no delay its low phase
would shrink to a cycle. A build with the option names itself `+after-edge`.

| | default | after-edge |
| --- | --- | --- |
| block-size test at 16.67 MHz, 200 runs a size | the last read fails every time | all whole |
| `DAP_Transfer` at 16.67 MHz | 19,200 clean | 19,200 clean |
| `dap.py limit` | clean to 10.00 | clean to 16.67; 128,000 reads at each of 16.67 and 10 |
| block read, 126 words, depth 2, 16.67 MHz | fails | 973 KiB/s |
| block read, same, 10 MHz, two rounds of three, alternated | 701.6-701.8 | 697.9-698.0 |

The 0.53% at 10 MHz is about four cycles a transfer, which is what saving, setting and
restoring PRIMASK costs. In `cycles.py --check` every SWD trace keeps its event count, and the
only change is that each sample now follows a rising edge rather than a falling one. Turning
the option on by default means re-blessing those five traces.

**What holds it back from the default.** A transfer stays masked for its whole length: 3 us at
16.67 MHz, 46 us at 1 MHz, longer below. SWO receives by interrupt into a 16-entry FIFO
(`RTE_USART3_DMA_EN 0`), which fills in about 20 us at its 8 MHz maximum. So SWO streamed fast
alongside slow SWD would drop bytes, where today it does not. (No longer: SWO is captured by DMA
since 2026-10-19, which a mask cannot hold up.) VCOM is polled from the main loop
and is not affected. The right shape is after the edge with a mask at the fast settings and
before the edge, unmasked, at the slow ones. That is a second copy of the delayed path, and as
unrolled C it does not fit in SRAM. Generating the bit paths made it free (CODEGEN.md).

Four-wire JTAG reads TDO, which changes on the falling edge, before the rising one: half a period
rather than a whole one.

A failed connect still clocks the wire for five seconds while probe-rs retries, and those groups
are not worth reading: the retry changes the requested rate, so the period in them is not the one
asked for.

## The LED: a timer's job, and what it costs the wire

Measured 2026-10-16, on ARM's paths at 10 MHz.

**Ours fades the LED with CTIMER3 as a PWM, and adds activity.** Breathing when nothing is using the probe,
blinking while commands arrive, fully lit when a host has said it is connected and is idle. The
period interrupt runs the policy and a command pays one byte store. `tools/image.py` runs the
interrupt against the image and checks all three, including that one command is visible whatever
tick it lands on; two mutations of the policy were confirmed to fail it.

Measured with `dap.py block --words 126 --blocks 400 --pipeline 2` at 10 MHz, three runs a build,
no spread within any build:

| build | KiB/s | against no LED |
| --- | --- | --- |
| no LED | 703.1 | |
| polled from the main loop, the first version | 701.6 | -0.21% |
| CTIMER3 PWM, 1 kHz tick | 702.5 | -0.09% |
| CTIMER3 PWM, 488 Hz tick, the one kept | 702.8 | -0.04% |
| 488 Hz with the handler in `.ramfunc` | 701.9 | -0.17% |

- **The polled version cost 1.5 us a command**, several times what its instructions explain. It
  ran on the path between one command and the next, which is exactly where the next one waits.
  Flash fetch misses are the likely reason and were not tested, because the timer design removes
  the call from that path whatever the cause.
- **The PWM's cost is its interrupt rate.** Each tick lands wherever the core is, usually in a
  transfer, and stretches it. Halving the rate recovered 0.23 KiB/s against a predicted 0.3.
  `LED_PERIOD` in `src/led.c` is the knob.
- **Moving the handler into SRAM made it worse.** It went in at the head of `.ramfunc`, shifted
  every bit path by 116 bytes and cost 0.11%. `src/hot.h` now says so.

**Confirmed by eye** on the bench unit with `tools/dap.py led`, which walks it through breathing,
blinking and lit and says which is due.

## The command pipeline over USB

Measured 2026-10-16, on ARM's paths at 10 MHz.

### The USB side: the probe held one packet and told the host eight

`tools/dap.py block --pipeline N` keeps N commands in flight instead of waiting for each reply.
Measured on a 64-word block read at 10 MHz asked:

| commands in flight | end to end |
| --- | --- |
| 1 | 581 KiB/s |
| 2 | 646 |
| 3 | 646 |
| 4 | times out |

**A second command in flight is worth 11%**, and it is worth it for a reason nothing on the wire
can fix: with one in flight the wire sits idle while the host notices the reply and sends the next
request. The third adds nothing, and the fourth does not work.

`DAP_Info` said eight packets, copied from ARM's reference. It became 2, which is what the stack
can actually hold: one packet in the endpoint buffer and one in the class buffer.

**TinyUSB's vendor class is a byte stream, not a packet interface**, and that is the whole reason.
`tud_vendor_read` drains whatever the receive buffer holds, so a buffer sized for two commands
hands both to one read and the second is answered by nobody. Raising both buffers to eight packets
was tried and made it worse: the host times out from three in flight upward and throughput falls
as 1/N. A buffer of exactly one packet is what keeps CMSIS-DAP's one-command-per-packet framing
true, so `CFG_TUD_VENDOR_RX_BUFSIZE` and `TX_BUFSIZE` must stay at 512.

### Taken: TinyUSB's non-FIFO vendor mode, which keeps packet boundaries

A buffer size of zero selects it. A received packet arrives at `tud_vendor_rx_cb` with its own
length, and `tud_vendor_write` becomes one `usbd_edpt_xfer`. The command loop holds two request
slots and arms the receive endpoint only while one is free, so a command it cannot place is
refused by NAK rather than dropped.

| | FIFO mode | non-FIFO, two slots |
| --- | --- | --- |
| 1 command in flight | 581 KiB/s | 608 |
| 2 in flight | 646 | 669 |
| 4 in flight | times out | 568, no data lost |

### Through probe-rs, which is the host that matters

Everything above is `tools/dap.py`, which reads a DP register and touches no access port. The
numbers a user sees come through probe-rs, and mculink-bench's `swd-throughput` measures those. At 10 MHz
asked, fitted ceiling over 64 to 4096 byte transfers:

| | stock | ours |
| --- | --- | --- |
| read32 | 469 KiB/s | 593 |
| write32 | 503 | 612 |

1.26x on reads and 1.22x on writes, against the same target through the same harness.

**probe-rs already pipelines, and it sizes the pipeline from what the probe reports minus one.**
The spare packet is where `DAP_TransferAbort` has to land: an abort exists to stop a transfer that
is running now, so it has to overtake the queue rather than join it. A probe advertising two
therefore gets a pipeline of one and no overlap at all, which is why `DAP_PACKET_COUNT` is three.
Measured, three runs each: reads 568 to 593 KiB/s and writes 584 to 612, every run of the deeper
pipeline ahead of every run of the shallower one.

**Our firmware was not honouring that contract.** The abort was queued like any other command, so
the flag was set only once the transfer it was meant to abort had finished. ARM's own MCU-Link
example sets it in the endpoint callback and never queues it; `tud_vendor_rx_cb` now does the
same.

### What limits the pipeline, and why two in flight is the number

The depth a host can use is the number of request slots in `src/main.c`, set by `-DREQUEST_SLOTS`
and reported to the host as `DAP_PACKET_COUNT`. Built with 2, 4 and 8 slots and swept:

| host depth | 2 slots | 4 slots | 8 slots |
| --- | --- | --- | --- |
| 1 | 608 KiB/s | 612 | 632 |
| 2 | 669 | 669 | 669 |
| 4 | 568 | 669 | 669 |
| 8 | host times out | host times out | 667 |
| 12 | | | host times out |

**Throughput saturates at two in flight and stays there.** Four and eight slots measure the same 669 KiB/s
as two. The wire is 92% of a block read, so the link only has to stay a little ahead of it, and
two commands is enough for that.

**Nothing above the slot count is the probe misbehaving.** With no free slot it NAKs, which is the
correct thing to do and loses nothing. What fails is a host that blocks on a write while refusing
to read replies, which deadlocks against its own backpressure -- `tools/dap.py` does exactly that,
and so would anything using synchronous bulk transfers. The probe recovers the moment the host
drains: a depth-8 run against two slots times out, and the next depth-2 run measures 669.5.

So the knob stayed at two then, which was honest and saturating (three since, above). It is worth re-measuring against a
target that answers far above 10 MHz, where the wire would stop being 92% of a block read and the
link could become the limit again.
