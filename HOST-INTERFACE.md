# Host interface

The firmware implements CMSIS-DAP for SWD, four-wire JTAG, two-pin cJTAG, SWO capture and target
serial ports. Standard commands follow CMSIS-DAP. This page covers the vendor commands that other
supported host tools use. Command and reply buffers include the command byte at index 0.

All multi-byte integer fields below are little-endian. A host should check reply lengths before
reading fields. An unknown command receives the CMSIS-DAP invalid-command reply.

The firmware validates each command's input and reply size before execution. A truncated command
or a reply that cannot fit receives `0xff`. SPI and I2C retain their four-byte invalid-argument
reply when it fits. Read blocks are shortened to the available reply space; SWO reads return at
most that space. Hosts should size requests to the transport's packet size.

If a batch contains a malformed command or runs out of reply space, execution stops before that
command. The reply retains the preceding subreplies and reports the number of commands executed.
A command that executes and returns an error still counts. This is the firmware's defensive
handling of malformed batches; it does not add nested batches or queued commands.

## Identify firmware: `0x82`

Send `[0x82]`. The reply contains:

| Offset from reply byte 1 | Field | Added in format |
| --- | --- | --- |
| 0–4 | ASCII `CJTAG` | 1 |
| 5 | Format version | 1 |
| 6 | Capability flags | 1 |
| 7 | Build string length `n` | 1 |
| 8 through `7+n` | `n` bytes of build string, without a terminator | 1 |
| `8+n` | Board kind: 0 full MCU-Link, 1 MCU-Link Pro, 2 on-board MCU-Link | 3 |
| `9+n` through `12+n` | Offered cJTAG formats as a u32 bit mask, bit `k` for SCNFMT `k` | 4 |
| `13+n` through `16+n` | Extended capabilities as a u32 bit mask | 6 |

The current capability flag bits are: bit 0 OScan1 support; bit 1 cJTAG framing active; bit 2
target-sense command support; bit 3 generated SWD paths; bit 4 host-driven OScan1 entry; bit 5
generated JTAG paths; bit 6 Pro DIO available; bit 7 Pro target-supply control (added in format 5).
Bits 0 and 4 are clear on the unsupported on-board variant. Bit 6 is clear if the Pro's J15 jumper
disabled USB-SIO when the probe started.

Format 6 extended-capability bit 0 advertises energy capture on Pro units with HW_VER_5 fitted.
Bit 2 advertises accumulated energy reads through `0x90` mode 3. Bits 3 and 4 advertise SPI and I2C
bridges on Pro units when J15 leaves USB-SIO enabled. Bit 5 advertises calibrated fields appended to
the `0x90` latest reply. Bit 6 advertises `0x8E` on Verge Aux, and is set with capability flag
bit 6. Bit 1 advertised the same fields as bit 5 with the current in µA; firmware no longer sets it.
Other extended bits are reserved.

Fields are appended in later formats. Use the format version before reading fields added by a
later format. Ignore fields you do not understand. The format 4 and later cJTAG mask reports what
the firmware offers; it does not prove that the target supports those formats.

## cJTAG control: `0x83`

The command starts with `[0x83, mode]`. Supported modes are:

| Mode | Extra request bytes | Action |
| --- | --- | --- |
| 0 | None | Stop two-pin framing and return to four-wire framing. |
| 2 | None | Start OScan1 framing after the host has activated OScan1 on the target. |
| 3 | `[format, tap_state]` | Start framing for the selected SCNFMT and TAP state after the host has switched the target. |
| 4 | Four-byte little-endian format mask; bit `k` selects SCNFMT `k` | Change the formats offered to hosts until the probe restarts. OScan1 remains offered. |

The reply is `[0x83, status]`, where `DAP_OK` is `0x00` and `DAP_ERROR` is `0xff`. Mode 1 is not
defined. Starting cJTAG on the unsupported on-board variant returns `DAP_ERROR`. A host must
switch the target and probe together; this command does not change the target's scan format.

## Pro DIO: `0x8E`

Send `[0x8E, mask, outputs, levels]`. Bit `k` names DIO pin `k`. Within `mask`, `outputs` sets
direction (`1` means output) and `levels` sets the output value. Bits outside `mask` are unchanged.
A zero mask reads the current state.

The reply is `[0x8E, status, outputs, levels, read, pins, group_count, groups...]`. `read` is the
sampled pin level; an output reads its own drive. `pins` is `0x0f` on a Pro with DIO available and
zero otherwise. Each group byte is a mask of pins that must change direction together. The Pro
reports groups `0x07` (DIO0–2) and `0x08` (DIO3).

An invalid pin, unavailable DIO, or direction change that splits a group returns `DAP_ERROR`
(`0xff`) and leaves the state unchanged. `DAP_OK` is `0x00`. The reply still reports the current
state. The state persists across
debug sessions and hosts, and resets to inputs when the probe restarts. J15 disables DIO when
fitted at startup. Verge Aux carries the same command on the same pins, so a host can use them while
a debugger holds CMSIS-DAP (extended capability bit 6).

The `verge dio` command is the supported user interface for these pins. It checks group changes
before sending them and shows the reply state.

## Pro target supply: `0x8F`

Send `[0x8F, mode]`. Mode 0 turns the regulator off, mode 1 turns it on, and mode 2 reads the
state. The reply is `[0x8F, status, enabled]`, where `enabled` is the state last set since the probe
started. Firmware drives the regulator off during startup.

This command is available only on the Pro. Other boards and unknown modes return `DAP_ERROR`
(`0xff`) without changing the output. `DAP_OK` is `0x00`.

J5 selects 1.8 V when shorted and 3.3 V when open. J6 must connect the regulator output to the
target. The regulator is rated for up to 400 mA. Read the voltage at J6 pin 1 and target-detect
state separately with `0x86`; the `0x8F` reply confirms the control state, not the voltage at the
target. After enabling the supply, allow the regulator and target rail to settle before accessing
the target.

## Pro energy capture: `0x90`

Energy capture is available when `0x82` format 6 sets extended-capability bit 0. It is off at
startup. On CMSIS-DAP the host polls snapshots; Verge Aux also streams every pair ("The energy stream").

When bit 2 is set, the firmware integrates every calibrated sample pair independently of host
polling. A successful start clears the accumulator. A failed start leaves the previous total intact.
Stop preserves the total for later reads. Pauses add no energy for time without samples.
Stop and pause include complete samples in the unfinished DMA segment and ADC FIFO. A partial
voltage/current/range tuple is discarded. Internal pauses preserve the latest-sample sequence.
The total resets when the probe restarts.

Voltage and current conversions use the highest ADC hardware averaging that sustains the effective
timer rate. The selector uses 128x through 1,794 pairs/s, 64x through 3,575, 32x through 7,096,
16x through 13,969, 8x through 27,088, 4x through 51,063, 2x through 91,603, then 1x through
100,000. The range-state conversion stays at 1x so gain changes appear in the next pair. The start
reply reports the timer's effective pair rate.

| Request | Action |
| --- | --- |
| `[0x90, 0, rate:u32LE]` | Start capture at the requested complete voltage/current pairs per second. |
| `[0x90, 1]` | Read the newest complete voltage/current pair. |
| `[0x90, 2]` | Stop capture. Repeated stop requests are safe. |
| `[0x90, 3]` | Read accumulated energy in nJ and the number of integrated pairs. |
| `[0x90, 4, range]` | Read the calibration points of the low range (0) or the high range (1). |

Every recognized request returns `DAP_OK` followed by a result byte. Start replies with the
effective rate as a u32 after the result. Latest replies with voltage code (u16), current code
(u16), and sample sequence (u32) after the result. When extended-capability bit 5 is set, the reply
appends voltage in mV (u32), current in nA (i32), range ID (u8), and calibrated-valid (u8). The
full reply is 21 bytes including the command byte. If the result is not `OK`, every sample field is
zero. The sequence counts complete pairs since capture started, beginning at 1, and never goes back
within a capture. Stop replies with only the result.

Mode 3 returns `[0x90, DAP_OK, result, energy_nJ:u64LE, pairs:u64LE]` in 19 bytes. It reports
`STOPPED` before the first successful start and `NO_SAMPLE` until it integrates a calibrated pair.
The pair count includes zero-current samples. Negative calibrated current is clipped to zero.
Each pair contributes one sample period using the fixed effective capture rate. Time while capture
is paused does not contribute. Neither does a read-latency measurement, which holds the conversion
trigger: at a connection at fast clocks, again when a measurement contradicts itself, and on vendor
`0x89` or `0x8A` mode 1. Reads do not reset the total, which remains readable after stop.

Mode 4 returns `[0x90, DAP_OK, result, count, points]` in 28 bytes. `points` is four entries of an
ADC code (u16) and the current through the sense resistor at that code in nA (u32), lowest first;
entries past `count` are zero. `count` is 0 until the probe has calibrated. The result is `OK` when
the latest calibration succeeded and 6 when it failed, with the points it measured before failing.
Firmware without mode 4 answers `DAP_ERROR`.

Range IDs are 1 for 350 mA high gain, 2 for 350 mA low gain, 3 for 50 mA high gain, 4 for 50 mA
low gain, and 0 when the range is unknown. `calibrated-valid` is 1 when the firmware can convert
both readings to physical units. Voltage and current are scaled to the Pro's 3.0 V reference.
Current calibration supports the 350 mA and 50 mA jumper settings. The current is the target's:
the firmware takes off what the probe's own voltage divider draws through the sense resistor.

The probe calibrates as it boots, before it answers the first command, and a start uses that
calibration. A start calibrates again only when the last calibration failed, or when the setting
J18 selects (CURR_RANGE_SET) differs from the one calibrated. A start that cannot read the setting
fails with 6 and keeps the calibration. Calibration disconnects J9 from the target for about
0.2 s, so a target powered through J9 loses its supply for that time. Moving the jumpers away and
back to the same setting goes unseen; reset the probe to calibrate again. PRO.md, "Energy
measurement", has the details.

| Result | Meaning |
| --- | --- |
| 0 | OK |
| 1 | Capture is stopped; mode 3 also returns this before the first successful start |
| 2 | No complete sample yet; mode 3 has not integrated a pair yet |
| 3 | Energy capture is unavailable on this board |
| 4 | Requested rate is zero or above 100,000 pairs/s |
| 5 | Invalid argument |
| 6 | Calibration failed: the ADC did not answer, or a range kept fewer than two usable points |
| 7 | DMA failed or the ADC FIFO overflowed |

Result 7 persists after stop and failed starts. The firmware retains the recorded total and stops
integrating when it detects the fault. A successful new start clears the fault and the total.
The retained total does not account for lost samples.

Raw ADC codes remain available in the reply prefix. Hosts can use the appended physical-unit fields
when `calibrated-valid` is 1.

## Pro SPI: `0x91`

SPI is available when `0x82` format 6 sets extended-capability bit 3. It uses the CMSIS-DAP vendor
command path, not a separate USB-SIO endpoint. The command runs on CMSIS-DAP bulk and HID transports;
the request and reply must each fit that transport's packet size. A standalone command allows up to
504 TX bytes on bulk or 56 on HID. A `DAP_ExecuteCommands` batch has less room for each command
because its header and neighboring requests or replies share the packet.

Send `[0x91, mode, clock_hz:u32LE, count:u16LE, tx[count]]`. Mode bit 0 sets CPOL, bit 1 sets CPHA,
and bit 2 selects LSB-first. Other mode bits are reserved. Each request is one full-duplex transfer
with SSEL0 asserted for the whole transfer. The SPI pins are also DIO0–DIO3, so the request returns
`BUSY` while any DIO pin is configured as an output. During SPI, the firmware owns the DIO pins and
leaves them as inputs afterward.

The reply is `[0x91, result, count:u16LE, rx[count]]`. The count is zero on errors. The firmware
accepts `clock_hz` from 1/4096 through 1/2 of the SPI source clock. Its current 96 MHz source gives
an accepted request range of about 23.4 kHz to 48 MHz. The divider quantizes the actual SCK rate.
A zero transfer length is rejected.

## Pro I2C: `0x92`

I2C is available when `0x82` format 6 sets extended-capability bit 4. It uses the same CMSIS-DAP
vendor command path and packet-size limits as SPI. A standalone command allows up to 502 TX bytes or
508 RX bytes on bulk, and 54 TX bytes or 60 RX bytes on HID. Batches have less room because their
header and neighboring requests or replies share the packet.

Send `[0x92, address, clock_hz:u32LE, tx_count:u16LE, rx_count:u16LE, tx[tx_count]]`. `address` is
7-bit. At least one count must be nonzero. A write followed by a read uses a repeated start and one
final stop. A read-only request starts with a read. The bus rate must be from 10 kHz through 1 MHz.

The reply is `[0x92, result, rx_count:u16LE, rx[rx_count]]`. The count is zero on errors. Flexcomm 3
also drives SWO, so I2C returns `BUSY` while SWO is configured. Turn SWO off before using I2C. The
firmware leaves P0_4 and P0_21 without internal pulls; provide external pull-ups to the bus voltage.

For both commands, result values are `OK` (0), `UNAVAILABLE` (1), `INVALID` (2), `BUSY` (3),
`TRANSFER_ERROR` (4), and `NACK` (5; I2C only). `UNAVAILABLE` means this is not a Pro or J15 disabled
USB-SIO at startup. These commands may be included in `DAP_ExecuteCommands` batches.

## The auxiliary interface: Verge Aux

On the MCU-Link Pro, the DIO, supply and energy commands are also on a second USB interface,
Verge Aux. A host can use it while a debugger holds the CMSIS-DAP interface.

| Field | Value |
| --- | --- |
| Interface class | `0xff`, subclass `0x00`, protocol `0x00` |
| Interface string | `Verge Aux` |
| Endpoints | EP5 OUT and EP5 IN, bulk, 512 bytes |
| Windows | WinUSB, DeviceInterfaceGUID `{A297EA18-39D8-41BE-B2E8-884CEC8FE819}` |

Every Pro has it, whatever its jumpers, and no other board does. Its interface number depends on
which other interfaces the jumpers leave, so find it by class and string, or on Windows by the GUID.
The string doesn't contain `CMSIS-DAP`, so debuggers pass it by.

A request is one transfer on EP5 OUT, in the format of the CMSIS-DAP command it names. The reply is
one transfer on EP5 IN, in the format of that command's reply on CMSIS-DAP. The interface carries
`0x82`, `0x8E`, `0x8F` and `0x90`. Any other command gets `[0xff]`, and so does a request too short
for its command. Firmware from before `0x8E` was carried leaves extended capability bit 6 clear and
answers it with `[0xff]`.

The probe takes one request at a time. The next one waits on USB until the probe has answered the
one before, so replies come in request order. A program that opens the interface reads and drops
whatever is waiting first: a reply or a stream message meant for the program before it. If a stream
is still running, the program ends it with mode 7. No other program can hold the interface, so
nothing else would read that stream, and the probe keeps sending it. The probe answers an aux
request before its next CMSIS-DAP command, so a request waits for at most the command running on
CMSIS-DAP.

The two interfaces share one set of DIO pins, one target supply and one energy capture. A start on
either restarts the capture and clears the total, and reads on either see the same capture.

## The energy stream

Through Verge Aux, a host can receive every pair the probe measures, with its time, so it can record
a power profile. Three `0x90` modes control the stream. They exist only on Verge Aux; on CMSIS-DAP
they answer `DAP_ERROR`.

| Request | Action |
| --- | --- |
| `[0x90, 5, rate:u32, dio_mask]` | Start a capture at `rate` pairs a second, as mode 0, and stream it from its first pair. |
| `[0x90, 6, dio_mask]` | Stream the capture that is running, from its next pair. The capture and its total carry on. |
| `[0x90, 7]` | Stop streaming. The capture carries on. |

Mode 5 replies as mode 0: `[0x90, DAP_OK, result, effective_rate:u32]`. Modes 6 and 7 reply
`[0x90, DAP_OK, result]`. Mode 6 returns 1 when no capture runs, and 7 when the capture has
failed or the probe has no room for the stream's first message. The reply always comes before the
stream's first message.

`dio_mask` picks the DIO pins whose edges the stream reports, bit `k` for DIO`k`. Zero reports
none. The header says which pins the stream reports: none when J15 turns the DIO pins off, or when
SPI has them at the start.

A stream message starts with a byte from `0xa0` to `0xa3`, outside the vendor command range, so a
host tells it from a reply by its first byte. Messages come between replies, never inside one.

A stream ends when a host sends mode 7, and when the capture stops, restarts or fails. Its last
message says which. After a restart, a host that wants the new capture sends mode 6.

### Header: `0xa1`

The first message of every stream. Everything a host needs to convert the pairs that follow is in
it. 68 bytes.

| Offset | Field |
| --- | --- |
| 0 | `0xa1` |
| 1 | Stream format version, 1 |
| 2–5 | Effective rate, pairs a second, u32 |
| 6 | Jumper setting: 1 for 350 mA, 2 for 50 mA, 0 unknown |
| 7–8 | Voltage full scale in mV, u16 |
| 9–12 | Sequence number of the first pair the stream sends, u32 |
| 13 | Range of the pair before that one: 1 low, 0 high; 1 at the start of a capture |
| 14 | Low range: point count `n`, up to 4 |
| 15–38 | Low range: four points of code (u16) then current through the sense resistor in nA (u32), lowest first; points past `n` are zero |
| 39 | High range: point count |
| 40–63 | High range: four points, as for the low range |
| 64 | DIO pins reporting edges, bit `k` for DIO`k` |
| 65 | Their levels when the stream started; pins not reporting read 0 |
| 66–67 | Edge time resolution in ns, u16; 0 when no pin reports |

The points are the ones `0x90` mode 4 reports.

### Pairs: `0xa0`

| Offset | Field |
| --- | --- |
| 0 | `0xa0` |
| 1 | Flags: bit 0, the capture paused before this message's first pair; bit 1, its conversions were held; other bits zero |
| 2–5 | Sequence number of this message's first pair, u32 |
| 6–13 | Time of this message's first pair, in ns since the capture started, u64 |
| 14–15 | Pair count `k`, u16, 1 to 99 |
| 16 onward | `k` pairs of 5 bytes each: voltage code (u16), current code (u16), then a byte with bit 0 set on the low range |

Pair `i` of a message (from 0) has sequence number `first + i` and was taken at
`time + i × 1,000,000,000 / rate` ns. That is when its voltage was converted. Its current was
converted half a period later, so a host lining currents up with DIO edges adds
`500,000,000 / rate` ns. A message never spans a pause or a hold, so its pairs are
evenly spaced. The probe pauses the capture for a moment when it reads the target's supply voltage,
and the next message sets flag bit 0. It holds the conversions while it measures a read latency, as a
debugger connects at a fast clock, and the next message sets bit 1. Either way the message's time
shows how long it took. Only a pause ends the pairs' run for step 5 below.

A message holds at most 99 pairs and less than 10 ms of them, so it fits one 512-byte packet. The
probe sends it once it is full, or once the pairs it has taken in reach 10 ms. It takes pairs in a
batch every 10 ms or so; below about 100 pairs a second a batch takes longer, up to 1.8 s at one
pair a second, and so does a message.

### DIO edges: `0xa3`

| Offset | Field |
| --- | --- |
| 0 | `0xa3` |
| 1 | Flags: bit 0, edges were lost before this message's first edge; other bits zero |
| 2–9 | Time of this message's first edge, in ns since the capture started, u64 |
| 10–11 | Edge count `e`, u16, 1 to 99 |
| 12 onward | `e` edges of 5 bytes each: time after the first edge in ns (u32), then the levels after the edge, bit `k` for DIO`k` |

Edges come in time order. Each one carries the levels of every reporting pin, so a host knows them
all from any edge on. Pins not reporting read 0. Edge times share the pairs' clock, so edges and
pairs lie on one timeline. The probe times an edge when its interrupt takes it, a little after the
edge itself, to the resolution the header gives.

A pin reports its level whatever drives it: the target as an input, or the host's own drive as an
output (`0x8E`). While SPI has the pins (`0x91`), the probe reports no edges, and the next edges
message sets bit 0.

The interrupt takes every edge since it last ran, on every pin. A pulse shorter than that shows as
two edges at one time. Two pulses on one pin before the interrupt runs show as one. The levels
always end where the pins are.

The probe starts timing a stream's edges a moment after its header: as the request is answered for
mode 6, and within a few milliseconds for mode 5. A pin that moves in that moment shows the move
when timing starts.

All the edges up to a pairs message's last pair arrive before the next pairs message, or before the
end. A host that holds back the pairs of one message until the next arrives knows the levels across
them. The probe sends edges with each pairs message, and on their own once they have waited 10 ms.

Each run of the interrupt costs the probe time it needs for USB. Past an average of 100,000 runs a
second, or 32 runs back to back, the probe stops taking edges for 1 to 2 ms and the next edges
message sets bit 0. A pin toggling faster than that keeps about 20,000 to 35,000 of its edges a
second.

### End: `0xa2`

| Offset | Field |
| --- | --- |
| 0 | `0xa2` |
| 1 | Why: 1 mode 7; 2 the capture stopped; 3 a new start restarted the capture; 4 the capture failed (result 7) |
| 2–5 | Sequence number of the last pair the capture took, u32 |

### When the host falls behind

The probe keeps 16 messages a host has not read yet. When they fill, it drops new pairs and edges
messages until there is room again, and keeps the last two places for a header and an end. The
capture and its total carry on regardless. A host sees lost pairs as a jump in sequence numbers,
and lost edges as bit 0 on the next edges message.

### Converting a pair

All arithmetic is on integers. Shifts and divisions round toward zero. The firmware's converter in
`src/energy_pairs.h` and `src/energy_samples.h` is the reference for this text. It writes
`tools/energy_vectors.json`, which a host converter can check itself against: samples and whole-range
digests of the conversion, and pair sequences with the totals they give.

1. **Voltage.** `mv = (voltage_code × 2 × full_scale_mv) >> 16`. The 2 undoes the divider that halves
   the target-side voltage on its way to the converter.
2. **Range.** The pair's bit 0 says which range's table converts the current.
3. **Current through the sense resistor.** The range's points make a piecewise-linear table.
   - Each segment's slope is `((I[j+1] − I[j]) << 16) / (code[j+1] − code[j])`, with `I` in nA.
   - The segment is the last `j` with `code ≥ code[j]`, among the first `n − 1` segments; a code
     below `code[0]` uses segment 0.
   - The current is `I[j] + (code − code[j]) × slope[j] / 65536`, in 64-bit signed arithmetic, then
     clamped to 0 to 2^32 − 1.
4. **The target's current.** The voltage divider draws current through the sense resistor too.
   `divider_na = (mv × 99,297) >> 16`. The target's current is the sense current less `divider_na`,
   or 0 if that would be negative.
5. **Range switches.** A pair converted while the range switched can mix readings from both ranges.
   So a pair whose range differs from the one before it uses the next pair's current from step 3
   in place of its own. Step 4 still uses its own voltage.
   - If the next pair switched too, the first keeps its own current.
   - A pair still waiting for the next one when the capture pauses keeps its own current: when the
     message after it sets flag bit 0, or when the stream ends. A hold, bit 1, does not end the run.
   - The header gives the range before the first pair a stream sends. After a pause the range
     before is the low range again, as at a start: the probe sets the range to low as it resumes.

A host that wants the probe's total adds `mv × target_na` picowatts for each pair, scaled by
`floor(2^32 / rate)` in 32-bit fixed point, carrying the fraction from pair to pair. For a stream
started with mode 5, once the capture stops, that sum equals the total `0x90` mode 3 reports,
provided no pairs were lost.

## Other commands

Commands `0x84` through `0x8D` are implemented for board control, target measurements and
development instruments. They are not documented here as a stable host API. `0x80` and `0x81` are
not implemented by this firmware. Command `0x90` handles energy capture; `0x91` and `0x92` provide
the Pro SPI and I2C bridges.
