# Host interface

Verge speaks CMSIS-DAP, Arm's protocol between a debugger and its probe. Any host that speaks
CMSIS-DAP, probe-rs for one, debugs through Verge unchanged. This doc is for a host that does more.
It covers how to recognise Verge, the commands Verge adds to CMSIS-DAP, the Pro's second USB
interface, Verge Aux, and the energy stream that interface carries.

## Conventions

- A request is a run of bytes, and its first byte is the command's ID. The reply starts with the
  same ID.
- `[0x90, 2]` stands for the two bytes `0x90` and `0x02`. A field written `rate:u32` takes four
  bytes.
- Integers are little-endian. `u8`, `u16`, `u32` and `u64` are unsigned integers of 1, 2, 4 and 8
  bytes; `i32` is a signed one of 4.
- `DAP_OK` is `0x00` and `DAP_ERROR` is `0xff`, as in CMSIS-DAP.
- A command the probe does not know gets `[0xff]`, CMSIS-DAP's reply to an invalid command.
- Check a reply's length before you read a field.

## Standard commands

The probe answers CMSIS-DAP's own commands as Arm's specification says. Where the specification
leaves a case open, it answers as follows.

- **Malformed commands.** The probe checks each command's length, and the room its reply needs,
  before it runs it. A command cut short, or one whose reply would not fit, gets `[0xff]`. SPI and
  I2C keep their four-byte invalid-argument reply where it fits. A block read is shortened to the
  room left for its reply, and an SWO read returns at most that much. Size requests to the
  transport's packet size.
- **Batches.** A malformed command in a `DAP_ExecuteCommands` batch, or one whose reply would not
  fit, stops the batch before it runs. The reply keeps the replies of the commands before it, and
  its count says how many ran. A command that runs and reports an error still counts. This
  handling adds no support for nested batches or queued commands.
- **Slow clocks.** `DAP_SWJ_Clock` answers `DAP_ERROR` for a clock under 9 Hz, as it does for 0 Hz,
  and keeps the clock it had.
- **Generated transfers.** The probe runs every transfer on code it generates for the clock and
  settings in use. If it ever cannot generate that code, the command fails before the wire moves.
  Transfers and block transfers report a protocol error (8) with no transfer done.
  `DAP_JTAG_Sequence`, `DAP_JTAG_IDCODE` and `DAP_WriteABORT` answer `DAP_ERROR`.

The last two apply to every build but the `+armwire` reference build. That build keeps Arm's own
transfer code, to compare the generated code against, and takes any clock above 0 Hz.

## How the probe identifies itself

In a USB listing, a Verge probe reads `Escapement Project Verge CMSIS-DAP (CJF2551AD5)`, with its
serial number last.

| Where | What it holds |
| --- | --- |
| USB VID:PID | `0x1FC9:0x0143`, the pair NXP's firmware uses |
| bcdDevice | `0x0100`, plus a bit for each function the probe offers: debug bit 0, the console (VCOM) bit 1, the Pro's second console bit 2, Verge Aux bit 3 |
| Manufacturer string, `DAP_Info` 0x01 | `Escapement Project` |
| Product string, `DAP_Info` 0x02 | `Verge CMSIS-DAP`, or `Verge Pro CMSIS-DAP` on a Pro |
| Serial number, `DAP_Info` 0x03 | `CJ` and 8 hex digits of a CRC32 over the chip's unique ID, or `CJNOUUID` without one |
| `DAP_Info` 0x04 | `2.1.2`, the version of Arm's CMSIS-DAP that Verge is built on |
| `DAP_Info` 0x05 to 0x08 | Empty. These four strings name a target, for a probe built onto one |
| `DAP_Info` 0x09 | The build string, below |
| Vendor `0x82` | `CJTAG`, the build string, the board and what the probe can do ([below](#identify-firmware-0x82)) |

`DAP_Info` also reports three packets of 512 bytes on the bulk interface at high speed, and the
capabilities SWD, JTAG, SWO in UART encoding, atomic commands and timestamps.
[`src/usb_descriptors.c`](../src/usb_descriptors.c), [`src/DAP_config.h`](../src/DAP_config.h) and
[`src/ser_num.c`](../src/ser_num.c) set these values. mculink-stock-notes' `identity.md` has what
NXP's firmware reports.

The build string tells two builds apart. Any CMSIS-DAP host can read it with `DAP_Info` 0x09, and
`0x82` gives it beside the capability flags. It is the output of `git describe --always --dirty
--tags`, made afresh on every build. A `-dirty` suffix means the build came from uncommitted work,
and nothing in the repository identifies it.

## Full speed

The probe is a high-speed USB device. If a bus reset leaves it at full speed, it leaves the bus for
300 ms and attaches again, to try for high speed. It tries three times after it is plugged in, then
stays at full speed. There every bulk endpoint is 64 bytes, and the packet size in `DAP_Info`
(`0xFF`) is 64, so each command and each reply fits one packet. The HID interface uses 64-byte
packets at either speed.

## Identify firmware: `0x82`

`0x82` changes nothing on Verge or on NXP's firmware. Other firmware gives the vendor IDs meanings
of its own: DAPLink's `0x82` sets its UART. So a host identifies the probe first, by `DAP_Info` 0x01
reading `Escapement Project`, or by the USB ID `0x1FC9:0x0143`, which NXP's firmware shares. A reply
without `CJTAG` after the ID is not from Verge. NXP's V3.172 echoes `0x82` alone, and V3.174 answers
`82 00 01`.

Send `[0x82]`. The reply's fields, counted from the byte after the ID:

| Offset | Field |
| --- | --- |
| 0 to 4 | `CJTAG` in ASCII |
| 5 | Format version: 8 |
| 6 | Capability flags |
| 7 | Length of the build string, `n` |
| 8 to 7+n | The build string, `n` bytes, with no terminator |
| 8+n | Board: 0 the standalone MCU-Link, 1 the MCU-Link Pro, 2 an on-board MCU-Link (one built onto an NXP evaluation board) |
| 9+n to 12+n | 0, u32 |
| 13+n to 16+n | Extended capabilities, u32 |

Format 8 holds every field above. From format 7 the probe has `0x95` and `0x96`
([cJTAG](#cjtag-0x95-and-0x96)); from format 8, `0x95` sent while disconnected selects the format
the next `DAP_Connect(2)` starts in. On the HID interface the reply is padded to 64 bytes, and the
bytes after the extended capabilities are padding. A later format adds fields only at the end, and
raises the format version, so read the fields you know and ignore any after them.

The capability flags, byte 6:

- **Bit 0:** 0.
- **Bit 1:** cJTAG framing is on now.
- **Bit 2:** [target sense](#target-sense-0x86), `0x86`.
- **Bit 3:** the SWD transfers are generated, and `0x89` is there
  ([Design](../DESIGN.md#vendor-commands)). Clear in the `+armwire` reference build.
- **Bit 4:** 0.
- **Bit 5:** the JTAG and cJTAG transfers are generated, and `0x8A` is there
  ([Design](../DESIGN.md#vendor-commands)). Clear in the reference build.
- **Bit 6:** the Pro's DIO pins, `0x8E`.
- **Bit 7:** the Pro's target supply, `0x8F`.

An on-board MCU-Link has no cJTAG. There `0x95` takes only 0, conventional JTAG, and `0x96`
refuses every request.

The Pro's J15 jumper, fitted as the probe boots, turns off USB-SIO: the Pro's DIO pins and its SPI
and I2C bridges. Bit 6 is clear then.

The extended capabilities:

- **Bit 0:** energy capture, `0x90`, on a Pro with its HW_VER_5 strap fitted
  ([Boards](../BOARDS.md) lists the straps).
- **Bit 2:** the energy total, `0x90` mode 3.
- **Bit 3:** SPI, `0x91`.
- **Bit 4:** I2C, `0x92`.
- **Bit 5:** the calibrated fields at the end of `0x90`'s latest reply.
- **Bit 6:** `0x8E` on Verge Aux.
- **Bit 7:** markers, `0x94`, and the stream's markers message.
- **Bit 8:** capture identities and conditional stops, `0x90` modes 8 to 13.
- **Bit 9:** the range setting, `0x90` mode 14, with the stream header's flag bit 2 and the pairs'
  under-range bit.

Bits 2, 5, 7, 8 and 9 come with bit 0. Bits 3 and 4 are set on a Pro whose J15 left USB-SIO on as
it booted. Bit 6 comes with capability flag bit 6. The other bits are reserved.

## cJTAG: `0x95` and `0x96`

cJTAG, IEEE 1149.7, carries JTAG over two pins: TCKC on SWCLK/TCK and TMSC on SWDIO/TMS. It frames
each JTAG clock in one of several scan formats. Verge frames OScan1, OScan2, OScan5 and OScan6.

Two commands drive it. They are the `DAP_JTAG_Format` and `DAP_JTAG_Escape` proposed for CMSIS-DAP
([CMSIS-DAP#8](https://github.com/ARM-software/CMSIS-DAP/pull/8), IDs `0x24` and `0x25` there), at
vendor IDs until Arm assigns them. The host activates the target itself, with escapes and raw bits.
The probe never changes the target's format.

### Select a format: `0x95`

Send `[0x95, format]`. The reply is `[0x95, status]`.

| `format` | Selects |
| --- | --- |
| 0 | Conventional JTAG |
| 1, 2, 5, 6 | OScan1, OScan2, OScan5, OScan6: 1149.7's SCNFMT 9, 10, 13 and 14 |

Any other value answers `DAP_ERROR` and changes nothing. So does any value over SWD, and every
value but 0 on an on-board MCU-Link, which has no cJTAG. A host finds which formats the probe has
by trying them while disconnected: OScan1, 2, 5 and 6, then 0, fit in one `DAP_ExecuteCommands`,
and move no pin.

- **Selected while disconnected, a format waits for the connection.** `0x95` with no connection
  moves no pin, and the next `DAP_Connect(2)` starts in that format. Every `DAP_Connect` the probe
  accepts uses the selection up, whichever port it opens, so a host that connects without selecting
  gets conventional JTAG. `DAP_Disconnect` clears the selection, and a `DAP_Connect` the probe
  refuses changes nothing. Send the two together, such as `[0x7F, 2, 0x95, 2, 0x02, 2]` for OScan2.
- **A connection into an OScan format** sets the pins straight to cJTAG: TCKC driven low first,
  then TMSC driven high, and TDI let go to the probe's pull-up. No conventional JTAG goes onto the
  wire, and TCKC makes no period. On the Pro, TCK's pull-down already holds TCKC low. Elsewhere TCKC
  may rest high on the target's pull-up before the connection, and the target then sees one fall; a
  target sampling TMSC on the falling edge reads 1 there.
- **On a JTAG connection, `0x95` changes the format at once.** A connection in conventional JTAG
  has its pins set up as Arm's are: TCK, TMS and TDI driven high. Into an OScan format TCKC goes low
  and TDI is let go, so TCKC makes one fall. Back to 0, TDI is driven high again.
- **From one OScan format to another,** nothing is sent. The host has already switched the target,
  with STFMT and its check packet.
- **`0x95`, `DAP_Connect` and `DAP_Disconnect` send no TCKC period of their own.** TCKC can still
  rise once. `DAP_Connect` in conventional JTAG sets the clock pin high, as Arm's does.
  After `DAP_Disconnect` TCKC rests on the target's pull-up. In OScan2 and OScan6 the last frame
  outside Shift moves the target's TAP at the next TCKC rise, whatever drives it (1149.7 25.6.1).
  So before it stops framing, a host sends one more frame, and the move lands on a rise it chose:
  TMS 0 in Run-Test/Idle, Pause-DR or Pause-IR, or TMS 1 in Test-Logic-Reset. That frame's own move
  keeps the TAP where it is, so whichever rise completes it changes nothing.
- **OScan2, 5 and 6** frame each JTAG clock by the state of the target's TAP, so the probe follows it
  from Run-Test/Idle, starting where the format takes effect: at the connection, or at `0x95` on a
  live one. Have the TAP in Run-Test/Idle then. Sending the same value again restates it.

With an OScan format selected on a JTAG connection, `DAP_JTAG_Sequence`, `DAP_JTAG_IDCODE`,
`DAP_Transfer`, `DAP_TransferBlock` and `DAP_WriteABORT` go out as frames. `DAP_SWJ_Sequence` sends
raw bits, one TCKC period each, as Arm's does. But TMSC moves only while TCKC is low, a quarter period
after the fall. Arm's moves it with SWCLK high, just before the fall, which leaves a target sampling
on the falling edge almost no setup, and leaves SWCLK high between commands.
`DAP_SWD_Sequence` and `DAP_SWJ_Pins` are Arm's, and keeping them off the cJTAG pins is the host's job.

TCKC rests low between commands and between frames. After each fall on which the target samples a bit
the probe drove, TMSC moves no sooner than a quarter period later, in frames and from one command to
the next: a quarter of the clock the probe runs, rounded up to a whole core cycle. A target sampling
on the falling edge gets that as hold. One sampling on the rising edge gets the rest of the low phase
as setup, which at the fastest clocks comes down to one core cycle, 6.7 ns.

### Send an escape: `0x96`

Send `[0x96, edges, level]`, with `edges` from 2 to 255 and `level` 0 or 1. The reply is
`[0x96, status]`.

The probe sets TMSC to `level` with TCKC low, a quarter period after the last fall and a quarter
period before the rise. Then it makes one TCKC period with `edges` TMSC transitions while TCKC is
high, so an odd count leaves TMSC at the other level. The rise, each edge and the fall come one
TCKC period after the one before, at the clock `DAP_SWJ_Clock` set. That meets 1149.7 10.4.2 c)
whenever the clock is within the target's rating. The host picks the count and the clock, and with
them how long TCKC stays high: `edges` + 1 periods. At 10 kHz a 10-edge escape takes 1.1 ms, past the
1 ms after which a TAP.7 in power-control mode 0 may power down (18.10). 1149.7 reads 4 or 5 edges as
a deselection, 6 or 7 as a selection and 8 or more as a reset. The target counts the escape's period
as a bit, so the probe waits a quarter period after its fall. Until the escape ends the probe
answers no other CMSIS-DAP or Verge Aux request. A 255-edge escape takes 28 s at 9 Hz, the slowest
clock the generated builds take, and longer still on the `+armwire` reference build.

It needs a JTAG connection, in any format, and leaves the format and framing as they were. With
no JTAG connection, 0 or 1 edges, or a `level` above 1, it answers `DAP_ERROR`.

## Target sense: `0x86`

`0x86` reads the voltage the target supplies on the debug connector, and whether a target is
plugged in. It works on both boards. The debug connector is J6 on the standalone MCU-Link and J7 on
the Pro. Its pin 1 carries the target's supply, and a target grounds its pin 9. The command is on
CMSIS-DAP only: Verge Aux does not carry it.

Send `[0x86, mode, channel]`, three bytes in every mode:

- **Mode 0** reads the target's supply on pin 1. It ignores `channel`.
- **Mode 1** reads any channel of the probe's ADC, ADC0: the low five bits of `channel` pick the
  channel, and bit 7 picks side B. It reports the voltage on the chip's own pin.

The reply is `[0x86, status, code:u16, mv:u16, detect]`, seven bytes:

- `code` is the ADC's reading.
- `mv` is the voltage in mV: on pin 1 of the debug connector in mode 0, and on the chip's pin in
  mode 1.
- `detect` is 1 while something grounds pin 9.

The probe pulls pin 1 up toward its own 3.3 V, so an empty connector reads about 3.3 V. Read
`detect` before you trust a reading: 0 means no target is plugged in. A target plugged in without
power reads somewhere below 3.3 V, wherever it pulls the line.

The probe measures against its ADC's reference. On the Pro that is a 3.0 V reference. On the
standalone MCU-Link it is the probe's own 3.3 V supply, so a reading is only as accurate as that
regulator.

A mode above 1 gets `DAP_ERROR`, with `code` and `mv` zero. So does a read the ADC fails, with `mv`
zero and `code` saying what failed: 1 or 2 for the ADC's calibration, 3 for no result, or for a
running capture the probe could not pause or resume. `detect` is read either way.

On a Pro with an energy capture running, a read pauses the capture for a moment, and the stream
marks the gap ([Pairs](#pairs-0xa0)).

## Pro DIO: `0x8E`

The Pro's J19 header has four DIO pins, DIO0 to DIO3. Each one is an input or a driven output.

Send `[0x8E, mask, outputs, levels]`. Bit k of each byte stands for DIO k. For each pin in `mask`,
`outputs` sets its direction, 1 for an output, and `levels` sets the level it drives. Pins outside
`mask` stay as they are. A zero mask changes nothing and reads the state.

The reply is `[0x8E, status, outputs, levels, read, pins, group_count, groups...]`:

- `outputs` is each pin's direction.
- `levels` is the level each pin drives, or will drive once it is an output.
- `read` is each pin's level as sampled. An output reads its own drive.
- `pins` is `0x0f` on a Pro with DIO on, and 0 otherwise.
- `groups` is `group_count` bytes, each a mask of pins that change direction together. A Pro reports
  `0x07` (DIO0 to DIO2) and `0x08` (DIO3).

A pin outside `pins`, DIO turned off, or a direction change that splits a group gets `DAP_ERROR`
and changes nothing. The reply still reports the state.

The state lasts across debug sessions and hosts. So a pin can hold a target's boot mode through a
reset and a reconnect. The pins go back to inputs when the probe restarts. A change reaches DIO2
and DIO3 a moment after DIO0 and DIO1: about 40 ns later for DIO2. J15, fitted when the probe
boots, turns DIO off.

Verge Aux carries the same command, on the same pins, so a host can use them while a debugger holds
CMSIS-DAP (extended capability bit 6). `verge dio` drives the pins from the command line. It checks
a change against the groups before it sends it.

## Pro target supply: `0x8F`

The Pro can power the target from a regulator of its own.

Send `[0x8F, mode]`: 0 switches the supply off, 1 switches it on, and 2 reads it. The reply is
`[0x8F, status, enabled]`. `enabled` is the state last set since the probe booted, not a measured
voltage. [Target sense](#target-sense-0x86) reads the voltage on the debug connector. The probe
switches the supply off as it boots.

Only the Pro has a supply. On other boards, and for any other mode, the reply is `DAP_ERROR` and
the supply stays as it was.

J5 picks the voltage: 1.8 V fitted, 3.3 V open. J6 connects the regulator to the target. The
regulator is rated to 400 mA. After you switch it on, give the regulator and the target's supply
time to settle before you talk to the target.

## Pro energy capture: `0x90`

The Pro measures the current that flows from J9 pin 1 to pin 3, and the voltage on the target's
side. It takes the two together, as a pair, at a steady rate of 1 to 100,000 pairs a second. It adds
each pair's energy to a running total. Each pair is read on one of two current ranges, low and high
([Current ranges](#current-ranges)). J16 to J18 pick the current setting: 350 mA or 50 mA at full
scale. [Energy
capture](https://github.com/Escapement-Project/verge/blob/HEAD/docs/energy.md)
explains all this for users of verge.

Energy capture is there when `0x82` sets extended capability bit 0. It is off when the probe
boots. On CMSIS-DAP a host polls: it reads the newest pair and the total. On Verge Aux the probe
can also send it every pair ([The energy stream](#the-energy-stream)).

### Requests

| Request | What it does |
| --- | --- |
| `[0x90, 0, rate:u32]` | Starts a capture at `rate` pairs a second |
| `[0x90, 1]` | Reads the newest pair |
| `[0x90, 2]` | Stops the capture. Safe to repeat |
| `[0x90, 3]` | Reads the total: the energy in nJ and the pairs counted |
| `[0x90, 4, range]` | Reads the calibration points of the low range (0) or the high range (1) |
| `[0x90, 14, ranges]` | Sets how the next start uses the current ranges: 0 switches automatically, 1 holds the high range |

Modes 5 to 7 control the energy stream, and modes 8 to 13 work with capture identities. Both are
below.

Every mode answers `[0x90, DAP_OK, result, ...]` on an interface that carries it. A result of 0 is
OK; the others are under [Results](#results).

- **Start (mode 0):** `[0x90, DAP_OK, result, rate:u32]`. `rate` is the rate the probe's timer
  runs at, to the nearest pair a second. A stream's header gives it exactly.
- **Latest (mode 1):** the voltage code (u16), the current code (u16) and the sequence number (u32),
  then the voltage in mV (u32), the current in nA (i32), the range (u8) and calibrated (u8): 21
  bytes in all. When the result is not 0, every sample field
  is zero.
- **Stop (mode 2):** the result alone.
- **Total (mode 3):** `[0x90, DAP_OK, result, energy_nJ:u64, pairs:u64]`, 19 bytes.

The sequence number counts the pairs since the capture started, from 1. It never goes back within a
capture.

The range is 1 for the low range in the 350 mA setting, 2 for its high range, 3 for the low range in
the 50 mA setting, 4 for its high range, and 0 when the range is unknown. Calibrated is 1 when the
probe can convert both codes to mV and nA. Voltage and current are scaled to the Pro's 3.0 V
reference. The current is the target's: the probe takes off what its own voltage divider draws
through the sense resistor. The raw codes stay at the front of the reply. Use the mV and nA fields
when calibrated is 1.

The probe converts a pair's voltage and current together. It averages as many conversions as the
rate leaves time for ([Boards](../BOARDS.md#accuracy-limits), "Accuracy limits"). It reads the range
with a single conversion, so a range change shows in the next pair.

### The total

The probe adds up every calibrated pair itself, whether or not a host reads them.

- A start that succeeds clears the total. One that fails leaves it.
- A stop keeps the total for later reads, and mode 3 reads it without clearing it.
- Mode 3 returns 1 (stopped) before the first start that succeeds, and 2 (no sample) until the
  probe has counted a calibrated pair.
- Each pair adds its power over one period of the probe's timer. A negative current counts as
  zero. The pair count includes pairs with no current.
- Time without pairs adds nothing. The probe pauses a capture for a moment while it reads the
  target's supply. It holds its conversions while it measures a target's read latency, how long
  after a clock edge the target's data arrives: when a debugger connects at a fast clock or changes
  to one, again when a measurement contradicts itself, and when a host asks for a measurement
  ([Design](../DESIGN.md#vendor-commands)).
- A stop or a pause counts every complete pair the probe has converted. A part pair is dropped.
- The probe's own pauses do not restart the sequence numbers.
- The total resets when the probe restarts.

### Current ranges

The probe reads current on two ranges. The low range reads small currents finely: 31 nA a code in
the 350 mA setting, and 7.6 nA in the 50 mA setting. The high range reads the rest.

Mode 14 sets how the next capture uses them:

- **0, the default,** switches automatically. A comparator moves the probe to the high range as the
  current rises. The probe moves back to the low range once it falls
  ([Boards](../BOARDS.md#range-switching), "Range switching").
- **1** holds the high range for the whole capture.

The next start that runs takes the setting, and the probe goes back to 0. A start refused for its
rate, its identity or its calibration leaves the setting for the one after. So a host that wants the
high range sends mode 14 before every start. A running capture keeps its setting through every pause
and resume. Mode 14 sent while it runs applies to the next start. Any other value returns 5 (invalid
argument) and changes nothing. The reply is `[0x90, DAP_OK, result]`.

The low range alone is not offered. Holding it would take away the switch up, and that switch
protects a target that draws more than the low range's full scale.

Switching automatically reads small currents finely, but each switch disturbs the reading for a
moment. Holding the high range disturbs nothing, but reads small currents unreliably, and the
stream marks those pairs under range ([Pairs](#pairs-0xa0)). [Energy
capture](https://github.com/Escapement-Project/verge/blob/HEAD/docs/energy.md#the-two-current-ranges)
says what each costs.

### Calibration

Mode 4 returns `[0x90, DAP_OK, result, count, points]`, 28 bytes. `points` is four entries, lowest
first. Each is an ADC code (u16) and the current through the sense resistor at that code, in nA
(u32). Entries past `count` are zero. `count` is 0 until the probe has calibrated. The result is 0
when the latest calibration succeeded. It is 6 when that calibration failed, with the points it
measured before failing.

The probe calibrates as it boots, before it answers the first command, and a capture start uses
that calibration. A start calibrates again only when the last calibration failed, or when the
setting J18 picks differs from the one calibrated. A start that cannot read the setting fails with
6 and keeps the calibration. Calibration disconnects J9 from the target for about 0.2 s, so a
target powered through J9 loses its supply for that time.

Moving the jumpers away and back to the same setting goes unseen. Restart the probe to calibrate
again. [Boards](../BOARDS.md#energy-measurement), "Energy measurement", has the details.

### Results

| Result | Meaning |
| --- | --- |
| 0 | OK |
| 1 | The capture is stopped. Mode 3 also returns it before the first start that succeeds |
| 2 | No complete pair yet. Mode 3 returns it until it has counted a pair |
| 3 | This board has no energy capture |
| 4 | The rate asked for is zero, or above 100,000 pairs a second |
| 5 | Invalid argument |
| 6 | Calibration failed: the ADC did not answer, or a range kept fewer than two usable points |
| 7 | The capture failed: a DMA fault, an ADC FIFO overflow, or the probe fell behind reading the samples |
| 8 | The capture identity does not match, and nothing changed (modes 11 and 13) |
| 9 | No identity: the random number generator failed, or the capture counter ran out. Any start can return it, modes 0 and 5 included |

Result 7 lasts through stops and failed starts. The probe keeps the total it had and stops adding
to it when it finds the fault. The total it keeps does not account for the samples it lost. A
start that succeeds clears the fault and the total.

### Capture identities

A Pro can serve two hosts at once: a debugger on CMSIS-DAP, and another tool on Verge Aux. Either
one can restart the energy capture. An identity names one capture. With it, a host can stop the
capture it started without stopping one that another host has started since.

An identity is 24 bytes: a 16-byte boot value, then an 8-byte capture counter, u64. Compare all 24
bytes, and read no meaning into the boot value. The all-zero identity names no capture. A reply
gives it when it has no identity to give, such as before the first start, and no request accepts
it.

**Where it comes from.** The probe makes the boot value at the first start with a valid rate, and
keeps it until it restarts. It takes it from the chip's hardware random number generator. If the
generator fails, the start fails with result 9. The boot value can be zero, but the counter never is
in a real identity, so no capture's identity is all zeros.

**The counter.** It starts at zero. Every start that succeeds, modes 0 and 5 included, adds one,
and publishes the new identity before the first conversion. The counter never wraps. Once it
reaches 2^64 − 1, new starts fail with result 9 until the probe restarts and makes a new boot
value. The capture already running carries on.

**How long it lasts.** A capture keeps its identity through pauses, holds, changes to the target
supply, and streams that attach and end. A stopped or failed capture keeps it too, with its total.
A start that fails publishes no identity. It can still pause or stop the running capture, as any
failed start can, and that capture keeps its identity. A probe restart clears the capture and its
identity, and the next start uses a new boot value.

**Who holds it.** An identity belongs to one probe, and names a capture, not a host. Any host can
read it, attach to the capture and stop it on purpose. A host holds an identity only when it got it
from its own start reply or stream header. Only an identity it holds may drive an automatic
cleanup. When the probe disconnects or enumerates again, the host drops the identity it holds,
until it starts or attaches again.

### Identity-aware requests

`id[24]` is an identity.

| Request | Bytes | Interfaces | What it does |
| --- | ---: | --- | --- |
| `[0x90, 8, rate:u32]` | 6 | CMSIS-DAP, Aux | Starts a new capture, as mode 0, and returns its identity |
| `[0x90, 9, rate:u32, dio_mask]` | 7 | Aux | Starts a new capture and a format 4 stream, and returns the capture's identity |
| `[0x90, 10, dio_mask]` | 3 | Aux | Attaches a format 4 stream to the running capture, as mode 6. Its header gives the identity |
| `[0x90, 11, id[24]]` | 26 | CMSIS-DAP, Aux | Stops the capture, only if the identity matches |
| `[0x90, 12]` | 2 | CMSIS-DAP, Aux | Reads the capture's state and identity, and changes nothing |
| `[0x90, 13, id[24]]` | 26 | Aux | Ends the stream, only if the capture's identity matches. The capture carries on |

On CMSIS-DAP, modes 9, 10 and 13 return `DAP_ERROR` and change nothing, as the other stream modes
do. A mode the probe does not know returns `DAP_ERROR` on either interface. A request that is too
short, or whose reply would not fit in the room left for it, is refused before it changes anything,
as any command is.

Each mode takes exactly the bytes the table gives. In a batch, the next command starts right after
them, and HID padding after a request is never read as parameters.

Modes 8 and 9 return 31 bytes:

| Offset | Field |
| --- | --- |
| 0 | `0x90` |
| 1 | `DAP_OK` |
| 2 | Result |
| 3 to 6 | The rate the timer runs at, to the nearest pair a second, u32 |
| 7 to 30 | The identity of the capture this start began |

The rate and the identity are the ones this start chose, not a later reading. When the result is
not 0, both are zero, and the start gives the host no identity.

Mode 10 returns `[0x90, DAP_OK, result]`, three bytes, as mode 6 does. It gives the host an identity
only when the stream's header arrives. Reading the state afterwards never does.

Modes 11 and 13 return `[0x90, DAP_OK, result]`, three bytes. Each compares the identity before it
changes anything: mode 11 before it pauses the ADC, mode 13 before it ends the stream. A mismatch
returns 8. It leaves the capture, the stream, the total, any fault and the identity as they were,
and sends no end message. An all-zero identity returns 8 too. A host can stop a matching identity
again and again. Mode 11 returns 0 if the capture had already stopped normally, or 7 after a
capture fault. Mode 13 returns 0 for a stream that has already ended.

Mode 12 returns 32 bytes:

| Offset | Field |
| --- | --- |
| 0 | `0x90` |
| 1 | `DAP_OK` |
| 2 | Result: 0 running, 1 stopped, 3 unavailable, 7 failed, 9 no identity |
| 3 | Running, 0 or 1. A capture paused or held for a moment still reports 1 |
| 4 to 7 | The rate the timer runs at, to the nearest pair a second, u32. Zero with any result but 0 |
| 8 to 31 | The capture's identity, running or kept. Zero before the first start that succeeds, or when there is none |

The state, rate and identity in one reply always describe the same capture. A reply never pairs one
capture's identity with the state of the capture that replaced it.

- A capture that has taken no pair yet reports result 0 with its identity, and mode 1 can still
  report no sample (2).
- Running is 1 only with result 0.
- A probe that has never started a capture reports result 1, running 0, rate 0 and the zero
  identity.
- If the first start fails on its rate or its calibration, the probe stays stopped.
- A capture fault, or a fault in controlling the capture, stays until a start succeeds. Until then
  mode 12 reports 7, with the zero identity if no start ever succeeded.

When the random number generator fails, every start tries it again, for up to about a second, so
result 9 lasts only until a try succeeds.

Mode 12 reports 9, with the zero identity, only when no identity exists yet. When the counter runs
out, the current identity stays readable, and its capture can still be stopped. Modes 8 and 9 check
the counter and the generator before they touch the running capture. The probe checks that energy
capture is there, and that the request is valid, before it compares identities. So a board without
energy capture returns 3, not 8.

Mode 2 stops whatever capture runs, whatever its identity. Captures started with modes 0 and 5 get
identities too.

A host that stops a capture on its own, when it exits for instance, holds the identity from its
start reply or the stream's header. It cleans up a capture it
started with mode 11, and a stream it attached to with mode 13. It never reads the current identity
with mode 12 and adopts it. That holds after result 8, and after a start reply that was lost or
malformed. The host then holds no identity, and the capture it finds may be another host's.

## Pro SPI: `0x91`

The Pro's DIO pins double as an SPI bus. SPI is there when `0x82` sets extended capability bit 3.

SPI is a vendor command like the others, not a separate USB-SIO endpoint. It runs on CMSIS-DAP's
bulk and HID interfaces, and the request and the reply must each fit that interface's packet. A
command on its own can send up to 504 bytes on bulk at high speed, or 56 on HID and on bulk at full
speed. A command in a `DAP_ExecuteCommands` batch has less room, because the batch's header and the
other commands and replies share the packet.

Send `[0x91, mode, clock_hz:u32, count:u16, tx[count]]`. Each request is one full-duplex transfer:
the probe sends `tx` and receives as many bytes, with SSEL0 asserted throughout. In `mode`, bit 0
sets CPOL, bit 1 sets CPHA, and bit 2 sends the least significant bit first. The other bits are
reserved.

The reply is `[0x91, result, count:u16, rx[count]]`. The count is zero on an error. A transfer of
no bytes is refused.

`clock_hz` can be from 1/4096 to 1/2 of the SPI's source clock. The source runs at 96 MHz, so the
probe takes about 23.4 kHz to 48 MHz. It runs the fastest clock it can make at or below
`clock_hz`: 96 MHz divided by a whole number.

The SPI pins are DIO0 to DIO3, so the command returns `BUSY` while any DIO pin is an output. During
a transfer the probe owns the pins, and it leaves them inputs afterwards.

## Pro I2C: `0x92`

I2C is there when `0x82` sets extended capability bit 4. It runs on the same path as SPI, with the
same limits on size. A command on its own can send up to 502 bytes or receive up to 508 on bulk at
high speed, and send 54 or receive 60 on HID and on bulk at full speed. A batch leaves less room, as
for SPI.

Send `[0x92, address, clock_hz:u32, tx_count:u16, rx_count:u16, tx[tx_count]]`. `address` is the
7-bit address. At least one count must be above zero. A write and then a read uses a repeated start
and one stop at the end. A read alone starts with the read. The bus clock must be from 10 kHz to
1 MHz.

The reply is `[0x92, result, rx_count:u16, rx[rx_count]]`. The count is zero on an error.

The I2C controller, Flexcomm 3, also drives SWO, so I2C returns `BUSY` while SWO is set up. Turn SWO
off before you use I2C. The probe leaves P0_4 and P0_21 without internal pull-ups, so fit pull-ups
to the bus voltage.

The results of both commands:

| Result | Meaning |
| --- | --- |
| 0 | `OK` |
| 1 | `UNAVAILABLE`: not a Pro, or J15 turned USB-SIO off as the probe booted |
| 2 | `INVALID` |
| 3 | `BUSY` |
| 4 | `TRANSFER_ERROR` |
| 5 | `NACK`, I2C only |

Both commands can go in a `DAP_ExecuteCommands` batch.

## Markers: `0x94`

A host debugging the target can mark a moment in an energy recording, such as a breakpoint hit or
the start of a test step. Markers are there when `0x82` sets extended capability bit 7.

Send `[0x94, number:u32, length, label[length]]`. The probe passes the number and the label on
unchanged, so they mean what the host wants. The label is up to 32 bytes, UTF-8 by convention, and
can be empty.

The reply is `[0x94, status, result]`:

| Result | Meaning |
| --- | --- |
| 0 | The marker is in the stream |
| 1 | No capture runs |
| 2 | A capture runs, but no host streams it |
| 3 | The probe had no room for the marker. The markers message that holds the stream's next marker says one was lost before it |

Only result 0 puts the marker anywhere. The status is `DAP_ERROR`, with no result, for a label
longer than 32 bytes, and on a board without energy capture. A request shorter than its length says
gets `[0xff]`, as any malformed command does. Verge Aux keeps the first 40 bytes of a request, so
there a label longer than 34 bytes gets `[0xff]` too.

The probe stamps the marker as it runs the command, on the clock that times the pairs and edges.
Inside a `DAP_ExecuteCommands` batch, the marker sits between the commands before and after it. So
a host can mark exactly where a sequence of debug operations starts or ends. The command works on
CMSIS-DAP and on Verge Aux. A marker sent through Verge Aux lands between two CMSIS-DAP commands.

The probe keeps 16 markers waiting to be sent. A marker that finds them full gets result 3.

## Reboot: vendor request `0x03` on endpoint 0

A host restarts the probe with a vendor control request to the device, not with a CMSIS-DAP
command. Send bmRequestType `0x40`, bRequest `0x03`, wIndex 0 and wLength 0. wValue 0 restarts
Verge, and wValue 1 starts the ROM bootloader. Any other value in those fields stalls. Once the
request is acknowledged, the probe leaves the bus for at least 20 ms, then resets.

The probe answers the request on the core that runs USB. So it works while a CMSIS-DAP command is
running, and when the jumpers leave no CMSIS-DAP interface.

## The auxiliary interface: Verge Aux

On the MCU-Link Pro, a second USB interface, Verge Aux, carries the DIO, supply, energy and marker
commands. A host can use it while a debugger holds the CMSIS-DAP interface.

| Field | Value |
| --- | --- |
| Interface class | `0xff`, subclass `0x00`, protocol `0x00` |
| Interface string | `Verge Aux` |
| Endpoints | EP5 OUT and EP5 IN, bulk, 512 bytes at high speed and 64 at full speed |
| Windows | WinUSB, DeviceInterfaceGUID `{A297EA18-39D8-41BE-B2E8-884CEC8FE819}` |

Every Pro has it, whatever its jumpers, and no other board does. It is the last interface. Its
number depends on which interfaces the jumpers leave before it, so find it by its class and string,
or on Windows by its GUID. Its string does not contain `CMSIS-DAP`, so debuggers pass it by.

**Requests and replies.** A request is one transfer on EP5 OUT, in the format of the CMSIS-DAP
command it names. Its reply is one transfer on EP5 IN, in the format of that command's reply on
CMSIS-DAP. Verge Aux carries `0x82`, `0x8E`, `0x8F`, `0x90` and `0x94`. Any other command gets
`[0xff]`, and so does a request too short for its command.

**Reading.** A stream message can be up to 512 bytes, so read EP5 IN with a 512-byte buffer. At
full speed a message can fill a whole number of 64-byte packets. The probe then ends it with a
zero-length packet.

**One at a time.** The probe takes one request at a time. The next one waits on USB until the probe
has answered the one before, so replies come in the order of the requests. The probe answers a
request on Verge Aux before its next CMSIS-DAP command. So a request waits at most for the command
running on CMSIS-DAP.

**Opening it.** The last program to use Verge Aux can leave a reply or a stream message unread.
Until it is read, the probe takes no request, and a program that sent a request first would take
it for its own reply. So a program that opens Verge Aux reads and drops whatever is waiting before
it sends anything. If a stream is still running, the program ends it with `0x90` mode 7.
No other program can hold the interface at the same time, so nothing else would read that stream,
and the probe would go on sending it.

**Shared state.** The two interfaces share one set of DIO pins, one target supply and one energy
capture. A start on either restarts the capture and clears the total, and reads on either see the
same capture.

## The energy stream

Through Verge Aux, a host can receive every pair the probe measures, with its time, to record how
the target's power changes. These `0x90` modes control the stream. They exist on Verge Aux only.
On CMSIS-DAP they answer `DAP_ERROR`.

| Request | What it does |
| --- | --- |
| `[0x90, 5, rate:u32, dio_mask]` | Starts a capture at `rate` pairs a second, as mode 0, and streams it from its first pair |
| `[0x90, 6, dio_mask]` | Streams the running capture, from its next pair. The capture and its total carry on |
| `[0x90, 7]` | Stops streaming. The capture carries on |

Modes 9, 10 and 13 do the same with capture identities ([Identity-aware
requests](#identity-aware-requests)).

Mode 5 replies as mode 0: `[0x90, DAP_OK, result, rate:u32]`. Modes 6 and 7 reply
`[0x90, DAP_OK, result]`. Mode 6 returns 1 when no capture runs. It returns 7 when the capture has
failed, or when the probe has no room for the stream's first message. The reply always comes
before the stream's first message.

`dio_mask` picks the DIO pins whose edges the stream reports, bit k for DIO k. Zero reports none.
The header says which pins the stream reports. It reports none when J15 turned the DIO pins off, or
when SPI has them as the stream starts.

A stream message starts with a byte from `0xa0` to `0xa5`. Those are outside the vendor command
range, so a host tells a message from a reply by its first byte. Messages come between replies,
never inside one.

A stream ends when a host sends mode 7, and when the capture stops, restarts or fails. Its last
message says which. After a restart, a host that wants the new capture sends mode 6.

### Header: `0xa1`

The first message of every stream. It holds everything a host needs to convert the pairs that
follow. Stream format 3 has 79 bytes, and stream format 4 has 103.

| Offset | Field |
| --- | --- |
| 0 | `0xa1` |
| 1 | The stream's format, 3 or 4 |
| 2 to 5 | The clock the probe times pairs and edges by, in Hz, u32 |
| 6 to 9 | The pair period, in counts of that clock, u32 |
| 10 | The current setting the jumpers pick: 1 for 350 mA, 2 for 50 mA, 0 unknown |
| 11 to 12 | The voltage's full scale in mV, u16 |
| 13 to 16 | The sequence number of the first pair the stream sends, u32 |
| 17 | The range of the pair before that one: 1 low, 0 high. At the start of a capture, 1 when it switches automatically and 0 when it holds the high range |
| 18 | Low range: the point count `n`, up to 4 |
| 19 to 42 | Low range: four points, lowest first, each a code (u16) and the current through the sense resistor in nA (u32). Points past `n` are zero |
| 43 | High range: the point count |
| 44 to 67 | High range: four points, as for the low range |
| 68 | The DIO pins that report edges, bit k for DIO k |
| 69 | Their levels when the stream started. Pins not reporting read 0 |
| 70 to 71 | The resolution of edge times in ns, u16. 0 when no pin reports |
| 72 to 73 | The range-switch weight `w`, u16, 0 to 32,768 ([Converting a pair](#converting-a-pair), step 5) |
| 74 | Range-switch flags. Bit 0: a pair that switched down keeps its own current. Bit 1: offsets 75 to 78 hold a current. Bit 2: the capture holds the high range (`0x90` mode 14) |
| 75 to 78 | The current through the sense resistor of the pair before the first one sent, from step 3 of [Converting a pair](#converting-a-pair), in nA, u32. 0 without flag bit 1 |

The capture's rate in pairs a second is the clock over the period. It is rarely a whole number:
asked for 7 pairs a second, the timer runs at 6.99999985. Time pairs and weigh them by the period
and the clock, never by a rounded rate.

The points are the ones `0x90` mode 4 reports.

Modes 5 and 6 send stream format 3, and modes 9 and 10 stream format 4. Stream format 4 keeps
offsets 0 to 71 as above, with 4 at offset 1. Offsets 72 to 95 hold the capture's 24-byte identity,
whose counter is never zero, and the fields at 72 to 78 above move to 96 to 102.

The header's identity, calibration, clock, period and first sequence number all describe the same
capture.
No other stream message carries the identity.

A mode 9 or 10 that succeeds starts a new stream. The probe drops the messages it had queued, and
the stream's header is the first stream message after the reply. One message already handed to USB
can still arrive before the reply, so a host drops anything it read before the reply. Mode 9's
header carries the same identity as its reply, and a host checks that it does. With mode 10, the
host holds an identity only once the header arrives. If the header never comes, the host holds
none, and it does not read the state to find one.

Mode 13 ends the stream as mode 7 does, with reason 1 in its end message, but only when the
identity matches. A restart ends it too, with reason 3.

A host holds the identity from its start reply or the stream's header so that it can clean up after
a failure. It drops the identity once it has cleaned up, or when the probe disconnects. An identity
read back from a saved recording, such as a verge capture file, belongs to that recording. Never
use it to stop or end a live capture.

### Pairs: `0xa0`

| Offset | Field |
| --- | --- |
| 0 | `0xa0` |
| 1 | Flags. Bit 0: the capture paused before this message's first pair. Bit 1: its conversions were held. The other bits are zero |
| 2 to 5 | The sequence number of this message's first pair, u32 |
| 6 to 13 | The time of this message's first pair, in ns since the capture started, u64 |
| 14 to 15 | The pair count `k`, u16, 1 to 99 |
| 16 on | `k` pairs of 5 bytes: the voltage code (u16), the current code (u16), then a byte. Its bit 0 is set on the low range. Its bit 1 is set on a high-range pair under range, whose reading is unreliable |

Pair i of a message, counting from 0, has the sequence number `first + i`. It was taken at
`time + i × period × 1,000,000,000 / clock` ns, with the header's period and clock. Its voltage and current were converted together from then,
averaged over the same stretch of at most one period. A message never spans a pause or a hold, so
its pairs are evenly spaced.

The probe pauses the capture for a moment when it reads the target's supply voltage, and the next
message sets flag bit 0. It holds its conversions while it measures a read latency, as when a
debugger connects at a fast clock or changes to one, and the next message sets bit 1. Either way,
the message's time shows how long the gap was. Only a pause ends a run of pairs for step 5 of
[Converting a pair](#converting-a-pair).

A pair read on the high range is under range when its current code is less than 64 codes above the
high range's no-load reading at the last calibration. Calibration keeps no point there, and the
amplifier's output stops at that reading, so a reading there is unreliable. Both range settings
mark these pairs. A capture that switches automatically reads some after the current falls, before
it switches down. Nothing is marked under range without a calibration. Low-range pairs are never
under range.

A message holds at most 99 pairs, which keeps it under 512 bytes, and less than 10 ms of them. The
probe sends it once it is full, or once its pairs reach 10 ms. Below about 230 pairs a second, a
message can wait longer for its pairs: up to 2.3 s at one pair a second.

### DIO edges: `0xa3`

| Offset | Field |
| --- | --- |
| 0 | `0xa3` |
| 1 | Flags, zero |
| 2 to 9 | The time of this message's first edge, in ns since the capture started, u64 |
| 10 to 11 | The edge count `e`, u16, 1 to 99 |
| 12 on | `e` edges of 5 bytes: the time after the first edge in ns (u32), then the levels after the edge, bit k for DIO k |

Edges come in time order. Each one carries the levels of every reporting pin, so a host knows them
all from any edge on. Pins not reporting read 0. Edge times share the pairs' clock, so edges and
pairs lie on one timeline.

The probe times an edge when its interrupt takes it, a little after the edge itself, to the
resolution the header gives. The interrupt takes every edge since it last ran, on every pin. A
pulse that starts and ends between two runs of the interrupt shows as two edges at one time. Two
pulses on one pin between two runs show as one. The levels always end where the pins are.

Each run of the interrupt costs the probe time it needs for USB. Past an average of 100,000 runs a
second, or 32 runs back to back, the probe stops taking edges for 1 to 2 ms, and says so with a DIO
lost message when it starts again. A pin toggling faster than that keeps about 20,000 to 35,000 of
its edges a second. The probe also stops for a moment when it holds 256 edges not yet sent.

A pin reports its level whatever drives it: the target, when the pin is an input, or the host's own
drive, when it is an output (`0x8E`). While SPI has the pins (`0x91`), the probe reports no edges.
A DIO lost message covers the transfer once SPI gives the pins back.

The probe starts timing a stream's edges a moment after its header. For modes 6 and 10 that is as
it answers the request, for mode 9 as the capture starts, and for mode 5 within a few milliseconds.
A pin that moves in that moment shows the move when timing starts.

All the edges and DIO lost messages up to a pairs message's last pair arrive before the next pairs
message, or before the end. So a host that holds back one message's pairs until the next one
arrives knows the levels across them. The probe sends edges with each pairs message, and on their
own once they have waited 10 ms.

### DIO lost: `0xa5`

| Offset | Field |
| --- | --- |
| 0 | `0xa5` |
| 1 | Flags, zero |
| 2 to 9 | The time from which edges may be missing, in ns since the capture started, u64 |
| 10 to 17 | The time the probe knew the levels again, in ns since the capture started, u64 |
| 18 | The levels from then on, bit k for DIO k. Pins not reporting read 0 |

The probe sends one whenever it lost edges: while its edge interrupt rested, while SPI had the pins,
and when the host fell behind. Between the two times the levels are unknown. From the second on
they are known again, so a stop shows even when no edge follows it.

The first time comes after every edge the stream has already sent, and before the second. For a
rest, it is the interrupt run that started the rest. For SPI, it is when SPI took the pins. For edges
the host fell behind on, it is just after the last run whose edges were sent, and the second time is
the last run dropped. Every edge sent after a DIO lost message comes after its second time, and so
does the next DIO lost message's first. Edges stamped together are sent or lost together. A stream
that ends while edges are being lost sends one before its end, up to when its next pair was due.

Edges and DIO lost messages come in time order. Their times convert from counts of the header's clock
as the pairs' and edges' do.

### Markers: `0xa4`

| Offset | Field |
| --- | --- |
| 0 | `0xa4` |
| 1 | Flags. Bit 0: markers were lost before this message's first. The other bits are zero |
| 2 | The marker count `m`, 1 to 11 |
| 3 on | `m` markers, each its time in ns since the capture started (u64), its number (u32), its label's length (u8), then the label |

Markers come in the order the probe ran them (`0x94`). Their times share the pairs' clock. A marker
can arrive before or after the pairs around it, so place it by its time. The probe sends markers
with each pairs message, and on their own once they have waited 10 ms. It sends only markers it ran
while the stream ran, from its header to its end.

### End: `0xa2`

| Offset | Field |
| --- | --- |
| 0 | `0xa2` |
| 1 | Why the stream ended: 1 a host ended it (mode 7, mode 13, or a new mode 6); 2 the capture stopped; 3 a new start restarted the capture; 4 the capture failed (result 7) |
| 2 to 5 | The sequence number of the last pair the capture took, u32 |

### When the host falls behind

The probe keeps 16 messages a host has not read yet. When they fill, it drops new pairs, edges and
markers messages until there is room again. It keeps the last two places for a header and an end.
The capture and its total carry on regardless. A host sees lost pairs as a jump in sequence
numbers, lost edges as a DIO lost message, and lost markers as bit 0 on the next markers message.

### Converting a pair

All the arithmetic is on integers. Shifts and divisions round toward zero. The probe's own
converter, in [`src/energy_pairs.h`](../src/energy_pairs.h) and
[`src/energy_samples.h`](../src/energy_samples.h), is the reference for this text. It writes
[`tools/energy_vectors.json`](../tools/energy_vectors.json), which a host's converter can check
itself against: samples and digests over whole ranges of the conversion, and runs of pairs with the
totals they give.

1. **Voltage.** `mv = (voltage_code × 2 × full_scale_mv) >> 16`. The 2 undoes the divider that
   halves the target's voltage on its way to the converter.
2. **Range.** The pair's bit 0 says which range's points convert the current.
3. **Current through the sense resistor.** The range's points make a table of straight segments.
   - Each segment's slope is `((I[j+1] − I[j]) << 16) / (code[j+1] − code[j])`, with `I` in nA.
   - The segment is the last `j` with `code ≥ code[j]`, among the first `n − 1` segments. A code
     below `code[0]` uses segment 0.
   - The current is `I[j] + (code − code[j]) × slope[j] / 65536`, in 64-bit signed arithmetic,
     then clamped to 0 to 2^32 − 1.
4. **The target's current.** The voltage divider draws current through the sense resistor too:
   `divider_na = (mv × 99,297) >> 16`. The target's current is the sense current less
   `divider_na`, or 0 if that would be negative.
5. **Range switches.** A pair converted while the range switched can mix readings from both ranges.
   So a pair whose range differs from the one before it may take another current from step 3 in
   place of its own. Step 4 still uses its own voltage.
   - A pair that switched up takes `(next × (65536 − w) + before × w + 32768) >> 16`, in 64-bit
     arithmetic. `next` is the next pair's current from step 3, `before` the previous pair's, and
     `w` the header's weight. Without a pair before it in the run, `w` counts as 0.
   - A pair that switched down keeps its own current if the header's flag bit 0 is set. Otherwise
     it takes the next pair's current.
   - If the next pair switched too, the first one keeps its own current.
   - A pair still waiting for the next one when the capture pauses keeps its own current: when the
     message after it sets flag bit 0, or when the stream ends. A hold, bit 1, does not end the
     run, and the pair before stays the pair before.
   - The header gives the range before the first pair a stream sends. With its flag bit 1, it also
     gives the step 3 current of the pair before it. After a pause, in a capture that switches
     automatically, the range before is the low range again, as at a start: the probe sets the
     range to low as it resumes. A capture that holds the high range stays on it. A pause, a start
     and lost pairs leave no pair before.

A host that wants the probe's total adds `mv × target_na` picowatts for each pair, scaled by
`floor(2^32 × period / clock)` in 32-bit fixed point, carrying the fraction from pair to pair. For a stream
started with mode 5, once the capture stops, that sum equals the total `0x90` mode 3 reports, as
long as no pairs were lost. A host that ignores the header's range-switch fields gets a total a
little apart from the probe's.

## Other vendor IDs

`0x80` and `0x81` are NXP's (mculink-stock-notes' `identity.md`). Verge answers neither, so a
LinkServer feature that rests on them does not work with Verge. Verge's own tools use a few more
IDs, whose bytes can change between builds ([Design](../DESIGN.md#vendor-commands)). Any other ID
from `0x80` to `0x9F` gets the invalid-command reply.
