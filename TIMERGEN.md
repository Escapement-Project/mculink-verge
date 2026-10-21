# A generator whose SWD clock comes from CTimer 0

Read CODEGEN.md first (the GPIO generator as built, which this extends), then this. The first part
is the 2026-10-16 design; "Built, 2026-10-21" at the end is what was implemented, which differs,
and what the bench found. Numbers are from the MSPM0L1306 bench.

## Why

The GPIO generator makes every clock edge with a core store, so a read clock is four
instructions (two edges, a sample, its use) and reads stop at P = 4, 37.5 MHz. SWCLK's pin
(PIO0_0) is also CTIMER0_MAT0. With the timer making the edges, a read bit is a load and a use and
a write bit an extraction and a store: two instructions, so every phase fits P = 3 (50 MHz).
The lab prototype (`timerdma.py`) read DPIDR 2000/2000 with the target's 38 clocks at exactly
50.0 MHz on the analyser. Scope: P = 3 only (requests >= 50 MHz). P >= 4 keeps the GPIO generator,
which the timer does not beat there.

## Measured facts this design rests on

- CTimer 0 PWM: CTIMERCLKSEL0 = 0 (main clock = core clock), PR = 0, PWMC = 1 (MAT0), MR3 =
  P - 1 with MCR.MR3R, MR0 = low cycles (output low while TC < MR0, high from MR0 to MR3). Locked
  to the core: every period from 2 to 10 cycles exact, no drift. A read sees rising edge 0 at
  6 + low cycles after the start store (in the frame of the instructions after that store).
- Stores to CTimer or IOCON registers (APB) stall the core: +4 cycles for one, +10 for two in a
  row; a CTimer load costs +9 (`apbstall.py`, CYCCNT from SRAMX). The m33 model does not know
  this yet. GPIO and SRAM accesses after an APB store do not stall further.
- TCR = 0 from the core: the clock freezes at the level a read 9 cycles after the store would see;
  instructions after the store run 3 cycles late. At P = 3 that stall always swallows one
  sample window, so no core store may stop or reconfigure the timer inside a timed phase.
- DMA stop: CTimer 1 (AHBCLKCTRL1 bit 27, CTIMERCLKSEL1 = 0) match 0 -> DMA0 channel 0
  (INPUTMUX DMA0_ITRIG_INMUX0 = 6; INPUTMUX clock AHBCLKCTRL0 bit 11, DMA0 clock bit 20) copying
  a zero word from SRAM to CTIMER0 TCR. CFG0 = HWTRIGEN | TRIGPOL (rising edge), XFERCFG0 =
  CFGVALID | CLRTRIG | WIDTH 32, one transfer. Clear CTimer 1 IR after setting MR0: TC = MR0 = 0
  after reset raises the request and an edge trigger then never fires. Cycle-exact over repeats
  only while the core keeps off the SRAM bank the DMA reads (keep samples in registers).
- Frame of the prototype (CTimer 0 start at slot 0, CTimer 1 start at slot 1, then one
  instruction a slot): rising edge k reads at 4 + 3k (P = 3, low 1 or 2); a one-shot delay of
  92 or 93 (low 1) freezes the clock high right after edge 37 (read at 115). Delay mod 3 picks the
  frozen level.
- Read window at P = 3: bit k is sampled correctly at slot 8 + 3k and 9 + 3k (100%), 10 + 3k
  only 65%; take 8.5 + 3k as the centre (use 9 + 3k with a sample-before-edge bias, or recalibrate).
- A filler must not read a load's destination: `mov.w r12, r12` right after `ldrb r12` stalls a
  cycle, and with a free-running clock every stall drifts all later samples. Use `mov.w r5, r5`.
- Probe-driven bits on the timer clock are NOT measured yet (the prototype's packet was GPIO).
  GPIO writes landed at every SWDIO placement off the edge cycles at P = 3..10, so a B-register
  store between two timer edges should do; measure before relying on it.

## Architecture

The pin stays on FUNC 3 for the whole generated block call; frozen high is the idle state.
CTimer 1 runs continuously for the block with a period equal to one transfer in core cycles, and
its two matches stop CTimer 0 at the two points every transfer must pause; the DMA channels
that do it reload themselves, so the core never touches the stop machinery inside the loop.

Per transfer (all slots fixed, interrupts masked):

1. Clock frozen high, SWDIO driven high (start bit). Core: CTimer 0 restart store (TCR = 1).
   Its 4-cycle stall lands before the first rising edge (6 + low later), harmless.
2. Packet bits 1..7: per bit an extraction and a `strb` to B[0][2], placed between rising edges
   k-1 and k (two candidate slots at P = 3; centre them). Park is 1, stop 0.
3. Release (TXEN CLR, DIRCLR) between the park edge and the turnaround edge.
4. Turnaround edge(s); acknowledge samples into registers (`ldrb` + `orr`, as the GPIO
   generator), at the window centres.
5. Decision stop: CTimer 1 match 0 -> DMA ch 0 -> TCR = 0, freezing the clock high right after
   the first common clock after the acknowledge (the turnaround for writes, data bit 1's edge for
   reads). The DMA latency is fixed, so match 0's value is a calibrated constant.
6. While frozen: combine the acknowledge, `cmp`, `bne` fail (the fail paths and WAIT retries are
   as in the GPIO generator, with tails clocked by GPIO after switching the pin back to FUNC 0;
   the frozen clock is high, so the switch makes no edge).
7. OK: reads restart the clock (core TCR = 1 store; nothing is timed in its stall) and keep
   sampling d1..d31, parity; writes enable SWDIO, store d0, restart, and store d1..parity
   between edges.
8. End stop: CTimer 1 match 1 -> DMA ch 1 -> TCR = 0 after the last edge (the turnaround after
   parity for reads, the parity edge for writes). Then the probe takes the wire (reads), idle
   cycles if configured (restart and stop again, or GPIO), SWDIO high.
9. Gap: parity check, store, loop. CTimer 1 keeps counting, so the gap's length is part of its
   period and must be fixed: no data-dependent timing anywhere in the loop body.

Exits (fail, parity error, done): stop CTimer 1 and CTimer 0, abort both DMA channels (ABORT0),
pin back to FUNC 0 with the GPIO latch high, unmask.

Cost estimate per read transfer at P = 3: 46 clocks x 3 = 138, plus two restarts (store + 6 to
the first edge, partly overlapped) and the decision (~4), plus the gap (~14): about 165 cycles
against 203 for the GPIO generator at P = 3 (37.5 MHz reads) — about 20% more reads per second on
the wire. Writes gain less (they already run at P = 3). probe-rs is USB-bound at about half the
37.5 MHz wire rate today, so end to end this shows only once the USB side improves.

Rejected: timer only for each read's data phase, with a one-shot CTimer 1 and the DMA re-armed
per transfer. Every transfer then pays the pin switch twice, two starts, the CTimer 1 reset and
the DMA re-arm, all APB stores at ~5 cycles: the read gains ~5%. A free-running clock through the
acknowledge: the WAIT path must drive SWDIO before the second edge after the turnaround, and at
P = 3 the decision takes too long (two edges late), so a floating line gives the target a start
bit.

## To measure before writing the generator (lab blobs)

1. Restart from frozen: TCR = 1 after a DMA freeze mid-period; latency to the next rising edge
   and that it is the same at every frozen TC value the design produces.
2. Self-reloading DMA: a descriptor linked to itself (XFERCFG RELOAD, table entry and reload
   descriptor both pointing at it) stops CTimer 0 on every CTimer 1 match over a long run, at
   the same cycle every period; two channels (match 0 and match 1).
3. DMA register stores (AHB, 0x4008_2000): do they stall like APB? Needed for exit cleanup only.
4. Timer-clocked writes: CTRL/STAT writes with SWDIO stores at each candidate slot between edges,
   checked by read-back (as gensweep.py does), at P = 3 low 1 and low 2.
5. A whole timer-clocked DPIDR read and CTRL/STAT write with both stops (decision, end) in one
   blob, soaked, and on the analyser (edge count 46 per transfer, 20 ns spacing).
6. The U575 as a second target: window and latency at P = 3 (only the L1306 has run this).

## Generator and tool changes

- `src/swd_gen.c`: a timer-mode emitter beside the GPIO one, chosen when the write period is 3.
  Schedules are by edge: edge k at a known slot from the frame, data stores and samples placed
  relative to edges; no edge instructions. Fillers `mov.w r5, r5` only. The peripheral setup
  (CTimer 0/1, DMA descriptors and channel config, INPUTMUX, clocks) belongs to C at build time;
  the descriptor table needs 512-byte alignment in SRAM (not the bank samples would use, though
  samples stay in registers anyway).
- Calibration constants (rising-edge frame offset, decision and end match values, sample
  centre) are measured per build at first use or fixed from the lab and checked by
  `dap.py gen --calibrate`-style DPIDR sweeps.
- Emulator: `tools/m33.py` needs the APB stall (+4 store, +9 load) and `tools/cycles.py` a CTimer
  0/1 + DMA model that turns timer state into SWCLK edges at the measured latencies, feeding
  `TimedSwdTarget`, so equiv (`--ignore-samples`), the gate and `gencheck.py` cover timer mode.
  Without it the timer mode is verified on hardware only.

## Option, not planned: P = 2 (75 MHz)

Decided 2026-10-16: 50 MHz is the target; this is recorded, not scheduled.
- Instruction count allows it: a write bit is `and` (extract, alternating temps so it sits 3
  cycles ahead of its store) + store; a read bit is `ldrb` + `orr`, interleaved so each use is 2
  after its load. Both exactly 2 cycles, zero slack; one stall cycle moves data by half a bit.
- Write parity (5 `eor` folds) has no free slot in a P = 2 data phase: compute it in the frozen
  decision window or in C. Packet and acknowledge could stay at P = 3 and the timer be
  reprogrammed to P = 2 while frozen at the decision stop (no wire cost).
- CTimer 0 PWM at P = 2 low 1 is exact (pwmclk.py, core reading its own pin).
- Unknowns: both phases are 6.7 ns; LPC55 edges 2-4 ns at SLEW = 1 (data sheet table 27); SWCLK
  and SWDIO pass 74LVC1T45/2T45 translators; the store-to-pin vs match-to-pin offset is
  unmeasured. MSPM0 data sheets (L1306 included) rate fSWD at 10 MHz, already exceeded 5x; the
  U575 data sheet gives none.
- The Logic 8 at 100 MS/s already misses ~1/4 of pulses at P = 3; at P = 2 only target replies
  would verify it.
- Gain: ~46 bits a transfer, 138 -> 92 cycles before the fixed stop/restart cost; invisible to
  probe-rs while it is USB-bound.

## Built, 2026-10-21 (branch `swd-timer-reads`; `src/swd_timer.c`, the timer section of `src/swd_gen.c`)

Scope: the read function only, at period 3 (requests of 50 MHz and up), turnaround 1, idle cycles 0;
anything else keeps the GPIO generator. `swd_gen_timer` (on by default) switches it, and is part of
`swd_gen_config`, so a change regenerates.

Design as built (differs from the architecture above):
- **Two runs a read**, each started by the core and stopped by DMA1, re-anchored at every start,
  so nothing drifts across a block and the frozen windows may take any time: run 1 is the packet,
  the turnaround and the acknowledge plus the clock after it (13 edges; on OK that is data bit 0's
  clock, else the turnaround the GPIO path clocks before deciding); run 2 is the other 33 (data
  bits 2..31 launched, parity, its clock, the turnaround). 46 edges a read. Splitting at 12 edges
  instead read one transfer right and lost step on the next: one clock short.
- **Stop timers CTimer 2 (run 1) and CTimer 4 (run 2), DMA1 channels 0 and 1** (input mux 6 and 7).
  CTimer 1 and DMA0 are out: SWO resets DMA0 and owns its descriptor table, and CTimer 1 has no DMA1
  trigger. MCR = MR0R | MR0S: the match stops the timer with its count at the match, and a start
  from there counts one more cycle than a start from zero, so setup parks each at its match and
  every run starts the same way. Descriptors reload themselves (RELOAD, linked to self): the core
  never re-arms anything, and the match flag never needs clearing (`rearm.py`, variant "none":
  every round froze at the same cycle).
- **DMA1 writes TCR = 2 (reset and hold), not 0.** A stop with TCR = 0 left the count at 1 or 2
  depending on the configuration, and the next run's edges moved with it. Held at 0, every run
  starts identically: SWCLK falls at the stop and stays low between runs, so before the pin goes
  back to GPIO the code lowers SWCLK's GPIO output too (a rising edge there would be a clock).
  `stop` is a word in SRAM, not flash.
- **Clock shape low 1, high 2.** Low 2 / high 1 (the GPIO writes' shape) failed every other read
  with a timer-clocked packet (`pktsweep.py`, 10/20 at every store slot); low 1 read 20/20.
- **Frame:** with CTimer 0 started at cycle 0 and its stop at 1, rising edge k is a load-visible
  edge at 4 + 3k, i.e. RUN_EDGE(k) = 1 + 3k in store terms, the frame the target latency counts in.
  DMA1's write lands 26 cycles after the stop's match: RUN_STOP(n) = 3n - 23 puts it on the
  falling edge after the n-th rise (`diag.py --match`: one cycle earlier cuts the last high phase
  to a cycle, two later lets an edge through). The next start must wait past the stop.
- **Packet bits on the timer clock** (never measured before): a store at RUN_EDGE(k) - 2 or - 1
  (one or two cycles after the previous edge) reads 20/20; - 3 and - 4 fail. The start bit is
  stored before the run: a sequence may have left SWDIO low (the first transfer after a line
  reset got no acknowledge until it was).
- **Samples** at the middle of [launch + A, next launch + A - 1]; served latencies A - 1..A + 1.
  Run 2 places all loads first, then the uses: placing them greedily pushed loads onto window edges.
  Parity folds into R_ACK bit 0 in run 2's spare cycles (load, orr, eor fill every cycle).
  The connect-time measurement works unchanged on timer reads: it measures latency 7 on the L1306.
- Cost: 13 + 33 edges at 3 cycles, plus two windows; about 160-180 cycles a read against 203.

Bench, plain MCU-Link and the L1306 (lab images `ecb6d02-dirty+lab`), `soak.py`, 2000 blocks of
64 DPIDR reads (128,000 transfers):
- GPIO path at 37.5 and 30 MHz: 0 errors.
- Timer reads at 50 MHz: about 1 in 10,000 transfers fails (10 to 14 blocks stopped, mostly ack 7,
  a few parity, plus 2 to 8 wrong words), at any position in a block. Moving the stop a cycle
  either way does not change it; pinning latency 6 or 8 makes it much worse (only 7 works).
- **Cause: CPU 0's LED.** With CTimer 3's period interrupt off (MCR = 0x400 from CPU 1,
  `ledmcr.py`), 384,000 reads with no error. The LED's tick (488 Hz) writes CTimer 3, on the same
  APB bridge as CTimer 2 and 4; a collision delays a stop timer's start, so its stop lands a cycle
  or more late (`diag.py --match` showed one APB access before the start moving the run a cycle).
  The rate fits: about 1,500 APB writes a second against two starts a transfer.
- Not the APB clock's phase: an APB load and 0 to 5 fillers before the start changed nothing.
  The generated code reads CTimer 0 before each start anyway (drains the bridge; costs its stall).

Since (commits `0814414`, `fe33026`):
- **The emulator models the runs** (`tools/cycles.py`, `Machine._timer_write`): IOCON PIO0_0,
  CTimer 0, 2 and 4 are register pages; a start schedules edges at c + 1 + 3k, the stop's start
  cuts them at stop-start + MR0 + 22, and a cut high phase, a run nothing stopped or SWCLK rising
  at the pin handover is a gate failure. It reproduces the bench's match sweep (3n - 23 and 3n - 22
  clean, one either side faulty), and a stop moved to 3n - 21 fails the gate (break-tested). Timer
  edges tied with a store on the same cycle come after it, as ARM's trace orders a fused fall.
  Against ARM's path the 50 MHz traces differ only in sample positions, SWDIO taken back after
  the falling edge that ends a read rather than before, and SWCLK left low: no rising edge and no
  level at one changes. APB stalls are still not modelled (cycle counts are optimistic there).
- **The LED handshake**: `dap_shared.swd_timer_busy` (set by the generated read's prologue, cleared
  on every exit) and `led_writing` (set by CPU 0's LED tick around its CTimer 3 writes, cleared
  after a read-back so the writes have left the bridge), each set before reading the other's
  (Dekker, with a DMB each side). The tick stands down (disables its own interrupt) while a read
  runs, and `led_resume` in CPU 0's loop turns it back on. Not yet run on the plain board, which
  went offline before it was built.
- `tools/swd_latency_test.c` runs the connect-time measurement against timer reads too: right at
  every latency 2 to 40.
- Review (2026-10-21): no wrong bit, ack, count or word on any path (OK, WAIT, FAULT,
  protocol error, parity error, either data phase); register use, settle counts and the parity
  fold checked. Its gaps, closed in `a5a3413`: equiv now compares a timer read on rising edges
  (`--ignore-falls`): 360/360 DAP_Transfer and 536/536 block cases at 50 MHz against ARM's build;
  the gate measures the latency at 50 MHz, which times the timer samples against a target (a run
  shifted two cycles fails it: break-tested). Left as is: outside begin()/end() the model counts
  an instruction a cycle, so timer edges there are approximate (nothing in the gate runs one);
  DAP_SWJ_Pins reads SWCLK as 0 after a timer read.
- Regression check on the Pro (CC1312R1 on J7, image with both commits): four-wire and two-pin
  ROM blocks 400/400 at 1, 10, 25, 50 MHz, throughput equal to PRO.md step 7's.

Open:
1. **Soak the handshake on the plain board** (`soak.py`, 2000 x 64 DPIDR at 50 MHz, a few runs,
   with the LED breathing), then the wire on the analyser: SWDIO-timed periods at 50 MHz reads.
   A lab script, `validate.sh`, does all of it, with the A/B, in one run.
2. **The plain board was off USB from 04:18 to about 07:50** (replugged): after
   `dap.py reboot` it re-enumerated but answered no control request, and a USB port reset took
   it off the bus. Not known whether this work caused it; the same lab image had rebooted cleanly
   an hour before. Back and answering at 07:52, still on the lab image of `0814414` (no LED
   handshake).
3. **LEDs on the Pro** (PRO.md step 8) plan a fade on CTimer 1, which shares APB0 with CTimer 0's
   start store and DMA1's stop write: it will need the same handshake, or a timer on another bridge.
4. A/B of block throughput against the GPIO path at 50 MHz in one image: `dap.py gen --timer
   off|on` switches `swd_gen_timer` (vendor 0x89 modes 4 and 5; its reply ends with the setting
   and whether the current build uses it).
