# Wires and clocks

Verge speaks SWD, JTAG, and cJTAG (IEEE 1149.7) in four scan formats: OScan1, OScan2, OScan5 and
OScan6. NXP's firmware has no cJTAG.

## Hosts

Any CMSIS-DAP host drives SWD and JTAG, as it drives NXP's firmware.

cJTAG takes a host that knows it. The host selects a scan format with vendor command `0x95`
([Host interface](host-interface.md#cjtag-0x95-and-0x96)) and connects over JTAG, and the probe
frames in that format from the connection on. Then the host activates the target and switches it to
the same format. `verge read --protocol cjtag` reads target memory over cJTAG, through a probe-rs
branch that drives the commands proposed for CMSIS-DAP. A probe-rs release has no cJTAG for
CMSIS-DAP probes.

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
cycles, and a 4-cycle clock leaves no room for that. The DMA can delay a write's store too, so a few
write clocks take 4 cycles instead of 3.

## JTAG

The probe has to sample TDO after the target drives it, so a slow target lengthens the clock. The
probe measures that delay when a host connects, and again when the host changes the clock:

| the target's TDO delay, in core cycles | the clock |
| --- | --- |
| up to 8 (53 ns) | 30 MHz |
| 9 or 10 | 25 MHz |
| 11 | 21.4 MHz |
| 12 | 18.75 MHz |
| 13 | 16.7 MHz |
| 14 | 13.6 MHz |
| 15 or 16 | 12.5 MHz |

Past 16 cycles, each cycle of delay adds one core cycle to the clock. A CC1312R1 measures 7 after a
pin reset, and 10 right after a system reset.

On the Pro, energy capture can make these clocks up to two cycles longer while it runs: its DMA can
delay the probe's read of TDO by two cycles. At 6 or 7 cycles of delay, `DAP_Transfer` and
`DAP_TransferBlock` keep 30 MHz with a few clocks at 25 MHz, and `DAP_JTAG_Sequence` scans at
25 MHz. At 8 cycles both run at 21.4 MHz, and at 9 or 10 at 18.75 MHz.

## cJTAG

The wire carries packets at up to 50 MHz, and in OScan1 three packets make one JTAG clock. The probe
samples TDO one cycle past the delay it measures, and each cycle of delay adds one core cycle to the
frame. A CC1312R1 measures 7 in every format, so its OScan1 frame takes 19 core cycles: 7.9 MHz of
JTAG clock.

The other formats send fewer packets for some clocks:

- **OScan2** sends a clock outside Shift-DR and Shift-IR as a single packet. `tools/dap.py`, a host
  in mculink-verge that drives the probe directly, reads a CC1312R1 6% faster in OScan2 than in
  OScan1 at 12.5 MHz, 4% faster at 25 MHz, and level with it at 50 MHz.
- **OScan5** sends a clock in Shift-DR and Shift-IR as two packets, the last of them leaving with an
  escape. On a CC1312R1 that frame takes 16 core cycles: 9.4 MHz. dap.py reads a CC1312R1 27% faster
  in OScan5 than in OScan1 at 12.5 MHz, 18% faster at 25 MHz and 5% faster at 50 MHz, where USB
  starts to set the rate.
- **OScan6** sends a clock outside Shift as one packet, and a clock in Shift as OScan5 does. On a
  CC1312R1 it reads 10% faster than OScan5 at 12.5 MHz, and level with it from 25 MHz.

verge connects in OScan1 unless asked for another format (`verge read --cjtag-format`). Ask only
for a format the target has: the probe frames in the format the host selects, and neither verge nor
probe-rs reads the target to check. The target's documentation, or its CNFG0 register, says which
formats it has: [cJTAG targets](cjtag-targets.md#other-formats) says how to read it on TI's parts. A
target without the asked format has not been tried. [cJTAG](../CJTAG.md) has the wire in detail.

## Limits

The fastest each transport runs, Verge beside NXP's firmware V3.174, from runs between 2026-11-04
and 2026-11-14. Verge's figures hold for the current firmware. The [measurement
log](../MEASUREMENTS.md#where-the-limits-figures-come-from) says how and when each was taken.

| Transport | Verge | Verge's rate depends on | NXP's firmware V3.174 |
| --- | --- | --- | --- |
| SWD writes | 50 MHz | The Pro's energy capture: a few clocks at 37.5 MHz while it runs | 10.7 MHz |
| SWD reads | 37.5 MHz | The Pro's energy capture: 30 MHz while it runs | 10.0 MHz |
| JTAG (reads and writes) | 30 MHz | The target's TDO delay; the Pro's energy capture | 5.4 MHz, at a requested 10 MHz; fails from 12.5 MHz |
| cJTAG (reads and writes) | 50 MHz per wire packet; per JTAG clock in Shift, 7.9 MHz in OScan1 and OScan2, 9.4 MHz in OScan5 and OScan6 | The target's TDO delay | No cJTAG |
| Console (VCOM) | 30 Mbaud | Nothing | 4 Mbaud |
| SWO | 30 Mbaud, UART encoding only | Nothing | Not measured |
| USB | High speed, 512-byte packets, 3 commands held | The host's port: 64-byte packets on a full-speed one | 512-byte packets, 8 commands reported |

The cJTAG row is a CC1312R1's, which measures 7 cycles of delay. NXP's SWD figures are data-bit
rates at requests of 25 MHz and above, where they stop rising. At those requests its reads were not
always right: probe-rs read 64 KiB of flash right 8 times in 10 at 25 MHz and 9 in 10 at 50, and
queued commands failed at 50 MHz. At 10 and 20 MHz every read was right. NXP's JTAG clock is the
median of an uneven one: its 5th to 95th percentile periods were 140 and 233 ns. NXP's console
figure is the fastest both of the Pro's ports ran at once without loss. NXP's USB figures are what
`DAP_Info` reports; how many commands its firmware holds was not measured.

[Trace and serial ports](trace-and-serial.md) says how the console and SWO rates were reached.
mculink-bench's [measurements page](https://escapement-project.github.io/verge/measurements/)
compares throughput through the same probe-rs workload, on the same target, with NXP's firmware and
a SEGGER J-Link Ultra. Its figures come through probe-rs 17beb8e80, a build of the project's fork
with changes for speed that the probe-rs under verge does not have.

### Past NXP's limits

Some of these rates are past NXP's specifications. They passed the tests in [Trace and serial
ports](trace-and-serial.md). The host asks for the rate, so whoever runs the probe decides whether
to use them.

| Feature | NXP's limit | Verge past it |
| --- | --- | --- |
| Console | The UART is specified to 10 Mbit/s (LPC55S6x data sheet, 7.26.3.4) | Above 10 Mbaud: 12, 16, 19.2, 21.4, 25 and 30 Mbaud ran clean |
| Console | The UART's clock divider is specified to 100 MHz (UM11126, 4.5.49) | For a rate the 96 MHz clock cannot make within 3%, such as 10 Mbaud: 10, 21.4, 25 and 30 Mbaud ran clean with it at 150 MHz |
| SWO | The same UART, to 10 Mbit/s | Above 10 Mbaud: 12, 16, 24 and 30 Mbaud ran clean |
| SWO | The UART's clock divider is specified to 100 MHz (UM11126, 4.5.49) | Above 20.1 Mbaud, where nothing inside the divider's limit comes within 0.5% of the rate: 21.4, 24, 25 and 30 Mbaud ran clean with it at 107 to 150 MHz |
