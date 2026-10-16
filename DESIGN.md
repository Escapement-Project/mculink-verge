# MCU-Link cJTAG firmware — design and references

What this firmware is for, what is built so far, what is known about the hardware and the stock
image, and where every reference lives. Written so that nothing established by measurement has to
be rediscovered.

Last updated 2026-10-16.

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

## Current state

Builds. `cmake -B build -G Ninja -DBOARD=mcu_link -DTOOLCHAIN=gcc . && ninja -C build` produces
`build/mculink_cjtag.{elf,hex,bin}`, 24,796 bytes of text.

**Nothing has been flashed or run on hardware yet.** The bench is unavailable; everything below
about the stock firmware comes from captures taken before it went away.

Implemented: the vendor-class (CMSIS-DAP v2) USB interface, the MCU-Link pin layer, the serial
number, ARM's DAP, SW_DP and JTAG_DP compiled unchanged, and the cJTAG activation sequence and
frame primitive behind two vendor commands.

All five JTAG entry points are routed. **None of it has run on hardware.**

### Parity against the stock image

| | stock | ours |
| --- | --- | --- |
| the 25 DAP commands, SWO included | yes | yes |
| `DAP_UART_*` (0x1F..0x23) | **no** | no |
| vendor 0x80, 0x81 | yes, LinkServer-specific | no |
| vendor cJTAG commands | no | 0x82, 0x83 |
| CMSIS-DAP v2, bulk, 512-byte packets | yes | yes |
| CMSIS-DAP v1, HID | yes | yes |
| CDC virtual COM port | yes | yes |

**HID is implemented here, and correctly, whatever stock turns out to do.** The two interfaces
cannot share a packet size: a HID report is bounded by its interrupt endpoint at 64 bytes while
bulk carries 512, and `DAP_Info` builds its reply from one constant with no idea which interface
asked. The HID path corrects the reply on the way out, so a v1 host reads 64 and a v2 host reads
512 from the same command layer.

Stock's own HID interface may not answer DAP at all -- its packet-size reply is a fixed 512,
which no v1 host could use, and the bench settles that. Either way ours is
a superset, so it is not a parity gap.

**The only remaining difference is stock's two vendor commands.**

Interface numbering differs from stock, which has HID between the vendor and CDC interfaces. Ours
is vendor, CDC, CDC data, and adding HID will append rather than insert. Hosts enumerate by
descriptor, so this is cosmetic.

### How OScan1 is wired in

ARM's `JTAG_DP.c` is compiled **twice**, and the wire framing is the only thing that differs:

- normally, with its five entry points renamed `jtag4_*`;
- again through `src/cjtag_jtag_dp.c`, renamed `cjtag_*`, with the pin layer shadowed so each
  four-wire clock becomes one OScan1 frame.

`src/cjtag_dp.c` owns the real names and picks a path per call. Neither path carries a per-bit
branch, and neither duplicates the other's TAP walking, `ir_before`/`ir_length` accounting or
transfer retry logic. The second copy costs about 2.9 KB.

**The contract this rests on** is that `JTAG_DP.c` expresses every clock through four macros which
expand, in order, to `PIN_TDI_OUT` (sometimes), `PIN_TCK_CLR`, `PIN_DELAY`, `PIN_TDO_IN`
(sometimes), `PIN_TCK_SET`, `PIN_DELAY`. Shadowing those names stages TMS and TDI, opens a frame on
`PIN_TCK_CLR`, and emits it at whichever of `PIN_TDO_IN` or `PIN_TCK_SET` comes first. The
submodule is pinned to a release, so the order cannot change without someone bumping the tag.

### Vendor commands

| id | | |
| --- | --- | --- |
| 0x82 | `CJTAG_INFO` | answers "CJTAG", a format version and whether OScan1 is active. Read-only, so a host may send it blindly: stock firmware answers `ID_DAP_Invalid`, which is how a host tells the two images apart. |
| 0x83 | `CJTAG_CONTROL` | one byte, non-zero activates OScan1 and zero returns to four-wire framing. |

Stock's 0x80 and 0x81 do something unknown and are **not implemented here**, so any LinkServer
feature resting on them will not work against this firmware.

## Architecture, and why

**TinyUSB's `mcu_link` board support does the board and USB work.** It carries the gcc toolchain
file, startup code, linker script, clock setup and the LPC55 `lpc_ip3511` device driver, all
maintained upstream. Using it is why this is a few hundred lines rather than a port.

**ARM's CMSIS-DAP sources compile as they ship.** The only accommodation is `-Wno-unused-variable`
on their files, because `DAP.c` leaves a variable unused when SWO is configured out and TinyUSB's
example flags are `-Werror`. No patches to upstream source.

So the code that is ours is small and all of it is board or protocol specific:

| file | what it is |
| --- | --- |
| `src/DAP_config.h` | ARM's MCU-Link config, with its MCUXpresso board includes replaced |
| `src/board_pins.h/.c` | the debug interface pin map and its idle state |
| `src/usb_descriptors.c` | CMSIS-DAP v2 vendor interface, WinUSB binding, strings |
| `src/ser_num.c` | serial number from the part's unique id |
| `src/main.c` | one DAP command per USB packet |

The MS OS 2.0 descriptor is **generated by a script rather than hand-written**: it has three nested
length fields that must agree with the bytes after them, nothing reports a mismatch at run time, and
the device simply fails to bind. A `TU_VERIFY_STATIC` checks the total at compile time.

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
the two configurations is live at a time and the mux follows the USART driver's lifecycle.

TinyUSB's board support claims Flexcomm 0 for its own debug logging on different pins, so
`vcom_init` runs after `board_init` and takes the peripheral back. The probe has no console of its
own, so nothing is lost.

Source: the board header in ARM's CMSIS-DAP MCU-Link example. **Independently confirmed** against
the schematic's net names.

**The two TXEN pins are buffer direction controls and they matter.** Releasing SWDIO means clearing
`TMS_SWDIO_TXEN` as well as making the pin an input: the buffer keeps driving the wire otherwise.
Releasing the wire is exactly what OScan1 needs, so this pin is load-bearing for the whole project.
`board_debug_pins_init` brings both TXEN pins up low before the pins they gate, so connecting the
probe to a running board disturbs nothing until the host asks for a port.

## The stock firmware

`MCU-LINK_CMSIS-DAP_V3_172.s19`, shipped by LinkServer at
`MCU-LINK/probe_firmware/` in its install.

**It is the image the bench probe runs**: `DAP_Info` over USB returns "NXP Semiconductors" and
"MCU-Link CMSIS-DAP V3.172". ARM's
reference example returns length 0 for both, so that example is a reference and not the thing
measured.

### The parity list

These are ARM's standard commands, which its reference answers:

```
0x00 Info            0x01 HostStatus      0x02 Connect         0x03 Disconnect
0x04 TransferConfig  0x05 Transfer        0x06 TransferBlock   0x08 WriteABORT
0x09 Delay           0x0A ResetTarget     0x10 SWJ_Pins        0x11 SWJ_Clock
0x12 SWJ_Sequence    0x13 SWD_Configure   0x14 JTAG_Sequence   0x15 JTAG_Configure
0x16 JTAG_IDCODE     0x17 SWO_Transport   0x18 SWO_Mode        0x19 SWO_Baudrate
0x1A SWO_Control     0x1B SWO_Status      0x1C SWO_Data        0x1D SWD_Sequence
0x1E SWO_ExtendedStatus
```

plus `0x7F ExecuteCommands`. **Our build has to answer all of these or it is a downgrade.**


### Stock's own vendor commands

**Not implemented here, and that is a decision rather than a gap.**

Nothing in probe-rs asks for either command, and the CMSIS-DAP specification leaves 0x80..0x9F
entirely to the probe, so there is no standard being violated by answering `ID_DAP_Invalid`.

### The vendor range

Stock answers 0x80 and 0x81 in the vendor range, so **0x82 through 0x9F are free**, which is where the cJTAG
vendor command goes with no risk of colliding with something LinkServer relies on.

## Measurements, and what the bar actually is

Stock firmware against an MSPM0L1306, measured with `swd-throughput`. KiB/s at 4096 bytes:

| kHz | read32 | write32 | read8 | write8 | effective MHz |
| --- | --- | --- | --- | --- | --- |
| 1000 | 75.7 | 75.9 | 19.3 | 19.2 | 0.89 |
| 5000 | 280.6 | 290.9 | 72.9 | 75.2 | 3.30 |
| 10000 | 469.1 | 502.9 | 124.8 | 132.3 | 5.52 |
| 14000 | 475.4 | 501.2 | 124.5 | 132.4 | 5.60 |

Round trip bottoms at 68 us. On the wire: 0.94 MHz actual for 1 MHz requested at 97% duty, 7.14 MHz
for 14 MHz requested at 90% duty. The two period histograms have the same shape offset by exactly
920 ns in every class, which is the programmable delay; what remains is the loop.

**476 KiB/s read and 502 KiB/s write is the bar, with a caveat that matters.** 14 MHz was the
highest clock the target would attach at.
This is not the probe's ceiling.

Captures: `mculink-swd-clock-*.sal`.

## Next steps

1. Route the JTAG entry points through `cJTAG_Frame` when the mode flag is set. The plan is to
   rename ARM's symbols at compile time with `-DJTAG_Sequence=jtag4_Sequence` and friends, and
   define the real names in our own file that dispatches on the flag. That keeps ARM's file
   unpatched and keeps the four-wire path free of a per-bit branch.
2. Flash and enumerate. Nothing has run on hardware. Until it does, every line of this is theory.
3. Bench: confirm probe-rs attaches over SWD, then the OScan1 read on a CC13xx.
4. Parity: SWO, the UART commands, and the HID and CDC interfaces.
5. Then, and only then, throughput.

### Open questions

- The UUID read at 0x0009_FC70 is untested on hardware. If that region faults rather than reading,
  it faults during `GetSerialNum` and the device will not enumerate. The uniform-value guard
  catches an unprogrammed region but cannot catch a bus fault.
- In `cJTAG_Frame`, TDO is sampled while TCKC is low, just before the rising edge, following the
  shape of ARM's `SW_READ_BIT`. Whether the target drives it early enough is an analyser question.
- The OScan1 path pays `PIN_DELAY` twice per frame that it does not need: once from the frame's own
  pacing and once from `JTAG_DP.c`'s macros, whose delays become dead time around a no-op clock.
  Harmless, and roughly doubles the time a two-pin frame takes.

## References

### Cloned under `references/`, gitignored

| repo | why |
| --- | --- |
| `openocd` | **the most useful one.** Has a host-side OScan1 implementation, `ftdi oscan1_mode on`. A second opinion on the bit encoding, from a working implementation, against which our capture decoding can be checked. |
| `cjtag` (kuopinghsu) | IEEE 1149.7 Class T4 in RTL, with a protocol document. 3-bit OScan1 scan packets. |
| `free-dap` | a tighter CMSIS-DAP implementation than ARM's reference; relevant to throughput. |
| `debugprobe` | Raspberry Pi's CMSIS-DAP on TinyUSB — the same architecture as this, useful for the USB glue. |
| `DAPLink` | ARM's own production probe firmware, for comparison on anything ambiguous. |

`stnolting/cjtag_bridge` was in the search results but the repository is gone, so it is not here.

No complete open-source probe firmware implementing OScan1 end to end was found, so that part
appears to be ours to write.

### What OpenOCD's implementation already settles

`references/openocd/src/jtag/drivers/ftdi.c` drives cJTAG activation as an explicit table of
`{TCK, TMS, TDI}` states, each entry commented with what it is doing. Reading it confirms, from a
working implementation and independently of our captures, the distinction the whole firmware change
rests on:

- **Escape edges are driven with TCK held high.** TMSC toggles in a run of states that all
  keep TCK at 1. That is what an escape edge is, and emitting one is deliberate.
- **The activation code bits are shifted with TMSC moving while TCK is low.** One state sets TMSC,
  and the next clocks it. Data never changes while the clock is high.

**That is exactly the rule our SWD bit path breaks**, and it is now confirmed by something other
than our own reading of a capture. ARM's `SW_WRITE_BIT` changes data before pulling the clock low,
so on a 1149.7 wire every data bit also emits an escape edge.

It also gives us the activation sequence as code rather than as something reconstructed from a
capture: the online sequence, then **OAC = 1100** for two wires, then EC, then CP, with
`ESCAPE_SEQ_OAC_BIT2` naming the offset that distinguishes OScan1 from JScan3. OpenOCD supports
both, and the only difference is two TDI bits in that table.

### In this repo

- `external/tinyusb` — pinned at 0.21.0.
- `external/CMSIS-DAP` — pinned at v2.1.2. `Firmware/Source` has DAP.c, SW_DP.c, JTAG_DP.c, SWO.c,
  UART.c and DAP_vendor.c; `Firmware/Examples/MCU-LINK/` has the board headers this started from.

## Bench notes

- **`DAP_TransferAbort` answers nothing.** Reading for a reply after sending it stalls the probe's
  bulk endpoint and every later command times out. `usb.core.Device.reset()` clears it without
  unplugging and does not disturb a halted target.
