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
| USB-SIO SPI | the high-speed SPI: SSEL0 P0_20, MOSI P0_26, SCK PIO1_2, MISO PIO1_3, each also a GPIO (DIO0 to DIO3) on J19 pins 2 to 5; level-translator directions IO_DIR1 PIO1_1 (DIO0 to DIO2, U17 and U13) and IO_DIR2 PIO1_25 (DIO3, U12), high to drive the header, each with a 1K pull-down (netlist, 2026-10-22). The header side runs at J19 pin 14 (VDD_IO_REF, 1.2 to 5.5 V), or 3.3 V with nothing there |
| USB-SIO I2C | Flexcomm 3: SDA P0_4, SCL P0_21. SWO's UART is Flexcomm 3 too, so the two exclude each other |
| energy measurement | voltage ADC0_0 on P0_23; current ADC0_8 on P0_16 and ACMP0_IN_D on PIO1_14; CURR_RANGE_SET ADC0_4 on PIO1_8; CTRL_HIGH_CURR on P0_15 (ADC0_2) and P0_30 (input); CTRL_HIGH_CURR_EN P0_17; CTRL_HIGH_CURR_DIS P0_29 (CMP0_OUT); calibration CTRL_CAL_R1 to R4 on PIO1_22, PIO1_23, PIO1_26, PIO1_29 and CTRL_CAL_DISC on PIO1_24 |
| triggers | SYNC_TRIG P0_6, SYNC_LEAD P0_7 (J2), MEAS_TRIG PIO1_7 |
| analog trace input | ANALOG_IN1, ADC0_11 on PIO1_0, behind a voltage follower |
| LPC804 | reset PIO1_9, ISP enable PIO1_31, its ISP UART on Flexcomm 4: TXD PIO1_20, RXD PIO1_21 |

## Hazard: do not flash today's image on a Pro

Resolved by the board-aware USB round (2026-10-20, below): the board is read on CPU 0 and was
read as a Pro on the bench, J8 stays off there, and the board support's console no longer starts
on P0_29 and P0_30. The image declares the Pro since phase 1 (2026-10-21), so verge puts it there.

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
     cleared it until OScan1 framing, the buffer turnaround within a frame included, had worked on
     the Pro's pins. It has (step 7, 2026-10-21): the Pro sets it now, and only an on-board
     MCU-Link, which nothing has tried, keeps it clear and refuses vendor 0x83 modes 1 and 2.
     Confirmed in probe-rs's code (`wip-cmsisdap-cjtag` at `7b17871e1`): with bit 4
     clear, selecting cJTAG fails as an unsupported protocol and probe-rs sends no 0x83 at all.
   `verge` gains the Pro as a batch `--board` and recognises stock's `MCU-LINK Pro` product string.
4. `tools/image.py` runs the boot setup against the register model with both strap states, and
   requires a Pro to leave PIO1_0 to PIO1_3 alone.

Phase 1, the Pro as a probe (needs the Pro on USB, a target on J7, and hands for cables):

Progress 2026-10-21, the L1306 on J7: the Pro `CJ8C2F60A3` runs `49633f3`, flashed from
stock with `tools/flash.py --cycle` (verge refuses an image that declares the plain board only, as
it should). Format 3, board Pro, flags 0x2c; DAP_Info as expected; target sense 3.294 V with pin 9
grounded; SWD reads of the L1306's factory region at 1, 10, 20 and 50 MHz. `swd-1` through the
Pro: 480 cases, none failed, probe-rs's half too (mculink-bench,
`results/throughput/2026-10-21-pro-gcc.json`). Against the plain board's `4a7ca13` run, 453 of
480 within 3%; every gap over 20% is a 1-word command at depth 8 and 10 to 50 MHz, the Pro ahead
by up to 2.6x. The build, not the board: `49633f3` on the plain board (mculink-bench
`results/throughput/2026-10-21-plain-gcc.json`) is up to 2.4x faster than `4a7ca13` in the same
cases, and the two boards on `49633f3` agree within 3% in 458 of 480. The rest are 1-word commands
at depth 8, whose own runs spread 5 to 33%, but for 32-word transfer reads at 50 MHz (the Pro 7%
faster) and 32-word block reads at 20 MHz (5% slower), runs within 2% on both.
`wire-1` through the Pro (analyser D6 SWDIO, D7 SWCLK at the L1306, 100 MS/s, the Logic 8's
ceiling; mculink-bench `results/wireclock/2026-10-21-pro-gcc.json`, captures in
`wireclock-pro-2026-10-21-grounded/`): no command failed at any clock, and
every clock from 1 to 30 MHz resolves from SWCLK (40 and 50 MHz from SWDIO only, as on the plain
board). Measured minus the emulator (`wireclock.py model --firmware`) is within 0.8 ns at every
position at 8 and 10 MHz but the write turnaround's known 4 cycles (26.7 ns, position 12). Fastest
periods: 7.94 MHz at 8 asked, 10.03 at 10. Item 16's early edge is not on the Pro: the first
RDBUFF after a write starts at 99.9 ns at 10 MHz, as on the plain board once its analyser was
grounded too (BACKLOG item 16).
The first run of the same matrix (captures in `wireclock-pro-2026-10-21/`) had the
analyser's ground floating: at 8 MHz SWDIO carried 2.89 million edges against 1.08 million
grounded, SWCLK the same 3.09 million both times, and SWCLK alternated 113 and 140 ns periods
wherever a spurious SWDIO edge landed on a rise. The re-take replaced that result. Found with a scratch image that mirrors SWCLK on PIO0_7
(J2 pin 3) from the same stores: with the ground floating the
short periods moved between channels with the pairing captured, the firmware unchanged, and
grounded none of the three pairings had any. With D3, D6 and D7 on at 100 MS/s the Logic 8
recorded nothing and Logic 2 reported no error (D1 to D3 at that rate worked before, BENCH.md), so
the pairs were taken one at a time.
Step 7, JTAG on J7 with the CC1312R1 LaunchPad (analyser D0 nRESET, D1 TMSC, D2 TCKC, D4 TDI, D5
TDO; logs and the capture script in `pro-jtag-2026-10-21/`): four-wire reads
CPUID through probe-rs, and 400 queued 127-word ROM blocks match the 1 MHz read at every clock from
1 to 50 MHz (93.5 KiB/s at 1 MHz, 828.5 at 10, 1639.8 at 25, 1591 from 30). Asked to measure at
50 MHz, the probe finds four-wire latency 7, the plain board's figure on this part after nRESET, so
the Pro's buffers add no whole cycle. Two-pin first read garbage at 1 MHz: the image refused vendor
0x83 mode 2 on the Pro, and `dap.py` carried on unframed (it stops on a refusal now). With the gate
lifted, the same blocks match at every clock from 1 to 50 MHz (31.2 KiB/s at 1 MHz, 301.8 at 10,
541.5 at 25, 678.3 at 50), probe-rs reads CPUID over cJTAG, and the measured two-pin latency at
50 MHz is 8, the default. nRESET reaches the target. Left: 8, 9.

5. Flash ours with `verge`: jumper-free from stock V3.122 or later, J4 otherwise.
6. Identify, `DAP_Info`, SWD on the L1306 through J7, then the `swd-1` and `wire-1` matrices. The
   latency measurement at connect covers different level shifters.
7. Four-wire JTAG and two-pin cJTAG on J7. Both are wired (schematic page 5); the two-pin buffer
   turnaround is the part to check, since bit 4 stays clear until it has worked.
8. The LEDs, reworked: a fade that looks linear to the eye, new timing, a better
   activity marker. Only FUNC and the status LED fade: FUNC on CTIMER1_MAT3, PIO1_16's only timer
   output, with match 0 as the period (UM11126 reads as if only matches 0 to 2 can be PWM outputs,
   but match 3 works on the Pro); the others switch, as GPIO.
   Decided 2026-10-21. The Pro's pins (schematic page 4): LED1 USB PIO1_12, LED2 SWO
   PIO1_13, LED3 status PIO0_5 (the plain board's LED), LED4 VCOM PIO1_15, LED5 FUNC PIO1_16, LED6
   SIO PIO1_17, LED7 NRG PIO1_18. Meanings follow UM11673 Table 4 where we have the function; NRG
   stays dark until phase 2, and SIO is lit while the host drives a DIO pin (item 13).
   - The fade follows CIE lightness (L*), so it looks linear to the eye.
   - Activity flickers: each burst of traffic inverts the LED, on the status, SWO and VCOM LEDs.
   - LED5 FUNC shows the wire by its pattern: SWD a repeating dark-to-light ramp, two-pin cJTAG a
     repeating light-to-dark ramp, four-wire JTAG light to dark to light repeating. Solid with
     the SWD jumper fitted (CMSIS-DAP off, the consoles only). Dark with no port connected.
     Every ramp lasts 512 ticks of the 488 Hz LED clock (1.05 s), so they all
     move at one rate and differ only in shape: the JTAG triangle is 1.05 s down and 1.05 s up.
   Built 2026-10-21 (`src/led.c`). Also: the status LED keeps a 4.2 s breath, twice
   the JTAG triangle, so the two are not confused; a flicker inverts for 65 ms then leaves the LED
   alone 65 ms, so steady traffic flickers at 7.6 Hz; SWO activity is trace bytes handed to the host,
   since nothing else on CPU 0 sees them arrive; USB is lit while configured and not suspended.
   Every level goes through each timer's match shadow register, so a write never lands
   mid-period. `tools/image.py` checks each pattern in the emulator. On the Pro FUNC shows its
   three ramps as agreed, and the status and VCOM LEDs flicker once per burst, then steadily
   (2026-10-21).
9. The probe's own LPC55S69 through the Pro's J3, debugged by the plain MCU-Link: a bring-up aid for
   faults on the Pro.
   Done 2026-10-21, reading only (`j3read.py`): the plain
   board on J3 reads the Pro's DPIDR (0x6BA02477), AP0 and AP1 (AHB-APs, CPU 0 and CPU 1), AP2
   (the debug mailbox), both cores' CPUID and DHCSR, and our firmware's RAM, all without halting:
   the LED tick advanced 488 a second as it was read. CPU 1's AP faults a secure access, so its CSW
   needs bit 30 (non-secure): three CSW values without it faulted, both with it read.
   Halting works too (`j3halt.py` beside it): each core halted for half a second,
   its registers read and resumed, and the Pro answered USB after. Idle, CPU 0 stopped in
   `vcom_ready` and CPU 1 in `wait_for_mailbox`; the LED tick stood still only while CPU 0 was
   halted. With the plain board on J3 the Pro started its bootloader at every reset: J3 pin 7 is
   the Pro's ISP pin, and the plain board never drove the P0_14 that feeds it, so its open-drain
   buffer held the pin low. Fixed in `125a6ce` for any NXP target; verge then put `8b90b88` on both
   units with the cable in place.

Phase 2, the extras (needs decisions b and c below):

10. The second VCOM on J26 (unbuffered, 3.3 V at most): a second CDC ACM interface.
11. Target supply: P0_8 enables the regulator, J6 joins its output to the target and J5 picks
    1.8 or 3.3 V. Firmware switches it; the jumpers decide whether that reaches the target.
12. Energy measurement: the ADC front end at up to 100 kS/s; per configuration, two ranges that
    hardware switches up and firmware switches back down on a monitor interrupt; self-calibration
    at power-up behind isolating transistors.
13. USBSIO, the SPI/I2C/GPIO bridge. GPIO first: the four DIO pins through
    vendor 0x8E (`src/dio.c`, the transport-independent core; `src/dap_vendor.c` has the wire
    format). LIBUSBSIO is BSD-3-Clause; its PyPI
    package is a wrapper over prebuilt libraries, so its wire protocol needs NXP's C source. Its
    GPIO calls take raw LPC port and pin numbers and a raw IOCON mode, and its own test suite says
    the standalone Pro has no GPIO under stock. A LIBUSBSIO HID with SPI and I2C may follow; it
    needs probe-rs's HID-as-v1 fix first (Cross-boundary, below). On the Pro 2026-10-22 (`968ea22`):
    with DIO0 looped to DIO3, levels read back
    both ways, one command hands the loop from one group to the other without an edge, a split is
    refused, each header moves once a command and LED6 lights; DIO2 follows DIO0 and DIO1 by about
    40 ns, a store apart on another port.
14. The analog trace input (J19 pin 12) and the LPC804's ISP and reset lines: last.

## Next round: board-aware USB (in progress)

Written 2026-10-20. It covers decisions d and e and the second
VCOM from c, and needs no Pro until step 7. Phase 1's probe work on J7 (items 5 to 9 above) follows
it.

Progress (2026-10-20, committed in `8e4033a`..`0ccc436`): steps 0 to 8 have run. The emulator
checks pass (step 7's `tools/image.py` part, break-tested) with the gate, and both boards ran the
image in every jumper state.
TinyUSB's changes are local commits on our fork: `dcd(ip3511): refuse endpoint numbers past
the controller's own list` on `ip3511-edpt-bounds` (its host test on `ip3511-edpt-bounds-test`)
and `usbd: add placeholder endpoints` on `usbd-edpt-placeholder`, both from upstream `782f0f1cd`;
the firmware's submodule is pinned to `mculink-verge`, `10d1941fe` with both on top. Nothing is
pushed, and no pull request is open. Left: the 1-word A/B, which needs a run-time flag (the
emulator puts the console loop at +14 cycles a pass on the plain board, +28 on the Pro); declaring
the Pro in the image once phase 1 has run a target on J7. Found on the way:
- The board support started Flexcomm 0 as a log console inside `board_init`, on P0_29 and P0_30.
  On the Pro P0_30 is the Q output of U18, the energy measurement's 74LVC1G175, so the console's
  idle TXD drove against a push-pull output from `board_init` until `vcom_init`. The build now
  leaves out the board's `peripherals.c` and `src/vcom.c` supplies an empty
  `BOARD_InitBootPeripherals`. On the Pro `vcom_init` leaves P0_29 (U18's reset) and P0_30 without
  a pull; on the plain board they keep their pull-ups. This was the hazard section's open P0_30
  check.
- The register model treats SYSCON as plain memory, so a driver that resets a peripheral waits
  for ever on PRESETCTRL. `check_clocks` had been timing out silently inside `vcom_init`; it now
  models SYSCON's SET and CLR registers and fails if `vcom_init` does not return.
- An oversized `DAP_TransferBlock` read was clamped and run on whatever path `transfer_block`
  picked, connected or not; with no port it now goes to ARM's handler, which answers it empty.
- With J4 and J5 both fitted the plain board has no function left, and ours offers a configuration
  of no interfaces, which still answers the EP0 reboot request. Stock was not observed that way,
  and no host has seen ours that way.

What the code did before this round: `src/usb_descriptors.c` had one static configuration (vendor interface 0
on EP1, CDC on 0x82 and EP3, HID v1 on EP4), the product string was `DAP_PRODUCT_NAME` in
`src/DAP_config.h`, read by CPU 0 for USB and by CPU 1 for `DAP_Info`, and CPU 1 alone read the
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
  at 3 Mbaud at once (clean on both). TinyUSB refuses such numbers before its IP3511 driver
  sees them: `CFG_TUD_ENDPPOINT_MAX` is 6 on the LPC55 and cannot be raised (`tusb_option.h`),
  `usbd_edpt_open` asserts below it, and so does the driver binding that walks every endpoint a
  class's descriptors name, opened or not (upstream #3828, September 2026). The IP3511 driver's
  missing bound is reachable only at EP5 on a controller with five pairs.
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
0. Observe stock, over USB only (decision 6c of item 9: observed behaviour). Done 2026-10-20 for
   every single jumper on both boards:
   each jumper removes its function's interfaces; J14 removes VCOM and keeps VCOM2; the SWD jumper
   (J13, J5) removes the CMSIS-DAP interface itself, and the trace half of the Pro's HID; stock
   packs its endpoints per state rather than keep them fixed; bcdDevice changes with every state;
   the "rXXX" in the product string is the straps.
1. Read the straps on CPU 0: the board (HW_VER_3, HW_VER_4) and the feature jumpers (HW_VER_6 VCOM
   off, HW_VER_7 SWD off; HW_VER_2 USB-SIO off, which keeps the DIO pins of item 13 off).
   `src/board_straps.c` on CPU 0 writes the board and the jumpers into `dap_shared` before
   `start_core1`, and CPU 1 takes the board from there rather than reading the pins again.
2. Descriptors built at boot from the board and the jumpers: the vendor interface stays interface 0
   on EP1 (the MS OS 2.0 descriptor names interface 0, and hosts find CMSIS-DAP by its string); the
   CDC drops out with VCOM off; on the Pro a second CDC on Flexcomm 1. Endpoints per the decision
   below. `CFG_TUD_CDC` becomes 2. bcdDevice is 0x01NN, NN the functions present (1 the CMSIS-DAP
   interfaces, 2 VCOM, 4 VCOM2): Windows keeps what it learnt about a device by VID, PID and
   bcdDevice, and stock gives each jumper state its own.
3. One image for both boards, as stock: it declares the boards it supports in a
   form `verge` reads from the file before flashing (UPDATE-TOOL.md decision 12), so an older image
   that declares nothing is never put on a Pro. The form (decided 2026-10-20): sixteen bytes right
   after CPU 0's vector table, little-endian: the ASCII magic `VERGEIMG`, a format byte (1), the
   record's length (16), two zero bytes, then a 32-bit mask with bit n set for each board the image
   runs on, numbered as 0x82's board byte. Fields are only appended. verge finds it by the magic,
   not the address; the signed release image carries it, so the release layout does not change.
   The Pro's bit is set since phase 1 finished (2026-10-21).
4. The product string names the board, from one table both cores read through `dap_shared`, for USB
   and for `DAP_Info` 0x02: "Verge Pro CMSIS-DAP" on a Pro (decision e).
5. SWD off (decided 2026-10-20, after step 0): as stock, the SWD jumper removes both
   CMSIS-DAP interfaces, so no host can send a command and nothing reaches CPU 1. The BOS goes with
   them, since its MS OS 2.0 descriptor names the vendor interface, and with no BOS the device is
   USB 2.0. The VCOMs stay, and the EP0 reboot request still works, so `verge` can update the unit;
   it can identify it only by its USB product string. (Planned before step 0, and dropped: keeping
   the interfaces and refusing every command that moves a pin.)
6. `src/vcom.c` serves two instances: Flexcomm 0 as now, Flexcomm 1 (PIO1_10 RXD, PIO1_11 TXD,
   function 2) on the Pro.
7. Checks: `tools/image.py` builds the descriptors for every board and jumper state and holds each
   to its totals and endpoint budget, models CPU 0's strap read as `check_board` models CPU 1's,
   and requires `DAP_Info` 0x02 to match the USB product string per board. The gate on both
   compilers. On the bench plain probe: enumeration, the console, J4 and J5 by hand (neither is
   fitted day to day), the never-opened notification endpoint under usbmon with the
   VCOM open, and 1-word commands at 8 queued with and without the change, through a run-time flag.
8. On the Pro (phase 1): flash, enumerate, both VCOMs looped back, the jumpers.

Decided 2026-10-20: both boards have the same endpoints; the plain MCU-Link is a Pro with
features disabled. Every CDC notification endpoint is declared at EP6 IN or EP7 IN, as stock does
on the Pro, with interval 16, and never opened: the firmware sends no notifications today
(`CFG_TUD_CDC_NOTIFY` 0), the boards wire no modem lines, and a never-armed real endpoint looks the
same to a host. The layout: EP1 CMSIS-DAP v2, EP2 the LPCSIO HID (Pro, after publishing), EP3 VCOM
data, EP4 HID v1, EP5 VCOM2 data (Pro), EP6 and EP7 the notifications. This moves the plain board's
notification off its real EP2, which the host polls every 125 us today (TinyUSB's
`TUD_CDC_DESCRIPTOR` uses interval 1).

Decided 2026-10-20: a disabled function is removed entirely, as stock removes a
jumper-disabled VCOM, never exposed as an interface that does nothing. Interface numbers after it
shift; the always-present ones come first to keep that to a minimum: 0 CMSIS-DAP v2, then VCOM, the
HID v1, VCOM2 (Pro), the LPCSIO HID (Pro, later).

Decided 2026-10-20: the TinyUSB changes are written to be correct and ready for upstream,
not the smallest fix that works here. Separate commits on our fork, the firmware's pin moved
to them, each tested on the bench (the gate never runs the USB stack):
- `dcd_lpc_ip3511.c`: `dcd_edpt_open` refuses an endpoint number at or beyond the controller's own
  `ep_pairs`, which the driver already records per controller from NXP's feature macros (five
  pairs on the LPC11, 13, 15, 51, 54 and the LPC55's USB0, six on its USB1). Today such an address
  indexes past that controller's list, and from EP6 past the 12-entry array. A fix in its own
  right.
- `cdc_device.c`: an opt-in setting, default unchanged, under which the class records the
  notification endpoint without opening it, for a device that declares the endpoint but never
  sends on it (as NXP's own MCU-Link firmware does at endpoint numbers its controller lacks). It
  has meaning only with `CFG_TUD_CDC_NOTIFY` 0.
- Neither gets EP6 or EP7 past usbd's binding check (constraints above). Decided 2026-10-20:
  not by skipping endpoints past the limit, which would let a likely mistake through
  silently, but with an API by which the application registers the endpoints it declares and the
  controller lacks. The design, decided the same day: "placeholder"
  endpoints, registered as IN and OUT bitmaps through TinyUSB's existing `tud_configure()` before
  `tusb_init`, behind `CFG_TUD_EDPT_PLACEHOLDER`. usbd never opens a placeholder on the controller,
  skips it at binding, refuses transfers on it, and answers a host's GET_STATUS and SET or
  CLEAR_FEATURE on it with a Request Error. Stock answers those with "not halted" instead;
  whether Windows' usbser clears halt
  on the notification endpoint when a port opens is for the Windows check. ISO placeholders are
  refused. The CDC setting above is dropped: CDC opens the notification endpoint as usual and usbd
  answers. "Phantom" was the working word; TinyUSB already uses it for audio phantom power.
TinyUSB style: the CDC commit is `cdc:`.
Before writing them: look upstream for prior art (issues, pull requests, discussions about an
optional or unopened CDC notification endpoint), read TinyUSB's contributing guide and CI, and add a
unit test wherever their harness (`test/unit-test`) reaches.
Cross-boundary: `verge` learns the Pro's product string (it identifies ours by 0x82 already) and
reads the image's declaration. Checked against probe-rs 2026-10-20 (`wip-cmsisdap-cjtag`
`7b17871e1`, upstream `cd181bec4`): probe-rs finds v2 by an interface string containing
"CMSIS-DAP" with class 0xFF/0x00 and takes the endpoints in descriptor order, and v1 by a HID
interface on a device whose product string contains "CMSIS-DAP"; no interface number, endpoint
address, VID:PID or `DAP_Info` string is hard-coded, so the layout changes nothing for it. Three
points from it, adopted:
- A probe selector can carry an interface number (`VID:PID-IF:SERIAL`). `probe-rs list` shows the
  v2 entry (`-0`), which never moves, but a hand-written HID selector `-3` stops matching when the
  VCOM jumper moves the HID interface to 1. The jumper documentation says so.
- With the SWD jumper fitted there is no CMSIS-DAP interface, so probe-rs finds no probe.
  probe-rs sets one
  condition: no HID other than CMSIS-DAP v1 on an image whose product string contains
  "CMSIS-DAP", since probe-rs takes any HID of such a device as v1 (it does so with stock's power
  and LPCSIO HIDs, a probe-rs bug). USB-SIO's HID will need this settled first.
- Stock's pending notification read was observed on the Pro. Whether a host sees a NAK (pending) or
  no handshake (transaction errors, and xHCI halts the endpoint) depends on the controller's answer
  to an endpoint it lacks: check it with usbmon on the plain board running ours before relying on
  it (step 7).

## Decisions

a. The schematic: done, `references/docs/MCU-LINK-PRO-SCH.pdf`.
b. How the extras reach a host: the second VCOM as a standard CDC interface;
   USB-SIO in NXP's public LIBUSBSIO protocol, so NXP's tools and library work (amended 2026-10-22,
   GPIO first as a CMSIS-DAP vendor command over a transport-independent core, LIBUSBSIO
   later if at all); target supply and
   energy measurement as vendor commands on the CMSIS-DAP interface, with probe-rs support.
c. Before publishing: phases 0 and 1 and the second VCOM. USB-SIO, the target
   supply, energy measurement, the analog input and the LPC804 come after publishing.
d. The feature jumpers: honour them as stock does, read once at boot. Pro J13
   (SWD and SWO off), J14 (VCOM off), J15 (USB-SIO off); plain J5 (SWD) and J4 (VCOM). On the Pro
   J13 also turns U5 round in hardware.
e. The product string: it names the board, as stock adds "Pro". Without
   "MCU-Link": `Verge CMSIS-DAP` and `Verge Pro CMSIS-DAP`, the vendor
   `Escapement Project` on both, the serial ports `Verge VCOM` and `Verge VCOM2`. CPU 0 then reads the straps before USB starts, so the
   detection in `src/board_pins.c` moves where both cores can use it.
