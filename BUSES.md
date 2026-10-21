# Bus ports: who touches what, and when

The LPC55S69's AHB matrix arbitrates per slave port: two masters that reach the same port in the
same cycle take turns, and the loser stalls. Code that times the wire by counting cycles (the SWD,
JTAG and two-pin paths, generated or not) is only exact while nothing else is on the ports it
uses. Twice already the other core broke that: CPU 0 polling the ordinary GPIO on port 8 misread
bits at 50 MHz, and its LED tick on CTimer 3 (port 7) moved the stop of the reads clocked from
CTimer 0 by a cycle about once in 10,000 transfers. Those reads are gone (TIMERGEN.md, branch
`swd-timer-reads`), largely because port 7 cannot be kept quiet.

Keep this file current. Adding a peripheral access, a DMA channel, or a buffer in a new SRAM bank
means a row here, and anything that lands on a port a timed window uses needs a handshake or
another port.

Source: UM11126 rev 2.8 tables 4, 5 and 6 (`references/docs/UM11126.pdf`).

## Slave ports

| Port | Addresses (non-secure; secure is +0x1000_0000) | What is there |
| --- | --- | --- |
| 0 | 0x0000_0000 flash, 0x0300_0000 boot ROM | Both cores' C code (CPU 0 from 0x200, CPU 1 from 0x4_0000) |
| 1 | 0x0400_0000 SRAMX, 32 KB | Generated code (SWD, JTAG), code lab arena at 0x0400_4000 |
| 2 | 0x2000_0000 SRAM0, 64 KB | CPU 0's data, stack and `.ramfunc` (`vcom_task` among them) |
| 3 | 0x2001_0000 SRAM1, 64 KB | CPU 1's `.data` and `.ramfunc` (cJTAG's block paths) |
| 4 | 0x2002_0000 SRAM2, 64 KB | CPU 1's `.ramfunc` (JTAG blocks, `SWD_TransferCommandGen`) to 0x2002_A440, then its `.bss`: `response_copy`, `request_copy` |
| 5 | 0x2003_0000 SRAM3, 64 KB | `dap_stage` in the top 16 KB from 0x2003_C000: the commands and replies CPU 0 stages for CPU 1. Below it, CPU 1's `.data` and `.bss` where they outgrow SRAM2 (the builds without generated SWD) |
| 6 | 0x2004_0000 SRAM4, 16 KB | CPU 1's stack (top at 0x2004_4000), the whole bank |
| 7 | 0x4000_0000 APB bridge 0 **and** 0x4002_0000 APB bridge 1 | Every APB peripheral, both bridges on one port (below) |
| 8 | 0x4008_0000 | DMA0 regs, SCTimer, Flexcomm 0 to 4, mailbox, ordinary GPIO |
| 9 | 0x4009_0000 | USB HS device regs, CRC, Flexcomm 5 to 7, SDIO, debug mailbox, HS SPI |
| 10 | 0x400A_0000 | ADC0, USB hosts, Hash-AES, Casper, PowerQuad, DMA1 regs, secure GPIO, AHB secure control |
| 11 | 0x4010_0000 USB SRAM, 16 KB | USB buffers (CPU 0), `dap_shared` at 0x4010_1000: queues, SWO ring, shared state |

APB bridge 0: SYSCON, IOCON, GINT0/1, PINT, secure PINT, INPUTMUX, CTimer 0 and 1, WWDT, MRT,
UTICK, analog controls. APB bridge 1: SYSCTL, CTimer 2, 3 and 4, RTC, OS event timer, flash
controller, PRINCE, USB HS PHY, RNG, PUF, PLU. Since both bridges are port 7, any APB access
contends with any other.

## Masters and what they reach

| Master | Port | What | When |
| --- | --- | --- | --- |
| CPU 1 | 1 | Generated code fetch | Every generated transfer |
| CPU 1 | 10 | Secure GPIO (`DBGIF_GPIO`): every SWDIO, SWCLK, TMS, TDI, TDO access | Inside every timed window |
| CPU 1 | 3, 4 | The `.ramfunc` bit paths: ARM's SWD, the JTAG and two-pin blocks, where nothing is generated | Inside their timed windows |
| CPU 1 | 5 | A staged command's request and reply in `dap_stage`: the generated transfers load write data and store read data there | Inside timed windows |
| CPU 1 | 0, 3, 4, 6 | C code, `.ramfunc`, data and stack between transfers | Between timed windows |
| CPU 1 | 10 | ADC0 (target sense, `dap_vendor`) | Own core, never during a window |
| CPU 1 | 7, 8 | SWO setup: SYSCON resets and clocks, Flexcomm 3, DMA0 regs | SWO commands only |
| DMA0 | 8 then 11 | Flexcomm 3 FIFO to the SWO ring | Whenever SWO bytes arrive |
| USB HS controller | 11 | Endpoint buffers in USB SRAM | Every USB transfer |
| CPU 0 | 0, 2 | Code and data | Always |
| CPU 0 | 9, 11 | USB device regs and buffers (TinyUSB) | Every USB transfer |
| CPU 0 | 11 then 5 | Each staged command, from its USB slot into `dap_stage`, and each staged reply back | As each arrives and as each starts to send, so during CPU 1's windows when commands are queued |
| CPU 0 | 8 | USART0/1 (VCOM), mailbox, DMA0 regs (SWO interrupt) | Continuously while VCOM or SWO run |
| CPU 0 | 7 | CTimer 3 (LED) | 488 times a second |
| CPU 0 | 7 | SYSCON and IOCON through `vcom_configure` (`USART_Init`, `CLOCK_*`) | When a host sets the line coding (a terminal opening the port) |
| CPU 0 | 7 | SYSCON and IOCON reads for the SWO status request; SYSCON NMISRC for the CPU 1 wake | Those vendor control requests |
| CPU 0 | 7 | SYSCON (CPU 1 start), PMC (reboot) | Boot and reboot only |
| CPU 0 | 8 | Ordinary GPIO (the board straps) | Boot only |

## Timed windows and their exposure

- **Bit paths, all wires** (port 10 secure GPIO; code from port 1, or ports 3 and 4 for the
  `.ramfunc` paths). Nothing else on port 10 runs during a window: ADC0 is CPU 1's own. The
  ordinary GPIO on port 8 shares with Flexcomm 0 to 4 and the mailbox, which CPU 0 polls; that is
  why the bit paths moved to the secure alias. No other master uses SRAMX, SRAM1 or SRAM2.
- **A staged command's buffers** (port 5). CPU 0 copies the next command in and the last reply out
  while CPU 1's transfer runs. CPU 0 boots with CPU 1's data bus at the top AHB matrix priority
  (SYSCON AHBMATPRIO, set in `start_core1`), so CPU 1 wins every tie there and on every other
  port. The lab measurement below found no cycle lost to CPU 0 at that priority.

No timed window uses port 7, so CPU 0's APB accesses (the LED, the VCOM line coding, the SWO
status request) cannot move an edge. A timed path that touches an APB peripheral would bring all
of them back as exposures.

## Measured costs on port 7

Code lab, 2026-10-16: a CTimer store
stalls the core about 4 cycles, two in a row about 10, a CTimer load about 9; none of it is in
the emulator's timing model. On the bench, 2026-10-21 (`drainab.py`): a CTimer 0 load cost 8
cycles each time inside the timer reads (TIMERGEN.md "Why").

## Measured: CPU 0 on CPU 1's SRAM port

Code lab, 2026-10-21 (`sramcontend.py`, logs `sramcontend-2026-10-21.txt`
and `sramcontend-staged-2026-10-21.txt` beside it). CPU 1 timed 256 loads and stores to SRAM3,
from SRAMX, while CPU 0 copied within the same bank. At the reset priorities, and with CPU 0
favoured, 8 to 55% of bodies lost cycles, 2 to 13 each. With CPU 1's data bus at priority 3, none
of 1,100 bodies lost a cycle, with CPU 0 copying word by word or with `memcpy`.

The same lab found SRAM4 slower than SRAM2 and SRAM3 with nothing else running: a load next to a
store costs an extra cycle (386 cycles against 259 for 128 load-store pairs). That is why
`dap_stage` is in SRAM3 and not SRAM4. SRAMX through the data side costs two cycles an access.
