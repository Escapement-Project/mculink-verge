# Runtime code generation of the bit paths

A companion to TIMING.md, which has the rules in prose: read it first. Dense on purpose.
Everything here was measured on the bench 2026-10-16 unless marked otherwise. BENCH.md section
"The cost model now times the bus, and reproduces every SWD result so far" has the evidence.

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
- When: `swd_gen_ready()` compares (period, latency, turnaround, data_phase, idle, stall) with the
  last build; `dap_dispatch.c` captures DAP_SWJ_Clock's Hz (ARM keeps only a delay) and calls it
  after SWJ_Clock / SWD_Configure / TransferConfigure, so generation never lands inside a transfer.
  `energy_monitor.c` sets the stall when a capture is asked for or stopped, on CPU 1 between
  commands, so the next transfer regenerates before it runs.
  `swd_gen_builds` / `swd_gen_failures` count outcomes (read them in the emulator).
- Periods: requested P = ceil(CPU/Hz). Probe-driven clocks (packet, write data) at max(P, 3), low
  = 2 at P = 3 else P/2; target-driven, turnaround and idle clocks at max(P, 4), low = P/2. No
  clock is ever shorter than asked; stretches only lengthen. P <= 2^24 else ARM's path.
- Stall (`swd_gen_stall`, 2 while energy capture runs): DMA0's reads of ADC0 share AHB port 10
  with the secure GPIO and hold a sample load up to 2 cycles (BUSES.md). A load before the next
  rising edge holds that edge back with it; one after it reads the next bit if held past its
  window. So a sample after the next rising edge keeps `stall` cycles from its window's end, and
  the served latencies count it. The read period starts at stall + 3 and grows (up to 3 more,
  either split of an odd period) until the latency keeps a cycle inside what the build serves:
  A = 7 reads at P = 5 (30 MHz), 244 cycles a transfer at a 37.5 MHz request against 210 without.
  The latency measurement builds without a stall: it holds the conversions. On a board that can
  capture, a connection measures an unmeasured target at every masked period: a capture starting
  or stopping changes which latencies the build serves, and only a measured one is served by both.
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
- **A = 7 against the U575 and the MSPM0L1306**, at P = 8..30. Internal self-loop 3 (store to B[SWDIO], load sees it 3 cycles later)
  + 4 external (buffers, cable, target). Calibrate at connect anyway (sweep d on DPIDR reads,
  A = smallest passing d); A is in core cycles: recompute if the core clock changes.
- **The L1306's "fails above 10 MHz" was the probe's sampling.** With model placement it reads
  and writes clean at P = 4 (37.5 MHz) and every P up to 40.
- From SRAM (not where generated code goes) the model is approximate: fetch and data share S-AHB
  one transaction a cycle; misses are -1 at the straddling alignment after a CYCCNT read (tried
  and rejected: a separate SRAM buffer depth, PPB ordering applied to fetch). Irrelevant to the
  generator except for comparing against the old C paths.

Verified end to end: `tools/lab.py swd_read_pipelined` at P = 4..8 (37.5..18.75 MHz), all
non-colliding sample delays, 45/45 configurations predicted exactly (verdict and cycle count) on
the U575. Soak: 200 reads each at P=4 d=9 (margin 1),
P=4 d=7 (margin 0, window edge), P=5 d=9, P=8 d=10: all clean, 7,200 target bits each.

## Throughput limits implied

- Read clock needs: fall store, rise store, sample load, sample consume (store to buffer or
  accumulate) = 4 instructions -> P >= 4 (37.5 MHz) at 150 MHz.
- Write clock: fall store, data store (`strb` of a register holding 0/1 to B[SWDIO]), rise
  store = 3 if the data bit is already in a register -> P >= 3 (50 MHz). Extracting bits from a
  word costs an extra instruction per bit (`lsr`/`ubfx`) unless precomputed.
- **Write window (measured, `tools/lab.py swd_write_asm`):** AP TAR writes with the SWDIO store `lead` cycles before the rising
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
- Equivalence: `tools/equiv.py` drives two builds over 536 SWD block cases (every ack, turnaround, data
  phase, idle cycles) and requires identical wire events and replies. A generated path must
  pass it against ARM's build; it is how the current C paths were proven.

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

