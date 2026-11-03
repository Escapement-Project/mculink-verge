# Timing on this part

These are the rules the generator for the bit paths places instructions by, and what
the emulator now models. Every rule comes from a measurement on the bench. MEASUREMENTS.md has the
measurements, under "The cost model times the bus".

## Where generated code runs

In SRAMX, the 32 KB at 0x0400_0000 that this image leaves unused.

The core fetches SRAMX over C-AHB and makes its data accesses over S-AHB, so the two never wait
for each other. ARM's bit paths run from SRAM, which the core fetches over S-AHB. There every
bus cycle carries one instruction word or one data access. A GPIO store can then hold back the
next instruction by a cycle, depending on where the code sits in memory. That is what made the
block's last read fail at 16.67 MHz while the loop's reads passed.

## What an instruction costs from SRAMX

| instruction | cost |
| --- | --- |
| any 16- or 32-bit instruction, including a GPIO or SRAM load or store | 1 cycle |
| an instruction that computes with a loaded register | issues 2 cycles after the load |
| a store of a loaded register | issues the cycle after the load |
| a taken branch: `b`, `b<cc>` or `cbz`, either encoding | 2 cycles, or 3 when the target is a 32-bit instruction that straddles a word |
| a conditional branch that falls through | 1 cycle, or 0 when it pairs |
| a 16-bit `b<cc>` that paired, when taken | 1 cycle more than a taken branch |
| a 16-bit NOP | 0 when it pairs, 1 otherwise |
| a data access over S-AHB right after one through the PPB, or the reverse | waits until 2 cycles after the other |
| `push`, `pop`, `ldm`, `stm` | 1 cycle a register |
| an instruction that uses a multiply's result | issues 2 cycles after the multiply |
| `udiv` | 5 cycles at 100 / 7; the core stops early by operand size |
| `it` | 0 cycles; an instruction its block skips still costs its cycle |
| `bl`, `blx`, `bx lr` | as a taken branch |

Only two instructions pair. A 16-bit NOP or a 16-bit `b<cc>` issues in the same cycle as a 16-bit
instruction before it, unless that instruction paired already. `cbz` never pairs, and neither
does anything after a 32-bit instruction.

Loads and stores pipeline. A GPIO access can issue every cycle, in any mix of loads and stores,
and none waits for the one before it.

Use `mov.w rN, rN` as filler. It costs one cycle at every alignment. A NOP does not.

Prefer `movw` and `movt` for constants. A literal-pool load is a data access on the bus the code
is fetched over, so it holds back the instruction behind it by a cycle unless a taken branch
follows. The model has that, but it is a cost a generator need not pay. A counted loop of `subs.w` and `bne.w` costs 3 cycles an iteration plus
its body when its first instruction is word-aligned, and 4 when it straddles a word.

The PPB is the bus the cycle counter sits on. No bit path reads it, but every timing measurement
does, so the last rule matters to anyone reading a count.

## When a store reaches the pin

Every GPIO store form reaches the pin at the same delay after it issues: SET, CLR and NOT, a byte
to the B register, a word to the W register, and the masked port register. Two edges are as many cycles apart on the wire as
their stores are in issue order. The analyser agrees to a tenth of a cycle. The only other effect
it sees is analog, about 1.7 ns between rising and falling edges at J6.

## When a load sees the target's bit

The target launches each bit on a rising SWCLK edge. The probe sees that bit from A cycles after
the store that raised the clock until A cycles after the next rising-edge store. So a sample that
issues d cycles after the store that launched its bit reads that bit when

    A <= d <= P + A - 1

where P is the clock period in cycles. The window is P cycles wide at every rate.

A is 7 against both targets on this bench, the U575 and the MSPM0L1306, each on its own probe
unit, and at every period measured from 8 to 30 cycles. A load sees the probe's own SWDIO store 3
cycles later, and the buffers, the cable and the target take the other 4. Another target or a
longer cable can add to it, so a generator should still measure it. Sweeping d on a DPIDR read at
connect time does that, as `tools/lab.py` does on the bench.

The window's ends are sharp. No sample position on the bench ever gave a mixture of results.

## What that allows

A read clock needs four instructions: the falling edge, the rising edge, the sample and a store of
the sample. At 150 MHz the shortest read period is therefore 4 cycles, 37.5 MHz. Above that, the
period can be any whole number of cycles, and the split between the high and low phases is free.
A generator picks the shortest period that does not exceed the rate asked for:

| asked | ARM's paths | generated |
| --- | --- | --- |
| 35 MHz | 16.67 | 30 |
| 25 MHz | 16.67 | 25 |
| 20 MHz | 16.67 | 18.75 |
| 12 MHz | 10 | 11.54 |
| 8 MHz | 7.14 | 7.89 |

The furthest a generated rate falls below the one asked is 20% between 20 and 37.5 MHz, 11%
between 10 and 20, and 6% between 5 and 10. ARM's paced paths fall up to 56%, 33% and 24% short in
the same bands, and it overshoots some requests: 16.67 MHz for 16, 7.14 for 7.

A prototype scheduler places a whole sequence from the model alone: two writes and five reads,
with every sample at the middle of its window and every turnaround. It ran clean on the bench at
every period from 4 to 40 cycles, 37.5 to 3.75 MHz, with every acknowledge, word and parity bit
right. Its cycle count matched the model's at every one of the 37 periods. The same scheduler, with
the A it measured, ran clean against the MSPM0L1306 at every one of those periods too. That part
fails above 10 MHz on ARM's paced paths: the failures were the probe's sampling, not the part.

At short periods the sample trails the clock. At P = 4 with A = 7, a bit is readable 7 to 10
cycles after its edge, which is two clocks later. Generated DPIDR reads at every period from 4 to
8 cycles matched the model at every sample position on the bench, the window's edges included.
DPIDR reads clean at 37.5 MHz.

A write clock needs three instructions, with the data bits held in registers, so writes run at
P = 3, 50 MHz. The target took every write at every placement of the SWDIO store, from one cycle
before its rising edge to one cycle after the previous one, at every period from 3 to 10 cycles.
On this target a write needs nothing more than its SWDIO store kept off the cycles of the edges.

## What the generator does with it

The firmware now writes its SWD transfer code at runtime, for the clock the host asks for
(`src/swd_gen.c`, the default build since 2026-10-20). Each clock period is a whole number of cycles
from 3 up, and no period is ever shorter than the host asked for.

A clock the probe drives needs three instructions, so packets and write data run at up to 50 MHz.
A clock the target drives needs four, so reads run at up to 37.5 MHz. Moving cycles between the
low and the high phase does not change that count. The core issues one of these instructions a
cycle and pairs none of them, which the bench measured.

On the MSPM0L1306 every period from 3 to 40 cycles ran clean, and so did 2, 1 and 0.1 MHz. Through
probe-rs, a 64 KB read took 1407 KiB/s at 50 MHz and 1312 KiB/s at 37.5 MHz. The previous firmware
could not read this part above 10 MHz, and managed 577 KiB/s there, against 629 now.

The generated code changes the data bit and the falling edge in one store, to the masked port
register. The data then has half a period before the rising edge and half a period after it.

## Rules for placing a sample

- **Put it in the middle of its window,** d = A + (P - 1) / 2, which leaves (P - 1) / 2 cycles
  either side. Of two middles, the later: a bit that arrives later than measured is the likelier
  mistake.
- **A sample that comes after the next rising edge needs interrupts masked.** An interrupt between
  that edge and the sample makes the sample late, and it reads the next bit. A sample before the
  next rising edge is safe: an interrupt there stretches the clock and nothing else. With A = 7
  that is possible only from P = 8 up, at d from 7 to P - 1.
- **Keep a load two cycles from anything that computes with it.** A store of the sample needs one.
- **Measure A for each target,** and recompute the placement when the clock changes. Sweep the
  sample delay on DPIDR reads at a period of 8 or more: A is the shortest delay that reads
  correctly, and the window is P cycles wide from there. The bench found A = 7 at P = 8, 12 and 16.
  The probe now does this itself for SWD (src/swd_sweep.c), with generated reads whose windows the
  generator reports. At P = 4 a sample can only go on every other cycle, so one period cannot tell
  A from A + 1; it reads at P = 5 to 7 as well.
- **Remember the second turnaround.** After the acknowledge of a write, and after the parity of a
  read, comes one more clock before the probe drives again. Leave it out and the target loses step
  and stops answering.

## What the emulator models

`tools/m33.py` decides when each instruction issues: from its fetch, from the instruction before
it, and from the bus slots data has already taken. `tools/cycles.py --engine` runs on it, and
`--latency 7` adds a SWD target that answers by time and reports every read's distance from both
ends of its window.

From SRAMX it matches 692 instruction sequences timed in isolation at four alignments, plus all 144
covering calls, stack, masking, multiplies, divides, IT blocks and literal loads. It matches 29 pulse trains, every generated read, and every scheduled sequence. It
is a cycle slow on an unconditional branch straight after a read of the cycle counter, which no
bit path does.

From SRAM it is a cycle fast at the alignment where 32-bit instructions straddle words, from the
cycle-counter read onwards. It still reproduces every SWD result the bench has for the current
firmware. Flash is not modelled, and part of the command layer still runs from there. The JTAG and
two-pin paths have timed targets of their own (`TimedJtagTarget` and `TimedCjtagTarget` in
`tools/cycles.py`, checked by `tools/jtagcheck.py`).

## How to measure more

Build with `-DCODE_LAB=ON` for vendor 0x88, which runs code the host sends and times it.
`tools/lab.py corpus` runs the instruction cases on the probe and in the emulator at the same
address, and `swd_read_pipelined` generates a read with the sample placed to the cycle. A case
that hangs the probe resets it, unless it loops for ever with interrupts off.
