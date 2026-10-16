# Generating the JTAG and two-pin (OScan1) paths at run time

Plan, written 2026-10-17 after two-pin first worked on hardware. Read CODEGEN.md
first: the SWD generator is the template, and its timing model, scheduler, register
conventions and SRAMX placement carry over unchanged. This file holds only what differs for
JTAG, and the wire facts the two-pin generator has to respect.

## Open decisions

1. **Where the activation lives.** Today the host drives it with standard commands (629
   `DAP_SWJ_Pins` writes in `DAP_ExecuteCommands` bundles, then vendor `0x83 0x02`, then 26
   frames through `DAP_JTAG_Sequence`). That is the J-Link's split: its firmware frames, its host
   activates. The 2026-10-14 plan wanted the activation inside the probe, so probe-rs sends one
   vendor command. The activation is TI-specific and partly unexplained (see "The activation"),
   which argues for the host, next to `icepick.rs`. Recommendation: host.
2. **Generate four-wire JTAG in the same step.** Same scheduler, same transfer logic, a different
   clock emitter. Recommendation: yes; it also retires the four-wire path's overshoot (44% at
   50 MHz, DESIGN.md) and the clang layout failure.
3. **Keep ARM's C paths as the fallback**, as `arm_SWD_Transfer` is for SWD. Recommendation: yes.

## The wire, as measured on a CC1312R1 (2026-10-16/17)

- **Frame**: three packets per JTAG clock, nTDI, TMS, TDO. A packet is: set TMSC with TCKC low,
  raise TCKC, lower TCKC. TMSC moves only while TCKC is low; a move with TCKC high is an escape
  edge (the gate's `moves_while_high` rule, and since 05f9d84 the activation's own rule).
- **TDO packet**: release TMSC (pin and `TMS_SWDIO_TXEN`) after the falling edge that ends
  packet 2, sample before the rising edge of packet 3, take the wire back after the falling edge
  that ends packet 3. Today's sample sits one delay after the release; the window's true edges
  are unmeasured (first measurement below).
- **Sampling edge**: the activation sends STC1 SEDGE=1, so the target samples TMSC on the rising
  edge (TRM Table 6-4). The generator places data in the low phase and lets the rise latch it.
- **Rates seen**: paced frames at 1 MHz (500 ns phases) clean; the unpaced fast path at "25 MHz"
  runs an irregular ~11 MHz (period 50-160 ns, high phases down to 20 ns, lows to 30 ns) and is
  clean, 20/20 64-word ROM blocks; at "37.5 MHz" highs reach ~10 ns and every transfer fails.
  So the target's limit is somewhere in 10-20 ns of high phase; a generated sweep will find it.
- **Throughput today**: ~230 KiB/s through `dap.py`, 64-word blocks, USB included, flat from
  15 MHz up because the fast path is unpaced C.
- **Never enter Test-Logic-Reset after activating.** Five TMS-high frames kill the link (reads
  then show 0x81010101), with or without ICEPick SYSCTRL KEEPPOWEREDINTLR, and nothing short of a
  full activation recovers it. The generator's tails must reach Run-Test/Idle without ever
  passing through it, and the host must not ask for it (probe-rs line resets, chain scans and
  `tap_reset` all do).
- **Chain after the ICEPick switches the Cortex-M TAP in**: `DAP_JTAG_Configure` two devices,
  IR lengths (4, 6), the DAP is index 0 (nearest TDO). The switched-in TAP answers only after
  >= 4 Run-Test/Idle clocks.
- **Pin reset does not reset the adapter** to its power-up state; it leaves the 0x81010101 state.
  A true power-up state needs a power cycle (not yet done).

## The activation (host side, `dap.py cjtag --synth`)

From the state after `DAP_Connect` (both pins high): 10-edge escape; 200 TMS-high clocks, 2 low;
7-edge escape; 36 low; then the command group as JScan TMS from Run-Test/Idle: ZBS, ZBS, DR1
(lock), DR1 DR1 (STC1 1: SEDGE=1), DR3 DR9 (STFMT 9: OScan1). OScan1 packet 1 is the clock right
after the operand's Update-DR, so two more raw TMSC-low packets complete frame 1. Then framing
on (`0x83 0x02`) and the J-Link's 26 tail frames, TMS `01110000001111111110110110`, TDI
`10111111110010100111000000`. Every clock count in the command group is a command: one short
turns STFMT into STC2, and STC2 9 switches the part to four-pin.

Unexplained, measured: the tail's TDI matters in frames 2-19 and 21, not in 20 or 22-27, and
every model tried (Test-Logic-Reset as JScan, three resume phases) contradicts the pad sweep.
The tail is used verbatim. Not needed from the post-pin-reset state: both escapes, 195 of the
200 reset clocks. The idle before the commands has a soft threshold near 9-12 clocks.

## Architecture

- **One JTAG transfer scheduler**, two clock emitters. A transfer is the TAP walk
  (Run-Test/Idle, Select-DR, Capture-DR, Shift-DR), `ir_before`/`ir_after`-style DR padding for
  the other TAPs (1 bypass bit for the ICEPick here), 3 ack + 32 data bits, Exit1, Update, idle.
  ~42 JTAG clocks a DPACC/APACC; the IR scan only when the cached IR changes (ARM keeps
  `DAP_Data.jtag_dev.ir`).
- **Four-wire emitter**: TCK fall/rise stores, TMS and TDI with the fall, TDO sampled in the
  window after the fall (window and A to measure, as SWD's).
- **OScan1 emitter**: each JTAG clock becomes three packets on TCKC/TMSC. Packets 1-2 are probe
  driven: an MPIN fall carrying TMSC (as SWD's probe-driven falls), a rise store. Packet 3: a CLR
  fall, then release (pin and TXEN), sample, rise, fall, take back. The release, the sample and
  the take-back are the only scheduling constraints SWD did not have.
- **CMSIS-DAP semantics to keep** (ARM's JTAG_DP.c, JTAG_Transfer): posted reads (the DR
  returns the previous read's data; RDBUFF ends a block), JTAG ack encoding (OK/FAULT 0b010,
  WAIT 0b001, mapped to DAP's), WAIT retry, match retry, `idle_cycles` in Run-Test/Idle,
  timestamps, `DAP_TransferAbort`. C keeps these, as for SWD; the generated function runs
  `count` transfers of one request and stops at the first non-OK ack.
- **SRAMX**: SWD has 0x0400_0000..0x0400_3FFF and uses part of it; the lab arena has the upper
  16 KB. Estimate ~1.6 KB per generated JTAG transfer variant (126 TCKC clocks, ~3 stores each),
  four variants (read/write x four-wire/OScan1). Budget it explicitly before step 3.
- **Expected wire throughput** at P = 4 (37.5 MHz TCKC): 126 TCKC a word = 3.4 us, ~1.1 MiB/s
  wire time for OScan1 (vs ~230 KiB/s with USB today); four-wire 42 clocks = 1.1 us a word. USB
  will bound both, as it does SWD (BENCH.md "The USB path at 50 MHz").

## To measure before writing the generator (lab blobs, as for SWD)

1. **The TDO window in packet 3**: sweep the sample cycle relative to the fall that ends packet 2,
   on IDCODE reads, at several P. Gives the earliest valid sample (target output delay, the
   OScan1 analogue of A) and whether the target still drives after the packet-3 rise.
2. **Take-back timing**: when the target stops driving after the packet-3 fall. Analog on D1
   (TMSC): a mid-level plateau at the take-back is contention. Today's take-back is one store
   after the fall.
3. **Minimum phases for packets 1-2**: sweep high and low separately with generated frames; the
   fast path says 20 ns highs pass and ~10 ns fail.
4. **Whether a TDO packet needs the same period as the others**, or can be stretched alone (the
   SWD generator stretches target-driven clocks to max(P, 4)).
5. **Four-wire TDO window and A**, on a four-wire target or on this part after `ctag_to_jtag`.

## Steps

1. Lab: the five measurements above.
2. Emulator: give the modelled 1149.7 target in `tools/cycles.py` the measured windows (a timed
   target, as `TimedSwdTarget`), SEDGE=1 sampling, and the Test-Logic-Reset kill.
3. Generator: `src/jtag_gen.c` on the SWD scheduler, OScan1 emitter first (the measured wire),
   four-wire second; behind a `JTAG_CODEGEN` option, off by default.
4. Gate: equivalence against ARM's paths with samples excluded, every P 3..40 on the timed
   target, zero moves while TCKC is high, baselines under `tools/baseline-codegen`.
5. Hardware: every P on the CC1312R1 (`dap.py cjtag --mem --sweep`), soak, then four-wire.
6. Host: probe-rs drives the activation and frames through the probe (outside this repo).

## Tools

- `dap.py cjtag --synth --pad 2 --pre-tms <tail TMS> --pre-tdi <tail TDI> --reset-clocks 0 --mem
  --sweep 1,5,10,25` activates, routes the ICEPick, reads CPUID and sweeps block reads.
- `--replay CSV --window a:b`, `--nreset-first`, `--pad` bisect a captured activation.
