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

## The two-pin generator as built (2026-10-17, commits 171c92a..d84161e)

- `src/gen.{c,h}`: the schedule and emitter, shared with swd_gen.c (SWD output byte-identical over
  the host sweep). SRAMX: SWD 0x0400_0000 (8 KB, its largest output is ~6 KB), frames 0x0400_2000
  (8 KB), lab upper 16 KB.
- `src/jtag_gen.c`: one function `fn(tdi, count, tms) -> tdo`, 1..32 OScan1 frames with TMS held,
  a loop of one frame. Packet = CLR fall, data `strb` to B[TMSC], SET rise (no store moves both
  pins). Release (TXEN CLR, DIRCLR) right after the fall that starts the TDO packet, take-back
  (DIRSET, TXEN SET) right after the next fall, which starts the loop body. Sample at
  fall + latency + EARLY_MARGIN (1), latency default 8 (vendor 0x8A sets it; JTAG_CODEGEN, variant
  `+jtaggen`, capability bit 5). Below P 9 the whole call runs masked; from 9 `msr primask` brackets
  each fall-to-release. Frame cycles: P3 20, P4 22, P6 25, P10 32, flat 3P from P12. At fast P the
  frame's own work (next nTDI, fold-in, subs) fills fall+3..+7, so the sample cannot sit earlier
  than fall+8 whatever the latency knob says; latency 5..7 all place it at +8.
- `src/jtag_gen_dp.c`: ARM's Sequence, IR, Transfer, ReadIDCode, WriteAbort as runs of frames, frame
  for frame; TDI where ARM sets none is the last frame's (`tdi_level`). Block via jtag_block.h.
  `cjtag_dp.c` routes to it while `jtag_gen_ready()`, else to ARM's copies; so does dap_dispatch's
  block route.
- Tools: cycles.py `RecordingTap`, `TimedCjtagTarget` (latency 8, hold 7, release by +3; flags
  escapes, contention, Test-Logic-Reset). The order-only model latches the frame's TDO at the TMS
  packet's rise (a real target holds it through the TDO packet; ARM's traces unchanged).
  `tools/cjtagcheck.py` (every P 3..40, 75, 150, 1500; in the gate for generated images).
  `equiv.py --wire cjtag --command sequence|transfer` (two-TAP chains, both ends).
- Model check: ARM's C paths on the timed target show margin 0 at 10/12.5 MHz and -3 from 15 MHz,
  the bench's probe-rs boundary exactly.
- Verified: gate (gcc, clang); equiv 720 sequence + 1080 transfer + 128 block cases, both
  compilers; cjtagcheck 41/41; gen_host 1008 two-pin configurations, four builds agree.
- Bench (CC1312R1, image 8007864-dirty+codegen+jtaggen+lab): probe-rs `--protocol cjtag` 4 KiB ROM
  correct at 1, 4, 10, 12.5, 15, 18.75, 25, 30, 37.5, 50 MHz (the previous image fails from 15);
  64 KiB flash 21/21 runs; 64-word writes to AUX RAM read back at 1..50 MHz; reset 3/3; detach
  still powers the domain down. dap.py 64-word ROM blocks 2000/2000 at 50/37.5/25/13.6 MHz.
- Throughput, dap.py 64-word blocks with USB, same session: previous image 172 KiB/s at 12.5,
  ~230 at 15..30, broken from 37.5; generated 223 at 12.5, 244..278 at 15..30, 290 at 37.5/50.
  Through probe-rs a 64 KiB read takes ~2.8 s at every rate from 10 MHz: host round trips per
  JTAG scan, not the wire (probe-rs would have to batch JTAG scans).
- Where the time goes (emulator, 50 MHz block read): ~1230 cycles a word, ~820 of them frames;
  the rest is C between the ~6 generated calls a transfer makes. Generated whole transfers would
  take the wire rate up ~45%.

## Both wires as straight-line blocks (2026-10-17, commits 435b79e..ec73cdc)

- `src/jtag_gen.c` now emits 32 identical blocks, one JTAG clock each, and enters at block
  32 - count (`mla` + padded `bx`); only shift immediates differ between blocks, so no clock pays
  for a loop. Only the wire in use is generated (`jtag_gen_ready(wire)`), into the same 8 KB.
  Largest output 5.6 KB.
- Two-pin block: the frame as before, sample in the block's last cycle. Frames: P3 18, P4 20,
  P5 22, P6 23, flat 3P from P12.
- Four-wire block: MPIN store (TCK low + TDI; GPIO MASK = ~(TCK|TDI) set in the prologue, the
  caller's mask restored at the end), SET rise, sample, fold-in, next TDI: five instructions, so a
  clock is max(P, 5) cycles (30 MHz top). TMS is a byte store once per call, while TCK is still
  high, as ARM sets it. Sample window [fall + 8 + 1, next fall + 6 - 1]: from P 10 inside its own
  clock and unmasked, below that after the next fall, masked. The layout tries every sample
  position, then longer clocks, and keeps the first that fits.
- C side: on four wires ARM's TDI pin is the state (`PIN_TDI_IN`, `set_tdi` drives it where ARM's
  PIN_TDI_OUT does). Vendor 0x8A takes `[two-pin latency, four-wire latency]`.
- Bug found and fixed on the way: the first block a call runs folds in a "previous" sample that
  never existed; R_SAMPLE still held the block size from the entry calculation and spilled into
  kept bits. It is zeroed before the `bx`.
- Emulator: unicorn 2.1.4 runs an IT block wrongly when a read hook fires on a load inside it, and
  the next branch's target skips its first instruction (a clang build's `blx` into the generated
  code lost its `push`). Loads are now counted from the code hook by their base register's bus;
  the timed sweeps are byte-identical either way on gcc. It was also the clang layout failure (a clang image's
  four-wire DAP_Transfer failing under the engine on one layout): its repro fails before the fix
  and passes after. The order-only
  four-wire model latches TDO at the fall; B-register reads return the pin level;
  `TimedJtagTarget` (latency 8, hold 6) serves generated images in the gate.
- Verified: gate (gcc, clang, both wires timed at all 41 periods); equiv four-wire 720 sequence,
  1080 transfer, 188 block cases, two-pin 720/1080/128, gcc and clang; gen_host 2016 JTAG
  configurations x 4 builds.
- Bench (ec73cdc+codegen+jtaggen+lab): probe-rs ROM reads correct at 1..50 MHz on both wires (with
  the probe-rs fix below); clang image too. dap.py 64-word blocks, USB included: four-wire 69 / 222
  / 303 / 351 / 381 / 390 / 389 KiB/s at 1 / 5 / 10 / 15 / 25 / 37.5 / 50 MHz; two-pin 25 / 102 /
  170 / 216 / 246 / 264 / 280.
- Cost now (emulator, 50 MHz block read): four-wire ~810 cycles a word, 205 of them clocks; two-pin
  ~1730, 740 of them frames. The rest is the C between ~6 generated calls a transfer (90..145
  cycles each). Next: generate whole transfers, as swd_gen.c does.

## Whole transfers (2026-10-17, commits 55f36f4..4305128)

- `emit_xfer` generates one read and one write function per configuration:
  `fn(request, words, count, tdi) -> ack | remaining << 32`. Each runs `count` transfers back to
  back as ARM's JTAG_Transfer clocks them, with the chain position (`before`/`after`) and idle
  cycles baked in, so `jtag_gen_ready` regenerates on DAP_JTAG_Configure and
  DAP_TransferConfigure too. Four-wire decides on the acknowledge before the next fall; two-pin
  before the next frame's first packet (a failing write's Exit1 TDI differs from the first data
  bit). The failure path is sequential code: Exit1, Update, Run-Test/Idle, TDI high, idle, return.
  C (`transfer_words` in `jtag_gen_dp.c`) retries WAIT per word as ARM's block loop does.
  `block_whole` is DAP_TransferBlock: posted read, n-1 reads, RDBUFF; or writes then RDBUFF.
- SWD and JTAG now share the lower 16 KB of SRAMX (`gen_owner`; each regenerates when it finds
  the other's code). Schedules keep their events in storage each generator provides (transfers:
  1280).
- Unrolled first. Where that does not fit (two-pin from P 12 at latency 8), the data bits use the
  same instructions every bit (reads `orr X_WORD, s, X_WORD, lsl #1` and an `rbit` after the last;
  writes `bic` from bit 0 and `lsr X_WORD, #1`), `find_loop` finds the run around data bit 16 that
  repeats every bit, cuts it where three cycles are free (subs, bne, the taken branch's bubble),
  and a filler after the loop stands for the untaken branch's missing cycle. The emulator's frames
  are exactly 3P through the loop at P 12 and 15. Vendor 0x8A's `ok` byte: bit 1 transfers, bit 2
  looped.
- gen_host: 5148 configurations, transfers in all 2268 JTAG ones since 2e2e08b. Before it, none
  for four-wire at latency 9 and up with a clock shorter than about latency + 6 (no sample slot);
  the transfers now lengthen the clock as the scan engine does (timed checks clean at latency 12,
  18, 24 from 5 to 50 MHz).
- Emulator: unicorn's IT-block fault fires on write hooks as well (clang `itt eq; ldreq; streq`,
  then the branch target lost its `uxtb`). Stores are now counted from the code hook like loads,
  and SRAMX code is checked against the bytes it was decoded from; nothing hooks memory.
  equiv.py sorts TMS and TDI changes between edges. QEMU harness: FPU on (gcc keeps a constant in
  d8 in `emit_xfer`).
- Verified: gate (both compilers); equiv four-wire 1080 transfer and 188 block cases, two-pin 128
  block cases, two-pin 1080 transfer cases (finished after the commit), gcc and clang.
- Bench (lab image, dap.py, 400 queued 127-word ROM reads, depth 3; every block checked against
  the 1 MHz read):

  | MHz | two-pin before | two-pin after | four-wire |
  | --- | --- | --- | --- |
  | 1 | 7410 ms | 6373 | 2136 |
  | 5 | 1753 | 1290 | 518 |
  | 10 | 1046 | 655 | 283 |
  | 15 | 522 | 522 | 207 |
  | 25 | 421 | 421 | 147 |
  | 50 | 342 | 342 | 131.6 (30 MHz top) |

  Before = transfers not generated at P >= 12 (scans). Best: four-wire 1508 KiB/s, two-pin 581.
- Per-word slope, bench against emulator: two-pin 10 MHz 12.34 / 12.31 us, 50 MHz 6.28 / 5.18;
  four-wire 10 MHz 5.16 / 4.10, 30 MHz 2.19 / 1.47. Commands cost ~30 us more than emulated (USB).
  The code lab times MPIN/SET/DIRSET store patterns and the 5-cycle clock with its TDO load
  exactly as the emulator does, so the excess is not the cost model.
- **The excess is WAIT.** The CC13xx's AP answers WAIT when the next Capture-DR comes too soon
  after the previous Update-DR. With wait_retry 0, a 127-word read stops at word 1 with WAIT at
  idle 0: four-wire at 5..30 MHz, two-pin at 15..50 MHz. Idle cycles for a clean block: four-wire
  2 at 5/10 MHz, 4 at 15, 6 at 30; two-pin 1 at 15/25, 2 at 50; none for two-pin at 10 and below.
  Neither a time nor a clock count alone fits. Four-wire 30 MHz by idle: 0 131.6, 1 134.6, 2
  137.7, 3 140.7, 4 144.0, 6 112.3, 8 117.8, 12 128.7 ms. probe-rs sends idle 0, wait_retry
  0xffff. With retry 1 every word completes: never more than one WAIT a word.
- Idle above 4 runs in `slow_idle`: 8 cycles a clock at P 5 (the loop is not folded into it).

## WAIT retried in the generated code: parked (branch `jtag-wait-retry`, 5185655)

- The failure path retries a WAIT itself: per-word budget from a fifth argument (the slot at
  [sp] holds the count it was taken for), DAP_TransferAbort checked, interrupts let in first,
  a write's word re-sent. DAP_Transfer passes 0 (ARM's loop retries). Gate, gen_host, equiv
  (blocks with a target that always WAITs, both wires, both compilers; four-wire transfers) pass.
- It is slower at idle 0, two-pin: 50 MHz 346.2 against 341.8 ms, 25 MHz 435 / 421, 15 MHz 549 /
  522. The retry from C made a new call for the rest of the block, and that call's time worked as
  idle for the next word, so only every other word waited. Retried in place, every word waits,
  and a WAIT's failed transfer (~9 clocks) costs more wire than the call did.
- What wins is not waiting: two-pin idle 2 is 297.6 / 371.7 / 463.0 ms at 50 / 25 / 15 MHz,
  11-13% under idle 0; four-wire idle 6 at 30 MHz is 15% under. Ways there:
  the probe adds Run-Test/Idle clocks after a WAIT (learned per configuration; DAP results
  unchanged, but clocks on the wire the host did not ask for, unlike ARM's); or the host sets
  idle cycles (probe-rs hardcodes 0).

## Through probe-rs (2026-10-17, c732a6a image)

- Correct on both wires at 1/5/10/15/25/50 MHz: 4 KiB and 64 KiB ROM reads hash the same at every
  clock.
- 64 KiB takes 6.43 / 3.47 / 3.35 s two-pin and 4.14 / 3.24 / 3.20 s four-wire at 1 / 10 / 50
  MHz (debug build; attach included), against ~45 ms in the probe at 30 MHz four-wire. probe-rs
  builds its ARM interface for CMSIS-DAP JTAG with `create_jtag`: its own JTAG-DP over
  DAP_JTAG_Sequence, a host round trip per scan, so DAP_Transfer and DAP_TransferBlock (and the
  generated transfers) are never used. Release build: 64 KiB in 1.11 s four-wire, 1.27 s
  two-pin at 50 MHz.

## probe-rs through DAP_Transfer (2026-10-17; probe-rs fa72f8fca..6444d1059, firmware dcd15c3)

- probe-rs change (committed locally on wip-cmsisdap-cjtag, not pushed): `JtagChainAccess::jtag_dp_transfers`
  (CMSIS-DAP returns itself); `ArmCommunicationInterface` sends DP/AP traffic through the SWD port
  engine whenever the probe offers it, so CMSIS-DAP JTAG uses `DAP_Transfer`/`DAP_TransferBlock`
  with the chain index. In JTAG mode the CMSIS-DAP batch runner sends ABORT as `DAP_WriteABORT`
  (a DP 0x0 write would reach DPACC), brings the TAP to Run-Test/Idle first and records it there
  after. Chain routing, ICEPick and sequences still go through probe-rs's scans. 479 probe-rs tests
  pass (two new), clippy clean. Changelog fragments added.
- End to end (release, attach included, `read b32 0x0`): 256 KiB four-wire 0.54 / 0.36 / 0.37 s
  at 10 / 25 / 50 MHz, two-pin 1.20 / 0.72 / 0.62 s; 64 KiB four-wire 0.14 s (was 1.11). Hashes
  equal on both wires at every clock. Flash + verify of 64 KiB at 0x20000: four-wire 0.66 s, two-pin
  0.76 s at 50 MHz.
- Idle cycles end to end (256 KiB read, 50 MHz): four-wire idle 6 0.34 against 0.36 s;
  two-pin idle 2 0.57 against 0.635 s. Taken in the firmware as learned pacing, below.

## Learned pacing (2026-10-17, 519898b)

- After each DAP_Transfer or DAP_TransferBlock that met a WAIT, the generated transfers get one
  more Run-Test/Idle clock after every transfer (`jtag_gen_paced`, up to 32), regenerated between
  commands; a change of clock, latency, chain position or host idle starts again from none.
  A target that never answers WAIT sees ARM's wire. Vendor 0x8B `mode` (0 off, 1 on, else query)
  answers `status, on, paced`; equiv.py turns it off.
- Bench, 400 queued 127-word reads, host idle 0, pacing off -> on (learned clocks): four-wire
  50 MHz 156.9 -> 128.4 ms (5), 25 MHz 156.8 -> 128.5 (5), 10 MHz 294.1 -> 252.0 (2); two-pin
  50 MHz 350.7 -> 301.5 (2), 25 MHz 430.3 -> 367.7 (1), 15 MHz 530.6 -> 470.5 (1), 10 MHz
  656.0 -> 657.8 (0: no WAITs there). Fixed idle 6 four-wire 50 MHz was 129.7: pacing finds it.
  Data checked against the 1 MHz read at every clock on both wires.
- probe-rs end to end, 256 KiB read at 50 MHz: four-wire 0.33 s, two-pin 0.56 s (0.37 and 0.62
  without pacing); 10 MHz 0.47 / 1.08 s. 64 KiB flash and verify 0.67 / 0.76 s.
- Emulator: against a target that always answers WAIT, one clock per command on both wires, and
  none after a clock change (a throwaway script, not kept).
- The in-code WAIT retry on branch jtag-wait-retry stays parked: with pacing there is next to
  nothing left for it to speed up.
- **Four-wire TDO is late after a system reset.** probe-rs's four-wire reset (AON_PMCTL SYSRESET)
  leaves the routed DAP sending TDO about 2 cycles later until the next pin reset: window from
  +10 to next fall + 8, against +8 to next fall + 7 after nRESET (`jtag4dap.py --dpacc
  [--no-reset]`, which routes the DAP and times its DPACC scan; jtag4lab.py
  measured the ICEPick's own TDO). The generated transfers read the acknowledge's last bit at
  latency + 1, so they failed there while ARM's C path and probe-rs's scans, which sample late,
  did not. Fixed in dcd15c3: four-wire latency default 10, and that bit sampled as late as the
  decision allows (at least latency + 4 on unmasked clocks). Cost: the four-wire clock floor is 6
  cycles (25 MHz), not 5.
- **probe-rs four-wire reset on the CC13xx never worked**, on any build: `Icepick::new` for four
  wires reset the TAPs but kept the chain state, which still held the DAP, so every ICEPick scan
  after the reset was 13 bits instead of 7. Fixed in icepick.rs by setting the ICEPick-only chain
  after the TAP reset, as the two-pin branch did. A 200 us power-up wait was tried and is not
  needed.

## The ICEPick join needs time, not a count (2026-10-17)

- probe-rs four-wire failed from 8 MHz on every image: the first DP access read all ones (the
  Cortex-M TAP not in the chain). Bisected by replaying probe-rs's command stream through dap.py
  with the clock lowered for a prefix: only the routing command matters (IR ROUTER, SD_TAP0 write,
  BYPASS, 10 Run-Test/Idle clocks, one DAP_JTAG_Sequence).
- The selected TAP joins only on Run-Test/Idle clocks that come about 2.5-3 us after the routing
  write. One contiguous run: 5 MHz 8 clocks pass; 10 MHz 16 fail, 20 pass; 30 MHz 20 fail. A wait
  without clocks after it does not help (the next command's DP access leaves Run-Test/Idle at
  once). Two-pin frames are slow enough that 10 clocks happened to cover it.
- Fix in probe-rs `icepick.rs`: `SD_TAP_JOIN_CLOCKS` 10 -> 256 (over 5 us up to 50 MHz).
  Uncommitted in probe-rs's tree.

## Measured again, 2026-10-19

- TDO windows on the dual-core image, IDCODE reads, 10 repeats: four-wire DAP [7, next fall + 6]
  after nRESET and [11, next fall + 8] after SYSRESET (10 marginal); ICEPick [6, next fall + 5]
  and [10, next fall + 7 or 8]; two-pin [8, at least next fall + 7] for both TAPs in both states.
  The figures below (8 and 10) came from DPACC reads of `0x00000002`, which read right early or
  late, and from the single-core image.
- A misread OK acknowledge is not harmless on JTAG. The probe leaves Shift-DR after 3 + 1 + index
  shifts, and the Update-DR makes an access spelled by the last captured data. At a stale latency
  (7 against 11) ROM block reads at 50 MHz answered mostly OK and left the AHB-AP's CSW at
  `0x01000040`. Hidden traffic has to be IDCODE scans, not DPACC reads.
- The probe now measures the latency at every `DAP_JTAG_Configure`; the defaults below stay the
  starting point.

## Four-wire on this part (measured 2026-10-17)

- After a pin reset the adapter is two-pin JScan: TDO is not driven. Four-wire needs probe-rs
  icepick.rs `ctag_to_jtag` first (BYPASS, zero-bit scans x2, 1-bit scan, STC2 = 2-bit scan,
  operand 9 = 9-bit scan, BYPASS, IDCODE).
- TDO window, sample offset from the falling-edge store that launches the bit (IDCODE reads, 5
  repeats, P = 6, 8, 10, 12, 16, 20, 30): good from +8 to next fall + 6, bad at +7 and at next
  fall + 7. One cycle narrower than the period; the extra cycle at the front is likely bit 0, whose
  output enable switches on at that fall.
- ARM's four-wire C path reads CPUID through probe-rs at 1..5 MHz and fails from 8 MHz, on the
  previous image as well as the generated one: not a regression. Cause: the ICEPick join (below the
  blocks section), a host matter.

## To measure before writing the generator (lab blobs, as for SWD)

1. Done: the TDO window (8 cycles to at least next fall + 7), minimum phases (1-cycle highs),
   TDO-packet stretching (works).
2. **Take-back timing**: when the target stops driving TMSC after the packet-3 fall. If it stops
   at the packet-3 rise, the take-back could move before the fall and shorten packet 1. Needs
   analog on D1 (contention shows as a mid-level), or a probe-side readback with the target's
   bit opposite the latch.
3. **The window on a second CC13xx/CC26xx die**, and calibration at connect (as SWD's A).
4. Done: **four-wire TDO window**, above.

## Steps

1. Lab: OScan1 window and four-wire window done; take-back timing bounded; second die open.
2. Emulator: done for both wires (`TimedCjtagTarget`, `TimedJtagTarget`).
3. Generator: both wires as straight-line blocks, and whole transfers, unrolled or looped (above).
   probe-rs reaches them through DAP_Transfer; the probe learns idle clocks from WAITs.
4. Gate: done for both wires.
5. Hardware: both wires correct at 1..50 MHz through probe-rs and dap.py.
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
