# Runtime code generation of the bit paths

Companion to TIMING.md (the human-readable rules). Read TIMING.md first, then this.
Dense on purpose. Everything here was measured on the bench 2026-10-16 unless marked otherwise.
BENCH.md section "The cost model now times the bus, and reproduces every SWD result so far" has
the evidence.

## Goal and decisions

- Emit straight-line Thumb-2 for each wire path at runtime, per clock setting, with every store
  and sample placed to the cycle. The reasons: cycle-exact placement; timing independent of
  the optimiser (Debug clocks like Release); more optimisation room; much finer rate selection
  (1-cycle periods instead of today's 6-cycle steps).
- **Generated code lives in SRAMX** (0x0400_0000, 32 KB, unused by the image; C-AHB fetch).
  Replacing the unrolled `.ramfunc` C paths (~146 KB of SRAM today, see below) frees SRAM
  for stack and buffers.
- Per-rate choice of sample placement and interrupt masking belongs to the generator. The
  `SWD_SAMPLE_AFTER_EDGE` CMake option (off) was the stopgap; the generator supersedes it.
- Flash is not a constraint; SRAM is (m_data ~85% today).
- Repo conventions: commit subject `scope: what changed`.
  Gate: `ninja -C build check`.
  Never put LED or other work in a bit path. Fix unbounded accesses when found.

## The timing model (exact from SRAMX; see TIMING.md for prose)

All costs in core cycles at 150 MHz (6.67 ns). Issue cycle = cycle an instruction enters execute.

- Every instruction 1 cycle (16- or 32-bit, ALU, `ldr*`/`str*` to GPIO or SRAM).
- GPIO/SRAM loads and stores pipeline back to back, any mix, no stalls between them.
- Load result: ALU consumer issues >= load + 2; a store of the loaded register (store-data
  operand) issues >= load + 1.
- Taken branch (b, b<cc>, cbz/cbnz, any encoding): target issues at branch + 2, + 1 more if the
  target is a 32-bit instruction at an address = 2 mod 4.
- Not-taken conditional branch: 1 cycle; 0 when a 16-bit b<cc> pairs (below).
- Pairing: a 16-bit NOP or 16-bit b<cc> issues with the preceding 16-bit instruction, unless that
  one already paired. cbz/cbnz never pair; nothing pairs after a 32-bit instruction. A paired
  b<cc> that is taken redirects 1 cycle later (target at pair cycle + 3).
- PPB (0xE000_0000+, DWT->CYCCNT) vs S-AHB data: an access on one waits until 2 cycles after the
  last access on the other. Bit paths never touch PPB; only timing instrumentation does.
- Filler: `mov.w rN, rN` (1 cycle anywhere). Never NOP.
- push/pop/ldm/stm: 1 cycle per register. ldrd/strd: 2 accesses. mul: result usable at +2.
  udiv: 5 at 100/7 (early termination by operand size; keep out of timed code). `it`: 0 cycles;
  an IT-skipped instruction still costs its cycle (unicorn does not run it; cycles.py issues it).
  bl/blx/bx: as a taken branch. cpsid/cpsie, mrs/msr primask: 1 cycle each.
- Literal-pool loads (C-AHB data access from SRAMX) cost +1 cycle each unless a taken branch
  follows; modelled exactly since the prefetch buffer is counted in halfwords (5 halfwords,
  release at issue: `Params.queue_halfwords`). Still prefer movw/movt: no cost at all.
- Loop `subs.w rX, rX, #1; bne.w head`: 3 cycles/iteration + body if `head` is word-aligned
  (4 if the head is a 32-bit instruction at 2 mod 4). Last iteration (fall-through): 2 + body.
- Store -> pin: identical delay for SET (GPIO+0x2200+4*port), CLR (+0x2280), NOT (+0x2300),
  B byte (GPIO + 32*port + pin), W word (GPIO + 0x1000 + 128*port + 4*pin). Edge spacing on the
  wire = store issue spacing (analyser, to 0.1 cycle; ~1.7 ns analog rise/fall skew at J6).
- **Read window:** target launches bit k on rising-edge store R_k. A load issued at R_k + d reads
  bit k iff `A <= d <= P + A - 1` (P = period in cycles, assuming uniform period; in general
  `A <= d < (R_{k+1} - R_k) + A`). d < A reads bit k-1 ("behind"), d >= next + A reads k+1
  ("ahead"). Boundaries sharp, deterministic, no mixtures seen in thousands of reads.
- **A = 7 against the U575 on the bench** (internal self-loop 3: store to B[SWDIO] then load of
  it sees the new level 3 cycles later; + 4 external: buffers, ~15 ns cable+target). A is per
  target/cable: calibrate at connect (sweep d on DPIDR reads, take the midpoint of the passing
  range). A is in core cycles: recompute if the core clock changes.
- From SRAM (not where generated code goes) the model is approximate: fetch and data share S-AHB
  one transaction a cycle; misses are -1 at the straddling alignment after a CYCCNT read (tried
  and rejected: a separate SRAM buffer depth, PPB ordering applied to fetch). Irrelevant to the generator
  except for comparing against the old C paths.

Verified end to end: `tools/lab.py swd_read_pipelined` at P = 4..8 (37.5..18.75 MHz), all
non-colliding sample delays, 45/45 configurations predicted exactly (verdict and cycle count) on
the U575. Soak: 200 reads each at P=4 d=9 (margin 1),
P=4 d=7 (margin 0, window edge), P=5 d=9, P=8 d=10: all clean, 7,200 target bits each.

## Scheduler prototype (validated end to end)

`tools/lab.py swd_schedule(period, transfers, latency=7)` + `schedule_asm()`: given a period and
`[("w", req, value), ("r", req), ...]`, lays every clock (fall at +0, SWDIO store at +1 for
probe-driven bits, rise at +P//2), samples each target bit at the max-margin cycle of its window
avoiding edge cycles, stores samples to the buffer in r1 by the next free cycle (rotating r0,
r12, lr), and inserts a `settle()` barrier where the probe takes the wire back (waits for all
outstanding samples; only ever lengthens a clock). Runs of >= 3 empty cycles become
`movw r7,#n; subs.w; bne.w` loops (3n cycles; all instructions 32-bit so every head is aligned;
the body start is `.balign 4`). Registers: r5 clock mask (also the data-1 source), r6 = 0,
r2 SWDIO mask, r3 TXEN mask, r8 B, r9 SET, r10 CLR, r11 DIRSET, r4 CYCCNT, r7 loop counter.

Bench: sequence = write TAR X, AP read TAR,
read RDBUFF (= X), read DPIDR, write TAR Y, AP read TAR, read RDBUFF (= Y). Every period 4..40
(37.5..3.75 MHz), 10 runs each: all acks, data and parity correct, and hardware cycle count ==
model cycle count at all 37 periods. Pitfall found on the way: **each transfer needs the second
turnaround clock** (after the ack for writes, after parity for reads); without it the target
loses step and every later ack reads 7.

Rate granularity (P = ceil(150 MHz / requested), never above the request): worst shortfall
20% in 20-37.5 MHz, 11% in 10-20, 6% in 5-10, 3% in 1-5; today's ladder 56/33/24/13% and it
overshoots some requests (16 -> 16.67, 7 -> 7.14).

Calibration procedure (validated): sweep the sample delay d with `swd_read_pipelined` at P >= 8;
A = smallest passing d; the passing range is exactly P wide (A = 7
at P = 8, 12, 16).

## Throughput limits implied

- Read clock needs: fall store, rise store, sample load, sample consume (store to buffer or
  accumulate) = 4 instructions -> P >= 4 (37.5 MHz) at 150 MHz.
- Write clock: fall store, data store (`strb` of a register holding 0/1 to B[SWDIO]), rise
  store = 3 if the data bit is already in a register -> P >= 3 (50 MHz). Extracting bits from a
  word costs an extra instruction per bit (`lsr`/`ubfx`) unless precomputed.
- **Write window (measured with `tools/lab.py swd_write_asm`):** AP TAR writes with the SWDIO
  store `lead` cycles before the rising
  edge, lead 1..P-1, P = 3,4,5,6,8,10, 10 writes each, read back at 1 MHz: all land. So setup
  and hold at the target are each under one cycle; any placement off the edge cycles works.
  50 MHz writes (P = 3) work on this harness, request phase included.
- Idle clocks after a transfer must drive SWDIO low: high on a rising edge is a start bit, the
  target reads a malformed packet and needs a line reset.
- Packet request bits are constant per request byte: emit them as stores of two constant
  registers (0 and 1), or specialise per request.
- Sample consume options: `strb.w rS, [rBuf, #k]` (no stall after the load; unpack and check
  parity in C afterwards, outside the timed region) or `orr.w rAcc, rAcc, rS, lsl #k` /
  `bfi rAcc, rS, #k, #1` (needs load + 2). Rotating sample registers (lab uses r0, r12, lr) let
  the consume lag the load.
- Rate table at 150 MHz: P=4 37.5, 5 30, 6 25, 7 21.43, 8 18.75, 9 16.67, 10 15, 12 12.5,
  15 10 ... Any P; high/low split free. CMSIS-DAP clock is a ceiling: never let any period run
  shorter than requested (the old gate's "overshoot" check). Finer steps at the top would need a
  different core clock (PLL) - not explored; USB has its own PHY clock but SWO baud/timers derive
  from the core.

## Interrupts

- A sample placed after the next rising edge (d >= P, i.e. always when P <= A) must run with
  interrupts masked: an interrupt between that edge and the sample delays the sample past its
  window ("ahead"). A sample before the next rising edge is interrupt-safe (an interrupt only
  stretches the clock). With A=7, interrupt-safe placement exists only for P >= 8 (d in [7, P-1]).
- SWO receives by USART3 interrupt into a 16-entry FIFO (`RTE_USART3_DMA_EN 0`); fills in ~20 us
  at 8 MHz SWO. A transfer masked for longer than that drops SWO bytes. Keep masked spans short
  (per transfer, not per command), or enable SWO DMA. VCOM is polled from the main loop.

## CMSIS-DAP semantics the generated paths must keep (from src/swd_dp.c, ARM's SW_DP.c/DAP.c)

- Request byte: bit0 APnDP, bit1 RnW, bit2 A2, bit3 A3, bit4 MATCH_VALUE, bit5 MATCH_MASK,
  bit7 TIMESTAMP. Packet: start 1, APnDP, RnW, A2, A3, parity(bits0-3), stop 0, park 1.
  `SWD_WithParity` stores the packet parity in request bit 6.
- After park: release SWDIO (`PIN_SWDIO_OUT_DISABLE`: TXEN P0_28 low via CLR, then DIRCLR
  SWDIO), turnaround = `DAP_Data.swd_conf.turnaround` clocks (1..4, from DAP_SWD_Configure;
  `SWD_Turnaround()` asserts the range), then 3 ack bits (ack0 first; OK=1, WAIT=2, FAULT=4).
- OK + read: 32 data bits LSB first + parity, turnaround, `PIN_SWDIO_OUT_ENABLE` (DIRSET then
  TXEN SET). Parity error -> `DAP_TRANSFER_ERROR` (0x08).
- OK + write: turnaround, enable, 32 data bits + parity (driven by probe).
- Then: timestamp if requested (DWT CYCCNT -> `DAP_Data.timestamp`); idle cycles
  (`DAP_Data.transfer.idle_cycles`, SWDIO driven 0); finally SWDIO driven 1.
- WAIT/FAULT: if `swd_conf.data_phase`, clock a dummy 33-bit data phase (read: before the
  turnaround; write: after it with SWDIO 0); turnaround; enable; SWDIO 1.
- Any other ack (protocol error): clock turnaround + 33 cycles, enable, SWDIO 1.
- Retries: `DAP_Data.transfer.retry_count` on WAIT, abortable by `DAP_TransferAbort`.
  Match retries: `DAP_Data.transfer.match_retry`, `match_mask`.
- DAP_Transfer posts AP reads (first AP read's data discarded; the value arrives with the next
  AP read or an RDBUFF read). DAP_TransferBlock: read block posts the AP read once, loops,
  reads the last word from RDBUFF (peeled into `SWD_TransferTail*`); write block checks the last
  write with an RDBUFF read.
- Equivalence: `tools/equiv.py` drives two builds over 528 cases (every ack, turnaround, data
  phase, idle cycles) and requires identical wire events and replies. A generated path must
  pass it against ARM's build; it is how the current C paths were proven.

## Current code map (what the generator replaces or plugs into)

- `src/swd_dp.c`: `SWD_TransferFunction(Fast|Slow)` (ARM's transfer, unrolled data phase),
  `SWD_TransferCommand{Fast,Slow}` (DAP_Transfer, generated macro `src/swd_transfer.h` from
  `tools/extract_swd_transfer.py`), `SWD_TransferBlock{Fast,Slow}` + `SWD_TransferTail*`.
  `DAP_Data.fast_clock` picks Fast (no delay loop) vs Slow (`PIN_DELAY_SLOW(clock_delay)`).
- `src/dap_dispatch.c`: owns `DAP_ProcessCommand`/`DAP_ExecuteCommand` (ARM's renamed to
  `arm_*`), routes SWD DAP_Transfer/TransferBlock to swd_dp.c; JTAG and cJTAG to their files.
- `src/hot.h`: `HOT_PATH` = noinline + `.ramfunc` (SRAM). Read its warnings: anything added to
  `.ramfunc` moves every bit path.
- `src/DAP_config.h`: pin macros (`PIN_SWCLK_TCK_SET/CLR`, `PIN_SWDIO_OUT(_ENABLE/_DISABLE)`,
  `PIN_SWDIO_IN`), `CPU_CLOCK`, `IO_PORT_WRITE_CYCLES 2`, `DAP_PACKET_SIZE 512`.
- ARM's `Set_Clock_Delay` (external/CMSIS-DAP/Firmware/Source/DAP.c:61): fast path when
  clock >= (CPU/2)/IO_PORT_WRITE_CYCLES (37.5 MHz request at 150 MHz; its wire rate is 30 MHz),
  else delay = ceil((ceil((CPU/2)/clock) - 2) / 3). Wire periods today: delay d -> 6d+3 cycles
  (delay 1 = 9 = 16.67 MHz, delay 2 = 15 = 10 MHz). PACING.md has the whole story. The generator replaces
  this mapping with period P = ceil(CPU / clock).
- `src/jtag_dp_block.c`, `src/cjtag_*.c`: JTAG/two-pin paths (two-pin `cjtag_BlockImpl` and
  `cjtagf_BlockImpl` are 35+32 KB of .ramfunc). Out of the generator's first scope; same approach later.
  JTAG samples TDO after the falling edge (half-period window); no timed TAP model exists yet.
- `.ramfunc` today (bytes): cjtag_BlockImpl 35k, cjtagf_BlockImpl 32k, SWD_TransferCommandSlow
  27k, Fast 16k, SWD_TransferBlockSlow 11k, Fast 6k, tails 4k, JTAG_TransferBlock 8k+5k.

## Pins and addresses (LPC55S69, non-secure aliases as the image uses)

- GPIO base 0x4008_C000. Port 0 only for debug pins. B (byte) regs at base + pin; W (word) at
  base+0x1000+4*pin; DIR +0x2000; MASK +0x2080; PIN +0x2100; MPIN +0x2180; SET +0x2200;
  CLR +0x2280; NOT +0x2300; DIRSET +0x2380; DIRCLR +0x2400.
- P0_0 SWCLK/TCK, P0_1 TDI, P0_2 SWDIO/TMS, P0_3 TDO/SWO, P0_19 nRESET, P0_28 SWDIO TXEN
  (buffer direction; high = probe drives), P0_13 nRESET TXEN. J8 marker pins: port 1, pins 0-3.
- SRAMX 0x0400_0000-0x0400_7FFF: executable, always clocked; nothing in the image uses it.
  Data accesses to SRAMX go over C-AHB and would contend with fetch: keep data in SRAM.
- DWT->CYCCNT 0xE000_1004 (enabled by `board_timestamp_init`).
- Code executes fine from 0x2000_0000 and 0x0400_0000 (SAU off, all secure).
- Barriers after writing code: `__DSB(); __ISB();` before calling it (code_lab.c does this).

## Tools

- `tools/m33.py`: timing engine (`Engine`, `Params`); defaults are the fitted values.
- `tools/cycles.py`: emulator. `--engine` uses m33; `--latency 7` adds `TimedSwdTarget`
  (per-read `Read.early/late/margin`; `serve="wire"` returns what timing puts on the pin,
  `"intended"` keeps the run going). Memory hook must cover plain memory only (an MMIO-covering
  hook changes unicorn's behaviour on the GPIO block). The gate (`--check`) passes on both models.
- `tools/lab.py` + vendor 0x88 (`src/code_lab.c`, `-DCODE_LAB=ON`, build dir `build-lab/`):
  LAB_INFO/WRITE/READ/RUN; arenas 0 = 4 KB SRAM buffer, 1 = SRAMX. `HardwareLab` and
  `EmulatedLab` share an interface (write/run/arenas), so the same blob runs in both.
  `timed(body)` template measures a body with CYCCNT (use `drained=True` cases to avoid the PPB
  boundary); `swd_read_asm` (sample within its clock) and `swd_read_pipelined` (sample at a delay
  from the launching edge, stored to a buffer) are working generator prototypes.
- `tools/dap.py`: `swd_reset` (line reset + DPIDR), `limit`, `block`, `sample` (vendor 0x87).

## Bench state and hygiene

- Probe `CJ6B19D4E7` runs `0f301e9+lab`. Flash: `uv run tools/flash.py --cycle --serial
  CJ6B19D4E7 --elf <elf>`. Target NUCLEO-U575ZI-Q on J6 (SWD), DPIDR 0x0BE12477. Analyser
  Logic 8 (no settable threshold): D0 SWDIO, D1 SWCLK at J6; 100 MS/s; cannot resolve 1-2 cycle
  pulses.
- A failed write leaves WDATAERR sticky: write ABORT 0x1E before trusting later acks. AP access
  needs CTRL/STAT = 0x5000_0000 (debug power-up) first. A second RDBUFF read returns 0 on this DP.
- Lab blobs: stay inside the template's stack scratch ([sp, #0..28]); a store above it corrupts
  the saved registers (that hung the probe once; lab builds now reset on HardFault, but an
  infinite loop with interrupts masked still needs a replug).

## Open questions for the generator

- Generator in firmware (C emitting Thumb-2 into SRAMX at DAP_SWJ_Clock / SWD_Configure /
  TransferConfig time) vs host-side. Firmware-side is the target; the Python prototypes in
  lab.py are the reference encodings.
- How to cover WAIT/FAULT/protocol-error branches: generated straight-line fast path plus
  out-of-line (non-timing-critical) handlers, or generated branches. A taken branch costs 2-3
  cycles and stretches a clock; place it where a stretch is harmless (low phase before a fall).
- A calibration at connect (A sweep) vs a fixed A with margin. Margin at P=4 is only 1 either
  side even when centred.
- Reads above 37.5 MHz need fewer than four instructions a read clock (not possible with one
  load and one consume per bit at 150 MHz). Clocks can differ in length within a transfer, so
  writes at P = 3 and reads at P = 4 can share one transfer.
- Timed TAP model for JTAG/two-pin (TDO window after the falling edge) not built.
