# Bus ports: who touches what, and when

The LPC55S69's AHB matrix arbitrates per slave port: two masters that reach the same port in the
same cycle take turns, and the loser stalls. Code that times the wire by counting cycles (the SWD,
JTAG and two-pin paths, generated or not) or by starting peripherals a cycle apart (the 50 MHz
timer reads) is only exact while nothing else is on the ports it uses. Twice already the other core
broke that: CPU 0 polling the ordinary GPIO on port 8 misread bits at 50 MHz, and its LED tick on
CTimer 3 (port 7) moved the timer reads' stop by a cycle about once in 10,000 transfers.

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
| 4 | 0x2002_0000 SRAM2, 64 KB | CPU 1's `.ramfunc` (JTAG blocks, `SWD_TransferCommandGen`) to 0x2002_A440, then its `.bss` to 0x2002_F964: `response_copy`, `request_copy`, DMA1's descriptors and `stop` word |
| 5 | 0x2003_0000 SRAM3, 64 KB | Nothing placed; CPU 1's stack only if it grows past SRAM4 |
| 6 | 0x2004_0000 SRAM4, 16 KB | CPU 1's stack (top at 0x2004_4000) |
| 7 | 0x4000_0000 APB bridge 0 **and** 0x4002_0000 APB bridge 1 | Every APB peripheral, both bridges on one port (below) |
| 8 | 0x4008_0000 | DMA0 regs, SCTimer, Flexcomm 0 to 4, mailbox, ordinary GPIO |
| 9 | 0x4009_0000 | USB HS device regs, CRC, Flexcomm 5 to 7, SDIO, debug mailbox, HS SPI |
| 10 | 0x400A_0000 | ADC0, USB hosts, Hash-AES, Casper, PowerQuad, DMA1 regs, secure GPIO, AHB secure control |
| 11 | 0x4010_0000 USB SRAM, 16 KB | USB buffers (CPU 0), `dap_shared` at 0x4010_1000: queues, SWO ring, flags |

APB bridge 0: SYSCON, IOCON, GINT0/1, PINT, secure PINT, INPUTMUX, CTimer 0 and 1, WWDT, MRT,
UTICK, analog controls. APB bridge 1: SYSCTL, CTimer 2, 3 and 4, RTC, OS event timer, flash
controller, PRINCE, USB HS PHY, RNG, PUF, PLU. Since both bridges are port 7, any APB access
contends with any other.

## Masters and what they reach

| Master | Port | What | When |
| --- | --- | --- | --- |
| CPU 1 | 1 | Generated code fetch | Every generated transfer |
| CPU 1 | 10 | Secure GPIO (`DBGIF_GPIO`): every SWDIO, SWCLK, TMS, TDI, TDO access | Inside every timed window |
| CPU 1 | 7 | IOCON PIO0_0 (SWCLK to CTimer 0 and back), CTimer 0 and CTimer 2/4 starts, CTimer 0 TCR drain loads | Each timer read: 2 IOCON stores, 2 loads, 4 stores |
| CPU 1 | 11 | `dap_shared.swd_timer_busy`, `led_writing` | Each timer read, before and after its runs |
| CPU 1 | 4 | `response_copy` store of the read word | After the data run, 7 cycles after DMA1's stop write (emulator) |
| CPU 1 | 0, 3, 4, 6 | C code, `.ramfunc`, data and stack between transfers | Between timed windows |
| CPU 1 | 10 | ADC0 (target sense, `dap_vendor`) | Own core, never during a window |
| CPU 1 | 7, 8 | SWO setup: SYSCON resets and clocks, Flexcomm 3, DMA0 regs | SWO commands only |
| DMA1 | 4 then 7 | Descriptor and `stop` from SRAM2, then CTimer 0 TCR | Ends each timer run, about 22 cycles after its match |
| DMA0 | 8 then 11 | Flexcomm 3 FIFO to the SWO ring | Whenever SWO bytes arrive |
| USB HS controller | 11 | Endpoint buffers in USB SRAM | Every USB transfer |
| CPU 0 | 0, 2 | Code and data | Always |
| CPU 0 | 9, 11 | USB device regs and buffers (TinyUSB) | Every USB transfer |
| CPU 0 | 8 | USART0/1 (VCOM), mailbox, DMA0 regs (SWO interrupt) | Continuously while VCOM or SWO run |
| CPU 0 | 7 | CTimer 3 (LED), behind the handshake | 488 times a second |
| CPU 0 | 7 | SYSCON and IOCON through `vcom_configure` (`USART_Init`, `CLOCK_*`) | When a host sets the line coding (a terminal opening the port) |
| CPU 0 | 7 | SYSCON and IOCON reads for the SWO status request; SYSCON NMISRC for the CPU 1 wake | Those vendor control requests |
| CPU 0 | 7 | SYSCON (CPU 1 start), PMC (reboot), GPIO port 8 (straps) | Boot and reboot only |

## Timed windows and their exposure

- **Bit paths, all wires** (port 10 secure GPIO). Nothing else on port 10 runs during a window:
  ADC0 is CPU 1's own. The ordinary GPIO on port 8 shares with Flexcomm 0 to 4 and the mailbox,
  which CPU 0 polls; that is why the bit paths moved to the secure alias.
- **Timer reads, the start pair** (port 7). CTimer 0 starts at cycle 0 and the stop timer at cycle
  1; a CPU 0 access to port 7 between them starts the stop late, and the run ends an edge late.
  Covered for the LED only.
- **Timer reads, the stop** (port 4 then 7). DMA1 reads SRAM2, then writes CTimer 0. Delayed by
  CPU 0 on port 7, or by any other master on SRAM2, it stops the clock late. CPU 1 stores to
  SRAM2 only after the stop has landed, and no other master uses SRAM2.

Not yet covered: CPU 0's port 7 accesses other than the LED. A host that sets the VCOM line
coding, or asks for the SWO status, while a 50 MHz read runs can collide with the start pair or
the stop. Rare, untested, and the fix is the same handshake around those accesses.

## Measured costs on port 7

Code lab, 2026-10-16: a CTimer store
stalls the core about 4 cycles, two in a row about 10, a CTimer load about 9; none of it is in
the emulator's timing model. On the bench, 2026-10-21 (`drainab.py`): each CTimer 0 drain load in
a timer read costs 8 cycles a read (TIMERGEN.md "Throughput").
