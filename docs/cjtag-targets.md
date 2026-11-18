# cJTAG targets

Verge frames cJTAG, and the host brings the target up: it activates the target's cJTAG adapter,
switches its format, and leaves it in a known state ([Host
interface](host-interface.md#cjtag-0x95-and-0x96)). This page says what that takes on TI's parts
with an ICEPick and a cJTAG adapter, such as the CC13xx and CC26xx families. We worked it out on a
CC1312R1. `tools/dap.py cjtag --ti`, in mculink-verge, runs the whole sequence against a probe
running Verge, and exits 1 if an IDCODE it reads is not the one expected.

## Power

The JTAG power domain holds the ICEPick and the cJTAG adapter (TRM 6.4, ICEMelter).

- **Power-on and a pin reset leave the domain off.** ICEMelter powers it up after 8 rising and 8
  falling TCKC edges, and TI requires 200 µs before further commands. A fresh domain has the adapter
  in two-pin JScan, where TMSC is TMS on every clock, and the TAP reset. So a pin reset leaves the
  debug side as a power cycle does.
- **Send the wake clocks in one command.** ICEMelter restarts its count if its 3rd to 8th rising
  edges span more than 4 ms. A domain left half-woken, its edges counted and its commands lost,
  needs more than 8 clocks: 20 wake it.
- **Test-Logic-Reset powers the domain off** when ICEPick's SYSCTRL KeepPoweredinTLR is clear, and
  only if TCKC stops there: more clocks wake it again. With the bit set it stays on. Powering the
  domain off this way before a system reset is what clears halt-in-boot (TRM 6.6).
- **Nothing on the target forces the domain on.** AON_PMCTL's JTAG_PD_FORCE_ON (0x40090034 bit 8)
  reads 0 on a CC1312R1, so the debug subsystem decides. TI's SDK clears the bit only before
  shutdown.

## The activation

This is TI's XDS110 sequence, from a TI engineer's post of the XDS110 firmware routine (TI E2E
thread 766675, "CC1352P: How to enable cJTAG OSCAN1 mode on CC13xx and CC26xx device"). Steps 1 to 3
are raw bits: the escape through `0x96`, and the clocks through `DAP_SWJ_Sequence`. Step 4 goes out
as OScan1 frames, so the host selects OScan1 when it connects.

1. **Reset the adapter and power up the JTAG domain:** a reset escape of 10 TMSC edges, at least 20
   TMS-high clocks, one idle clock, then 200 µs. TI's routine has 8 clocks and no wait. The escape
   and the extra clocks bring back a target a previous session left behind, and without the wait
   the commands that follow are lost.
2. **In JScan, one TMS bit a clock, from Run-Test/Idle:** a BYPASS IR scan (64 shift clocks), two
   zero-bit scans and a 1-bit DR scan. That is control level 2: the command window opens, and the
   device TAP leaves the scan path. Then STC1 1 (SEDGE=1: TMSC sampled on the rising edge) and STFMT
   9 (OScan1). Each DR scan's count of Shift-DR clocks is the command, so one clock too few is a
   different command: STFMT short by one is STC2, and STC2 9 switches the part to four-pin. TI ends
   the STFMT operand's scan in Select-DR.
3. **4 clocks with TMS low** that the adapter's control state machine takes and the TAP never sees.
   OScan1 frames start after them.
4. **In OScan1 frames:** finish the scan to Pause-DR. STMC 4, a zero-bit scan then a 4-clock scan,
   deselects every controller. MSC 0, a 7-clock scan then a zero-bit scan, selects controller 0.
   ECL, a zero-bit scan then a 1-clock scan, closes the command window and brings the device TAP
   back. After each of the three, send 7 raw clocks with TMS low: one start packet that does not
   move the TAP, three packets in OScan1, then the 4-clock check packet (1149.7 Table 21-5). After
   ECL any count from 4 to 8 works: the extra TMS-low start packets leave the TAP in Run-Test/Idle.
   Without the deselect and the select, every read is all ones.
5. The IR holds BYPASS from step 2. Load IDCODE, or route the ICEPick, next. The activation moved
   the target's TAP with raw bits, so bring it to Run-Test/Idle and send the format again before
   framing on ([Host interface](host-interface.md#select-a-format-0x95)).

With step 1 as above, the activation reads the target from any state it can be left in: fresh from
a pin reset, a live OScan1 or OScan2 session with KeepPoweredinTLR set or clear, after a
Test-Logic-Reset either way, and a half-woken domain. Without the escape, TI's sequence fails from a
live OScan1 session, whose adapter reads the JScan clocks as OScan1 frames. So a host can attach
without nRESET. A fresh domain needs no escape, but a selection escape followed by fewer idle clocks
than the adapter wants swallows the command scans after it.

## During a session

- **Never enter Test-Logic-Reset.** Any entry, even three frames from Run-Test/Idle, drops OScan1:
  the reads then show `0x81010101`, an adapter in JScan, whatever KeepPoweredinTLR says. Line
  resets, chain scans and TAP resets all pass through it, so a host sends none of them. The next
  attach recovers the target.
- **The chain.** Once the ICEPick switches the Cortex-M TAP in, the chain holds two devices, with IR
  lengths 4 and 6, and the DAP is device 0, nearest TDO. The switched-in TAP answers only after at
  least 4 Run-Test/Idle clocks.
- **The routed TAP joins on time, not on a count.** After the routing write, the selected TAP joins
  only on Run-Test/Idle clocks that come about 2.5 to 3 µs later, in one run. A wait without clocks
  does not help: the next command leaves Run-Test/Idle at once. So send enough clocks to span that
  time at the clock in use: 256 cover it up to 50 MHz.
- **Check the wire, not only the result.** IDCODE after the activation proves little: TMS-high reset
  clocks pass Test-Logic-Reset, which powers the JTAG domain down and reads ones, and without them
  the IR still holds BYPASS (`0xfffffffe`). Route the ICEPick and read CPUID instead. Even then, a
  wrong framing can read right: ECL framed in OScan1 to a target already in OScan2 still read
  CPUID. Decode the wire when the framing changes.

## Other formats

A CC1312R1 has every optional format: its CNFG0 reads `0x03c00714`, class 4 (T4), revision 1,
OSCANS 111 (OScan2 to OScan7), no SScan, two-pin by default, and no TRESET, FRESET or duty-cycle
control. CNFG1 reads 0 and CNFG2 0x120. To read CNFG0: TI's activation up to its
ECL, then MCM 0 (opcode 6, operand 0) and SCNS 2 (opcode 9, operand 2), then a 32-bit DR scan from
Run-Test/Idle, least significant bit first, then ECL. Each command is two DR scans through Pause-DR,
as TI writes them, with 7 raw TMS-low clocks after it. Select the controller with MCM, not STMC 7:
STMC's CGM=1 allows many members, and with more than one every controller drives 1 through a
register read, which then reads `0xffffffff` (1149.7 13.9 and 14.8).

To switch the target to another format, send STFMT with its number as an OScan1 command after MSC 0,
with its 7 raw clocks. Select the same format on the probe at Run-Test/Idle, then send ECL in the
new format. The raw clocks after ECL are its start packet and the 4-clock check packet: 5 in OScan2,
where a start packet outside Shift is one bit; 7 in OScan5, which frames outside Shift as OScan1
does; 5 in OScan6.

TI's own tools use cJTAG on this part by default (CCS's target database,
`ccs_base/common/targetdb/devices/cc1312r1f3.xml`): the XDS110 in OScan2, the XDS100v3 and the
XDS200 in OScan5. For a DPACC or APACC scan, 4 clocks outside Shift and 36 in it, OScan2 sends 7%
fewer packets than OScan1, and OScan5 30% fewer before its exit escape. The XDS110 activates in its
own firmware, from one host command that carries the settings (`CJTAG_CONNECT`, 0x2b in OpenOCD's
`xds110.c`). TI's `cc26xx_icepick.gel` writes WAKEUPMCU on connect: the WUC TAP's
MCU_VD_FORCE_ACTIVE (TRM Table 6-23), which powers the MCU domain under a sleeping application. The
same TAP holds TMSC's drive strength and slew (TMS_PAD_CFG), chip erase, and the MCU and system
reset requests. OpenOCD's `ti-cjtag.cfg` switches a part to four-pin with STC2; it does not run
two-pin.

## JTAG on these parts

- **After a pin reset the adapter is in two-pin JScan,** and TDO is not driven. Conventional JTAG
  takes a switch to four-pin first: BYPASS, two zero-bit scans, a 1-bit scan, STC2 (a 2-bit scan)
  with operand 9 (a 9-bit scan), BYPASS, then IDCODE.
- **TDO's window**, from the falling edge that launches a bit: the DAP's from 7 core cycles to 6
  past the next fall, and the ICEPick's from 6 to 5 past it.
- **After a system reset TDO comes later:** a CC1312R1 measures 10 cycles on the first JTAG
  connection after one, against 7 after a pin reset. The probe measures the latency at each
  connection, so it follows ([Wires and clocks](wires.md#jtag)).

## Detach

To leave the part as a pin reset would, with the JTAG domain off, its 400 µA or so gone and
halt-in-boot cleared (TRM 6.4, 6.6): clear KeepPoweredinTLR, or never set it in cJTAG; walk from
Run-Test/Idle to Test-Logic-Reset with exactly three framed TMS-high clocks; then send no TCKC edge.
In OScan2, send the trailing frame first ([Host interface](host-interface.md#select-a-format-0x95)).
A host that sets KeepPoweredinTLR, and ends a JTAG session in Test-Logic-Reset, leaves the domain on
and halt-in-boot armed. That follows from the TRM, and we have not measured it over JTAG.
