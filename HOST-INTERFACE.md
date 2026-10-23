# Host interface

The firmware implements CMSIS-DAP for SWD, four-wire JTAG, two-pin cJTAG, SWO capture and target
serial ports. Standard commands follow CMSIS-DAP. This page covers the vendor commands that other
supported host tools use. Command and reply buffers include the command byte at index 0.

All multi-byte integer fields below are little-endian. A host should check reply lengths before
reading fields. An unknown command receives the CMSIS-DAP invalid-command reply.

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
Bit 1 advertises calibrated fields appended to the `0x90` latest reply. Bit 2 advertises accumulated
energy reads through `0x90` mode 3. Other extended bits are reserved.

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
fitted at startup.

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
startup. The host polls snapshots; the firmware does not stream samples.

When bit 2 is set, the firmware integrates every calibrated sample pair independently of host
polling. A successful start clears the accumulator. A failed start leaves the previous total intact.
Stop preserves the total for later reads. Pauses add no energy for time without samples.
The total resets when the probe restarts.

Voltage and current conversions use the highest ADC hardware averaging that sustains the effective
timer rate. The selector uses 128x through 1,900 pairs/s, 64x through 3,800, 32x through 7,600,
16x through 15,200, 8x through 29,197, 4x through 54,794, 2x through 97,560, then 1x through
100,000. The range-state conversion stays at 1x so gain changes appear in the next pair. The start
reply reports the timer's effective pair rate.

| Request | Action |
| --- | --- |
| `[0x90, 0, rate:u32LE]` | Start capture at the requested complete voltage/current pairs per second. |
| `[0x90, 1]` | Read the newest complete voltage/current pair. |
| `[0x90, 2]` | Stop capture. Repeated stop requests are safe. |
| `[0x90, 3]` | Read accumulated energy in nJ and the number of integrated pairs. |

Every recognized request returns `DAP_OK` followed by a result byte. Start replies with the
effective rate as a u32 after the result. Latest replies with voltage code (u16), current code
(u16), and sample sequence (u32) after the result. When extended-capability bit 1 is set, the reply
appends voltage in mV (u32), current in µA (i32), range ID (u8), and calibrated-valid (u8). The
full reply is 21 bytes including the command byte. If the result is not `OK`, every sample field is
zero. The sequence counts complete pairs since capture started, beginning at 1. Stop replies with
only the result.

Mode 3 returns `[0x90, DAP_OK, result, energy_nJ:u64LE, pairs:u64LE]` in 19 bytes. It reports
`STOPPED` before the first successful start and `NO_SAMPLE` until it integrates a calibrated pair.
The pair count includes zero-current samples. Negative calibrated current is clipped to zero.
Each pair contributes one sample period using the fixed effective capture rate. Time while capture
is paused does not contribute. Reads do not reset the total, which remains readable after stop.

Range IDs are 1 for 350 mA high gain, 2 for 350 mA low gain, 3 for 50 mA high gain, 4 for 50 mA
low gain, and 0 when the range is unknown. `calibrated-valid` is 1 when the firmware can convert
both readings to physical units. Voltage uses the probe's nominal 3.3 V ADC reference. Current
calibration supports the 350 mA and 50 mA jumper settings.

| Result | Meaning |
| --- | --- |
| 0 | OK |
| 1 | Capture is stopped; mode 3 also returns this before the first successful start |
| 2 | No complete sample yet; mode 3 has not integrated a pair yet |
| 3 | Energy capture is unavailable on this board |
| 4 | Requested rate is zero or above 100,000 pairs/s |
| 5 | Invalid argument |
| 6 | ADC calibration failed |
| 7 | DMA failed |

Raw ADC codes remain available in the reply prefix. Hosts can use the appended physical-unit fields
when `calibrated-valid` is 1.

## Other commands

Commands `0x84` through `0x8D` are implemented for board control, target measurements and
development instruments. They are not documented here as a stable host API. `0x80` and `0x81` are
not implemented by this firmware.
