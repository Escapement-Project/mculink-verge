# Generating the JTAG and two-pin (OScan1) paths at run time

Plan, written 2026-10-17 after two-pin first worked on hardware. Read CODEGEN.md
first: the SWD generator is the template, and its timing model, scheduler, register
conventions and SRAMX placement carry over unchanged. This file holds only what differs for
JTAG, and the wire facts the two-pin generator has to respect.

## Decisions (2026-10-17)

1. **The activation lives in the host**, in probe-rs next to `icepick.rs`: standard commands plus
   vendor `0x83 0x02`, in one or two `DAP_ExecuteCommands` packets. The firmware frames.
2. **The generator covers OScan1 and four-wire JTAG in the same step**, one scheduler with two
   clock emitters. It also retires the four-wire overshoot (44% at 50 MHz, DESIGN.md) and the
   clang layout failure.
3. **ARM's C paths stay as the fallback**, as `arm_SWD_Transfer` is for SWD.
4. **OScan1 first.** OScan2 and OScan5 come later, built from a capture of TI's XDS110 (which
   defaults to OScan2 on this part; see "What TI's own configurations say"). OScan5 still needs
   a reference for how it leaves Shift.
5. **Detach powers the JTAG domain down** (see "Detach").

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
- **Never enter Test-Logic-Reset during a session.** Any entry, even three frames from
  Run-Test/Idle, drops OScan1 (reads then show 0x81010101, the signature of an adapter in JScan),
  whatever ICEPick SYSCTRL KeepPoweredinTLR says. The generator's tails must reach Run-Test/Idle
  without passing through Test-Logic-Reset, and the host must not ask for it (probe-rs line
  resets, chain scans and `tap_reset` all do). The next attach recovers (see "Attach from any
  state"). The earlier note that only a pin reset recovers once KeepPoweredinTLR is set was
  wrong: TI's sequence with its 200 us wait recovers it 3/3.
- **Chain after the ICEPick switches the Cortex-M TAP in**: `DAP_JTAG_Configure` two devices,
  IR lengths (4, 6), the DAP is index 0 (nearest TDO). The switched-in TAP answers only after
  >= 4 Run-Test/Idle clocks.
- **Power (TRM 6.4 ICEMelter, measured).** The JTAG power domain holds the ICEPick and the cJTAG
  adapter. POR and pin reset leave it off; ICEMelter powers it up after 8 rising and 8 falling
  TCK edges (7 never does, 8 always does, with or without a reset among them), and TI requires
  200 us before further commands (40 us measured enough here; 10-20 us is flaky). A fresh
  domain has the adapter in two-pin JScan (TMSC is TMS on every clock) and the TAP reset. A pin
  reset therefore gives the same debug-side state as a power cycle. ICEMelter restarts its count
  if the 3rd to 8th rising edges span more than 4 ms (TRM 6.4), so the wake clocks belong in one
  command. A domain left half-woken (edges counted, commands lost) needs more than 8: 8 wake
  clocks attach 0/3 from it, 20 attach 3/3.
- **Power-down (measured).** Test-Logic-Reset with KeepPoweredinTLR clear powers the domain off,
  but only if TCKC stops there. Three frames from Run-Test/Idle left it off; eight frames (the
  last five reach the adapter as JScan clocks) woke it straight back; with the bit set it stayed
  on (2/2, 3/3, 2/2; the test sends TI's commands with no wake clocks and no wait, which fails
  0/3 after a pin reset). AON_PMCTL JTAGCFG.JTAG_PD_FORCE_ON (0x40090034 bit 8, reset value 1,
  forces the domain on) reads 0 here (the register reads 0x3), so the debug subsystem decides;
  TI's SDK clears the bit only before shutdown. TRM 6.6: powering the domain off with
  Test-Logic-Reset before a system reset is what clears halt-in-boot.

## The activation (TI's XDS110 sequence; `dap.py cjtag --ti`)

Source: a TI engineer's post of the XDS110 firmware routine, TI E2E thread 766675 ("CC1352P: How
to enable cJTAG OSCAN1 mode on CC13xx and CC26xx device").
`WriteTmsPattern(p, n)` is n TMS bits of p, least significant first.
Verified on the CC1312R1 from a pin reset: IDCODE, then ICEPick routing and CPUID, 3/3.

1. **Reset the adapter and power up the JTAG domain**: a reset escape (10 TMSC edges with TCKC
   high), >= 20 TMS-high clocks, one idle clock, then wait 200 us. TI's routine has an 8-clock
   Test-Logic-Reset and one idle clock here and omits the wait; without the wait the commands are
   lost. The escape and the extra clocks are for a target a previous session left behind (below);
   the J-Link sends the escape and 200 clocks.
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

**Attach from any state (measured 2026-10-17).** With step 1 as above, the activation plus
ICEPick routing read CPUID 3/3 from each starting state, with 20 and with 200 wake clocks
(36/36): fresh (pin reset), a live OScan1 session with KeepPoweredinTLR set or clear, after a
Test-Logic-Reset with it set or clear, and a half-woken domain. Without the escape, TI's
sequence fails 0/3 from a live OScan1 session, whose adapter reads its JScan clocks as OScan1
frames. So probe-rs can attach without nRESET.

## Detach

To leave the part as a pin reset would (JTAG domain off, its ~400 uA gone, halt-in-boot cleared;
TRM 6.4, 6.6): clear KeepPoweredinTLR, or never set it in two-pin mode; walk Run-Test/Idle to
Test-Logic-Reset with exactly three framed TMS-high clocks; send no TCKC edge after that. probe-rs's
`icepick.rs` sets KeepPoweredinTLR, so a four-wire session that ends in Test-Logic-Reset leaves the
domain on and halt-in-boot armed (inferred from the TRM, not measured on four-wire).

## What TI's own configurations say (CCS 21.0 and 12.8, read 2026-10-17)

- `ccs_base/common/targetdb/devices/cc1312r1f3.xml` makes two-pin cJTAG this part's default on
  every TI probe, in a faster format than ours: XDS110 OScan2 (its connection file offers only
  OScan1 and OScan2), XDS100v3 OScan5 with its converter at 10 MHz, XDS200 OScan5. We and the
  J-Link use OScan1.
- TRM Table 6-3: OScan1 sends nTDI, TMS, TDO in every state; OScan2 sends only TMS outside
  Shift and nTDI, TMS, TDO in Shift; OScan5 sends nTDI, TMS, TDO outside Shift and nTDI, TDO in
  Shift. Nothing here says how OScan5 leaves Shift without a TMS packet, and we have no IEEE
  1149.7. For a DPACC/APACC (4 non-shift and 36 shift clocks) OScan2 sends 7% fewer packets
  than OScan1, OScan5 30% fewer.
- The XDS110 activates in its firmware, with one `CJTAG_CONNECT`
  (command 0x2b, as in OpenOCD's xds110.c) carrying the DOT7.* settings. A capture of it is the
  only TI reference for OScan2 within reach, and needs the LaunchPad's XDS110 jumpered back to the
  target.
- `cc26xx_icepick.gel` writes `IP_CC26xx_WAKEUPMCU = 1` on connect: the WUC TAP's
  MCU_VD_FORCE_ACTIVE (TRM Table 6-23), which powers the MCU domain under a sleeping
  application. The same TAP holds TMS_PAD_CFG (`IP_CC26xx_TMSSTRENGTH`: TMSC drive strength and
  slew), chip erase and MCU and system reset requests.
- OpenOCD's `ti-cjtag.cfg` is the switch to four-pin (STC2), not two-pin.

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
6. Host: probe-rs drives the activation and frames through the probe (outside this repo),
   attaches from any state as in "Attach from any state", and detaches as in "Detach".
   Working 2026-10-17 as `--protocol cjtag` (probe-rs branch wip-cmsisdap-cjtag, commits
   23717ccd0, 96dfbd7da, 973b3e9e3): CPUID and
   4 KiB ROM reads, reset, re-attach after a killed session, alternating with four-wire. Reads
   are clean up to 12.5 MHz requested and fail from 15 MHz; at 25 MHz CTRL/STAT reads come
   back with bits missing. probe-rs reaches the DP through framed `DAP_JTAG_Sequence` captures,
   not `DAP_Transfer`, so the generated paths need to cover that command too.
7. Later: capture TI's XDS110 on this part, then OScan2, then OScan5 if the capture or a spec
   shows how it leaves Shift.

## Tools

- `dap.py --serial CJ3E7A91C2 cjtag --ti --ti-from 1 --ti-prefix h8,l1,p200 --nreset-first
  --reset-clocks 0 --mem --sweep 1,5,10,25` runs TI's sequence with the 200 us power-up wait,
  routes the ICEPick, reads CPUID and sweeps block reads. `--ti-prefix` tokens: `eN` escape, `hN`
  / `lN` TMS-high / low clocks, `pN` an in-packet pause of N us, `wN` a DAP_Delay of its own.
  `--ti-from 2` starts at the zero-bit scans (skips TI's reset and BYPASS; IR keeps IDCODE).
- Older paths, kept for bisecting: `--synth` (the J-Link's pass decoded, with its tail as
  `--pre-tms`/`--pre-tdi`), `--replay CSV --window a:b`, `--pad`.
