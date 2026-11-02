# Bus ports: who touches what, and when

The LPC55S69's AHB matrix arbitrates per slave port: two masters that reach the same port in the
same cycle take turns, and the loser stalls. Code that times the wire by counting cycles (the SWD,
JTAG and two-pin paths, generated or not) is only exact while nothing else is on the ports it
uses. Twice already the other core broke that: CPU 0 polling the ordinary GPIO on port 8 misread
bits at 50 MHz, and its LED tick on CTimer 3 (port 7) moved the stop of the reads clocked from
CTimer 0 by a cycle about once in 10,000 transfers. Those timer-clocked reads reached 50 MHz and
gained 1% on blocks, but lost 6% on DAP_Transfer to port 7 stalls, and were removed, largely because
port 7 cannot be kept quiet.

Keep this file current. Adding a peripheral access, a DMA channel, or a buffer in a new SRAM bank
means a row here, and anything that lands on a port a timed window uses needs a handshake or
another port.

Source: UM11126 rev 2.8 tables 4, 5 and 6.

## Slave ports

| Port | Addresses (non-secure; secure is +0x1000_0000) | What is there |
| --- | --- | --- |
| 0 | 0x0000_0000 flash, 0x0300_0000 boot ROM | Both cores' C code (CPU 0 from 0x200, CPU 1 from 0x4_0000) |
| 1 | 0x0400_0000 SRAMX, 32 KB | Generated code (SWD, JTAG), code lab arena at 0x0400_4000 |
| 2 | 0x2000_0000 SRAM0, 64 KB | CPU 0's data, stack (top 4 KB, see [STACK.md](STACK.md)) and `.ramfunc` (`vcom_task` and the energy integration among them); the consoles' receive rings, send buffers and ring descriptors, and the energy ring, its reload descriptors and its integration state (`energy_integration`), which DMA0 reaches |
| 3 | 0x2001_0000 SRAM1, 64 KB | CPU 1's full 16 KiB stack in the bottom quarter, followed by its `.ramfunc` (command processing around the generated code, and the sample probe's reads), `.data` and, in a product image, all of its `.bss` and the start of `.cold_bss` |
| 4 | 0x2002_0000 SRAM2, 64 KB | The rest of CPU 1's `.cold_bss` in a product image. In the ARM reference build, ARM's bit paths in `.ramfunc` and its `.bss`, which on GCC run on into SRAM3. tools/image.py refuses a product image whose RAM code or data, `.cold_bss` aside, reaches SRAM3 |
| 5 | 0x2003_0000 SRAM3, 64 KB | `dap_stage` in the top 8 KB from 0x2003_E000: packet-sized commands and replies CPU 0 stages for CPU 1, and Verge Aux's request slot. Below it, nothing of a product image's CPU 1. `.cold_bss` (the JTAG generator's two saved builds, 32 KB, touched only on a wire or configuration switch) comes after every other `.bss` and may reach here. GCC's ARM reference build outgrows SRAM2 with `.ramfunc`, so some of ARM's bit paths run from here: a test build |
| 6 | 0x2004_0000 SRAM4, 16 KB | Stream message storage in the lower 8 KB from 0x2004_0000; upper 8 KB unused |
| 7 | 0x4000_0000 APB bridge 0 **and** 0x4002_0000 APB bridge 1 | Every APB peripheral, both bridges on one port (below) |
| 8 | 0x4008_0000 | DMA0 regs, SCTimer, Flexcomm 0 to 4, mailbox, ordinary GPIO |
| 9 | 0x4009_0000 | USB HS device regs, CRC, Flexcomm 5 to 7, SDIO, debug mailbox, HS SPI |
| 10 | 0x400A_0000 | ADC0, USB hosts, Hash-AES, Casper, PowerQuad, DMA1 regs, secure GPIO, AHB secure control |
| 11 | 0x4010_0000 USB SRAM, 16 KB | USB buffers (CPU 0), `dap_shared` at 0x4010_1000: queues, DMA0's descriptor table, SWO ring, shared state |

APB bridge 0: SYSCON, IOCON, GINT0/1, PINT, secure PINT, INPUTMUX, CTimer 0 and 1, WWDT, MRT,
UTICK, analog controls. APB bridge 1: SYSCTL, CTimer 2, 3 and 4, RTC, OS event timer, flash
controller, PRINCE, USB HS PHY, RNG, PUF, PLU. Since both bridges are port 7, any APB access
contends with any other.

## Masters and what they reach

| Master | Port | What | When |
| --- | --- | --- | --- |
| CPU 1 | 1 | Generated code fetch | Every generated transfer |
| CPU 1 | 3 | Shared generator event workspace, 1280 events with JTAG or 256 for SWD only | Builds run serially between timed windows; each schedule is consumed before reuse |
| CPU 1 | 10 | Secure GPIO (`DBGIF_GPIO`): every SWDIO, SWCLK, TMS, TDI, TDO access | Inside every timed window |
| CPU 1 | 3, 4, 5 | The `.ramfunc` bit paths of the ARM reference build: ARM's SWD, the JTAG and two-pin blocks | Inside their timed windows |
| CPU 1 | 5 | A staged command's request and reply in `dap_stage`: the generated transfers load write data and store read data there | Inside timed windows |
| CPU 1 | 5 | Verge Aux's slot in `dap_stage`: its state before each command, and an aux request and its reply | Between commands |
| CPU 1 | 7 | CTimer 4's counter, the energy stream's clock: read at each start, resume, hold and release, and to stamp each marker | Energy commands and `0x94`, and around a read-latency measurement, outside its window |
| CPU 1 | 0, 3, 4, 5 | C code, `.ramfunc`, data and stack between transfers | Between timed windows |
| CPU 1 | 11 | `dap_shared`: unstaged requests and replies, the queues, the LED inputs | Between timed windows |
| CPU 1 | 10 | ADC0 (target sense, `dap_vendor`, the Pro's energy calibration) and settled energy FIFO results | Own core, never during a window: at boot on the Pro, a host's sense reads, a start that calibrates; FIFO drain at capture pause/stop |
| CPU 1 | 2 | CPU 0's `energy_integration` in SRAM0: ring reads for latest-pair replies and the unconsumed capture tail; each start's identity, publication sequence, tables, constants, descriptors and position; the running flag; the segment count a capture started at; the energy stream's timeline, and its message indices at a start, pause or stop; a hold's record after each read-latency measurement; a marker's record, and the stream's marking and busy flags, for each `0x94` | Host polls while capture runs; tail drain and capture setup around the quiet handshake; the running flag before each read-latency measurement and around each sense conversion; the DIO edges recorded since the last message, at a pause or stop | Never during a window |
| CPU 1 | 8 | Ordinary GPIO ports 0 and 1: the Pro's DIO pins and their translators' directions (`dio.c`), port 0 pin 8 for the target regulator (`target_power.c`), and the energy circuit's latch and calibration pins (`energy_monitor.c`) | Vendor 0x8E, 0x8F and 0x90, and the energy calibration at boot; own core, never during a window |
| CPU 1 | 7 | RNG registers, SYSCON clock/reset and PMC power control for the capture's 128-bit boot value | At a capture start, until one initialization succeeds; bounded entropy/health waits; own core, outside every wire window |
| CPU 1 | 7 | CTimer 2 for the Pro's energy-capture trigger; SYSCON, IOCON and INPUTMUX for setup | Energy capture start/stop, and IOCON at boot on the Pro; CTimer 2 stopped and started around each read-latency measurement |
| CPU 1 | 8, 10 | DMA0's busy bit and ADC0's status, polled until the held capture is idle | Before each read-latency measurement, while capture runs |
| CPU 1 | 8, 10 | DMA0's error bits and ADC0's status, to tell a failed capture (`energy_capture_failed`) | Energy commands and `0x94` while a capture runs; own core, never during a window |
| CPU 1 | 7 | SYSCON clocks and IOCON for the DIO pins (`dio_init`) | Boot only, on the Pro |
| CPU 1 | 7 | PINT's enables for the DIO pins, turned off as SPI takes them (`dio_spi_begin`) | Each `0x91` SPI command, on the Pro; own core, never during a window |
| CPU 1 | 7, 8 | SWO setup: SYSCON resets and clocks, Flexcomm 3, DMA0 channel 8's regs and its bit in the shared ones, each written one bit per channel | SWO commands only |
| DMA0 | 8 then 11 | Flexcomm 3 FIFO to the SWO ring | Whenever SWO bytes arrive |
| DMA0 | 8 then 2 | Flexcomm 0 and 1 FIFOs to the consoles' receive rings | Whenever console bytes arrive |
| DMA0 | 2 then 8 | The consoles' send buffers to Flexcomm 0 and 1 FIFOs | Each send from the host |
| DMA0 | 11, 2 | Descriptors: channel entries and SWO's ring in `dap_shared`, the consoles' rings in SRAM0 | Each send, and each segment of a ring |
| DMA0 | 10, 2, 11 | ADC0 RESFIFO to the energy ring in SRAM0, two segments of 16 to 512 words, about 1 ms each at the capture's rate; channel 21's table entry in `dap_shared`, its reload descriptors in SRAM0 | While energy capture runs, except during a read-latency measurement; about seven DMA reads per voltage/current/range sample, measured: the voltage's and the current's results arrive together, and up to two reads after each burst find the FIFO empty; 1,000 to 2,000 interrupts/s from 2,286 pairs/s, about 1,370/s at 100 kpair/s (from seven words a pair; 684/s, against 680 measured, with 10 ms segments), seven sixteenths of the pair rate below 2,286 |
| USB HS controller | 11 | Endpoint buffers in USB SRAM | Every USB transfer |
| CPU 0 | 0, 2 | Code and data | Always |
| CPU 0 | 3, 4 or 5 | `DAP_TransferAbort`, one byte in CPU 1's `.bss`, through `dap_shared.transfer_abort` | The moment an abort arrives, so possibly during a window |
| CPU 0 | 9, 11 | USB device regs and buffers (TinyUSB) | Every USB transfer |
| CPU 0 | 11 then 5 | Each staged command, from its USB slot into `dap_stage`, and each staged reply back | As each arrives and as each starts to send, so during CPU 1's windows when commands are queued |
| CPU 0 | 11 then 5 | Each Verge Aux request, from its USB buffer into `dap_stage`, and each reply back | On the Pro, as each arrives and as each reply is taken, so during CPU 1's windows |
| CPU 0, CPU 1 | 6 | Stream message payload slots, 16 × 512 bytes, reserved at 0x2004_0000 in both linkers | The capture owner builds each message before publishing its SRAM0 length/index; CPU0 sends published slots. CPU1 writes only outside timed windows under capture ownership; no DMA uses this bank. |
| CPU 0 | 6 then 11 | Each energy stream message, from its reserved SRAM4 slots to Verge Aux's USB buffer | While a stream runs, up to about 1,000 times a second |
| CPU 0 | 8 | USART0/1 (VCOM), mailbox, DMA0 regs: its setup, the console channels, the interrupt for both rings | Continuously; the consoles' ring positions while it waits |
| CPU 0 | 2, 6, 8, 10, 11 | Integrates each completed energy segment in the DMA interrupt, running from SRAM0: checks DMA0's error and ADC0's overflow bits, reads the samples and its state in SRAM0, and writes stream payloads in SRAM4, and updates the accumulator in `dap_shared` | Each completed energy segment, except while CPU 1 owns capture state |
| CPU 0 | 8 | GPIO0: reads P0_29 and P0_30, pulses P0_17, to re-arm the energy capture's low range | At most once per completed energy-ring segment, after one that stayed on the high range under the low range's limit |
| CPU 0 | 11 | The consoles' channel entries in DMA0's table | Each send |
| CPU 0 | 7 | CTimer 3 (the status LED), and on the Pro CTimer 1 (FUNC) | 488 times a second |
| CPU 0 | 7, 8, 2 | The DIO edge interrupt (`dio_edges.c`): CTimer 4's counter and PINT's latches on port 7, both GPIO ports' PIN registers on port 8, an entry in the edge record in SRAM0 | Each run, while a stream reports edges: at most 100,000 a second on average |
| CPU 0 | 7 | PINT's enables and latches, CTimer 4's counter for the 10 ms check | CPU 0's loop, while a stream reports edges |
| CPU 0 | 7, 2 | CTimer 4's counter and the oldest marker's stamp in SRAM0, for the markers' 10 ms check | CPU 0's loop, while a stream has markers waiting |
| CPU 0 | 7 | INPUTMUX's pin interrupt selects, PINT's reset and mode | Boot only, on the Pro |
| CPU 0 | 8 | Ordinary GPIO port 1 (the Pro's switched LEDs) | 488 times a second, on the Pro |
| CPU 0 | 7 | SYSCON and IOCON through `vcom_start` (`USART_Init`, `CLOCK_*`) | Console initialization; line-coding changes touch only port 8 |
| CPU 0 | 7 | SYSCON and IOCON reads for the SWO status request; SYSCON NMISRC for the CPU 1 wake | Those vendor control requests |
| CPU 0 | 7 | SYSCON (CPU 1 start), PMC (reboot) | Boot and reboot only |
| CPU 0 | 8 | Ordinary GPIO (the board straps) | Boot only |

CPU 1 stops energy DMA and waits for a generation acknowledgement from CPU 0's main loop before
resetting capture state. The acknowledgement follows the last handler and range-latch task.
While quiet, CPU 0 leaves energy completion/error bits to CPU 1. SWO and VCOM continue normally.
An energy position sequence spans completion clearing through count publication, so readers wait
for the whole update before using the DMA position. Pause drains the settled ring and FIFO before reuse.

## Timed windows and their exposure

- **Bit paths, all wires** (port 10 secure GPIO; code from port 1, or ports 3 and 4 for the
  `.ramfunc` paths, and 5 for those GCC's ARM reference build places in SRAM3). The ordinary GPIO on port 8 shares with Flexcomm 0 to 4 and the mailbox, which
  CPU 0 polls; that is why the bit paths moved to the secure alias. During energy capture, DMA0
  reads ADC0 on port 10 and writes the ring on port 2. CPU 1's data bus wins every tie, but a DMA0
  read of ADC0 already under way holds a secure GPIO access up to 2 cycles. A held store holds
  every edge after it, which only stretches a phase. A held sample placed after the next rising
  edge reads the next bit: SWD reads at P = 4 failed about once in 100 transfers at 100k pairs/s,
  scaling with DMA0's reads, and four-wire JTAG reads a target that answers 10 to 13 cycles after
  a fall failed in the emulator. With capture asked for, the generated SWD and four-wire paths keep
  2 cycles at the late end of such a sample's window, and their clocks lengthen where that leaves
  no room (`gen_stall`, CODEGEN.md); two-pin samples come before that edge. Holding the
  conversion trigger for a measurement (`energy_monitor_hold`) stops the ADC reads, the ring
  writes and CPU 0's segment interrupts together, so latency measurements run without the stall.
  Capture is disabled by default.
  CPU 0's segment interrupt reads ADC0's status once a segment, on port 10 too.
  Apart from CPU 0's abort byte, no other master accesses SRAMX, SRAM1 or SRAM2. During capture, DMA0 writes SRAM0 and CPU 1 reads
  it, between windows.
- **A staged command's buffers** (port 5). CPU 0 copies the next command in and the last reply out
  while CPU 1's transfer runs. CPU 0 boots with CPU 1's data bus at the top AHB matrix priority
  (SYSCON AHBMATPRIO, set in `start_core1`), so CPU 1 wins every tie there and on every other
  port. The lab measurement below found no cycle lost to CPU 0 at that priority.

No timed window uses port 7, so CPU 0's APB accesses (the LED, the DIO edge interrupt, the console
initialization, the SWO status request) cannot move an edge. A timed path that touches an APB peripheral would bring all
of them back as exposures.

## Measured costs on port 7

Code lab, 2026-10-16: a CTimer store
stalls the core about 4 cycles, two in a row about 10, a CTimer load about 9; none of it is in
the emulator's timing model. On the bench, 2026-10-21: a CTimer 0 load cost 8 cycles each time
inside the timer reads.

## Measured: CPU 0 on CPU 1's SRAM port

Code lab, 2026-10-21. CPU 1 timed 256 loads and stores to SRAM3,
from SRAMX, while CPU 0 copied within the same bank. At the reset priorities, and with CPU 0
favoured, 8 to 55% of bodies lost cycles, 2 to 13 each. With CPU 1's data bus at priority 3, none
of 1,100 bodies lost a cycle, with CPU 0 copying word by word or with `memcpy`.

The same lab found SRAM4 slower than SRAM2 and SRAM3 with nothing else running: a load next to a
store costs an extra cycle (386 cycles against 259 for 128 load-store pairs). That is why
`dap_stage` is in SRAM3 and not SRAM4. SRAMX through the data side costs two cycles an access.

Capture identity publication uses an odd/even sequence around the 24-byte copy. CPU 0 retries
its conditional stream-end comparison if the sequence is odd or changes across its snapshot.
Acquisition start and conditional acquisition stop run on CPU 1. CPU 0's quiet acknowledgement
serializes its stream mutation with a start. DMA remains masked while CPU 0 attaches or ends a
stream. A stale acquisition stop compares before requesting quiet.

CPU 1 reserves its full 16 KiB stack at 0x20010000..0x20013fff, in SRAM1 on port 3.
Its data/code starts at 0x20014000. CPU 0 reaches SRAM1/2 only to set `DAP_TransferAbort`.
Stream payloads use SRAM4 on port 6, which no CPU 1 timed window reaches.
Both images resolve energy_stream_slots to the same reserved array. There is one payload queue;
a second independent stream cannot use it while the first has queued messages.
