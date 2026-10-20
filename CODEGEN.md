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

## The generator as built (src/swd_gen.c, behind SWD_CODEGEN, on by default since 2026-10-20)

- `swd_gen_build(config)` writes two functions into SRAMX 0x0400_0000..0x0400_3FFF (lab arena is
  the upper 16 KB): read and write, each `uint64_t fn(packet, words, count)`. Runs `count`
  transfers of one request back to back, stops at the first non-OK ack; returns ack (ERROR for a
  read parity mismatch) in r0 and transfers not completed (failing one included) in r1. `packet`
  = `swd_gen_packet(request)`: bit n clocked on clock n (start..park).
- C keeps ARM's command logic (`src/swd_dp.c` codegen section): posted AP reads, the RDBUFF read
  ending a block, write-block RDBUFF check, WAIT retries (`transfer_words` retries the failing
  transfer with count 1, ARM's `retry--` semantics), timestamps (captured after the transfer;
  only OK). DAP_Transfer reuses the generated `swd_transfer.h` macro with `SWD_Transfer` = one
  generated transfer. `arm_SWD_Transfer` (SW_DP.c's, renamed) runs when generation fails
  (`arm_words` wraps it in the same contract).
- When: `swd_gen_ready()` compares (period, latency, turnaround, data_phase, idle) with the last
  build; `dap_dispatch.c` captures DAP_SWJ_Clock's Hz (ARM keeps only a delay) and calls it after
  SWJ_Clock / SWD_Configure / TransferConfigure, so generation never lands inside a transfer.
  `swd_gen_builds` / `swd_gen_failures` count outcomes (read them in the emulator).
- Periods: requested P = ceil(CPU/Hz). Probe-driven clocks (packet, write data) at max(P, 3), low
  = 2 at P = 3 else P/2; target-driven, turnaround and idle clocks at max(P, 4), low = P/2. No
  clock is ever shorter than asked; stretches only lengthen. P <= 2^24 else ARM's path.
- Clock = fall store + rise store `str r5, [r9, #SET]`. Probe-driven falls are MPIN stores
  carrying SWDIO (MASK0 = ~(SWCLK|SWDIO), set at build; nothing else uses MPIN), so data changes
  with the fall: trace order W-then-v matches ARM's, and setup = low, hold = high. Target-driven
  falls are CLR stores (they must not touch SWDIO's latch: the enable shows its level).
- Samples: masked iff read period < A + 6 (centre of [A, P+A-1], may follow the next rise;
  `msr primask` at body start, unmask in the gap so pending IRQs run between transfers); else
  unmasked, as late as possible before the next rise (early margin P-1-A >= 5, late margin A).
- Registers: r0 packet, r1 words (post-inc), r2 SWDIO mask (=4, MPIN 1), r3 TXEN mask, r4 count,
  r5 1 (SWCLK mask, B-register 1), r6 0, r7 delay loops, r8 GPIO (B regs), r9 MPIN0 (SET +0x80,
  CLR +0x100, DIRSET +0x200, DIRCLR +0x280), r10 word, r11 ack/temp, r12 + lr samples/temps.
  Packet extraction temps alternate r11/r12 (never overwrite a register a store read the cycle
  before); write data temps r12/r11, parity folded into lr in free slots.
- Scheduler: sorted (cycle, insn) events (256 max, static 2 KB); `clock()` bumps a clock until
  both edge cycles are free; `op()` = before the next clock (pushes `t`); `side()` = must not
  delay a clock (avoids the grid of the clocks still to come); `sample()` picks the max-margin
  free cycle. Emission fills gaps with `mov.w r6, r6` or `movw r7,#n; subs.w; bne.w` (3n cycles).
  Tails (WAIT/FAULT/protocol error, long idle runs) are sequential code with clock loops on r12.
- Two bugs found by equiv, both now structural rules: a delay loop between the ack `cmp` and its
  `bne` clobbered the flags (cmp now sits in the cycle before the branch); the branch was placed
  before the last turnaround clock (now after the last common rising edge).
- Verified: equiv `--ignore-samples` 536/536 blocks at 50/37.5/25/12.5/10/1/0.1 MHz, 360/360
  DAP_Transfer at 50/37.5/10/1. `tools/gencheck.py` (engine + timed target serving the wire):
  every P 3..40, 75, 150, 1500 clean; margins P4:1 P5-6:2 P8:3 P9-10:4 P11-13:5 P>=15:7. Running
  block cycles a transfer vs floor (8 packet clocks at the write period, 38 at the read period):
  P3 read 203/176 write 164/143; P4 210/184, 197/184; from P = 29 exactly the floor. Gate: generated
  images run on the engine against `tools/baseline`, ARM's against `tools/baseline-armwire` (S
  positions differ).
  Hardware (L1306): see Bench state.

## Timer-clocked SWCLK (lab prototype, 2026-10-16)

- PIO0_0 FUNC 3 = CTIMER0_MAT0. CTIMERCLKSEL0 = 0 (main clock = core clock), AHBCLKCTRL1 bits
  26/27 (CTimer 0/1), PR 0, PWMC bit 0, MR3 = period-1 with MR3R, MR0 = low cycles (output low for
  TC < MR0). Locked to the core: P = 2..10 exact, first rising edge read 6 + low cycles after the
  start store, deterministic.
- Any store to a CTimer register (APB) stalls the core ~4 cycles (CYCCNT: +4 for one, +10 for two;
  a CTimer load ~+9). A TCR = 0 store freezes the clock at the level a read 9 cycles later would
  see; what follows the store runs 3 cycles late. At P = 3 that stall always swallows one sample
  window, so the stop is a DMA write: CTimer 1 one-shot (MR0 = delay, MR0S) started a cycle after
  CTimer 0, its match 0 -> DMA0 ch 0 (INPUTMUX DMA0_ITRIG_INMUX0 = 6, input mux clock
  AHBCLKCTRL0 bit 11), one word 0 -> CTIMER0 TCR; clear CTimer 1 IR after setting MR0 (TC = MR0 =
  0 after reset raises the request). Deterministic only if the core keeps off the SRAM bank the DMA
  reads: keep samples in registers.
- Prototype read (timerdma.py): packet on GPIO, pin switch, both starts, 38 edges from the timer at
  P = 3, samples at 9 + 3k in that frame (window ~[7.5, 9.5] + 3k), delay 92/93 freezes high after
  edge 37, pin back to GPIO: 2000/2000 DPIDR reads, LA 38 edges at exactly 50.0 MHz.
- Fillers must not read a load's destination (mov.w r12, r12 after ldrb r12 stalls a cycle): with
  a free-running clock every stall drifts the later samples.
- Generator integration not done. Per transfer it costs the pin switch twice, two starts and a DMA
  re-arm (APB stores, ~5 cycles each), so a read transfer gains ~10% unless the timer clocks the
  whole transfer and only stops at the acknowledge decision. probe-rs throughput is USB-bound at
  ~half the 37.5 MHz wire rate already.

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
  B byte (GPIO + 32*port + pin), W word (GPIO + 0x1000 + 128*port + 4*pin), and MPIN (+0x2180;
  low phases of MPIN-fall and CLR-fall clocks agree to 0.2 ns over 2.4 M clocks, L1306, P=20). Edge spacing on the
  wire = store issue spacing (analyser, to 0.1 cycle; ~1.7 ns analog rise/fall skew at J6).
- **Read window:** target launches bit k on rising-edge store R_k. A load issued at R_k + d reads
  bit k iff `A <= d <= P + A - 1` (P = period in cycles, assuming uniform period; in general
  `A <= d < (R_{k+1} - R_k) + A`). d < A reads bit k-1 ("behind"), d >= next + A reads k+1
  ("ahead"). Boundaries sharp, deterministic, no mixtures seen in thousands of reads.
- **A = 7 against the U575 (CJ6B19D4E7) and the MSPM0L1306 (CJ3E7A91C2)**, at P = 8..30
  Internal self-loop 3 (store to B[SWDIO], load sees it 3 cycles later)
  + 4 external (buffers, cable, target). Calibrate at connect anyway (sweep d on DPIDR reads,
  A = smallest passing d); A is in core cycles: recompute if the core clock changes.
- **The L1306's "fails above 10 MHz" was the probe's sampling.** With model placement it reads
  and writes clean at P = 4 (37.5 MHz) and every P up to 40.
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

- 2026-10-16 evening: `CJ3E7A91C2` runs `5f3db32+codegen+lab` (build-genlab/), MSPM0L1306
  (erased, SWD stays up, DPIDR 0x6BA02477) with the analyser at the target: D0 NRST, D1 SWDIO,
  D2 SWCLK, D3 probe J8 pin 4 (command marker); 100 MS/s only with two channels. Results there:
  a lab sweep (DPIDR blocks + CTRL/STAT writes checked on 18 bits) clean at every P 3..40,
  50, 75, 150, 1500; probe-rs SRAM write+readback clean 0.1..50 MHz; probe-rs 64 KB read
  1407 KiB/s at 50 MHz, 1312 at 37.5, 629 at 10 (default build: fails above 10, 577 at 10).
- Probe `CJ6B19D4E7` runs `0f301e9+lab` (not on the bench now). Flash: `uv run tools/flash.py --cycle --serial
  CJ6B19D4E7 --elf <elf>`. Target NUCLEO-U575ZI-Q on J6 (SWD), DPIDR 0x0BE12477. Analyser
  Logic 8 (no settable threshold): D0 SWDIO, D1 SWCLK at J6; 100 MS/s; cannot resolve 1-2 cycle
  pulses.
- A failed write leaves WDATAERR sticky: write ABORT 0x1E before trusting later acks. AP access
  needs CTRL/STAT = 0x5000_0000 (debug power-up) first. A second RDBUFF read returns 0 on this DP.
- Lab blobs: stay inside the template's stack scratch ([sp, #0..28]); a store above it corrupts
  the saved registers (that hung the probe once; lab builds now reset on HardFault, but an
  infinite loop with interrupts masked still needs a replug).

## Open questions for the generator

- A calibration at connect (A sweep) vs a fixed A with margin: answered, measured at connect
  (src/swd_sweep.c). Margin at P=4 is still only 1
  on one side.
- Reads above 37.5 MHz: done in a lab prototype with a timer-made clock (below); not in the
  generator. Without a peripheral: not in software. A read clock is 2 edge stores + 1 load + 1 use, and the
  core pairs nothing but a 16-bit NOP or b<cc>: the corpus timed `ldr sp/adds`, `adds/ldr sp`,
  `str sp/adds`, `adds/adds` pairs at 1 cycle each. Duty cycle
  does not change the count. Only a peripheral clocking/shifting the wire would (SWDIO P0_2 is
  FC3 MISO; P0_0's functions are in the LPC55S6x datasheet).
- Overhead left at the top: P4 read 26 cycles a transfer over the floor (release 2, decision ~3,
  settle ~4, gap ~14: unmask, 8-instruction parity check, store, loop). The read parity fold
  could move into the settle wait.
- Default on since 2026-10-20: equiv and gate pass, and the L1306 passes with
  its AP answering WAIT.
- Timed TAP model for JTAG/two-pin (TDO window after the falling edge) not built.
