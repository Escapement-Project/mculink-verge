# Wires and clocks

Verge speaks SWD, JTAG, and cJTAG (IEEE 1149.7) in four scan formats: OScan1, OScan2, OScan5 and
OScan6. NXP's firmware has no cJTAG.

## Hosts

Any CMSIS-DAP host drives SWD and JTAG, as it drives NXP's firmware.

cJTAG takes a host that knows it. The host switches the target to a scan format, then tells the
probe to frame that format, with vendor command `0x83` ([Host
interface](host-interface.md#cjtag-control-0x83)). It picks a format both the probe and the
target offer. `verge read --protocol cjtag` reads target memory over cJTAG today.

## From the clock asked for to the clock on the wire

The probe's core runs at 150 MHz, and every wire period is a whole number of its cycles. So the
probe rounds a requested clock down to the next step it can make: 30 MHz is 5 cycles, 18.75 MHz is
8. A request above the fastest clock gets the fastest. A request below 9 Hz gets `DAP_ERROR`, and
the clock stays as it was: its period would be more than 2^24 core cycles, longer than the probe's
transfer code can wait.

Every transfer runs on code the probe generates at run time, for the clock and the settings in use
([Code generation](../CODEGEN.md)).

## SWD

A write clock takes 3 core cycles and a read clock 4. So at a requested 50 MHz, a transfer's request
and write data run at 50 MHz, and its read data at 37.5 MHz. The target's reply delay slows neither.

On the Pro, energy capture slows reads. While a capture runs, a read clock takes at least 5 cycles
(30 MHz), or 6 for some reply delays. The capture's DMA can delay the probe's read of SWDIO by two
cycles, and a 4-cycle clock leaves no room for that.

## JTAG

The probe has to sample TDO after the target drives it, so a slow target lengthens the clock. The
probe measures that delay when a host connects:

| the target's TDO delay, in core cycles | the clock |
| --- | --- |
| up to 8 (53 ns) | 30 MHz |
| 9 or 10 | 25 MHz |
| 12 | 18.75 MHz |
| 16 | 12.5 MHz |

Past 10 cycles, each cycle of delay adds one core cycle to the clock. A CC1312R1 measures 7 after a
pin reset and 11 after a system reset.

On the Pro, energy capture can make these clocks up to two cycles longer while it runs: its DMA can
delay the probe's read of TDO by two cycles. Scans then run at 25 MHz at 6 or 7 cycles of delay,
21.4 MHz at 8 and 18.75 MHz at 9 or 10.

## cJTAG

The wire carries packets at up to 50 MHz, and in OScan1 three packets make one JTAG clock. The probe
samples TDO one cycle past the delay it measures, and each cycle of delay adds one core cycle to the
frame. A CC1312R1 measures 8 in OScan1, so its frame takes 19 core cycles: 7.9 MHz of JTAG clock
(the generator's frame lengths, 2026-10-26).

The other formats send fewer packets for some clocks:

- **OScan2** sends a clock outside Shift-DR and Shift-IR as a single packet. Through probe-rs that
  reads a CC1312R1 4 to 7% faster than OScan1.
- **OScan5** sends a clock in Shift-DR and Shift-IR as two packets, the last of them leaving with an
  escape. A CC1312R1 measures 7 there, and its frame takes 16 core cycles: 9.4 MHz. `tools/dap.py`,
  a host in mculink-verge that drives the probe directly, reads a CC1312R1 27% faster in OScan5 than
  in OScan1 at 12.5 MHz, 15% faster at 25 MHz and 6% faster at 50 MHz, where USB starts to set the
  rate.
- **OScan6** sends a clock outside Shift as one packet, and a clock in Shift as OScan5 does. On a
  CC1312R1 it reads level with OScan5 at 12.5 MHz, and slower from 25 MHz, where the target answers
  WAIT more often to the shorter frames.

The host picks one of the formats both the probe and the target offer. `verge cjtag --formats`
limits the formats the probe offers, until it restarts. It takes names or SCNFMT numbers:
`--formats oscan1,oscan5` or `9,13`. OScan1 stays offered whatever the list says. `--oscan1-only`
offers OScan1 only, which is the easiest to read on an analyser, and `--all` offers every format
again. [cJTAG](../CJTAG.md) has the wire in detail.

## Limits

The fastest each transport runs, Verge beside NXP's firmware. The figures were measured or reported
on 2026-10-20 and 2026-10-21, on older builds than the current one. The SWD rows are the build
measured beside NXP's V3.172 on 2026-10-20; mculink-bench's [notes for that
date](https://github.com/Escapement-Project/mculink-bench/blob/master/results/throughput/2026-10-20.md)
record the builds and the methods.

| Transport | Verge | How it was found | Verge's rate depends on | NXP's firmware V3.172 |
| --- | --- | --- | --- | --- |
| SWD writes | 50 MHz | Bench, 2026-10-20 | Nothing | 10.7 MHz |
| SWD reads | 37.5 MHz | Bench, 2026-10-20 | The Pro's energy capture: 30 MHz while it runs (bench, 2026-10-29) | 9.6 MHz |
| JTAG (reads and writes) | 30 MHz | Bench and firmware timing report | The target's TDO delay; the Pro's energy capture (emulator, 2026-10-29) | Not measured yet |
| cJTAG (reads and writes) | 50 MHz per wire packet; per JTAG clock in Shift, 7.9 MHz in OScan1 and OScan2, 9.4 MHz in OScan5 and OScan6 | Firmware timing report; depends on the target | The target's TDO delay | No cJTAG |
| Console (VCOM) | 19.2 Mbaud | Bench loopback | Nothing | 3 Mbaud or more; its limit not measured yet |
| SWO | 30 Mbaud, UART encoding only | Bench capture | Nothing | Not measured yet |
| USB | High speed, 512-byte packets, 3 commands held | Firmware report | The host's port: 64-byte packets on a full-speed one (bench, 2026-11-03) | 512-byte packets, 8 commands reported |

NXP's SWD figures are measured data-bit rates at requests of 25 MHz and above, where they stop
increasing. At a requested 10 MHz, NXP's firmware reads at 6.4 MHz. From a requested 25 MHz it
misreads some acknowledgements, and queued commands fail at 50 MHz. Requests up to 20 MHz passed,
with data rates of 9.1 MHz for reads and 9.4 MHz for writes. NXP's console figure is the fastest
tested with both Pro ports running. NXP's USB figures are what `DAP_Info` reports; we did not
measure how many commands its firmware holds.

[Measurements](../MEASUREMENTS.md#where-the-limits-figures-come-from) says where each figure comes
from, and [Trace and serial ports](trace-and-serial.md) how the console and SWO figures were
reached. mculink-bench's
[measurements page](https://escapement-project.github.io/verge/measurements/) compares throughput
through the same probe-rs workload, on the same target, with NXP's firmware and a SEGGER J-Link
Ultra.

### Past NXP's limits

Some of these rates are past NXP's specifications. They passed the tests in [Trace and serial
ports](trace-and-serial.md). The host asks for the rate, so whoever runs the probe decides whether
to use them.

| Feature | NXP's limit | Verge past it |
| --- | --- | --- |
| Console | The UART is specified to 10 Mbit/s (LPC55S6x data sheet, 7.26.3.4) | Above 10 Mbaud: 12, 16 and 19.2 Mbaud ran clean |
| SWO | The same UART, to 10 Mbit/s | Above 10 Mbaud: 12, 16, 24 and 30 Mbaud ran clean |
| SWO | The UART's clock divider is specified to 100 MHz (UM11126, 4.5.49) | Above 20.1 Mbaud, where nothing inside the divider's limit comes within 0.5% of the rate: 21.4, 24, 25 and 30 Mbaud ran clean with it at 107 to 150 MHz |
