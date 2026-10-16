# Generating the JTAG and two-pin (OScan1) paths at run time

Plan, written 2026-10-17 after two-pin first worked on hardware. Read CODEGEN.md
first: the SWD generator is the template, and its timing model, scheduler, register
conventions and SRAMX placement carry over unchanged. This file holds only what differs for
JTAG, and the wire facts the two-pin generator has to respect.

## Open decisions

1. **Where the activation lives.** It is now TI's documented sequence (below), fully understood,
   and needs raw clocks between frames, which standard commands can carry (`DAP_SWJ_Pins` in
   `DAP_ExecuteCommands`, then vendor `0x83 0x02` and `DAP_JTAG_Sequence`). The J-Link splits it
   the same way: its firmware frames, its host activates. The 2026-10-14 plan wanted it inside
   the probe so probe-rs sends one vendor command; the sequence is TI-specific (ICEMelter,
   controller selection), which still argues for the host, next to `icepick.rs`.
   Recommendation: host.
2. **Generate four-wire JTAG in the same step.** Same scheduler, same transfer logic, a different
   clock emitter. Recommendation: yes; it also retires the four-wire path's overshoot (44% at
   50 MHz, DESIGN.md) and the clang layout failure.
3. **Keep ARM's C paths as the fallback**, as `arm_SWD_Transfer` is for SWD. Recommendation: yes.

## The wire, as measured on a CC1312R1 (2026-10-16/17)

- **Frame**: three packets per JTAG clock, nTDI, TMS, TDO. A packet is: set TMSC with TCKC low,
  raise TCKC, lower TCKC. TMSC moves only while TCKC is low; a move with TCKC high is an escape
  edge (the gate's `moves_while_high` rule, and since 05f9d84 the activation's own rule).
- **TDO packet** (measured 2026-10-17): release TMSC (TXEN, then
  the pin) right after the falling edge that starts packet 3. **TDO is valid from 8 cycles
  (53 ns) after that fall**, the same at every period 10-30: a fixed latency,
  the two-pin counterpart of SWD's A = 7. It stays valid through the rise and at least 7 cycles
  past the next fall (driven or merely held; digital sampling cannot tell). Take the wire back
  (pin direction, then TXEN, two stores) after that next fall, then store packet 1's data.
- **Probe-driven packets**: a 1-cycle (6.7 ns) high phase is accepted. The low phase is set by
  what must happen in it: one data store for packet 2 (low >= 2), take-back plus data for
  packet 1 after a TDO packet (low >= 4).
- **Shortest frame measured**: packets 1-2 at 5 cycles (low 4, high 1), TDO packet 9 (sample at
  8): 19 cycles a JTAG clock, 10/10. Packet 2 at 3 cycles would make it 17. Uniform packets
  bottom out at P = 8 (24 cycles). So the generator should stretch only the TDO packet, as the
  SWD generator stretches target-driven clocks.
- **Why the fast path fails at 37.5 MHz requested**: its TDO sample comes a couple of cycles
  after the release, inside the 8-cycle latency; at 25 MHz the C between packets happened to
  delay it enough.
- **Sampling edge**: the activation sends STC1 SEDGE=1, so the target samples TMSC on the rising
  edge (TRM Table 6-4). The generator places data in the low phase and lets the rise latch it.
- **Rates seen through the firmware**: paced frames at 1 MHz clean; the unpaced fast path at
  "25 MHz" runs an irregular ~11 MHz (period 50-160 ns) and is clean, 20/20 64-word ROM blocks;
  at "37.5 MHz" every transfer fails (TDO sampled too early, below).
- **Throughput today**: ~230 KiB/s through `dap.py`, 64-word blocks, USB included, flat from
  15 MHz up because the fast path is unpaced C.
- **Never enter Test-Logic-Reset after activating.** Any entry, even three frames from
  Run-Test/Idle, drops OScan1 (reads then show 0x81010101, the signature of an adapter in JScan).
  With ICEPick SYSCTRL KeepPoweredinTLR clear (its reset value), the reset also powers the JTAG
  domain down, and a full activation recovers it. With it set, the adapter is left in a state
  that re-sending the activation does not fix; a pin reset does. probe-rs's `icepick.rs` sets
  that bit, so in two-pin mode it must not, or must never reset the TAP afterwards. The
  generator's tails must reach Run-Test/Idle without passing through Test-Logic-Reset, and the
  host must not ask for it (probe-rs line resets, chain scans and `tap_reset` all do).
- **Chain after the ICEPick switches the Cortex-M TAP in**: `DAP_JTAG_Configure` two devices,
  IR lengths (4, 6), the DAP is index 0 (nearest TDO). The switched-in TAP answers only after
  >= 4 Run-Test/Idle clocks.
- **Power (TRM 6.4 ICEMelter, measured).** The JTAG power domain holds the ICEPick and the cJTAG
  adapter. POR and pin reset leave it off; ICEMelter powers it up after 8 rising and 8 falling
  TCK edges (7 never does, 8 always does, with or without a reset among them), and TI requires
  200 us before further commands (40 us measured enough here; 10-20 us is flaky). A fresh
  domain has the adapter in two-pin JScan (TMSC is TMS on every clock) and the TAP reset. A pin
  reset therefore gives the same debug-side state as a power cycle.

## The activation (TI's XDS110 sequence; `dap.py cjtag --ti`)

Source: a TI engineer's post of the XDS110 firmware routine, TI E2E thread 766675 ("CC1352P: How
to enable cJTAG OSCAN1 mode on CC13xx and CC26xx device").
`WriteTmsPattern(p, n)` is n TMS bits of p, least significant first.
Verified on the CC1312R1 from a pin reset: IDCODE, then ICEPick routing and CPUID, 3/3.

1. **Power up the JTAG domain**: >= 8 TCK clocks (TI's 8-clock Test-Logic-Reset and one idle clock
   are 9), then wait 200 us. TI's routine omits the wait; without it the commands are lost.
2. **JScan, one TMS bit a clock, from Run-Test/Idle**: BYPASS IR scan (64 shift clocks; an inert
   opcode for the command window), two zero-bit scans and a 1-bit DR scan (control level 2: the
   command window opens and the device TAP is decoupled from the scan path), STC1 1 (SEDGE=1:
   TMSC sampled on the rising edge), STFMT 9 (OScan1). Every DR scan's Shift-DR clock count is
   the command, so one clock too few is a different command (STFMT short by one is STC2, and
   STC2 9 switches the part to four-pin). TI ends the STFMT operand scan in Select-DR.
3. **4 clocks "in the CSM"**: raw TCK clocks with TMS low that the adapter's control state machine
   consumes and the TAP controller never sees. OScan1 frames start after them.
4. **In OScan1 frames**: finish the scan to Pause-DR; STMC 4 (zero-bit scan, then a 4-clock scan:
   deselect all controllers), 7 CSM clocks; CCE 0 (a 7-clock scan, then a zero-bit scan: select
   controller 0), 7 CSM clocks; ECL (zero-bit scan, then a 1-clock scan: close the command window,
   which re-couples the device TAP), 7 CSM clocks. CSM clocks follow every command that selects,
   deselects or closes. Skipping deselect-and-select reads all ones (TAP not coupled).
5. IR now holds BYPASS from step 2; load IDCODE or route the ICEPick next.

The J-Link's activation is the same protocol: its two 4-clock bursts at a different clock rate
are CSM clocks, its 54-clock burst is CCE 0, and its final `110110` IR scan closes the window
(consumed by the adapter, so IR keeps IDCODE). Rebuilt that way it reads IDCODE with TDI held at
0 or 1, 6/6. The TDI sensitivity seen
when replaying its tail came from a 2-clock pad: the CSM swallowed two packets of our first
frame, the target's framing ran two packets off ours, and our TDI bits fell in its TMS slots.
Escapes are not needed from a fresh domain; a selection escape followed by fewer idle clocks
than the adapter wants swallows the next command scans.

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
- **Expected wire throughput**: at the measured 17-19-cycle frame, ~42 JTAG clocks a word cost
  ~750 cycles = 5 us, ~780 KiB/s wire time for OScan1 (vs ~230 KiB/s with USB today); four-wire
  42 clocks at P = 4 = 1.1 us a word. USB will bound both, as it does SWD (BENCH.md "The USB path
  at 50 MHz").

## To measure before writing the generator (lab blobs, as for SWD)

1. Done: the TDO window (8 cycles to at least next fall + 7), minimum phases (1-cycle highs),
   TDO-packet stretching (works).
2. **Take-back timing**: when the target stops driving TMSC after the packet-3 fall. If it stops
   at the packet-3 rise, the take-back could move before the fall and shorten packet 1. Needs
   analog on D1 (contention shows as a mid-level), or a probe-side readback with the target's
   bit opposite the latch.
3. **The window on a second CC13xx/CC26xx die**, and calibration at connect (as SWD's A).
4. **Four-wire TDO window and A**, on a four-wire target or on this part after `ctag_to_jtag`

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

- `dap.py --serial CJ3E7A91C2 cjtag --ti --ti-from 1 --ti-prefix h8,l1,p200 --nreset-first
  --reset-clocks 0 --mem --sweep 1,5,10,25` runs TI's sequence with the 200 us power-up wait,
  routes the ICEPick, reads CPUID and sweeps block reads. `--ti-prefix` tokens: `eN` escape, `hN`
  / `lN` TMS-high / low clocks, `pN` an in-packet pause of N us, `wN` a DAP_Delay of its own.
  `--ti-from 2` starts at the zero-bit scans (skips TI's reset and BYPASS; IR keeps IDCODE).
- Older paths, kept for bisecting: `--synth` (the J-Link's pass decoded, with its tail as
  `--pre-tms`/`--pre-tdi`), `--replay CSV --window a:b`, `--pad`.
