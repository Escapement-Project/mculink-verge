# Generating the JTAG and two-pin paths at runtime

The two-pin (IEEE 1149.7) wire as a CC1312R1 needs it, and how the JTAG generator drives it. Read
CODEGEN.md first: the SWD generator is the template, and its timing model, scheduler, register
conventions and SRAMX placement carry over unchanged. This file holds what differs for JTAG, and
the wire facts the two-pin generator has to respect.

## The wire, as measured on a CC1312R1 (2026-10-16/17)

- **Frame**: three packets per JTAG clock, nTDI, TMS, TDO. A packet is: set TMSC with TCKC low,
  raise TCKC, lower TCKC. TMSC moves only while TCKC is low; a move with TCKC high is an escape
  edge (the gate's `moves_while_high` rule).
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
to enable cJTAG OSCAN1 mode on CC13xx and CC26xx device"). `WriteTmsPattern(p, n)` is n TMS bits of p, least significant first.
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
   deselect all controllers), 7 CSM clocks; MSC 0 (a 7-clock scan, then a zero-bit scan: select
   controller 0), 7 CSM clocks; ECL (zero-bit scan, then a 1-clock scan: close the command window,
   which re-couples the device TAP), 7 CSM clocks. CSM clocks follow every command that selects,
   deselects or closes. They are one SP that does not move the TAP, three packets in OScan1, then
   the 4-clock check packet (1149.7 Table 21-5); in OScan2 the SP is one packet, so 5. Skipping
   deselect-and-select reads all ones (TAP not coupled).
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
  Shift. How OScan5 leaves Shift is in IEEE 1149.7 (next section). For a DPACC/APACC (4
  non-shift and 36 shift clocks) OScan2 sends 7% fewer packets than OScan1, OScan5 30% fewer
  before its exit escape.
- The XDS110 activates in its own firmware: the host sends one `CJTAG_CONNECT` (command 0x2b in
  OpenOCD's xds110.c) carrying the DOT7.* settings. A capture of it is the
  only TI reference for OScan2 within reach, and needs the LaunchPad's XDS110 jumpered back to the
  target.
- `cc26xx_icepick.gel` writes `IP_CC26xx_WAKEUPMCU = 1` on connect: the WUC TAP's
  MCU_VD_FORCE_ACTIVE (TRM Table 6-23), which powers the MCU domain under a sleeping
  application. The same TAP holds TMS_PAD_CFG (`IP_CC26xx_TMSSTRENGTH`: TMSC drive strength and
  slew), chip erase and MCU and system reset requests.
- OpenOCD's `ti-cjtag.cfg` is the switch to four-pin (STC2), not two-pin.

The J-Link's activation is the same protocol: its two 4-clock bursts at a different clock rate
are CSM clocks, its 54-clock burst is MSC 0, and its final `110110` IR scan closes the window
(consumed by the adapter, so IR keeps IDCODE). Rebuilt that way it reads IDCODE with TDI held at
0 or 1, 6/6. The TDI sensitivity seen
when replaying its tail came from a 2-clock pad: the CSM swallowed two packets of our first
frame, the target's framing ran two packets off ours, and our TDI bits fell in its TMS slots.
Escapes are not needed from a fresh domain; a selection escape followed by fewer idle clocks
than the adapter wants swallows the next command scans.

## What IEEE 1149.7-2022 says, as far as this code uses it

Clause 25 covers the OScan formats, 10.4 the escapes, and 9.10 the configuration registers.

- **STFMT's operand is the format number**, with OScan formats from 8. Our STFMT 9 is OScan1, so
  OScan2 is STFMT 10 and OScan5 STFMT 13 in the same activation.
- **Which bits a format sends** (Table 25-1): OScan1 sends nTDI, TMS and TDO for every JTAG
  clock; OScan2 drops nTDI and TDO outside Shift; OScan5 drops TMS in Shift; OScan6 is OScan5
  with one PAD bit on entering Shift. A missing nTDI means TDI 1.
- **OScan5 and 6 leave Shift with an EOT escape on an nTDI bit** (Figure 25-11): the probe holds TCKC high
  through that bit and toggles TMSC twice in it, and the bit's TMS becomes 1.
- **Escapes** (10.4.1) are counted in TMSC edges while TCKC is high: 2-3 for EOT, 8 or more for a
  reset. Each edge comes at least one minimum TCKC period after the rise or the previous edge.
- **The TAP advances** (25.6) on the TDO bit's rising edge when the packet has one, and otherwise
  a period after the packet ends, so OScan2's packets outside Shift are pipelined.
- **CNFG0 says which optional formats a part has**: its OSCANS field flags the pairs OScan3:2,
  OScan5:4 and OScan7:6.
- **The CC1312R1 has every optional OScan format** (read 2026-10-21, 3 runs of 3): CNFG0 `0x03c00714`, class
  4 (T4), revision 1, OSCANS 111 (OScan2 to OScan7), no SScan, two-pin by default, no TRESET,
  FRESET or duty-cycle control. CNFG1 0, CNFG2 0x120 (user-defined).
- **How it was read**: TI's activation up to its ECL; MCM 0 (opcode 6, operand 0); SCNS 2
  (opcode 9, operand 2); a 32-bit DR scan from Run-Test/Idle, LSB first, is the CR scan; then ECL.
  Each command is two DR scans through Pause-DR, as TI writes them, and seven TMS-low raw clocks
  (the check packet) after it. **Select with MCM, not STMC 7**: STMC's CGM=1 counts as possibly
  many members, and with more than one every controller drives 1 through a register read, so it
  read 0xffffffff (13.9, 14.8). MCM 0 makes it the only member, and only then does it drive.
- **Reading IDCODE after the activation is not a wire check.** With TMS-high reset clocks the TAP
  passes Test-Logic-Reset, which on this part powers the JTAG domain down (ones); without them the
  IR still holds BYPASS (`0xfffffffe`). `cjtag --ti --reset-clocks 0 --mem --sysctrl 0x81000000`
  (route the ICEPick, read CPUID) is the check.

## OScan2 on ARM's path (2026-10-21)

- The probe frames OScan2 through a fourth copy of ARM's JTAG_DP.c (`cjtag2_*`,
  `src/cjtag_jtag_dp_tracked.c`, paced only, not flattened: +10 KB). `cJTAG_Frame2` follows the TAP
  from the TMS it sends: three packets in Shift-DR/IR, one TMS packet elsewhere, which returns
  TDO 1, as OScan1's joint drive gives outside Shift. The generated paths are OScan1 only for now
  (`jtag_gen_usable`), and the connect-time latency sweep is skipped in OScan2.
- Vendor `0x83 03 <SCNFMT> <TAP state>` turns it on; mode 2 stays OScan1. A mode, not a byte after
  mode 2: a vendor command has no length, so an optional byte would be read from the next queued
  command. `0x82` format 4 appends the u32 mask of formats framed.
- `dap.py cjtag --ti --format 10 --switch command|jscan`: STFMT 10 as an OScan1 command after MSC
  0, its 7-clock SP+CP, then mode 3 at Run-Test/Idle and ECL in OScan2 with a 5-clock SP+CP (1-bit
  SP, 1149.7 Table 21-5); or STFMT 10 in JScan in place of TI's
  STFMT 9, then mode 3 at Select-DR. Both read CPUID; 64-word ROM blocks match the 1 MHz read at
  1, 2, 5 and 10 MHz and fail from 15, ARM's OScan1 boundary too.
- Any raw count after ECL from 4 to 8 works: extra TMS-low SPs from Run-Test/Idle stay there. A
  first attempt framed ECL in OScan1 while the target was in OScan2 (the format step waited for the
  next framing entry) and still read CPUID at 4, 7 and 10 raw clocks: a wrong framing can pass, so
  check the decode, not only the result.

## OScan2 generated (2026-10-21/22)

- Wire `JTAG_GEN_OSCAN2` (2) beside four-wire and OScan1; `jtag_gen_wire()` (src/cjtag_dp.c) picks
  it from the framing, `JTAG_GEN_PINS(wire)` indexes the latency (both two-pin formats read the
  same TDO packet). `emit_tms` generates `jtag_gen_tms`, `count` one-packet frames with TMS held
  (fall, TMS store, rise; never masked, since nothing is released), after the scan function, for
  OScan2 only, so OScan1's SRAMX layout is unchanged. `frames_oscan2` (src/jtag_gen_dp.c) splits a
  held-TMS run where the tracked TAP enters or leaves Shift. Transfers lay Select-DR, Capture-DR,
  the Capture-to-Shift clock, Update-DR, Run-Test/Idle and the idle clocks as one-packet frames
  (`cx_tms_frame`, with the take-back after a released frame); their failure path and the idle
  loop too. The connect-time latency sweep and vendor 0x8A's measurement run in OScan2 again.
- Checks: gen_host 6282 configurations, 1134 of them OScan2, all with transfers, four builds
  agree; `jtagcheck.py --wire cjtag2` clean at all 41 clocks on `TimedCjtagTarget(oscan2=True)`;
  `equiv.py --wire cjtag2` against ARM's OScan2 copy; the gate runs `cjtag2` in both.
- Bench (CC1312R1, plain probe, 127-word ROM blocks, dap.py, 300 per clock after 30 to let
  pacing learn; two rounds agree within 1.5%):

  | MHz | OScan1 | OScan2 | OScan1 unpaced | OScan2 unpaced |
  | --- | --- | --- | --- | --- |
  | 12.5 | 330 KiB/s | 352 | 295 | 292 |
  | 25 | 464-468 | 481 | 420 | 408 |
  | 50 | 565-566 | 577-586 | 505-506 | 487-491 |

  Every block matched at 1 to 50 MHz in both formats. OScan2 is 2 to 7% faster with pacing, about
  what its packet count predicts. Pacing learns 5 idle clocks in OScan2 against 2 in OScan1: the
  CC1312's AP WAITs when a Capture-DR follows the last Update-DR too closely, and OScan2 sends
  those states in a third of the packets. Without pacing OScan2 is slower than OScan1, from the
  extra WAITs. Each learned clock regenerates the code, which costs a short run: 50 blocks right
  after a clock change put OScan2 at 323 KiB/s against OScan1's 423.
- Detach and attach in OScan2 (2026-10-22): after a session that walks Run-Test/Idle to
  Test-Logic-Reset with three framed TMS-high clocks and KeepPoweredinTLR clear, then mode 0 (its
  trailing frame completes the last advance, 1149.7 25.6.2), no-wake commands fail 3/3 and a wake
  without escape attaches 3/3, the same as after OScan1: the domain is off. With KeepPoweredinTLR
  set the no-wake commands attach (2/2), so the check can tell. A target left live in OScan2
  (no detach, either SYSCTRL) is attached by the escape-first sequence 12/12, in OScan1 or OScan2.
  probe-rs's OScan2 activation read 64 ROM words at 1, 5 and 10 MHz byte-identical to a four-wire
  read, CNFG0 logged each time.
- Attach cost (2026-10-22). probe-rs found an OScan2 attach 13-38 ms slower than
  OScan1 (115-122 against 83 ms at 12.5 MHz) while its 256 KiB reads ran 1.5-3.3% faster. Every
  framing switch regenerated scans and transfers: 16.2-17.5 ms at 12.5 MHz, 5.9-6.3 at 50. An
  OScan2 attach frames OScan1 first and switches, and its learned idle clocks restarted with every
  configuration change, so it relearned them each session, a regeneration a clock. Two changes:
  `jtag_gen_ready` builds the scans alone, and `jtag_gen_ready_transfers` the transfers when a
  transfer command needs them (a switch now costs 0.6-0.8 ms); learned clocks are kept per wire
  for the configuration they were learned for, and come back when the host does. A dap.py attach
  at 12.5 MHz now pays one generation in either format, at its first transfer (17.1 ms OScan2,
  18.5 ms OScan1), where it paid four in OScan2 and two in OScan1 before; the first OScan2 session
  after power-up still learns its clocks. Through probe-rs (its CLI, 256 KiB, 5 interleaved reps): attach OScan1/OScan2 78.2/73.7 ms at 12.5 MHz (was 79.9/117.8),
  80.2/81.8 at 25, 81.7/80.7 at 50; reads 774/737, 563/540, 473/444 ms, OScan2 +5.1, +4.3 and
  +6.7% (577 KiB/s at 50 MHz); all 30 reads matched four-wire. A session's first transfer still pays one
  generation, in both formats.
- Before generating OScan5/6: the clock sweep 2026-10-21 put OScan1 at 7.8 us a word at 50 MHz
  and 9.2 at 25 (127-word blocks), so about 1.3 us of a word at 50 MHz scales with the clock.

## OScan5 on ARM's path (2026-10-22)

- The TAP-tracked copy of ARM's JTAG_DP.c (`cjtagt_*`, `src/cjtag_jtag_dp_tracked.c`, was
  `cjtag2_*`) frames OScan5 too, through `cJTAG_FrameTap`: OScan1's three packets outside Shift,
  nTDI and TDO in Shift, and TMS 1 in Shift as an EOT escape on the nTDI packet (TCKC held high,
  TMSC to its inverse and back, each edge and the fall a host period after the last; Figure
  25-11). Every OScan5 frame carries TDO, so detach needs no trailing frame. Vendor 0x83 mode 3
  takes SCNFMT 13 and 0x82 offers it. `JTAG_GEN_OSCAN5` (3) exists so the generator refuses it and
  the routes fall back to ARM's copy; generating it is the next step.
- `dap.py cjtag --ti --format 13 --switch command`: STFMT 13 as an OScan1 command, mode 3 at
  Run-Test/Idle, ECL framed in OScan5 with 7 raw clocks after it (a non-Shift OScan5 SP is OScan1's
  three bits, then the 4-clock CP; Table C-17). With the robust attach (`--ti-from 1 --ti-prefix
  e10,h20,l1,p200 --reset-clocks 0 --mem --sysctrl 0x81000000`) it reads CPUID, and 64-word ROM
  blocks match the 1 MHz read 20/20 at 1, 2, 5, 10, 12.5, 15, 20 and 25 MHz (plain MCU-Link, CC1312R1).
- Decoded on the wire (escape edges counted inside each TCKC high phase): every rise of the capture accounted for, 30 EOT escapes for
  30 Shift exits, no TMSC edge with TCKC high anywhere else, the TAP back in Run-Test/Idle.

## OScan5 generated (2026-10-22)

- Wire `JTAG_GEN_OSCAN5` (3). Outside Shift a frame is OScan1's three packets, so the scan function
  is OScan1's. In Shift, `jtag_gen_shift` frames nTDI and TDO alone (TMS 0), and `jtag_gen_eot`, one
  block, the frame that leaves with an EOT escape: two stores of TMSC's mask to the GPIO NOT
  register with TCKC high, each a period after the rise or the last edge, the fall a period after
  that. No data-dependent register is needed for the escape. `layout_two_pin` and the transfers'
  `cx_shaped` take the frame's shape (three packets, Shift, EOT); `frames_oscan5`
  (src/jtag_gen_dp.c) splits scans by the tracked TAP as OScan2's does. Transfers walk from
  Run-Test/Idle: bypass, request and data bits in Shift as two-packet frames, the last with the
  escape; a failing acknowledge leaves by `slow_eot_frame`.
- Checks: gen_host 7416 configurations, 1134 OScan5, all with transfers, four builds agree;
  `jtagcheck.py --wire cjtag5` clean at all 41 clocks (escape spacing included; a second escape
  edge one cycle after the first fails it at every clock); equiv against ARM's OScan5 path: block
  128/128, transfer 1080/1080, sequence 840/840 (including a command part's one-bit Shift entered
  from Exit2-DR); the gate runs cjtag5 for both compilers.
- Bench (plain MCU-Link, CC1312R1, dap.py robust attach then STFMT 13): the capture decodes as
  intended (926 frames, every rise used, 30 escapes for 30 Shift exits, no stray edge), CPUID,
  64-word blocks 20/20 at 1 to 50 MHz. Rates, 127-word ROM blocks, 300 a clock after 300 to let
  pacing learn, two rounds within 1%:

  | MHz | OScan1 | OScan2 | OScan5 |
  | --- | --- | --- | --- |
  | 12.5 | 327-329 KiB/s | 349-350 | 415-419 |
  | 25 | 471 | 477-482 | 542-546 |
  | 50 | 551-556 | 567-569 | 588-592 |

  OScan5 is 27% faster than OScan1 at 12.5 MHz, where the packets set the rate (two a Shift clock
  against three), and 6% at 50 MHz, where the TDO latency sets most of a Shift frame (16 core cycles
  against OScan1's 18 at latency 8) and USB starts to bound the rest.

## OScan6 on ARM's path (2026-10-22)

- `cJTAG_FrameTap` frames OScan6: OScan2's single TMS packet outside Shift, OScan5's nTDI and TDO
  in it with the EOT escape for TMS 1, and in the first Shift frame after one outside it a PAD
  packet between nTDI and TDO (rule 25.4.2 g, Figure 25-12), driven at nTDI's level so that it
  moves nothing. The tracker says which frame that is (`cJTAG_TapEntered`). Its frames outside
  Shift carry no TDO, so detach sends OScan2's trailing frame. `JTAG_GEN_OSCAN6` (4) was refused by
  the generator at first; generated since (next section). dap.py `--format 14`: ECL then 5 raw clocks (a one-bit SP and the CP).
- Bench (plain MCU-Link, robust attach): CPUID, the 96-bit DR pattern, 64-word blocks 20/20 at 1 to 50
  MHz. Decoded: 927 frames using all 2125 rises,
  30 EOT escapes for 30 Shift exits, 30 PAD packets for 30 Shift entries, no stray edge.
- Emulator: both target models take `cjtag6`; the timed one passes ARM's path at 1 to 25 MHz and
  fails it when the probe frames OScan5 instead (no PAD), so the PAD is checked.

## OScan6 generated (2026-10-22)

- Wire `JTAG_GEN_OSCAN6` (4) takes OScan2's TMS packets (`jtag_gen_tms`) outside Shift and OScan5's
  `jtag_gen_shift` and `jtag_gen_eot` in it. The first Shift frame after one outside it has the PAD:
  `frames_oscan6` (src/jtag_gen_dp.c) frames it through the scan function, OScan1's three packets
  with the PAD in the TMS packet's place, or through `jtag_gen_eot_pad` (one block, the escape then
  the PAD) where it also leaves. The PAD goes out at the frame's nTDI level, as ARM's path sends it.
  Transfers send their first Shift frame the same way; `x->d1` became the last store that reads the
  nTDI register, so that the next frame's nTDI is not computed into it before the PAD is sent (the
  first equiv run caught that: the PAD carried the next bit's level). `frames()` and `hold()` are
  `COMMAND_PATH` now: each grew past gcc's inlining with another format's branch, and the image
  check caught the veneer into flash.
- Checks: gen_host 8550 configurations, 1134 OScan6, all with transfers, four builds agree;
  `jtagcheck.py --wire cjtag6` clean at all 41 clocks; equiv against ARM's path 128/128, 1080/1080,
  840/840; the gate runs cjtag6. Bench: decode clean (927 frames, every rise, 30 escapes, 30 PADs),
  blocks 20/20 at 1 to 50 MHz.
- Rates, all four formats in one run on one image (127-word blocks, 300 after 300; two rounds within
  1%):

  | MHz | OScan1 | OScan2 | OScan5 | OScan6 |
  | --- | --- | --- | --- | --- |
  | 12.5 | 306 KiB/s | 304-305 | 392-394 | 397-398 |
  | 25 | 454-456 | 427-433 | 510-514 | 438 |
  | 50 | 519-523 | 502-511 | 560 | 547-548 |

  OScan5 is the fastest on this target from 25 MHz up and level with OScan6 at 12.5. OScan6 and
  OScan2 send the clocks outside Shift as one packet, and the CC1312's AP answers WAIT when a
  Capture-DR comes too soon after an Update-DR, so pacing adds idle clocks there. Every figure here
  is 5-12% below the afternoon's (OScan5 table): the afternoon's image, measured again in
  this run, matched these within 2%, so the bench changed, not the firmware. OScan2 fell most, which
  points at the target answering WAIT more (not measured).

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
  four variants (read/write x four-wire/OScan1).
- **Expected wire throughput**: at the measured 17-19-cycle frame, ~42 JTAG clocks a word cost
  ~750 cycles = 5 us, ~780 KiB/s wire time for OScan1 (vs ~230 KiB/s with USB today); four-wire
  42 clocks at P = 4 = 1.1 us a word. USB will bound both, as it does SWD (BENCH.md "The USB path
  at 50 MHz").

## Learned pacing (2026-10-17)

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
- A WAIT retry inside the generated code was tried and set aside: with pacing there is next to
  nothing left for it to speed up.
- **Four-wire TDO is late after a system reset.** probe-rs's four-wire reset (AON_PMCTL SYSRESET)
  leaves the routed DAP sending TDO about 2 cycles later until the next pin reset: window from
  +10 to next fall + 8, against +8 to next fall + 7 after nRESET (measured by routing the DAP and timing its DPACC scan, and the ICEPick's own TDO). The generated transfers read the acknowledge's last bit at
  latency + 1, so they failed there while ARM's C path and probe-rs's scans, which sample late,
  did not. Fixed: four-wire latency default 10, and that bit sampled as late as the
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
- The fix belongs in the host: probe-rs's cJTAG branch joins in `icepick.rs` with 256 clocks
  rather than 10, over 5 us up to 50 MHz.

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

