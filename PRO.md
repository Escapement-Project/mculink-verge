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
- The Pro's schematic, rev A, from nxp.com's MCU-LINK-PRO page. Sheet 3 is the LPC55S69 pin table
  the map below comes from. Sheet 2 has the power and the 3.0 V reference, sheet 5 the debug
  interface and the target supply, and sheet 7 the energy circuit.
- NXP's AN13660, "MCU-Link Energy Measurement Capabilities", Rev 0.1, section 5: how the ranges
  switch, and calibration at power-on.
- NXP's UM11126, the LPC55S6x user manual, Rev 2.8: the comparator (chapter 39, Fig 145, and its
  register in Table 304), analog pin mode (section 15.4.2.5) and the ADC's reference select
  (Table 720). The LPC55S6x data sheet, Rev 2.5: pin functions (Table 3) and the comparator's
  characteristics (Table 45).
- Data sheets for four parts of the energy circuit: TI's LM4040 (U3, the 3.0 V reference), TI's
  SN74LVC1G175 for the function table of U18 (a Nexperia 74LVC1G175, the same function), and
  Nexperia's PMPB15XP (Q3, the range switch) and PMF250XNE (the calibration switches).

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
  energy. The probe calibrates against its own loads as it boots. A capture start calibrates again
  only after a failed calibration or a change of jumper setting. The accumulator
  updates without host polling and remains readable after capture stops. "Energy measurement"
  below derives the circuit's settings from the documents.

Not yet: the analog trace input and the LPC804.

## What the two boards share

- The debug interface pins: P0_0 SWCLK, P0_1 TDI, P0_2 SWDIO, P0_3 TDO/SWO, P0_13 RESET_TXEN,
  P0_14 the target's ISP-EN, P0_19 nRESET, P0_22 detect, P0_28 SWDIO_TXEN, P0_31 VREF (schematic
  sheet 3). Four-wire JTAG reaches J7: TMS 2, TCK 4, TDO 6, TDI 8, nRESET 10, ISP-EN 7, detect 9
  (sheet 5). TCK and TDI share one 74LVC2T45 whose direction pin is HW_VER_7, so with J13 fitted
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

The rest of the Pro's pins, from schematic sheet 3:

| what | pins |
| --- | --- |
| LEDs, active low | LED1 USB PIO1_12, LED2 SWO PIO1_13, LED3 status P0_5 (red), LED4 VCOM PIO1_15, LED5 FUNC PIO1_16, LED6 SIO PIO1_17, LED7 NRG PIO1_18 |
| target supply | P0_8 TARGET_PWR_EN enables a TPS73601; its output reaches the target only through J6, at the voltage J5 picks |
| second VCOM (J26) | Flexcomm 1: RXD PIO1_10, TXD PIO1_11. J26 is a 1x6 header labelled "firmware debug console" in the FTDI TTL-232R-3V3 cable's order (page 3): 1 GND, 4 the cable's TXD into PIO1_10 through 499R, 5 PIO1_11 out to the cable's RXD through 499R, 2, 3 and 6 not connected. ESD diodes, no level shifter: 3.3 V only. So a target's TXD goes to pin 4 and its RXD to pin 5; UM11673 section 4.3 says the reverse, which the schematic contradicts. Check on the bench before wiring a target |
| SPI | the high-speed SPI: SSEL0 P0_20, MOSI P0_26, SCK PIO1_2, MISO PIO1_3, each also a GPIO (DIO0 to DIO3) on J19 pins 2 to 5; level-translator directions IO_DIR1 PIO1_1 (DIO0 to DIO2, U17 and U13) and IO_DIR2 PIO1_25 (DIO3, U12), high to drive the header, each with a 1K pull-down (netlist, 2026-10-22). The header side runs at J19 pin 14 (VDD_IO_REF, 1.2 to 5.5 V), or 3.3 V with nothing there. The host uses vendor command 0x91; it temporarily owns the DIO pins |
| I2C | J19 pin 6 SDA P0_4, pin 7 SCL P0_21, through a PCA9617. R94/R95 pull up the LPC side to 3.3 V; the target or fixture must pull up the J19 side to pin 14's I/O reference. SWO's UART is Flexcomm 3 too, so the two exclude each other. The host uses vendor command 0x92 |
| energy measurement | voltage ADC0_0/CH0A on P0_23; current ADC0_8/CH0B on P0_16 and ACMP0_IN_D on PIO1_14; CURR_RANGE_SET ADC0_4 on PIO1_8; CTRL_HIGH_CURR on P0_15 (ADC0_2) and P0_30 (input); CTRL_HIGH_CURR_EN P0_17; CTRL_HIGH_CURR_DIS P0_29 (CMP0_OUT); calibration CTRL_CAL_R1 to R4 on PIO1_22, PIO1_23, PIO1_26, PIO1_29 and CTRL_CAL_DISC on PIO1_24; the comparator's ladder input ACMPVREF on PIO1_19, tied to VREF through R9 (0R) |
| triggers | SYNC_TRIG P0_6, SYNC_LEAD P0_7 (J2), MEAS_TRIG PIO1_7 |
| analog trace input | ANALOG_IN1, ADC0_11 on PIO1_0, behind a voltage follower |
| LPC804 | reset PIO1_9, ISP enable PIO1_31, its ISP UART on Flexcomm 4: TXD PIO1_20, RXD PIO1_21 |

## Energy measurement

This section derives the firmware's energy measurement from the schematic (sheets 2, 3 and 7),
UM11673 section 4.4, AN13660 section 5, the LPC55S6x manuals and the part data sheets listed under
Sources.

### The circuit

- **The path.** Current to the target enters J9 pin 1 and leaves at pin 3 (UM11673 section 4.4).
  Between them it passes two pairs of back-to-back P-channel switches and the sense resistors.
  The switches conduct while CTRL_CAL_DISC (PIO1_24) is low. Taking it high opens them and turns on
  Q22 and Q2, which feed the sense resistors from the probe's own 3.3 V instead. That is the
  isolation UM11673 describes for calibration.
- **The sense resistors.** In the low range one resistor carries the current: R68 (50 Ω) with J16
  at 1-2, or R11 (200 Ω) at 2-3. In the high range, Q3 adds R69 (0.24 Ω) or R71 (2.0 Ω) in
  parallel, chosen by J17. Q3's on-resistance is part of the high range's resistance, and so is
  J17's contact. Q3's gate is at 0 V while it conducts, and its source is at the supply, so its
  on-resistance follows the supply: about 16.5 mΩ at 3.3 V (PMPB15XP, Figs. 8 and 9).
- **The amplifier.** U20A, Q4, R78 and R83 turn the voltage across the sense resistor into
  R83/R78 = 10 times itself at TP6. U20B adds a gain of 1 + R84/R80 = 3. Its output is 30 times the
  voltage across the resistor, the sheet's own equation. It goes to ADC0_8 on PIO0_16 and to the
  comparator's input D on PIO1_14.
- **The voltage.** R73 and R75 (330 kΩ each) halve the voltage on the target side of the sense
  resistors. U19 buffers it to ADC0_0 on PIO0_23. The divider draws its current through the sense
  resistor: 5.0 µA at 3.3 V and 2.7 µA at 1.8 V, as the sheet's note on bleeding resistors says.
- **The reference.** U3, an LM4040 at 3.0 V, drives the VREF net (sheet 2). VREF is the ADC's
  VREFP. It also reaches the comparator's ladder input, ACMPVREF on PIO1_19, through R9 (0 Ω,
  sheet 3). And it feeds R63, R8 and R3, from which J18 picks two thirds of VREF at 1-2 or one third
  at 2-3. That level, CURR_RANGE_SET on PIO1_8, is how the firmware reads the jumper setting.

### Ranges

The firmware references the ADC to VREFP (UM11126 Table 720). Full scale is then 3.0 V at the
amplifier's output, which is 100 mV across the sense resistor.

| setting (J16 to J18) | range | resistance | full scale | per ADC code | switches up at |
| --- | --- | --- | --- | --- | --- |
| 350 mA (1-2) | low | 50 Ω | 2.0 mA | 31 nA | 1.81 mA |
| | high | 0.255 Ω | 392 mA | 6.0 µA | |
| 50 mA (2-3) | low | 200 Ω | 500 µA | 7.6 nA | 452 µA |
| | high | 2.00 Ω | 50.1 mA | 0.76 µA | |

The 50 mA setting's high range ends at UM11673's 50 mA. The 350 mA setting's ends above the 350 mA
the probe's target supply can give.

### Range switching

- **The latch.** U18, a 74LVC1G175 D flip-flop, holds the range. Its output, CTRL_HIGH_CURR, drives
  Q3's gate, and its D input is tied high. A rising edge on its clock, CTRL_HIGH_CURR_EN (PIO0_17),
  sets the output and turns Q3 off: the low range. Its reset, CTRL_HIGH_CURR_DIS (PIO0_29), clears
  the output whatever the clock does and turns Q3 on: the high range (SN74LVC1G175 Table 1). The
  firmware reads the output back on PIO0_30. It also converts it on ADC0_2 after every sample, so
  each sample carries its range.
- **Up: the comparator.** PIO0_29's function 7 is CMP0_OUT (data sheet Table 3). The comparator
  takes the amplifier's output on input D, PIO1_14 (NMUX 4), and the ladder on its other input
  (PMUX 0) (UM11126 Table 304). Its output falls, and resets the latch, when the current goes past
  the ladder's voltage. No firmware runs on the way up, which is how UM11673 and AN13660 describe
  the switch to the high range.
- **The ladder.** The ladder takes VREF through ACMPVREF (VREFINPUT 0). It picks one of 32 steps
  from ground to that voltage (UM11126 section 39.5.2 and Fig 145): 31 steps of 96.8 mV, which is
  3.23 mV across the sense resistor. PIO1_19's analog switch is open out of reset, which leaves the
  ladder with no reference, so the firmware closes it (UM11126 section 15.4.2.5). The comparator runs in
  its high-speed mode, without hysteresis, because the latch already holds the range.
- **The threshold.** The firmware sets step 28: 2.71 V at the comparator, 90.3 mV across the sense
  resistor. UM11673 Table 4 puts the top of the low range at 400 µA in the 50 mA setting and 1.5 mA
  in the 350 mA setting. Those are 80 mV and 75 mV across the resistor. The ADC saturates at 100 mV.
  Step 28 sits midway between the 50 mA setting's 80 mV and full scale, about 10 mV from each. The
  sheet's note says 50 mV, "adjustable". At 50 mV the low range would end at 250 µA and 1 mA, short
  of Table 4.
- **Down: the firmware.** UM11673 says the firmware switches back to the low range on an interrupt
  from a monitoring circuit. Here the capture itself is the monitor. After each DMA segment, the
  firmware looks at the samples taken on the high range. If every one read below Table 4's limit for
  its setting, it clocks the latch. The comparator still holds the latch's reset, so a current that
  has come back above the threshold keeps the high range. Between Table 4's limit and the
  threshold, a current stays on whichever range it is on. A DMA segment holds 341 samples, and the
  first whole segment under the limit re-arms the low range. That is within 6.8 ms of the current
  falling at 100,000 samples a second, and within 68 s at 10.

### Calibration

UM11673 says the circuit calibrates itself at power-on, with transistors isolating the sensing
circuitry and a known voltage applied. The firmware calibrates as it boots, before it answers the
first command, and keeps the result. A capture start calibrates again only when the last
calibration failed, or when CURR_RANGE_SET reads a different setting from the one calibrated.

Each step of a calibration waits 20 ms before it converts, so a calibration takes about 0.2 s. With
1 ms waits, the first calibration after flashing read the no-load point at the amplifier's floor.
With 5 ms it read it right.

- **Isolation.** CTRL_CAL_DISC disconnects J9 and puts the probe's 3.3 V on the sense resistors.
- **The loads.** Four switched resistors load the target side to ground: R97 (6.65 kΩ,
  CTRL_CAL_R1 on PIO1_22), R98 (66.5 Ω, CTRL_CAL_R2 on PIO1_23), R62 (1.65 kΩ, CTRL_CAL_R3 on
  PIO1_26) and R64 (16.5 Ω, CTRL_CAL_R4 on PIO1_29), all 0.5%. A load's current is the measured
  target-side voltage over its resistance. Its switch, a PMF250XNE, adds about 0.25 Ω, and the
  firmware counts it. The data sheet gives 212 mΩ typical with 4.5 V on the gate and 269 mΩ with
  2.5 V. The probe drives the gate at 3.3 V.
- **Which loads calibrate which range.** From 3.3 V, three of the loads put about 97 mV across one
  sense resistor each: R1 in the 50 mA setting's low range, R3 in the 350 mA setting's low range and
  R2 in the 50 mA setting's high range. R4 puts 49 mV across the 350 mA setting's high range. Each
  range uses the loads that land inside its full scale.
- **The point with no load.** With no load on, only the divider's 5 µA flows. The low ranges take
  that as a point too. The high ranges cannot: their sense resistors turn 5 µA into microvolts, and
  the amplifier's output does not go that low. On one unit it stopped at about 57 ADC codes, which
  in the 350 mA setting's high range is about 0.3 mA. The divider keeps the low ranges clear of
  that floor whenever the target is powered: 2.7 µA at 1.8 V is still 88 codes in the 350 mA
  setting and 355 in the 50 mA setting.

| setting | range | points | across the resistor |
| --- | --- | --- | --- |
| 350 mA | low | no load, R1, R3 | 0.25 mV, 25 mV, 97 mV |
| | high | R3, R2, R4 | 0.5 mV, 13 mV, 49 mV |
| 50 mA | low | no load, R1 | 1 mV, 97 mV |
| | high | R1, R3, R2 | 1 mV, 4 mV, 96 mV |

- **The latch during calibration.** The low ranges' loads put more than the threshold across their
  resistor, so the comparator would reset the latch under them. The firmware takes PIO0_29 as an
  output and holds the reset high. It clocks the latch to the low range and takes those points.
  Then it drives the reset low for the high range's points. Afterwards PIO0_29 goes back to the
  comparator.
- **Saturation.** A load sized for 97 mV on a range that ends at 100 mV leaves 2.6% for tolerances
  before the firmware's limit, a point that reads above 99.8% of full scale, which it leaves out.
  The sheet gives no tolerance for the amplifier's resistors. A range left with fewer than two points fails calibration, and the
  capture does not start. The 50 mA setting's low range has only R1, so it is the range a unit
  with too much gain would lose.
- **The conversion.** Each range's points make a piecewise-linear table from ADC code to the
  current through the sense resistor. From every sample the firmware then takes the divider's
  current, the target-side voltage over 660 kΩ. Each calibration conversion is averaged 128 times,
  whatever rate the capture runs at. A host can read each range's points back (HOST-INTERFACE.md,
  `0x90` mode 4).

### Accuracy limits

- The 3.0 V reference sets the absolute scale of every voltage and current: 0.5% initial and
  100 ppm/°C for the LM4040's C grade.
- Q3's on-resistance follows the target's voltage, and calibration runs at 3.3 V. With a 1.8 V
  target, the 350 mA setting's high range reads 2.8% high: Q3 goes from 16.5 mΩ to 23.7 mΩ
  (PMPB15XP Fig. 8) beside R69's 0.24 Ω. In the 50 mA setting the same target reads 0.35% high.
  UM11673 gives the 350 mA setting's high range 5% accuracy and the others 1%.
- J17's contact is in the high range's resistance too. Calibration measures it, but moving the
  jumpers away and back leaves the setting as it was, so only a reset calibrates again.
- The calibration switches are counted at their typical on-resistance. Their spread moves the
  350 mA setting's high range by up to about 0.3%, through R4.
- The sheet gives no tolerance for the divider. It moves the current taken off each sample, which
  matters in the low ranges.
- Each sample converts the voltage, then the current, averaged over up to 128 conversions, and
  then the range. A range switch while the current converts mixes codes from both ranges, and
  neither table reads the mix right. So when a sample's range differs from the one before it, the
  energy total takes the next sample's current for it, unless that sample switched too. The sample
  keeps its own voltage. A start counts the sample before it as on the low range, because it
  clocks the latch to the low range. That includes each resume, and capture resumes after every
  target-sense read a host asks for. When capture pauses after a switching sample, before the next
  one arrives, that sample keeps its own current. The reply with the latest sample shows each
  sample as it was converted.
- The reply with the latest sample reports whole microamps. The energy total adds up nanoamps.
- While the probe measures the target's read latency, it stops the conversions, and the energy
  total leaves that time out. It measures at a connection at fast clocks, again when a measurement
  contradicts itself, and when a host asks. With capture running, the measurement found later
  latencies, or none, and those connections failed.
