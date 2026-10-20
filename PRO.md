# MCU-Link Pro support: plan (BACKLOG.md item 10)

Written 2026-10-20. Phase 0 is built (`268458f`); the rest is not.

Goal (BACKLOG item 10): one image for the plain MCU-Link and the Pro, with the Pro's extras (second
VCOM, SPI/I2C/GPIO bridge, energy measurement, target supply) reachable from probe-rs, and the LEDs
reworked around the Pro's seven.

## Sources

Public sources only (item 9).

- `references/docs/UM11673.pdf`, the Pro's user manual, Rev 1.2: features, jumpers (Table 1), J19
  pinout (Table 3), energy ranges (Table 4, J16/J17/J18), status LEDs (the second Table 4).
- LinkServer's `/opt/linkserver/MCU-LINK/Readme.txt`: one image for every LPC55S69 MCU-Link; the
  plain board is LQFP64, the Pro LQFP100 or BGA98; the product string is
  `MCU-LINK [Pro|MR|on-board|<board>] (r<RRR>) CMSIS-DAP V<V>.<BBB>`, the Pro reporting `r1**`; the
  Pro's feature jumpers are J13 (SWD and SWO off), J14 (VCOM off) and J15 (USB-SIO off); `HW_VER5`
  high turns energy measurement off.
- DAPLink, Apache-2.0, `references/DAPLink/source/hic_hal/nxp/lpc55xx/IO_Config.h` and
  `source/board/mcu-link.c`: one image for both boards, the strap pins, the Pro test, the Pro's LED
  pins.
- `references/docs/MCU-LINK-PRO-SCH.pdf`, the Pro's schematic, rev A (EAGLE, 8 pages; text in
  `extracted/`), and the rev A design files (`MCU-LINK-PRO-DESIGN-FILES/`: gerbers, assembly,
  DXF), from nxp.com's MCU-LINK-PRO page. Page 1 is the LPC55S69 pin table the
  map below comes from; page 5 has the debug interface and the target supply.

## What the two boards share

- The debug interface pins: P0_0 SWCLK, P0_1 TDI, P0_2 SWDIO, P0_3 TDO/SWO, P0_13 RESET_TXEN,
  P0_14 the target's ISP-EN, P0_19 nRESET, P0_22 detect, P0_28 SWDIO_TXEN, P0_31 VREF (schematic
  page 1). Four-wire JTAG reaches J7: TMS 2, TCK 4, TDO 6, TDI 8, nRESET 10, ISP-EN 7, detect 9
  (page 5). TCK and TDI share one 74LVC2T45 whose direction pin is HW_VER_7, so with J13 fitted
  neither is driven at all, whatever the firmware does. The reference-voltage follower is the
  plain board's, with a 30K1 pull-up for its 33K.
- VCOM on Flexcomm 0, P0_24/P0_25; the status LED on P0_5 (LED3, red, on the Pro); the 16 MHz
  crystal (DAPLink `hic_init.c`); the high-speed USB port.

Strap pins, all inputs needing a pull-up (DAPLink `IO_Config.h`):

| signal | pin | meaning |
| --- | --- | --- |
| HW_VER_0 | PIO1_27 | USB power negotiation when low |
| HW_VER_1 | PIO1_28 | 1-bit board id code |
| HW_VER_2 | PIO1_30 | USB-SIO off when low (Pro J15) |
| HW_VER_3 | PIO1_4 | on-board (0) or Pro (1) |
| HW_VER_4 | PIO1_5 | board id code valid when low |
| HW_VER_5 | PIO1_6 | energy measurement on when low |
| HW_VER_6 | PIO0_18 | VCOM off when low (plain J4, Pro J14) |
| HW_VER_7 | PIO0_27 | SWD off when low (plain J5, Pro J13) |

A Pro reads HW_VER_4 low and HW_VER_3 high. The plain board's 64-pin package (HTQFP64) bonds
neither PIO1_4 nor PIO1_5 (`LPC55S6x.pdf` Table 3), so with their pull-ups both read high: not a
Pro. HW_VER_4 low
with HW_VER_3 low is an on-board MCU-Link, which this firmware does not support.

The rest of the Pro's pins, from schematic page 1:

| what | pins |
| --- | --- |
| LEDs, active low | LED1 USB PIO1_12, LED2 SWO PIO1_13, LED3 status P0_5 (red), LED4 VCOM PIO1_15, LED5 FUNC PIO1_16, LED6 SIO PIO1_17, LED7 NRG PIO1_18 |
| target supply | P0_8 TARGET_PWR_EN enables a TPS73601; its output reaches the target only through J6, at the voltage J5 picks |
| second VCOM (J26) | Flexcomm 1: RXD PIO1_10, TXD PIO1_11. J26 is a 1x6 header labelled "firmware debug console" in the FTDI TTL-232R-3V3 cable's order (page 3): 1 GND, 4 the cable's TXD into PIO1_10 through 499R, 5 PIO1_11 out to the cable's RXD through 499R, 2, 3 and 6 not connected. ESD diodes, no level shifter: 3.3 V only. So a target's TXD goes to pin 4 and its RXD to pin 5; UM11673 section 4.3 says the reverse, which the schematic contradicts. Check on the bench before wiring a target |
| USB-SIO SPI | the high-speed SPI: SSEL0 P0_20, MOSI P0_26, SCK PIO1_2, MISO PIO1_3, each also a GPIO (DIO0 to DIO3); level-shifter directions IO_DIR1 PIO1_1 and IO_DIR2 PIO1_25 |
| USB-SIO I2C | Flexcomm 3: SDA P0_4, SCL P0_21. SWO's UART is Flexcomm 3 too, so the two exclude each other |
| energy measurement | voltage ADC0_0 on P0_23; current ADC0_8 on P0_16 and ACMP0_IN_D on PIO1_14; CURR_RANGE_SET ADC0_4 on PIO1_8; CTRL_HIGH_CURR on P0_15 (ADC0_2) and P0_30 (input); CTRL_HIGH_CURR_EN P0_17; CTRL_HIGH_CURR_DIS P0_29 (CMP0_OUT); calibration CTRL_CAL_R1 to R4 on PIO1_22, PIO1_23, PIO1_26, PIO1_29 and CTRL_CAL_DISC on PIO1_24 |
| triggers | SYNC_TRIG P0_6, SYNC_LEAD P0_7 (J2), MEAS_TRIG PIO1_7 |
| analog trace input | ANALOG_IN1, ADC0_11 on PIO1_0, behind a voltage follower |
| LPC804 | reset PIO1_9, ISP enable PIO1_31, its ISP UART on Flexcomm 4: TXD PIO1_20, RXD PIO1_21 |

## Hazard: do not flash today's image on a Pro

- `MARKER` is on by default (`CMakeLists.txt:40`). It drives PIO1_0 to PIO1_3 push-pull, high during
  every command (`src/marker.h`, `src/board_pins.c`), and `ID_VENDOR_J8` hands them to the host. On
  the plain board they reach only J8. On the Pro they are the analog trace input (the follower's
  output), IO_DIR1 (a level shifter's direction toward J19) and the USB-SIO SPI's SCK and MISO:
  the marker would drive against the follower and MISO, and turn a buffer round toward the
  target.
- TinyUSB's `BOARD_InitBootPins` (the `mcu_link` board) muxes port 1 pins to peripheral functions
  with pull-ups: USB host PORTPWRN and OVERCURRENTN on PIO1_12, PIO1_29, PIO1_30; Flexcomm 6 and
  CTimer on PIO1_13 and PIO1_16; Flexcomm 4 on PIO1_20/21; MCLK on PIO1_31. On the Pro PIO1_12,
  PIO1_13 and PIO1_16 are LEDs and PIO1_30 is HW_VER_2. Those peripherals are not started, so
  nothing should drive. The board layer should still set every port 1 pin it owns per board rather
  than inherit that mux.
- The same BSP puts Flexcomm 0 on P0_29 and P0_30 for its log console, and `src/vcom.c` later makes
  both pull-up inputs. On the Pro P0_29 is CTRL_HIGH_CURR_DIS, which the LPC55S69 drives from its
  comparator, and P0_30 is CTRL_HIGH_CURR, an input from the measurement circuit; if the BSP's
  console ever transmits, its TXD on P0_30 drives against that circuit. Check before the first
  boot on a Pro.

## Plan

Phase 0, no Pro needed. Done 2026-10-20 (`268458f`): the gate's `check_board` runs the detection
with each board's straps, break-tested three ways; on the plain bench probe 0x82 answers format 3,
flags 0x3d, board 0. Today's image (`1cde223` on) is safe to flash on a Pro as far as J8 goes; the
P0_30 check below is still open.


1. Board detection on CPU 0 at boot, before USB: the strap pins read once into `dap_shared` for
   CPU 1.
2. The marker and `ID_VENDOR_J8` on the plain board only, decided at run time so the image stays one.
3. The board in vendor 0x82's reply: one byte after the build string (0 plain, 1 Pro), the format
   raised to 3. probe-rs ignores the format byte and
   anything after the build string, so it needs no change. Three conditions, all on this side:
   - Fields are only ever appended. probe-rs reads any layout as format 2, so moving the capability
     byte or the build string would be misread silently. `src/dap_vendor.c`'s layout comment says
     so.
   - A reader takes the board byte only at format 3 or above. Over a v1 HID read the reply is padded
     to the report, so a format 2 image has a byte there too (`dap.py` and `verge` read 0x82).
   - Bit 4 (`CJTAG_CAP_OSCAN1_ENTER`) is all probe-rs checks before running cJTAG, so the Pro image
     clears it until OScan1 framing, the buffer turnaround within a frame included, has worked on
     the Pro's pins.
     Confirmed in probe-rs's code (`wip-cmsisdap-cjtag` at `7b17871e1`): with bit 4
     clear, selecting cJTAG fails as an unsupported protocol and probe-rs sends no 0x83 at all.
   `verge` gains the Pro as a batch `--board` and recognises stock's `MCU-LINK Pro` product string.
4. `tools/image.py` runs the boot setup against the register model with both strap states, and
   requires a Pro to leave PIO1_0 to PIO1_3 alone.

Phase 1, the Pro as a probe (needs the Pro on USB, a target on J7, and hands for cables):

5. Flash ours with `verge`: jumper-free from stock V3.122 or later, J4 otherwise.
6. Identify, `DAP_Info`, SWD on the L1306 through J7, then the `swd-1` and `wire-1` matrices. The
   latency measurement at connect covers different level shifters.
7. Four-wire JTAG and two-pin cJTAG on J7. Both are wired (schematic page 5); the two-pin buffer
   turnaround is the part to check, since bit 4 stays clear until it has worked.
8. The LEDs, reworked: a fade that looks linear to the eye, new timing, a better
   activity marker. A fade on more than P0_5 needs a timer match or SCT output on each LED pin:
   PIO1_12 has CTIMER1_MAT1 and PIO1_16 CTIMER1_MAT3 (from the BSP's pin table); check the rest in
   UM11126.
9. The probe's own LPC55S69 through the Pro's J3, debugged by the plain MCU-Link: a bring-up aid for
   faults on the Pro.

Phase 2, the extras (needs decisions b and c below):

10. The second VCOM on J26 (unbuffered, 3.3 V at most): a second CDC ACM interface.
11. Target supply: P0_8 enables the regulator, J6 joins its output to the target and J5 picks
    1.8 or 3.3 V. Firmware switches it; the jumpers decide whether that reaches the target.
12. Energy measurement: the ADC front end at up to 100 kS/s; per configuration, two ranges that
    hardware switches up and firmware switches back down on a monitor interrupt; self-calibration
    at power-up behind isolating transistors.
13. USBSIO, the SPI/I2C/GPIO bridge. NXP publishes its host library, LIBUSBSIO, so its protocol can
    be met from public source (check that library's licence first).
14. The analog trace input (J19 pin 12) and the LPC804's ISP and reset lines: last.

## Next round: board-aware USB (plan, not started)

Written 2026-10-20. It covers decisions d and e and the second
VCOM from c, and needs no Pro until step 7. Phase 1's probe work on J7 (items 5 to 9 above) follows
it.

What the code does today: `src/usb_descriptors.c` has one static configuration (vendor interface 0
on EP1, CDC on 0x82 and EP3, HID v1 on EP4), the product string is `DAP_PRODUCT_NAME` in
`src/DAP_config.h`, read by CPU 0 for USB and by CPU 1 for `DAP_Info`, and CPU 1 alone reads the
straps, in `board_debug_pins_init`.

Constraints found while planning:
- USB1, the high-speed controller, has six logical endpoints counting control (UM11126 §43.2): five
  IN and five OUT. In use: IN 0x81 0x82 0x83 0x84, OUT 0x01 0x03 0x04. A second CDC needs two more
  INs (notification and data), so with the HID v1 interface it does not fit.
- Stock on the Pro (observed 2026-10-20):
  seven interfaces and no HID v1. EP1 to EP5 carry data in both directions (CMSIS-DAP v2, a HID
  for trace and power, the two VCOMs' data, the LPCSIO HID), and the two CDC notification
  endpoints are EP6 IN and EP7 IN, numbers USB1 does not have, polled every 4.1 s. On Linux an
  open VCOM's notification read stays pending with no error, also with both VCOMs looped back
  at 3 Mbaud at once (clean on both). TinyUSB's IP3511 driver does not
  bounds-check endpoint numbers, so doing the same needs our TinyUSB fork to keep the CDC class
  from opening them.
- PIO1_10 and PIO1_11 (Flexcomm 1, J26) are not bonded on the plain board's 64-pin package, so the
  second VCOM exists on the Pro only.
- The BSP muxes PIO1_30 (the Pro's HW_VER_2) to USB1_OVERCURRENTN; reading straps needs each pin
  set to GPIO with its pull-up, after `board_init` and before `tusb_init`.
- `dap_shared` is exactly 0x3000 bytes, but only from alignment: its header has slack for a byte or
  two (`tools/image.py` checks both images agree on every member's offset).
- CPU 0's per-command cost is the 1-word limit at 8 queued (BACKLOG item 5), and a second CDC adds
  polling to CPU 0's loop. A whole-image A/B cannot resolve a CPU 0 change at 1 word; flag it at
  run time.

Steps:
0. Observe stock, over USB only (decision 6c of item 9: observed behaviour). Done with no jumpers
   (above). Left: with J14 (VCOM off) and with J13 (SWD off); the same on the
   plain board with J4 and J5. Record the interfaces and endpoints, what `DAP_Info` and
   `DAP_Connect` answer with SWD off, and whether J14 removes both VCOMs. Needs the Pro on USB and
   jumpers moved by hand.
1. Read the straps on CPU 0: the board (HW_VER_3, HW_VER_4) and the feature jumpers (HW_VER_6 VCOM
   off, HW_VER_7 SWD off; HW_VER_2 USB-SIO off is read but means nothing until USB-SIO exists). A
   source both images compile replaces `board_detect` in `src/board_pins.c`; CPU 0 writes the board
   and the jumpers into `dap_shared` before `start_core1`, and CPU 1 reads them there rather than
   reading the pins again.
2. Descriptors built at boot from the board and the jumpers: the vendor interface stays interface 0
   on EP1 (the MS OS 2.0 descriptor names interface 0, and hosts find CMSIS-DAP by its string); the
   CDC drops out with VCOM off; on the Pro a second CDC on Flexcomm 1. Endpoints per the decision
   below. `CFG_TUD_CDC` becomes 2.
3. The product string names the board, from one table both cores read through `dap_shared`, for USB
   and for `DAP_Info` 0x02. Until item 9 picks the form, "MCU-Link Pro Verge CMSIS-DAP" on a Pro.
4. SWD off: CPU 1 refuses `DAP_Connect` (and whatever else step 0 shows stock refusing).
5. `src/vcom.c` serves two instances: Flexcomm 0 as now, Flexcomm 1 (PIO1_10 RXD, PIO1_11 TXD,
   function 2) on the Pro.
6. Checks: `tools/image.py` builds the descriptors for every board and jumper state and holds each
   to its totals and endpoint budget, models CPU 0's strap read as `check_board` models CPU 1's,
   and requires `DAP_Info` 0x02 to match the USB product string per board. The gate on both
   compilers. On the bench plain probe: enumeration, the console, J4 and J5 by hand (check first
   whether either is fitted today), and 1-word commands at 8 queued with and without the change,
   through a run-time flag.
7. On the Pro (phase 1): flash, enumerate, both VCOMs looped back, the jumpers.

Decided 2026-10-20: both boards have the same endpoints; the plain MCU-Link is a Pro with
features disabled. Every CDC notification endpoint is declared at EP6 IN or EP7 IN, as stock does
on the Pro, with interval 16, and never opened: the firmware sends no notifications today
(`CFG_TUD_CDC_NOTIFY` 0), the boards wire no modem lines, and a never-armed real endpoint looks the
same to a host. The layout: EP1 CMSIS-DAP v2, EP2 the LPCSIO HID (Pro, after publishing), EP3 VCOM
data, EP4 HID v1, EP5 VCOM2 data (Pro), EP6 and EP7 the notifications. This moves the plain board's
notification off its real EP2, which the host polls every 125 us today (TinyUSB's
`TUD_CDC_DESCRIPTOR` uses interval 1). The TinyUSB fork needs the CDC class not to open the
notification endpoint when notifications are compiled out, and, as a fix in its own right, the
IP3511 driver to refuse endpoint numbers beyond the controller's six. Open: whether a disabled
function's interface is absent (stock removes a jumper-disabled VCOM, and later interface numbers
shift) or present and inert.
Cross-boundary: none for probe-rs if the vendor interface keeps its number and string; `verge`
learns the Pro's product string (it identifies ours by 0x82 already).

## Decisions

a. The schematic: done, `references/docs/MCU-LINK-PRO-SCH.pdf`.
b. How the extras reach a host: the second VCOM as a standard CDC interface;
   USB-SIO in NXP's public LIBUSBSIO protocol, so NXP's tools and library work; target supply and
   energy measurement as vendor commands on the CMSIS-DAP interface, with probe-rs support.
c. Before publishing: phases 0 and 1 and the second VCOM. USB-SIO, the target
   supply, energy measurement, the analog input and the LPC804 come after publishing.
d. The feature jumpers: honour them as stock does, read once at boot. Pro J13
   (SWD and SWO off), J14 (VCOM off), J15 (USB-SIO off); plain J5 (SWD) and J4 (VCOM). On the Pro
   J13 also turns U5 round in hardware.
e. The product string: it names the board, as stock adds "Pro", in the form item
   9 chooses once "MCU-Link" comes out. CPU 0 then reads the straps before USB starts, so the
   detection in `src/board_pins.c` moves where both cores can use it.
