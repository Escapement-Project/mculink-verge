# MCU-Link Pro support: plan (BACKLOG.md item 10)

Written 2026-10-20. Nothing here is built.

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
- Missing: the Pro's schematic, "MCU-Link Pro Schematics" in the design files on nxp.com's
  MCU-LINK-PRO page, behind a sign-in. UM11673 cites its page 5 for the level shifters and target
  power. Everything in phase 2, and the hazard below, waits for it.

## What the two boards share

- The debug interface pins. DAPLink drives both boards from one pin header, and NXP ships one image:
  P0_0 SWCLK, P0_1 TDI, P0_2 SWDIO, P0_3 TDO/SWO, P0_13 RESET_TXEN, P0_19 nRESET, P0_22 detect,
  P0_28 SWDIO_TXEN, P0_31 VREF. DAPLink only proves the SWD subset reaches J7. UM11673 mentions
  only SWD and SWO, so whether TDI and TDO reach J7 is a schematic question; two-pin cJTAG needs
  neither.
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

Pro LEDs, all active low: LED1 USB PIO1_12, LED2 SWO PIO1_13, LED3 status P0_5, LED4 VCOM PIO1_15,
LED5 FUNC PIO1_16, LED6 SIO PIO1_17, LED7 NRG PIO1_18.

## Hazard: do not flash today's image on a Pro

- `MARKER` is on by default (`CMakeLists.txt:40`). It drives PIO1_0 to PIO1_3 push-pull, high during
  every command (`src/marker.h`, `src/board_pins.c`), and `ID_VENDOR_J8` hands them to the host. On
  the plain board they reach only J8. The Pro's 100-pin package bonds them too, and what they
  connect to there is unknown until the schematic.
- TinyUSB's `BOARD_InitBootPins` (the `mcu_link` board) muxes port 1 pins to peripheral functions
  with pull-ups: USB host PORTPWRN and OVERCURRENTN on PIO1_12, PIO1_29, PIO1_30; Flexcomm 6 and
  CTimer on PIO1_13 and PIO1_16; Flexcomm 4 on PIO1_20/21; MCLK on PIO1_31. On the Pro PIO1_12,
  PIO1_13 and PIO1_16 are LEDs and PIO1_30 is HW_VER_2. Those peripherals are not started, so
  nothing should drive. The board layer should still set every port 1 pin it owns per board rather
  than inherit that mux.

## Plan

Phase 0, no Pro needed:

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
   `verge` gains the Pro as a batch `--board` and recognises stock's `MCU-LINK Pro` product string.
4. `tools/image.py` runs the boot setup against the register model with both strap states, and
   requires a Pro to leave PIO1_0 to PIO1_3 alone.

Phase 1, the Pro as a probe (needs the Pro on USB, a target on J7, and hands for cables):

5. Flash ours with `verge`: jumper-free from stock V3.122 or later, J4 otherwise.
6. Identify, `DAP_Info`, SWD on the L1306 through J7, then the `swd-1` and `wire-1` matrices. The
   latency measurement at connect covers different level shifters.
7. Four-wire JTAG and two-pin cJTAG on J7, once the schematic says what reaches it.
8. The LEDs, reworked: a fade that looks linear to the eye, new timing, a better
   activity marker. A fade on more than P0_5 needs a timer match or SCT output on each LED pin:
   PIO1_12 has CTIMER1_MAT1 and PIO1_16 CTIMER1_MAT3 (from the BSP's pin table); check the rest in
   UM11126.
9. The probe's own LPC55S69 through the Pro's J3, debugged by the plain MCU-Link: a bring-up aid for
   faults on the Pro.

Phase 2, the extras (needs the schematic and decisions a to c below):

10. The second VCOM on J26 (unbuffered, 3.3 V at most): a second CDC ACM interface.
11. Target supply: J6 enables it and J5 picks 1.8 or 3.3 V, both by jumper. Whether firmware also
    switches it is a schematic question.
12. Energy measurement: the ADC front end at up to 100 kS/s; per configuration, two ranges that
    hardware switches up and firmware switches back down on a monitor interrupt; self-calibration
    at power-up behind isolating transistors.
13. USBSIO, the SPI/I2C/GPIO bridge. NXP publishes its host library, LIBUSBSIO, so its protocol can
    be met from public source (check that library's licence first).
14. The analog trace input (J19 pin 12) and the LPC804's ISP and reset lines: last.

## Decisions

a. The schematic: download it from nxp.com into `references/docs/`.
b. How the extras reach a host: the standard class where one exists (CDC for the second VCOM);
   NXP's protocol where it is public (LIBUSBSIO); our own vendor commands on the CMSIS-DAP
   interface, with probe-rs support, for target supply and energy
   measurement. MCUXpresso IDE's energy view speaks a protocol NXP has not published, so matching
   it would mean reading the stock image, which item 9 rules out.
c. Which extras come before publishing: all of phase 2, or phases 0 and 1 with the second VCOM and
   the rest after.
d. Whether to honour the feature jumpers (J13/J14/J15 on the Pro, J4/J5 on the plain board) as
   stock does. Ours reads none of them today.
e. The product string per board, decided with item 9's change to it.
