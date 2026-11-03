# MCU-Link cJTAG firmware — design and references

What this firmware is for, how its two-pin support fits into ARM's code, what is known about the
board, and the projects it draws on.

## Why this exists

probe-rs cannot talk to a two-pin IEEE 1149.7 (cJTAG) target through a CMSIS-DAP probe, and the
reason is in the probe's firmware rather than in the host.

ARM's reference SWD bit path moves SWDIO while SWCLK is high:

```c
#define SW_CLOCK_CYCLE()  PIN_SWCLK_CLR(); PIN_DELAY(); PIN_SWCLK_SET(); PIN_DELAY()
#define SW_WRITE_BIT(bit) PIN_SWDIO_OUT(bit); PIN_SWCLK_CLR(); ...
```

`SW_CLOCK_CYCLE` leaves the clock high, and `SW_WRITE_BIT` changes data before pulling it low.
Harmless for SWD, where the target samples on the rising edge. On a 1149.7 wire **a TMSC transition
while TCKC is high is an escape edge by definition**, so this emits one on every bit change.

Measured, TMSC transitions while TCKC is high, per clock:

| | activation | read |
|---|---|---|
| our CMSIS-DAP path | 0.348 | 0.418 (59 inside a 141-clock read) |
| working J-Link path | 0.006 | 0.000 |

**The host cannot avoid this.** Releasing the wire for the target's half of an OScan1 frame is only
reachable through `DAP_SWD_Sequence`, and that command's loop is the bit path above.

**So the fix is to move TMSC only while TCKC is low**, which is a reordering inside the probe, and
that is why this firmware exists.

**Calibration, do not overstate it.** Escape edges are a strong candidate, not a proven cause. Our
activation carries the same edge density and its command still lands, so they are evidently
tolerated in the command window. A predicted read-edge count of 43 did not match the measured 59.

## How OScan1 is wired in

In the ARM reference build, ARM's `JTAG_DP.c` is compiled **three times**, and the wire framing is
the only thing that differs:

- normally, with its five entry points renamed `jtag4_*`;
- again through `src/cjtag_jtag_dp.c`, renamed `cjtag_*`, with the pin layer shadowed so each
  four-wire clock becomes one OScan1 frame;
- and once more through `src/cjtag_jtag_dp_fast.c`, renamed `cjtagf_*`, which drives the frame
  without its pacing loop for requests above the switch point.

`src/cjtag_dp.c` owns the real names and picks a path per call. Neither path carries a per-bit
branch, and neither duplicates the other's TAP walking, `ir_before`/`ir_length` accounting or
transfer retry logic. The second copy costs about 2.9 KB.

**The contract this rests on** is that `JTAG_DP.c` expresses every clock through four macros which
expand, in order, to `PIN_TDI_OUT` (sometimes), `PIN_TCK_CLR`, `PIN_DELAY`, `PIN_TDO_IN`
(sometimes), `PIN_TCK_SET`, `PIN_DELAY`. Shadowing those names stages TMS and TDI, opens a frame on
`PIN_TCK_CLR`, and emits it at whichever of `PIN_TDO_IN` or `PIN_TCK_SET` comes first. The
submodule is pinned to a release, so the order cannot change without someone bumping the tag.

**Where a frame rests, and why it is not where ARM's four-wire code rests.** A frame is entered and
left with TCKC low and the wire driven. That is deliberate and it is the second half of the fix.
Taking the wire back after the TDO packet moves TMSC from whatever the target drove to whatever our
output latch holds; doing it while TCKC was still high put one escape edge in every frame, which is
the same density as the SWD path this firmware exists to replace. OpenOCD's OScan1 driver sequences
the same three packets the same way, with TCK resting low between them.

## Vendor commands

| id | | |
| --- | --- | --- |
| 0x82 | `CJTAG_INFO` | answers "CJTAG", a format version, a capability bitmap and the build string. Read-only, so a host may send it blindly: stock V3.172 echoes the command with nothing after it, which is how a host tells the two images apart. `BENCH.md` has the byte layout. |
| 0x83 | `CJTAG_CONTROL` | a mode byte: 0 returns to four-wire framing, 2 frames as OScan1 after an activation the host drove, 3 with two more bytes frames scan format `f` from TAP state `s`, and 4 with a u32 mask limits the formats offered. Anything else answers `DAP_ERROR`. |

| 0x84 | `J8` | hands J8's four pins to the host, on the plain MCU-Link |
| 0x86 | `TARGET` | reads the target's supply and J6 pin 9's detect line |
| 0x87 | `SAMPLE` | DPIDR reads with the SWDIO sample placed by the host; an instrument |
| 0x88 | `LAB` | runs host-supplied code from RAM; built only with `CODE_LAB` |
| 0x89 | `SWD_GEN` | reports the generated SWD paths and sets the read latency they are generated for |
| 0x8A | `JTAG_GEN` | the same for the generated JTAG and two-pin paths |
| 0x8B | `JTAG_PACING` | switches the generated JTAG transfers' learned pacing |
| 0x8C | `STAGE` | switches whether CPU 0 stages commands for CPU 1 |
| 0x8D | `JTAG_LOG` | reads back the JTAG generator's log of what each generation was for |
| 0x8E | `DIO` | drives and reads the Pro's DIO pins on J19 |

`src/dap_vendor.c` has each command's bytes. Anything else in 0x80..0x9F answers `ID_DAP_Invalid` here. `DAP_ProcessCommand` returns what
`DAP_ProcessVendorCommand` returns without inspecting it, so the rejection has to be written by the
default case rather than left to the caller.

Stock's 0x80 and 0x81 do something unknown and are **not implemented here**, so any LinkServer
feature resting on them will not work against this firmware.

## Architecture, and why

**TinyUSB's `mcu_link` board support does the board and USB work.** It carries the gcc toolchain
file, startup code, linker script, clock setup and the LPC55 `lpc_ip3511` device driver, all
maintained upstream. Using it is why this is a few hundred lines rather than a port.

**ARM's CMSIS-DAP sources compile as they ship.** The only accommodation is `-Wno-unused-variable`
on their files, because `DAP.c` leaves a variable unused when SWO is configured out and TinyUSB's
example flags are `-Werror`. No patches to upstream source.

The code that is ours is board and protocol specific: the pin layer, the bit paths and the
generators that write them at run time, the two-pin framing, the USB descriptors, and the command
path between the two cores.

The MS OS 2.0 descriptor set is **built at boot rather than hand-written**: it has nested length
fields that must agree with the bytes after them, nothing reports a mismatch at runtime, and the
device simply fails to bind. Which vendor interfaces it binds, and at which interface numbers,
follows the jumpers. `tools/ms_os_20_test.c` checks every length field under sanitizers, and
`tools/image.py` checks each strap state's set against its configuration.

## Board facts

| signal | pin |
| --- | --- |
| TCK/SWCLK | P0_0 |
| TDI | P0_1 |
| TMS/SWDIO | P0_2 |
| TDO/SWO | P0_3 |
| RESET_TXEN | P0_13 |
| nRESET | P0_19 |
| TMS_SWDIO_TXEN | P0_28 |
| LED1 | P0_5 |
| target console RXD | P0_24, Flexcomm 0 |
| target console TXD | P0_25, Flexcomm 0 |

SWO arrives on P0_3 as `FC3_RXD`, `IOCON_FUNC1`. That pin is JTAG's TDO as well, so only one of
the two configurations is live at a time: `src/swo.c` hands the pin to Flexcomm 3 while capture
runs and back to the secure GPIO when it stops.

TinyUSB's board support claims Flexcomm 0 for its own debug logging on different pins, so
`vcom_init` runs after `board_init` and takes the peripheral back. The probe has no console of its
own, so nothing is lost.

Source: the board header in ARM's CMSIS-DAP MCU-Link example, checked against the schematic's net
names.

### Console DMA recovery

A DMA failure disables the affected channel. The main loop waits for it to become idle before
restarting it. RX recovery discards the unread receive ring. TX recovery drops the interrupted
buffer without replaying a prefix already handed to the UART. Other channels continue running.

### What the board support does not start

Three clock facts, all from the manual and the board support rather than the bench, each of which
would present as a silent dead peripheral.

**The board support's 150 MHz clock is 144 MHz.** `BOARD_InitBootClocks` calls
`BOARD_BootClockPLL150M`, whose name is wrong: it runs PLL1 at 16 MHz times 18 and attaches that to
MAIN_CLK. `board_core_clock_init` then moves MAIN_CLK to PLL0 at 150 MHz (`src/core_clock.h`;
`-DCORE_150MHZ=OFF` keeps 144). `CPU_CLOCK` and `TIMESTAMP_CLOCK` follow `CORE_CLOCK_HZ`, and every
requested SWD or JTAG clock is computed from that number.

**FRO_HF is off and so is its divider.** The 96 MHz output of FRO192M is disabled out of reset
(UM11126: `ENA_96MHZCLK` resets to 0) and the board support only ever asks for the 12 MHz one,
because the core runs off PLL1. `FROHFDIV`'s HALT bit resets to 1 as well (UM11126 table 104). Both
the console and SWO clock off `FRO_HF_DIV`: with either missing, `CLOCK_GetFroHfFreq` reports 0 Hz,
`USART_Init` rejects the console's baud rate and `SWO_Baudrate` answers 0, and neither says why. `board_clocks_init` starts both. Anything else added here that attaches to
`FRO_HF_DIV` depends on that call.

**DWT is dormant until it is told otherwise.** `TIMESTAMP_GET` reads `DWT->CYCCNT`, which does not
run until TRCENA and CYCCNTENA are set. Two commands need it: `DAP_SWJ_Pins` with a non-zero wait
spins until the counter passes a deadline, so a pin that never reaches the asked level would hang
the probe rather than time out, and a transfer with `DAP_TRANSFER_TIMESTAMP` set would report 0 to
a host that asked for a time. `board_timestamp_init` starts it.

**The two TXEN pins are buffer direction controls and they matter.** Releasing SWDIO means clearing
`TMS_SWDIO_TXEN` as well as making the pin an input: the buffer keeps driving the wire otherwise.
Releasing the wire is exactly what OScan1 needs, so this pin is load-bearing for the whole project.
`board_debug_pins_init` brings both TXEN pins up low before the pins they gate, so connecting the
probe to a running board disturbs nothing until the host asks for a port.

## References

### Related projects

| project | why |
| --- | --- |
| [OpenOCD](https://github.com/openocd-org/openocd) | a host-side OScan1 implementation (`ftdi oscan1_mode on`): a second opinion on the bit encoding |
| [kuopinghsu/cjtag](https://github.com/kuopinghsu/cjtag) | IEEE 1149.7 Class T4 in RTL, with a protocol document |
| [free-dap](https://github.com/ataradov/free-dap) | a tighter CMSIS-DAP implementation than ARM's reference |
| [debugprobe](https://github.com/raspberrypi/debugprobe) | Raspberry Pi's CMSIS-DAP on TinyUSB, the same architecture as this |
| [DAPLink](https://github.com/ARMmbed/DAPLink) | ARM's production probe firmware, for comparison on anything ambiguous |

No open-source probe firmware implementing OScan1 end to end was found.

### In this repo

- `external/tinyusb` — TinyUSB's master with three changes (caller-owned vendor buffers, an
  endpoint bounds check and placeholder endpoints), from a fork until they are upstreamed.
- `external/CMSIS-DAP` — pinned at v2.1.2. `Firmware/Source` has DAP.c, SW_DP.c, JTAG_DP.c, SWO.c
  (not built: `src/swo.c` replaces it), UART.c and DAP_vendor.c; `Firmware/Examples/MCU-LINK/` has the board headers this started from.

