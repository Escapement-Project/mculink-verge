# The MCU-Link Pro

One image runs on the plain MCU-Link and the MCU-Link Pro. It reads the board's straps at boot
and starts what that board has. This file has the Pro's pins and straps, and what the firmware
does with them.

## Sources

- NXP's UM11673, the Pro's user manual, Rev 1.2: features, jumpers (Table 1), the J19 pinout
  (Table 3), energy ranges and status LEDs (the two Tables 4).
- The Readme in LinkServer's MCU-LINK directory: one image for every LPC55S69 MCU-Link, the
  product string's form, and the Pro's feature jumpers (J13 SWD and SWO off, J14 VCOM off, J15
  USB-SIO off).
- DAPLink (Apache-2.0), `source/hic_hal/nxp/lpc55xx/IO_Config.h` and `source/board/mcu-link.c`:
  the strap pins, the Pro test, the Pro's LED pins.
- The Pro's schematic, rev A, from nxp.com's MCU-LINK-PRO page. Page 1 is the LPC55S69 pin table
  the map below comes from; page 5 has the debug interface and the target supply.

## What runs on a Pro

- **As a probe**, everything the plain board does: SWD, four-wire JTAG on J7, two-pin cJTAG, SWO,
  and the target console. The feature jumpers work as README.md "Jumpers" describes.
- **The second serial port** on J26, a second CDC interface. 3.3 V only: J26 has no level shifter.
- **The DIO pins** on J19, driven from the host (README.md "The Pro's DIO pins").
- **SPI and I2C** through CMSIS-DAP vendor commands `0x91` and `0x92`, not a separate USB-SIO
  endpoint. SPI uses DIO0–DIO3 and is unavailable while a DIO pin is an output. I2C uses Flexcomm 3,
  which also drives SWO, so turn SWO off before using I2C. J15 disables both bridges.
- **The target supply** is switched by the host. J5 selects 1.8 V when shorted or 3.3 V when open;
  J6 must connect the supply to the target. The regulator is rated to 400 mA. See HOST-INTERFACE.md
  for the command.
- **The LEDs**, with the meanings UM11673 gives them where the function exists. LED1 is lit while
  USB is configured. The status, SWO and VCOM LEDs flicker with traffic. LED5 (FUNC) shows the
  wire by the shape of its ramp: dark to light for SWD, light to dark for two-pin cJTAG, light to
  dark to light for four-wire JTAG, solid with the SWD jumper fitted, dark with no port connected.
  LED6 (SIO) is lit while the host drives a DIO pin or runs SPI. LED7 (energy) stays dark. The fades
  follow CIE lightness, so they look linear to the eye.

- **Energy capture** is available on Pro units with HW_VER_5 fitted. The host can start a background
  ADC capture and read raw voltage/current codes, calibrated voltage/current, and accumulated
  energy. Calibration is verified with the internal loads in both the 350 mA and 50 mA jumper
  settings. The accumulator updates without host polling and remains readable after capture stops.

Not yet: the analog trace input and the LPC804.

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
| SPI | the high-speed SPI: SSEL0 P0_20, MOSI P0_26, SCK PIO1_2, MISO PIO1_3, each also a GPIO (DIO0 to DIO3) on J19 pins 2 to 5; level-translator directions IO_DIR1 PIO1_1 (DIO0 to DIO2, U17 and U13) and IO_DIR2 PIO1_25 (DIO3, U12), high to drive the header, each with a 1K pull-down (netlist, 2026-10-22). The header side runs at J19 pin 14 (VDD_IO_REF, 1.2 to 5.5 V), or 3.3 V with nothing there. The host uses vendor command 0x91; it temporarily owns the DIO pins |
| I2C | J19 pin 6 SDA P0_4, pin 7 SCL P0_21, through a PCA9617. R94/R95 pull up the LPC side to 3.3 V; the target or fixture must pull up the J19 side to pin 14's I/O reference. SWO's UART is Flexcomm 3 too, so the two exclude each other. The host uses vendor command 0x92 |
| energy measurement | voltage ADC0_0/CH0A on P0_23; current ADC0_8/CH0B on P0_16 and ACMP0_IN_D on PIO1_14; CURR_RANGE_SET ADC0_4 on PIO1_8; CTRL_HIGH_CURR on P0_15 (ADC0_2) and P0_30 (input); CTRL_HIGH_CURR_EN P0_17; CTRL_HIGH_CURR_DIS P0_29 (CMP0_OUT); calibration CTRL_CAL_R1 to R4 on PIO1_22, PIO1_23, PIO1_26, PIO1_29 and CTRL_CAL_DISC on PIO1_24 |
| triggers | SYNC_TRIG P0_6, SYNC_LEAD P0_7 (J2), MEAS_TRIG PIO1_7 |
| analog trace input | ANALOG_IN1, ADC0_11 on PIO1_0, behind a voltage follower |
| LPC804 | reset PIO1_9, ISP enable PIO1_31, its ISP UART on Flexcomm 4: TXD PIO1_20, RXD PIO1_21 |

