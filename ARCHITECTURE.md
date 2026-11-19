# Architecture

The LPC55S69 has two Cortex-M33 cores. In this firmware CPU 0 talks to the host and CPU 1 talks to
the target. This document says what each core runs, where its code and data live, and the path
each USB interface takes to the code that serves it.

BUSES.md says which bus port each access uses, and why the other core cannot move a wire edge.
STACK.md covers the stacks, docs/host-interface.md the protocol a host sees, and BOARDS.md the
pins.

![The host's USB reaches CPU 0, which answers the host. CPU 1 drives the target. Between them are the mailbox each raises in the other and the memory both use: commands, replies, staged copies and stream messages. CPU 1 drives the target's SWD, JTAG and cJTAG wires, its DIO, SPI and I2C pins and its supply, and takes SWO in from it. CPU 0's consoles reach the target's serial lines.](docs/figures/architecture-blocks.svg)

## What it is built from

TinyUSB's `mcu_link` board support does the board and USB work: the gcc toolchain file, startup
code, linker script, clock setup and the LPC55's `lpc_ip3511` device driver, all maintained
upstream. ARM's CMSIS-DAP sources compile as they ship, with no patches. Their one accommodation is
`-Wno-unused-variable` on their files, since `DAP.c` leaves a variable unused when SWO is configured
out and TinyUSB's example flags are `-Werror`. The code that is the project's own is board and
protocol specific: the pin layer, the bit paths and the generators that write them at runtime, the
cJTAG framing, the USB descriptors, and the command path between the two cores.

The MS OS 2.0 descriptor set is built at boot rather than written by hand. It has nested length
fields that must agree with the bytes after them. Nothing reports a mismatch at runtime: the device
fails to bind. Which vendor interfaces it binds, and at which interface numbers, follows the
jumpers. `tools/ms_os_20_test.c` checks every length field under sanitizers, and
`tools/image.py` checks each strap state's set against its configuration.

## The two cores

CPU 1 runs CMSIS-DAP. The commands a host sends, on CMSIS-DAP or on Verge Aux, run there, and so
does the code that drives the wire. Two exceptions stay on CPU 0: `DAP_TransferAbort`, which CPU 0
passes straight to the transfer running, and the four Verge Aux requests that attach a stream to an
energy capture or end it. CPU 1 takes no interrupts. It waits for work by polling a mailbox
register that CPU 0 raises, so nothing on its own core can land in the middle of a transfer.

CPU 0 runs everything that has to answer when something happens: TinyUSB and every endpoint, the
consoles, the DMA interrupt, the LEDs and the energy stream. It never waits on CPU 1 for long: its
only waits are two short spins with a bound, around the energy stream. A CPU 1 that has stopped
answering can still be sent to the bootloader over USB, because CPU 0 answers that request.

| work | core | source |
| --- | --- | --- |
| USB: TinyUSB, the descriptors, every endpoint | CPU 0 | `main.c`, `usb_descriptors.c` |
| Receiving commands and sending replies | CPU 0 | `main.c` |
| `DAP_TransferAbort`: setting CPU 1's abort flag | CPU 0 | `main.c` |
| Running CMSIS-DAP commands, vendor commands included | CPU 1 | `core1_main.c`, `dap_dispatch.c`, ARM's `DAP.c`, `dap_vendor.c` |
| Generating and running the wire code: SWD, JTAG, cJTAG | CPU 1 | `swd_gen.c`, `jtag_gen.c`, `swd_dp.c`, `cjtag_dp.c` |
| Measuring the target's read latency | CPU 1 | `swd_sweep.c`, `jtag_sweep.c` |
| The consoles, VCOM and the Pro's VCOM2 | CPU 0 | `vcom.c` |
| SWO capture: setup and reads | CPU 1 | `swo.c` |
| Energy capture: start, stop, calibration, readings | CPU 1 | `energy_monitor.c` |
| Energy capture while it runs: integrating, stream messages, the range re-arm | CPU 0, and CPU 1 while it owns the capture state (below) | `main.c`, `energy_integration.h`, `energy_stream.c` |
| Verge Aux: the endpoints, sending stream messages, attaching and ending a stream | CPU 0 | `verge_aux.c` |
| DIO pins, SPI and I2C, target supply, target sense | CPU 1 | `dio.c`, `pro_bus.c`, `target_power.c`, `target_sense.c` |
| DIO edges for the energy stream | CPU 0 | `dio_edges.c` |
| The DMA controller: its reset, descriptor table and interrupt | CPU 0 | `main.c` |
| LEDs | CPU 0 | `led.c` |
| Restart, and entering the bootloader | CPU 0 | `reboot.c` |
| Falling back to full speed | CPU 0 | `usb_speed.c` |
| The core clock and the 96 MHz clock | CPU 0, before CPU 1 starts | `board_clocks.c` |

The two cores run separate images, built and linked on their own. CPU 1 has no FPU, no DSP
extension and no MPU, so its image is compiled for that core. CPU 1's image sits inside CPU 0's,
in flash from `0x4_0000`.

## Starting up

CPU 0 boots first. It sets up the clocks: the core at 150 MHz, and the 96 MHz clock the consoles run
from. It reads the board's straps, which say whether this is a standalone MCU-Link or a Pro, which
function jumpers are fitted, and whether the Pro can measure energy. Then it starts the LEDs, sets
the chip's secure controller so that CPU 1 and DMA can reach USB SRAM, and starts the DMA
controller, the consoles and the DIO edge interrupts. Then it starts CPU 1, and last of all USB.

Before CPU 1 starts, CPU 0 gives CPU 1's data bus the top priority on the bus matrix. Where both
cores reach the same memory in the same cycle, CPU 1 goes first, so CPU 0's copies cannot delay a
wire edge (BUSES.md).

CPU 1 copies its data into RAM and clears the rest. It starts the cycle counter, runs ARM's
`DAP_Setup`, and turns the Pro's target supply off. It sets up target sense, energy capture and the
DIO pins. It tells CPU 0 where its `DAP_TransferAbort` flag is, so CPU 0 can set it directly. On a
Pro that can measure energy, it then calibrates, before it takes the first command. Calibration
disconnects J9 for about 0.2 s, and no session is under way yet. It first waits for CPU 0's loop to
hand it the capture state, so CPU 1 cannot finish starting until CPU 0 is running. Then it waits
for commands.

CPU 0 sets up the core clock and the 96 MHz clock, and CPU 1 changes neither. CPU 1 does choose the
clock source of the peripherals it runs: Flexcomm 3 for SWO, the ADC and the timers for energy
capture, and the SPI and I2C bridges.

## Memory

![A memory map, high addresses at the top. USB SRAM, from 0x4010_0000, holds TinyUSB's buffers, Verge Aux's included, which CPU 0 and the USB controller write, and from 0x4010_1000 dap_shared: the request and reply slots, queues, DMA's channel table and the SWO ring, written by both cores, the USB controller and DMA. SRAM4, from 0x2004_0000, holds the stream messages, 16 slots of 512 bytes, written by the core that owns the capture state. SRAM3's top 8 KiB, from 0x2003_E000, hold dap_stage: staged commands and replies, the HID replies and Verge Aux's slot, written by both cores. SRAM1 starts with CPU 1's 16 KiB of stack. From 0x2001_4000 come CPU 1's command path, the SWD generator and the JTAG generator's scans, then its data, running on into SRAM2. How far depends on the compiler, and rarely used data may reach SRAM3. CPU 1's data holds the abort flag CPU 0 sets. SRAM0 holds CPU 0's stack in its top 4 KiB, then its data, its loop and the code a command passes through, the consoles' rings and the energy ring, which DMA fills, and energy capture's state. SRAMX holds the generated wire code, written at runtime. Flash holds CPU 0's code from 0x200 and CPU 1's from 0x4_0000. Both link scripts place dap_shared, dap_stage and the stream messages at one address. CPU 0 reaches the abort flag, and CPU 1 the energy state, through a pointer in dap_shared.](docs/figures/architecture-memory.svg)

Nothing passes between the images by symbol. Each defines `dap_shared` in a section that both
link scripts place at the same address, and `tools/image.py` checks that both images agree on
every member's offset. `dap_stage` and the stream slots are placed the same way. Two shared objects
are placed by no link script. CPU 1 reaches the energy state in SRAM0 through a pointer CPU 0 puts
in `dap_shared`. CPU 0 reaches CPU 1's abort flag through a pointer CPU 1 puts there.

`dap_shared` is in USB SRAM because the USB controller reaches no other memory, and the command and
reply slots are what it reads and writes. USB SRAM is Device memory to both cores, where an
unaligned access faults. The generated code loads and stores words at any offset: a block write's
data starts at byte 5, and inside `DAP_ExecuteCommands` a reply can start anywhere. So CPU 1 never
runs a command in place. It runs it from a copy in ordinary RAM. By default CPU 0 makes that copy
as the command arrives, into `dap_stage`, and copies the reply back out as it sends it. That is
staging. Vendor command `0x8C` turns it off, and CPU 1 then copies each command and reply itself.

## USB

### The configuration

One endpoint layout serves both boards. The standalone MCU-Link is a Pro with functions left out. A
function the board lacks, or a jumper turns off, is not in the configuration at all.

| function | interface | endpoints | present |
| --- | --- | --- | --- |
| CMSIS-DAP v2 | vendor class, string starting `CMSIS-DAP` | EP1 OUT and IN, bulk | unless the SWD jumper is fitted (J5 standalone, J13 Pro) |
| VCOM | CDC ACM, two interfaces | EP2 OUT and IN, bulk; notification `0x86` | unless the VCOM jumper is fitted (J4 standalone, J14 Pro) |
| CMSIS-DAP v1 | HID, 64-byte reports | EP3 OUT and IN, interrupt | with CMSIS-DAP v2 |
| VCOM2 | CDC ACM, two interfaces | EP4 OUT and IN, bulk; notification `0x87` | on the Pro |
| Verge Aux | vendor class, string `Verge Aux` | EP5 OUT and IN, bulk | on the Pro and the standalone MCU-Link, whatever the jumpers |

The interfaces come in this order, and those after a missing one number down. The endpoints keep
their numbers. CMSIS-DAP v2 is interface 0 whenever it is there, and Verge Aux is always last, so
the interfaces NXP's firmware also has keep the numbers it gives them. The USB-SIO jumper (J15)
changes nothing here: it turns off the DIO, SPI and I2C bridges, not an interface.

The controller has endpoints 0 to 5. The two notification endpoints are numbers it does not have,
declared the way NXP's firmware declares them on the Pro, and never opened: the firmware sends no
notification, and the boards wire no modem lines.

Bulk endpoints are 512 bytes at high speed and 64 at full speed. docs/host-interface.md covers the
fall back to full speed and how a host finds each interface.

### Requests on endpoint 0

CPU 0 answers every control request.

- Standard requests and the CDC and HID class requests, through TinyUSB. Setting a console's line
  coding reconfigures its UART. A HID SET_REPORT on endpoint 0 joins the CMSIS-DAP v1 queue, as a
  report on EP3 would.
- Vendor request 2 returns the Microsoft OS 2.0 descriptor.
- Vendor request `0x03` restarts the probe, into the firmware or the ROM bootloader
  (docs/host-interface.md, "Restart").
- Vendor requests `0x04` and `0x05` are diagnostics: `0x04` reads CPU 1's progress, fault record,
  queues or SWO registers for `tools/dap.py core1`, and `0x05` stops CPU 1 with an NMI that records
  where it was.

## A command's path

A CMSIS-DAP v2 command goes like this:

![The steps below as a sequence between the host, CPU 0, the shared memory, CPU 1 and the target. An abort branches off at step 2: it only sets CPU 1's flag.](docs/figures/architecture-command.svg)

1. CPU 0 arms EP1 OUT into a free request slot in `dap_shared`. There are three. With none free, it
   leaves the endpoint unarmed, and the controller answers the host's next packet with NAK. A
   command the probe cannot place waits on the host's side, and none is dropped.
2. A packet arrives. TinyUSB's task, in CPU 0's loop, hands it to `tud_vendor_rx_cb`. If it is
   `DAP_TransferAbort`, CPU 0 sets CPU 1's abort flag straight away and goes no further: an abort
   has to reach the transfer running now, not wait behind it.
3. Otherwise CPU 0 copies the command into `dap_stage`, gives it the next sequence number and
   publishes its length. It raises CPU 1's mailbox, then arms EP1 OUT into the next free slot.
4. CPU 1 takes the oldest waiting command, by sequence number, from either transport whose reply
   ring has a free slot. It releases the request slot at once, so CPU 0 can receive the next command
   while this one runs. A command numbered before the current USB configuration came from a host
   that has gone, and CPU 1 drops it unrun.
5. CPU 1 runs it. `dap_dispatch.c` sends SWD and JTAG transfers, `DAP_Transfer` and
   `DAP_TransferBlock`, down a short route to the generated code. Everything else goes through
   ARM's `DAP.c`, and vendor ids on to `dap_vendor.c`. The reply goes into `dap_stage`, or, for a
   command CPU 0 did not stage, into CPU 1's own buffer and from there to the reply slot.
6. CPU 1 publishes the reply in the bulk reply ring and raises CPU 0's mailbox.
7. CPU 0 copies a staged reply into the slot's USB buffer and starts the IN transfer. It sends one
   reply at a time, from the head of the ring, so replies leave in the order their commands ran.

`DAP_Info` reports three packets, the number of request slots. probe-rs keeps one of them free for
`DAP_TransferAbort` and pipelines two commands, which is enough to keep the wire busy
(MEASUREMENTS.md, "The command pipeline over USB").

CMSIS-DAP v1 commands take the same path through a queue of their own. Two things differ. CPU 1
runs every HID command's reply in its own buffer, and copies the 64-byte report into the HID reply
ring in `dap_stage`, from which it leaves on EP3 IN. And TinyUSB re-arms the HID endpoint itself,
so a report that finds the queue full has nowhere to wait and is dropped. Only a host with more than
three reports outstanding hits that, since `DAP_Info` told it three.

Each transport has a reply ring of its own because a reply waits on its endpoint until a host reads
it, and a host that has gone never will. With a ring each, such replies hold back only their own
interface (CONCURRENCY.md, "Commands and replies").

Each time the host sets a configuration, which follows every bus reset, CPU 0 starts a new
numbering. CPU 1 drops older commands unrun and CPU 0 drops their replies unsent. CPU 0 also raises
the abort flag, to cut short a command a gone host left running.

### One command, end to end

The figure follows one command through the code that handles it: a `DAP_Transfer` that reads the
DP's CTRL/STAT register over SWD, which a host polls while the target's debug domain powers up. It
goes down the left to the wire, and its reply comes back up the right. Each step that reads or
writes the packet has the bytes it handles underlined.

![One DAP_Transfer that reads the DP's CTRL/STAT, bytes 05 00 01 06, followed down the left from the host through the USB controller, CPU 0, the mailbox and CPU 1 to the wire, and back up the right as the reply, 05 01 01 and the register's four bytes. Each step names its function and file and underlines the bytes it reads or writes: CPU 0's tud_vendor_rx_cb reads the first byte to rule out an abort and copies the packet into dap_stage; CPU 1's dap_task takes it; DAP_ProcessCommand reads 05, DAP_Transfer on SWD; SWD_TransferCommand reads the index, the count and the request, which swd_gen_packet makes 8D; the generated swd_gen_read drives the pins. Along the bottom, the SWD read in time: the probe sends 8D, a turnaround of two clocks follows, then the target sends ACK OK, the 32-bit value and its parity. Back up the right, SWD_TransferCommand writes 01 01 and the value into dap_stage, dap_task publishes the reply, CPU 0's send_bulk_head copies it into the USB slot, and the controller sends it on EP1 IN.](docs/figures/architecture-packet.svg)

1. The host sends four bytes: the command, `05`; the DAP index, `00`; the number of transfers,
   `01`; and the request, `06`, a read of DP register 0x4.
2. The USB controller writes the packet straight into the request slot CPU 0 armed, in
   `dap_shared`. The controller reaches no memory but USB SRAM, so the slots are there.
3. The controller's interrupt queues the transfer's completion for TinyUSB. TinyUSB's task, in CPU
   0's loop, calls `tud_vendor_rx_cb` in `main.c`, which reads the first byte to catch
   `DAP_TransferAbort`. This command is not an abort. CPU 0 copies it into `dap_stage` in SRAM3,
   gives it the next sequence number and publishes its length. Then it raises CPU 1's mailbox and
   arms the endpoint into the next free slot.
4. CPU 1 has been polling its mailbox register. `dap_task` in `core1_main.c` takes the oldest
   waiting command and frees its USB slot, so that CPU 0 can receive the next command while this
   one runs. It runs the command from its copy in `dap_stage`, and the reply goes into `dap_stage`
   as well.
5. `DAP_ProcessCommand` in `dap_dispatch.c` reads the command byte. `05` is `DAP_Transfer`. With
   SWD selected, it goes straight to `SWD_TransferCommand`, not through ARM's `DAP.c`. The
   dispatcher writes the reply's first byte, `05`, itself.
6. `SWD_TransferCommand` in `swd_dp.c` runs ARM's transfer loop, a copy of the one in `DAP.c`. The
   loop skips the DAP index, takes the count and reads the request. A DP read is not posted, so
   the loop reads it straight away. `swd_gen_packet` turns the request's four bits into the SWD
   request, `8D`: a start bit, the four bits, their parity, a stop bit and a park bit.
7. `swd_gen_read` is code CPU 1 generated into SRAMX for the clock and settings in use. It sends
   `8D` on SWDIO. Then it turns SWDIO's buffer around with TXEN, P0_28, and reads the target's
   three-bit acknowledge, its 32 bits and their parity. Each edge is placed to the cycle: SWCLK
   falls on the same store that sets SWDIO, and rises on a store to the port's SET register. The
   target's bits are read at its read latency ([The wire](#the-wire), below).
8. The generated code returns the acknowledge and the word. ARM's loop writes the number of
   transfers done, `01`, the last acknowledge, `01` for OK, and the word, its lowest byte first.
9. `dap_task` publishes the reply in the bulk reply ring and raises CPU 0's mailbox.
10. `send_bulk_head` in `main.c` takes the head of the ring. It copies the seven bytes into the
    slot's USB buffer in `dap_shared` and starts the IN transfer on EP1.
11. The controller sends them when the host next asks EP1 for data. Then `tud_vendor_tx_cb` frees
    the reply slot and raises CPU 1's mailbox, since CPU 1 may be waiting for one.
12. The host reads seven bytes: `05 01 01` and the register's value.

The wire along the bottom is that read in time. The probe changes SWDIO as SWCLK falls, and the
target changes it as SWCLK rises, so the target's bits sit half a cycle later. A dashed stretch is a
turnaround, where neither side drives the line. The probe lets go just after the request's last
rising edge. It takes the line back after the last rising edge of the turnaround after the data.
This read has a turnaround of two clocks. A host picks one to four: the probe's with
`DAP_SWD_Configure`, and the target's in the DP's DLCR. Both start at one. The middle of the data is
cut out.

## Verge Aux

Verge Aux carries `0x82`, `0x8E`, `0x8F`, `0x90` and `0x94`, so a tool can use the DIO pins and, on
the Pro, the supply, energy capture and markers while a debugger holds CMSIS-DAP.

A request takes one slot, in `dap_stage`. CPU 0 copies it in from EP5 OUT and raises CPU 1's
mailbox. CPU 1 answers it before its next CMSIS-DAP command, with the same code that answers that
command on CMSIS-DAP, so a request waits for at most the command running now. CPU 0 sends the reply
on EP5 IN. It arms EP5 OUT again only once the slot is free, so the interface takes one request at
a time. A request from a host that has since gone still runs, since an Aux command needs no session
state. Only its reply is dropped.

Four `0x90` modes never reach CPU 1: modes 6, 7, 10 and 13 attach a stream to a running capture or
end it, without touching the capture. CPU 0 answers them itself, because it owns the capture state
while a capture runs (below). When CPU 1 holds the state for a moment, the answer waits. Starting a
capture with a stream, modes 5 and 9, is CPU 1's, like any start.

The energy stream's messages come out of 16 slots in SRAM4. Whichever core owns the capture state
writes them, and CPU 0 sends them on EP5 IN between replies. A capture start's header waits until
the start's reply has gone, so the reply always comes first.

## The consoles

The consoles are CPU 0's. VCOM is Flexcomm 0 on P0_24 and P0_25, on both boards. The Pro's VCOM2 is
Flexcomm 1 on J26. DMA moves every byte. Each console receives into a ring that DMA fills without
stopping, and sends what the host wrote from a buffer of its own, one transfer at a time. CPU 0's
loop moves runs of bytes between TinyUSB and those buffers, so it has no deadline to meet. A
console's line coding comes from the host on endpoint 0, and CPU 0 sets the UART to it. TinyUSB's
board support claims Flexcomm 0 for its own debug logging, on other pins, so `vcom_init` runs after
`board_init` and takes it back. The probe has no console of its own, so nothing is lost.

## The DMA controller

DMA0 is CPU 0's. CPU 0 resets it, points it at its channel table in `dap_shared`, and takes its
interrupt. The consoles use channels 4 to 7. CPU 1 programs only its own two, channel 8 for SWO and
channel 21 for energy capture. CPU 0's interrupt counts their completed segments, and passes their
errors back to CPU 1 through `dap_shared`, along with a lost count for SWO.

## SWO

SWO capture is split between the cores. CPU 1 runs the `DAP_SWO_*` commands. To start a capture
it hands TDO's pin, P0_3, to Flexcomm 3's UART, and points a DMA channel at a ring in USB SRAM. DMA
then fills the ring without stopping, paced only by the UART. CPU 0's DMA interrupt counts each
segment of the ring as it completes. When a host polls with `DAP_SWO_Data`, CPU 1 turns that count
into a position and copies out what has arrived. When capture stops, the pin goes back to the
GPIO for JTAG.

## Energy capture

Energy capture runs on the Pro, with HW_VER_5 fitted, and uses both cores.

![CPU 1 starts, stops and reads a capture (0x90), and calibrates. CTimer 2 triggers each sample: ADC0 reads the target's voltage and current together. DMA moves the samples into a ring in SRAM0, in segments of about 1 ms or longer. CPU 0 adds each segment to the energy total and, while a stream runs, writes the samples into stream messages, 16 slots in SRAM4, which leave on Verge Aux's EP5 IN. CPU 0's loop switches the current range back to low. CPU 1 owns the capture state until a start, CPU 0 while the capture runs, and CPU 1 again after the stop. Samples, DIO edges and markers are stamped on CTimer 4, the stream's clock.](docs/figures/architecture-energy.svg)

- **CPU 1 controls it.** `0x90` starts, stops and reads a capture, from either interface. CPU 1
  calibrates at boot, and again at a start when the last calibration failed or the current range
  jumper changed.
- **The hardware samples it.** A timer, CTimer 2, triggers each conversion of ADC0, which reads the
  target's voltage and current together: a sample. A DMA channel moves the results into a ring in
  SRAM0, in segments of about a millisecond, or longer below about 2,000 samples a second.
- **CPU 0 integrates it while it runs.** Its DMA interrupt takes each completed segment, adds it
  to the energy total and, while a stream runs, writes the samples into stream messages. Its loop
  switches the current range back to low when a segment shows the current has fallen, timed
  between samples where the rate leaves room. Where the low range keeps giving way, it waits
  longer each time. A capture can hold the high range instead (`0x90`
  mode 14); then nothing switches.
- **One core owns the state at a time.** CPU 1 owns it while no capture runs and for moments
  inside some commands; CPU 0 owns it while a capture runs. CONCURRENCY.md has the handover.
- **A latency measurement holds it.** When CPU 1 measures the target's read latency, it stops the
  conversion timer for the measurement, so the capture's DMA cannot delay a sample on the wire.

The stream's clock is another timer, CTimer 4. Samples, DIO edges and markers are all stamped on it,
so they share one timeline. Both timers and the ADC run from PLL0, at 150 MHz from the board's
16 MHz crystal. DIO edges come from CPU 0's pin interrupts, the only interrupts above
CPU 0's others. A marker (`0x94`) runs on CPU 1, like any command. It goes out with the next
samples message, or on its own after 10 ms.

## The wire

Every transfer runs on code CPU 1 generates at runtime into SRAMX, for the clock, the turnaround,
the idle cycles and the target's read latency in use. The code is placed to the cycle. SWD and JTAG
share one region of SRAMX, and each generates its code again when it finds the other's there.

CPU 1 generates the code again whenever a setting it depends on changes: as the command that
changes it runs, or at the start of the next transfer command. Either way the wire waits while the
code is generated. CODEGEN.md lists what triggers it.

ARM's command logic stays in C around the generated code: posted reads, WAIT retries, value
matching and timestamps. The sequence commands `DAP_SWJ_Sequence` and `DAP_SWD_Sequence` run
ARM's C. So do vendor `0x96`'s escapes and a raw `DAP_SWJ_Sequence` sent with a cJTAG format
selected (`cJTAG_Escape` and `cJTAG_Clock`, paced by the clock in use). `DAP_JTAG_Sequence` runs on
the generated scans.

A connection measures the target's read latency on the wire before it trusts a sample position
([When the probe measures the target](docs/wires.md#when-the-probe-measures-the-target) says when).
`0x89` and `0x8A` can pin one, and report the generated code.

TIMING.md has the timing rules the code is placed by, CODEGEN.md the generators, and CJTAG.md the
cJTAG wire.

## LEDs

CPU 0 drives the LEDs from one timer interrupt, CTimer 3's, 488 times a second. CTimer 3 also fades
the status LED. On the Pro, CTimer 1 fades the FUNC LED, and the other LEDs are on GPIO port 1. CPU
1's part is a byte store for each thing it reports, which the next tick reads (`led.h`). BOARDS.md
says what each LED means.

## Restart

Vendor request `0x03` reaches CPU 0 on endpoint 0. CPU 0 leaves the bus, waits at least 20 ms, and
resets the chip. For the bootloader it first writes the boot mode into an always-on register that
the ROM reads at reset. The ROM's own call for this does not work on this part (erratum ISP.1).

## When CPU 1 faults

CPU 1 takes no interrupts, so any exception it takes is a fault. It records its fault registers and
where it was in `dap_shared`, then stops. CPU 0 keeps running and reports the record through vendor
request `0x04`, which `tools/dap.py core1` reads. Each core has a stack limit, so an overflow faults
rather than overwriting the memory below (STACK.md).

## References

| project | why |
| --- | --- |
| [OpenOCD](https://github.com/openocd-org/openocd) | a host-side OScan1 implementation (`ftdi oscan1_mode on`): a second opinion on the bit encoding |
| [kuopinghsu/cjtag](https://github.com/kuopinghsu/cjtag) | IEEE 1149.7 Class T4 in RTL, with a protocol document |
| [free-dap](https://github.com/ataradov/free-dap) | a tighter CMSIS-DAP implementation than ARM's reference |
| [debugprobe](https://github.com/raspberrypi/debugprobe) | Raspberry Pi's CMSIS-DAP on TinyUSB, the same architecture as this |
| [DAPLink](https://github.com/ARMmbed/DAPLink) | ARM's production probe firmware, for comparison on anything ambiguous |

In this repository:

- `external/tinyusb`: TinyUSB's master with four changes, from a fork until they are upstreamed:
  caller-owned vendor buffers, an endpoint bounds check, placeholder endpoints, and a callback that
  lets the application refuse a line coding. The fork also moves `tools/get_deps.py`'s NXP SDK and
  CMSIS 6 pins forward.
- `external/CMSIS-DAP`: pinned at v2.1.2. `Firmware/Source` has `DAP.c`, `SW_DP.c`, `JTAG_DP.c`,
  `SWO.c` (not built: `src/swo.c` replaces it), `UART.c` and `DAP_vendor.c`;
  `Firmware/Examples/MCU-LINK/` has the board headers this started from.
