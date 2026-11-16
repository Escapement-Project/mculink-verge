# The bench

How the firmware is checked on real hardware: reading the target's supply, the hazards, the marker
header, what each failure looks like, and what the emulator predicts. MEASUREMENTS.md has what the
bench measured. Flash and recover units with verge (its docs/updating.md, "ISP mode");
mculink-stock-notes' `updating.md` has what the bootloader does with the ISP jumper and without it.
BOARDS.md has both boards' pins.

## Target sense, on the plain board's J6

**The target needs its own supply; the probe cannot power it.** J6 pin 1 is a sense input. A voltage
follower (U6) buffers it, and the buffered level becomes the reference for every level shifter on
the debug lines. The only path from the probe's 3V3 to pin 1 is R16, 33 kOhm, which gives the
shifters a defined reference when nothing is plugged in and collapses under any real load. That is
why a target hung off it stayed unpowered. No other connector carries power either: J7 is RX, TX and
ground. The one path is a hand-wired mod from J2's unpopulated pad 1, which is the probe's own 3V3
rail. That rail has a 700 mA regulator the probe's own MCU and USB PHY also draw on, with no current
limit and no switch, so a target fault resets the probe. The MCU-Link Pro supplies a target by
design (its J6 to enable, J5 for 3.3 or 1.8 V). So the target powers itself. With pin 1 unwired the
shifters have no reference, and the probe drives nothing at all.

**The probe can measure the target, and tell an empty connector from a dead target.** The buffered
level goes through a 2K/2K divider (R22/R23) to `DBGIF_VREF`, an analog input on PIO0_31, so the
MCU reads half the target voltage. The buffer exists so the divider's ~0.8 mA comes from the probe
and not the target. J6 pin 9 is `IF_DETECT`, through 2K to `DBGIF_DETECT` on PIO0_22, which needs
the MCU's pull-up. A target connector that grounds pin 9 pulls it low. Together:

| state | J6 pin 1 | pin 9 | measured on the MSPM0 bench board |
| --- | --- | --- | --- |
| nothing connected | ~3.3 V, through R16 | open | 3.276 V, open |
| target connected and powered | the target's VDD | grounded | 3.268 V, grounded |
| target connected and unpowered | wherever the dead board pulls R16 | grounded | 1.084 V, grounded |

**Empty and powered differ by 8 mV here**, so pin 9 is what separates them, as predicted. The
empty reading is also the chain's own error: pin 1 then sits at the probe's 3V3 through an unloaded
R16, and the ADC's reference is that same rail, so it should read exactly full-rail. It reads 0.7%
low, which is divider, buffer and converter together. Take readings as good to about 1%.

**An unpowered target does not read zero.** This MSPM0 board, dead, loads R16 like roughly 16K,
so pin 1 sits at 1.08 V. That is unmistakable against a 3.3 V supply, but the level depends on the
board. Against a 1.2 V target a dead board and a live one could read the same.

A 3.3 V target and an empty connector read the same voltage. Nothing loads the sense node but the
buffer's input, so the pull-up delivers the probe's own 3V3 almost exactly, within the divider's
tolerance of any real regulator. What differs is source impedance, 33 kOhm against well under an
ohm, and the board has no switchable load to measure it with. SJ1 in its 2-3 position would put
the divider straight on pin 1, and then an empty connector would read ~0.36 V. That is a solder
change, and it loads every target with ~0.8 mA. So presence comes from pin 9, or from the target
answering an IDCODE or DPIDR read. The connector standard names pin 9 GNDDetect and has the target
ground it, so a board with the standard 10-pin header usually does it already; check continuity on
an unfamiliar one. **With flying leads, ground pin 9 in the harness**, or the detect pin says
nothing. **Ours reads both**: vendor 0x86, `tools/dap.py target`. The ADC
channel was not in any document here and one third-party source contradicted the obvious guess,
so it was found on the board: `dap.py target --sweep` with the target unpowered and then powered.
ADC0 3A went 542 to 1634 mV at the pin, half the supply. No other channel moved by more than
174 mV. Five repeat reads agree to 2 codes in 65536. On the plain board the reading is only as
absolute as the probe's own 3V3, which VDDA and VREFP are tied to. The Pro's VREFP is a 3.0 V
reference (BOARDS.md).

**Check the target before every attach**: `dap.py target` should say about 3.3 V with pin 9
grounded. Well under that means the target is unpowered, and 3.3 V with pin 9 open means the
harness is not plugged in.

## Hazards

- **`DAP_TransferAbort` answers nothing.** Reading for a reply after sending it stalls the probe's
  bulk endpoint, and every later command times out. `usb.core.Device.reset()` clears it without
  unplugging and does not disturb a halted target.
- **Run the gate (`uv run tools/gate.py`) before flashing,** or at least `ninja -C build check` on
  the image you flash. It refuses a build that clocks any wire faster than the host asked for,
  beyond what `tools/baseline/known-overshoot.txt` records: nothing for the default build. ARM's
  paths alone (`+armwire`) have theirs in `tools/baseline-armwire/`.
- **Placement alone moves throughput.** Adding code that never runs cost 0.06% of block
  throughput (126 words: 702.8 to 702.3 KiB/s, three runs each). The ADC only wakes when asked, so
  this is placement in flash, not work. Treat anything under about 0.1% as inside the floor for any
  change to the image, unless placement is controlled for.

## J8, the marker header

The plain board's four DIO pins: through-hole pads at 50-mil pitch beside the MCU, and the only
pins on the board nothing else uses. No buffer, no series resistor and no ground pin, so the
analyser takes its ground from the debug connector.

| J8 pin | MCU pin | signal |
| --- | --- | --- |
| 1 | 4 | `PIO1_0` |
| 2 | 39 | `PIO1_1` |
| 3 | 41 | `PIO1_2` |
| 4 | 42 | `PIO1_3` |

**Pin 1 is the leftmost pad with the board the right way up**, USB to the left and the 10-pin debug
connector to the right. Settled by driving one pin at a time and watching the analyser, not by
reading the silkscreen. `uv run tools/dap.py j8 --walk` repeats it in twelve seconds.

The pins boot as DIO inputs, so a capture that wants the command marker gives it the pins first:
`uv run tools/dap.py j8` gives it all four. They then go high together while a host command runs,
which is one store and makes the header orientation-free. `--marker 0x8` gives it J8 pin 4 alone,
and `--release` gives the pins back to DIO, as a restart does.

## Failure modes

README.md, "When a build misbehaves", has what each failure looks like.

## What the emulator predicts, so you can check it

KiB/s of wire time over a 64-word block, on the generated transfers, timed by the m33 model.
Regenerate with `uv run tools/cycles.py --table --words 64`.

JTAG runs at the probe's default read latency, 10 cycles, which holds it to 25 MHz. A connection
measures the target's latency, and a target that answers within 8 cycles runs at 30 MHz
(docs/wires.md, "JTAG").

cJTAG is shown in two formats. OScan1 has the longest frames on the wire and OScan6 the shortest.
The modelled target never answers WAIT. A CC1312R1's own figures are in docs/wires.md, "cJTAG".

| asked | SWD read | SWD write | JTAG read | JTAG write | OScan1 read | OScan1 write | OScan6 read | OScan6 write |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 5 MHz | 415 | 416 | 461 | 462 | 157 | 157 | 236 | 236 |
| 10 MHz | 817 | 827 | 890 | 893 | 311 | 311 | 460 | 461 |
| 16 MHz | 1194 | 1227 | 1274 | 1280 | 460 | 461 | 670 | 672 |
| 25 MHz | 1911 | 1981 | 1939 | 1952 | 574 | 594 | 758 | 794 |
| 50 MHz | 2723 | 3362 | 1939 | 1952 | 718 | 749 | 864 | 910 |

Single-register transfers are a different command and move independently:
`uv run tools/cycles.py --wire swd --transfer --words 16` gives 634/642 at 10 MHz and
1493/1677 at 50.

Wire time is not throughput: USB and the host add to every command (MEASUREMENTS.md, "The USB path
at 50 MHz"), so treat the table as a ceiling. A request between two whole cycle counts gets the
slower one (docs/wires.md, "From the clock asked for to the clock on the wire").
