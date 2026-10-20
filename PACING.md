# How the wire clock is paced, and what is wrong with it

Three findings share one cause. Four-wire JTAG clocks the target faster than the host asked for.
The two-pin path has under a cycle of margin, and which side of the line it lands on is decided by
optimisation flags. And the achievable rate goes up in steps of six cycles, so between 28.8 MHz
and 13.1 MHz there is nothing at all.

All three come from one number, `DAP_Data.clock_delay`, being calculated once for a bit that only
one of the three wires actually has.

Everything below is the emulator's cost model. **No analyser has checked it.** Read the last
section first if you are about to change a constant.

## Where the number is set

A host sends `DAP_SWJ_Clock` (command `0x11`) with a frequency in Hz.

1. `src/dap_dispatch.c` passes it through to ARM's command processing, which is renamed
   `arm_DAP_ProcessCommand` by `CMakeLists.txt`.
2. `external/CMSIS-DAP/Firmware/Source/DAP.c:420`, inside `DAP_SWJ_Clock`, calls
   `Set_Clock_Delay(clock)`.
3. `external/CMSIS-DAP/Firmware/Source/DAP.c:61` is `Set_Clock_Delay`. It is twenty lines and it
   is the whole of the problem:

   ```c
   if (clock >= MAX_SWJ_CLOCK(DELAY_FAST_CYCLES)) {
       DAP_Data.fast_clock  = 1U;
       DAP_Data.clock_delay = 1U;
   } else {
       DAP_Data.fast_clock  = 0U;
       delay = ((CPU_CLOCK/2U) + (clock - 1U)) / clock;
       if (delay > IO_PORT_WRITE_CYCLES) {
           delay -= IO_PORT_WRITE_CYCLES;
           delay  = (delay + (DELAY_SLOW_CYCLES - 1U)) / DELAY_SLOW_CYCLES;
       } else {
           delay  = 1U;
       }
       DAP_Data.clock_delay = delay;
   }
   ```

   `MAX_SWJ_CLOCK` is at `DAP.c:48` and is `(CPU_CLOCK/2) / (IO_PORT_WRITE_CYCLES + delay_cycles)`.

The four constants it reads:

| constant | value | where |
| --- | --- | --- |
| `CPU_CLOCK` | 144000000 | `src/DAP_config.h:57` |
| `IO_PORT_WRITE_CYCLES` | 2 | `src/DAP_config.h:65` |
| `DELAY_SLOW_CYCLES` | 3 | `external/CMSIS-DAP/Firmware/Include/DAP.h:327` |
| `DELAY_FAST_CYCLES` | 0 | `external/CMSIS-DAP/Firmware/Include/DAP.h:348` |

The model is `period = 2 * (IO_PORT_WRITE_CYCLES + delay * DELAY_SLOW_CYCLES)`. One half-bit is one
I/O write plus one delay loop, and there are two half-bits.

## Where the number is consumed

`DAP_Data.clock_delay` reaches the wire through `PIN_DELAY()`, which each bit path defines for
itself. `DAP_Data.fast_clock` selects between a paced copy and a delay-free one, once per transfer
rather than once per bit.

- **SWD** — `src/swd_dp.c:48`, `:211` and `:215`. `SWD_TransferFunction` is expanded twice, with
  `PIN_DELAY()` meaning `PIN_DELAY_SLOW(DAP_Data.clock_delay)` for the `Slow` copy and
  `PIN_DELAY_FAST()` for the `Fast` one. `SWD_Transfer` picks between them on `fast_clock`.
- **Four-wire JTAG** — `src/jtag_dp_block.c:58`, `:159` and `:164`, the same arrangement around
  ARM's `JTAG_DP.c`.
- **Two-pin** — `src/cjtag.c:13` defines `CJTAG_DELAY()` the same way, and
  `CJTAG_FRAME_FUNCTION` at `src/cjtag.c:130` is expanded twice at `:148` and `:149`. The two-pin
  copies of `JTAG_DP.c` shadow the pin layer so each four-wire clock becomes one OScan1 frame, and
  they suppress `PIN_DELAY` entirely — see `src/cjtag_jtag_dp.c:79` — because the frame does its
  own pacing.

The two delay primitives are in `external/CMSIS-DAP/Firmware/Include/DAP.h`.
`PIN_DELAY_SLOW` at `:335` is inline assembly, a `subs` and a `bne`, which is where
`DELAY_SLOW_CYCLES` of 3 comes from. `PIN_DELAY_FAST` at `:350` expands to nothing, because
`DELAY_FAST_CYCLES` is 0.

## Why one constant cannot serve three wires

Count the I/O writes in one clock of each:

| | writes per clock | where |
| --- | --- | --- |
| SWD bit | 3 | `SW_WRITE_BIT` and `SW_READ_BIT`, `src/swd_dp.c` |
| four-wire JTAG clock | 2 | `JTAG_CYCLE_TCK`, `external/CMSIS-DAP/Firmware/Source/JTAG_DP.c:39` |
| two-pin frame | 13 | `CJTAG_FRAME_FUNCTION`, `src/cjtag.c:130` |

`IO_PORT_WRITE_CYCLES` is 2, which is calibrated for the three-write SWD bit. A four-wire JTAG
clock does one write fewer, so it finishes early and the wire runs fast: 10.29 MHz for a 10 MHz
request, 72 MHz for 50. The two-pin frame does far more, and borrows the same number without
anything ever telling `Set_Clock_Delay` what a packet costs. It lands on 14 or 15 cycles for a
10 MHz request, against the 14.4 it needs, and the compiler decides which.

`tools/baseline-armwire/known-overshoot.txt` records `jtag@10MHz` and `jtag@50MHz` so the gate
fails on a *new* overshoot rather than on these two.

## The steps, which may be worth more than the overshoot

Measured steady-state SWD data-bit periods, from `uv run tools/cycles.py --wire swd --clock N`:

| asked | period | wire |
| --- | --- | --- |
| 40–50 MHz | 5 cycles | 28.8 MHz |
| 16–30 MHz | 11 cycles | 13.1 MHz |
| 10–14 MHz | 17 cycles | 8.5 MHz |
| 8 MHz | 23 cycles | 6.3 MHz |

Every step is six cycles: two delays, three cycles each, because the delay is a counted loop.
Between 28.8 and 13.1 MHz there is nothing, so a target good for 20 MHz runs at 13.1 and gives up
about half its throughput. That is larger than every optimisation in `OPTIMISATIONS.md` combined.

**Runtime code generation is the wrong fix.** Reaching a generated delay needs a branch, and a
`bl` with its return is about four cycles against a delay-free bit that costs five. Any scheme
that calls the delay — generated code, or a computed jump into a sled of `nop`s — loses more at
the top of the range than it gains in the middle.

**Compile-time specialisation is the right one, and the pattern is already here.** `SWD_Transfer`
is already compiled twice and chosen per transfer on `fast_clock`. Compile the fast path five or
six times instead, with 0, 1, 2 … `nop`s baked into `PIN_DELAY`, and have `Set_Clock_Delay` pick
the copy. That fills in periods 6 through 10 where the gap is, costs a `nop` rather than a branch
per bit, and the dispatch is once per transfer and already paid. It costs flash, a couple of KB a
copy.

An odd number of `nop`s makes the high and low phases unequal. That is not a new risk: a `nop`
only ever lengthens a phase, so neither edge gets tighter than the delay-free path that already
ships.

## Telling the host what it actually got

`DAP_SWJ_Clock` takes four bytes of frequency and answers one byte, `DAP_OK` or `DAP_ERROR`. There
is no field anywhere in CMSIS-DAP for the rate the probe will actually run at — `DAP_Info` has no
entry for it, and the nearest precedent, `DAP_ID_TIMESTAMP_CLOCK`, is a different clock. So a host
asks for 30 MHz, is told yes, and gets 13.1.

We own `0x84` upward, so a vendor command can answer it: given a frequency, report what the probe
would actually clock; given nothing, report what it is clocking now. `0x82` already lets a host
recognise this firmware before asking, so nothing else needs to change.

**Build it as part of this work rather than before it.** The number such a command would report is
computed from `clock_delay` and the same per-path constants that are wrong, so shipping it first
would publish the model that is in question, with a precision it has not earned. Once the capture
below has settled the costs and the paths have pacing of their own, the command reports something
true — and it is then also the cheapest way for a host to find the right request, which with steps
this coarse is worth more than it sounds.

## Settle the cost model before changing a constant

Every figure above is arithmetic over a cost model whose constants are command-line arguments to
`tools/cycles.py` — one cycle for a store, two for a load, two for a taken branch. If a GPIO store
costs two cycles on this part rather than one, the JTAG overshoot and the two-pin margin both
disappear. The step structure survives either way, because six cycles comes from the loop rather
than from the store.

One analyser capture settles it. Measure the real wire period at two delay settings and solve for
the store and branch costs; `uv run tools/cycles.py --calibrate` does the solving. The one bench
number that exists agrees with the model — a 1 MHz request measured 0.94 MHz on stock and the
emulator says 0.94 — but that is the slowest setting, where the delay loop dominates and the store
cost barely shows.

## If the model turns out to be right

Three ways out of the JTAG overshoot, all measured, all costing throughput:

- `DELAY_FAST_CYCLES = 1` fixes 50 MHz, leaves 10 and 30 over, and costs SWD 22% at the top,
  because SWD shares the same delay.
- Overriding `PIN_DELAY_FAST` for the four-wire copy alone fixes everything above the switch point
  and leaves SWD untouched, for about 22% of JTAG's top end. The 3% at 10 MHz stays.
- Doing nothing keeps the throughput and leaves a probe that will overdrive a target asked for a
  clock near its limit.

The two-pin margin wants a pacing calculation of its own with a cycle of headroom, rather than a
number calibrated for a four-wire bit. That is the same piece of work as the specialisation above,
so do them together.

Until then, `ninja -C build check` refuses a build that clocks faster than asked beyond the two
recorded cases. **Run it before flashing anything.**
