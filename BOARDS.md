# The boards

One image runs on the plain MCU-Link and the MCU-Link Pro. It reads the board's straps at boot
and starts what that board has. This file has both boards' pins and straps, then the Pro's own
pins and circuits, and what the firmware does with them. docs/pro.md says what the firmware does on
a Pro for its user, and docs/probes.md what each feature needs.

## Sources

- The board header in ARM's CMSIS-DAP MCU-Link example, checked against the plain board's
  schematic net names: the debug pins both boards share.
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

## What the two boards share

The debug pins (the Pro's schematic, sheet 3):

| signal | pin | note |
| --- | --- | --- |
| TCK/SWCLK | P0_0 | TCKC for cJTAG |
| TDI | P0_1 | |
| TMS/SWDIO | P0_2 | TMSC for cJTAG |
| TDO/SWO | P0_3 | also `FC3_RXD` (`IOCON_FUNC1`) for SWO, so only one of the two configurations is live at a time: `src/swo.c` hands the pin to Flexcomm 3 while capture runs and back to the secure GPIO when it stops |
| RESET_TXEN | P0_13 | buffer direction |
| ISP-EN | P0_14 | the target's ISP line |
| nRESET | P0_19 | |
| detect | P0_22 | |
| TMS_SWDIO_TXEN | P0_28 | buffer direction, load-bearing for OScan1 |
| VREF | P0_31 | |
| target console RXD / TXD | P0_24 / P0_25 | Flexcomm 0 |
| status LED | P0_5 | LED1 on the plain board, LED3 (red) on the Pro. Also `ISP1`: the pin the ROM bootloader samples at reset, and what the ISP jumper pulls |

The debug connector is J6 on the plain board and J7 on the Pro (sheet 5), in the standard 10-pin
Cortex Debug layout: 1 VREF, 2 TMS, 4 TCK, 6 TDO, 8 TDI, 10 nRESET, 3 and 5 ground, and 9 detect,
the standard's GNDDetect, which a target grounds. Pin 7 is ISP-EN, where the standard has a key;
leave it unconnected at the target. There is no nTRST, so the TAP is reset through TMS. On the Pro,
TCK and TDI share one 74LVC2T45 whose direction pin is HW_VER_7, so with J13 fitted neither is
driven at all, whatever the firmware does. The reference-voltage follower is the plain board's, with
a 30K1 pull-up for its 33K.

The two TXEN pins are buffer direction controls. Releasing SWDIO means clearing `TMS_SWDIO_TXEN`
as well as making the pin an input: the buffer keeps driving the wire otherwise. Releasing the wire
is exactly what OScan1 needs, so this pin is load-bearing for the whole project.
`board_debug_pins_init` brings both TXEN pins up low before the pins they gate, so connecting the
probe to a running board disturbs nothing until the host asks for a port.

Both boards also have the 16 MHz crystal (DAPLink `hic_init.c`) and the high-speed USB port.

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
Pro. HW_VER_4 low with HW_VER_3 low is an on-board MCU-Link. The firmware runs there without cJTAG,
but the image does not declare that board, so verge does not install it there. Energy capture
needs HW_VER_5 low.

The rest of the Pro's pins, from schematic sheet 3:

| what | pins |
| --- | --- |
| LEDs, active low | LED1 USB PIO1_12, LED2 SWO PIO1_13, LED3 status P0_5 (red), LED4 VCOM PIO1_15, LED5 FUNC PIO1_16, LED6 SIO PIO1_17, LED7 NRG PIO1_18 |
| target supply | P0_8 TARGET_PWR_EN enables a TPS73601; its output reaches the target only through J6, at the voltage J5 picks |
| second VCOM (J26) | Flexcomm 1: RXD PIO1_10, TXD PIO1_11. J26 is a 1x6 header labelled "firmware debug console" in the FTDI TTL-232R-3V3 cable's order (page 3): 1 GND, 4 the cable's TXD into PIO1_10 through 499R, 5 PIO1_11 out to the cable's RXD through 499R, 2, 3 and 6 not connected. ESD diodes, no level shifter: 3.3 V only. So a target's TXD goes to pin 4 and its RXD to pin 5: the probe transmits on pin 5. UM11673 section 4.3 gives pins 4 and 5 the other way round |
| SPI | the high-speed SPI: SSEL0 P0_20, MOSI P0_26, SCK PIO1_2, MISO PIO1_3, each also a GPIO (DIO0 to DIO3) on J19 pins 2 to 5; level-translator directions IO_DIR1 PIO1_1 (DIO0 to DIO2, U17 and U13) and IO_DIR2 PIO1_25 (DIO3, U12), high to drive the header, each with a 1K pull-down (netlist). The header side runs at J19 pin 14 (VDD_IO_REF, 1.2 to 5.5 V), or 3.3 V with nothing there. The host uses vendor command 0x91; it temporarily owns the DIO pins |
| I2C | J19 pin 6 SDA P0_4, pin 7 SCL P0_21, through a PCA9617. R94/R95 pull up the LPC side to 3.3 V; the target or fixture must pull up the J19 side to pin 14's I/O reference. SWO's UART is Flexcomm 3 too, so the two exclude each other. The host uses vendor command 0x92 |
| energy measurement | voltage ADC0_0/CH0A on P0_23; current ADC0_8/CH0B on P0_16 and ACMP0_IN_D on PIO1_14; CURR_RANGE_SET ADC0_4 on PIO1_8; CTRL_HIGH_CURR on P0_15 (ADC0_2) and P0_30 (input); CTRL_HIGH_CURR_EN P0_17; CTRL_HIGH_CURR_DIS P0_29 (CMP0_OUT); calibration CTRL_CAL_R1 to R4 on PIO1_22, PIO1_23, PIO1_26, PIO1_29 and CTRL_CAL_DISC on PIO1_24; the comparator's ladder input ACMPVREF on PIO1_19, tied to VREF through R9 (0R) |
| triggers | SYNC_TRIG P0_6, SYNC_LEAD P0_7 (J2), MEAS_TRIG PIO1_7 |
| analog trace input | ANALOG_IN1, ADC0_11 on PIO1_0, behind a voltage follower |
| LPC804 | reset PIO1_9, ISP enable PIO1_31, its ISP UART on Flexcomm 4: TXD PIO1_20, RXD PIO1_21 |

## What the board support does not start

Three clock facts from the manual and the board support, each of which shows only as a silent,
dead peripheral.

- **The board support's 150 MHz clock is 144 MHz.** `BOARD_InitBootClocks` calls
  `BOARD_BootClockPLL150M`, whose name is wrong: it runs PLL1 at 16 MHz times 18 and attaches that
  to MAIN_CLK. `board_core_clock_init` then moves MAIN_CLK to PLL0 at 150 MHz (`src/core_clock.h`;
  `-DCORE_150MHZ=OFF` keeps 144). `CPU_CLOCK` and `TIMESTAMP_CLOCK` follow `CORE_CLOCK_HZ`, and
  every requested SWD or JTAG clock is computed from that number.
- **FRO_HF is off, and so is its divider.** The 96 MHz output of FRO192M is disabled out of reset
  (UM11126: `ENA_96MHZCLK` resets to 0), and the board support only asks for the 12 MHz one, because
  the core runs off PLL1. `FROHFDIV`'s HALT bit resets to 1 as well (UM11126 table 104). The console
  and SWO clock off `FRO_HF_DIV`: with either missing, `CLOCK_GetFroHfFreq` reports 0 Hz,
  `USART_Init` rejects the console's baud rate and `SWO_Baudrate` answers 0, and neither says why.
  `board_clocks_init` starts both. Anything else that attaches to `FRO_HF_DIV` depends on that call.
- **DWT is dormant until told otherwise.** `TIMESTAMP_GET` reads `DWT->CYCCNT`, which does not run
  until TRCENA and CYCCNTENA are set. `DAP_SWJ_Pins` with a non-zero wait spins until the counter
  passes a deadline, so a pin that never reaches the asked level would hang the probe rather than
  time out, and a transfer with `DAP_TRANSFER_TIMESTAMP` set would report 0. `board_timestamp_init`
  starts it.

## Energy measurement

This section derives the firmware's energy measurement from the schematic (sheets 2, 3 and 7),
UM11673 section 4.4, AN13660 section 5, the LPC55S6x manuals and the part data sheets listed under
Sources.

### The circuit

- **The path.** Current to the target enters J9 pin 1 and leaves at pin 3 (UM11673 section 4.4).
  Between them it passes two pairs of back-to-back P-channel switches and the sense resistors.
  The switches conduct while CTRL_CAL_DISC (PIO1_24) is low. Taking it high opens them and turns on
  two transistors that feed the sense resistors from the probe's own 3.3 V instead. That is the
  isolation UM11673 describes for calibration.
- **The sense resistors.** In the low range one resistor carries the current: 50 Ω with J16 at
  1-2, or 200 Ω at 2-3. In the high range, Q3 adds 0.24 Ω or 2.0 Ω in parallel, chosen by J17. Q3's on-resistance is part of the high range's resistance, and so is
  J17's contact. Q3's gate is at 0 V while it conducts, and its source is at the supply, so its
  on-resistance follows the supply: about 16.5 mΩ at 3.3 V (PMPB15XP, Figs. 8 and 9).
- **The drop across them.** The sense resistor is in series with the target, so on the low range
  the target's supply sits lower by its current times 50 Ω or 200 Ω: 54 mV at 1.08 mA, and up to
  about 90 mV just under the switch to the high range. The voltage the probe reports is the
  target's side, so it shows the drop.
- **The amplifier.** A first stage turns the voltage across the sense resistor into 10 times
  itself, and a second adds a gain of 3. Its output is 30 times the voltage across the resistor,
  the sheet's own equation. It goes to ADC0_8 on PIO0_16 and to the comparator's input D on
  PIO1_14.
- **The voltage.** Two 330 kΩ resistors halve the voltage on the target side of the sense
  resistors, and a buffer takes it to ADC0_0 on PIO0_23. C61, 1 nF across the lower resistor,
  filters it with a time constant of 165 µs, so the reading follows a step in the supply that much
  late (sheet 7). The divider draws its current through the sense
  resistor: 5.0 µA at 3.3 V and 2.7 µA at 1.8 V, as the sheet's note on bleeding resistors says.
- **The reference.** An LM4040 at 3.0 V drives the VREF net (sheet 2). VREF is the ADC's VREFP.
  It also reaches the comparator's ladder input, ACMPVREF on PIO1_19 (sheet 3). And it feeds a
  divider from which J18 picks two thirds of VREF at 1-2 or one third at 2-3. That level, CURR_RANGE_SET on PIO1_8, is how the firmware reads the jumper setting.

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

- **The latch.** A 74LVC1G175 D flip-flop holds the range. Its output, CTRL_HIGH_CURR, drives
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
  threshold, a current stays on whichever range it is on. Just under the limit, the firmware can
  wait longer (the backoff, below).
- **The re-arm's delay.** The firmware sizes the segments to about a millisecond of samples: the
  most words that fit in 1 ms, as a power of two. Unless the backoff holds it, the first whole
  segment under the limit re-arms the low range, so it comes back within about 2 ms of the current
  falling at any rate from 2,286
  samples a second. Below that rate a segment is its floor of 16 words, about two and a third
  samples, and the low range comes back within five. From 2,286 samples a second up, segments of
  about a millisecond keep that delay the same at every rate.
- **Between samples.** CPU 0 clocks the latch only while the ADC is between samples, in the first
  half of the time from one sample's end to the next one's trigger, so the sample after a switch
  down is the low range's alone. It tells where the ADC is from the conversion timer. Where the rate
  leaves under 2 µs between samples, just under each averaging step's limit, it clocks the latch
  whenever it gets to it.
- **What a switch down does to the target.** The low range's resistor goes back in series, and
  the target's supply falls by the drop above. The target's decoupling gives up part of its
  current while the supply settles, so for about a millisecond the reading dips. On an MSPM0L1306
  LaunchPad the reading falls from 1.08 mA to 0.26 mA and comes back with a time constant of
  0.26 ms: 50 Ω times about 5 µF, the LaunchPad's decoupling. A debug session on the target moves
  both, until its supply is cycled.
  The current rises further on the low range before a switch up, as the decoupling feeds the
  target, until it reaches the threshold. The switch up then recharges the decoupling in a spike of
  a few microseconds. The energy total misses most of that spike (Accuracy limits).
- **A target the low range cannot hold.** A target can reach the threshold on the low range
  alone, again and again: after a switch down its reading dips, then rises past its steady current
  as the decoupling refills, up to the threshold. Re-armed a segment after each switch up, such a
  target switches hundreds of times a second, and the dips, the rises and the recharges move its
  reading with the rate. The backoff, below, holds it on the high range instead.
- **The backoff.** Just under Table 4's limit, a re-arm waits for some number of segments in a row
  under it, at first one. That band runs from half the limit up to it: 0.75 to 1.5 mA in the
  350 mA setting and 0.2 to 0.4 mA in the 50 mA setting, where the drop is 37 to 90 mV. The
  number doubles each time the low range gives way, up to about a second of segments: a low phase
  shorter than 3 ms after a re-arm from the band, then the high range still reading the target
  under the limit at least half a millisecond after the sample the switch up fell in, past the
  recharge. At the slowest rates that reading is the next sample, and a low phase of one sample
  still counts as short. No re-arm comes before that reading. A low phase of 3 ms sets the number
  back to one. A target that switched up because it bursts still reads over the limit there while
  the burst lasts, so a short low phase before such a burst doesn't double the number. At the
  slowest rates the re-arm alone can take most of a short sleep. Under the band one segment is
  still enough, so a target that falls asleep gets the low range back as before. A target that
  reaches the switch up on the low range alone within 3 ms of each re-arm is held on the high
  range.
- **Holding the high range.** `0x90` mode 14 can hold the high range for a capture instead
  (docs/host-interface.md). The firmware drives U18's reset low from PIO0_29 as a GPIO output, as
  calibration does, so the latch stays clear whatever the comparator says, and CPU 0 never
  re-arms. Nothing switches, so none of the above happens to the target. The cost is that small
  currents read unreliably (Accuracy limits).

### Calibration

UM11673 says the circuit calibrates itself at power-on, with transistors isolating the sensing
circuitry and a known voltage applied. The firmware calibrates as it boots, before it answers the
first command, and keeps the result. A capture start calibrates again only when the last
calibration failed, or when CURR_RANGE_SET reads a different setting from the one calibrated.

Each step of a calibration waits 20 ms before it converts, so a calibration takes about 0.2 s. A
much shorter wait reads the no-load point at the amplifier's floor on the first calibration after
a flash.

- **Isolation.** CTRL_CAL_DISC disconnects J9 and puts the probe's 3.3 V on the sense resistors.
- **The loads.** Four switched resistors load the target side to ground: R1, 6.65 kΩ
  (CTRL_CAL_R1 on PIO1_22); R2, 66.5 Ω (PIO1_23); R3, 1.65 kΩ (PIO1_26); and R4, 16.5 Ω (PIO1_29);
  all 0.5%. A load's current is the measured
  target-side voltage over its resistance. Its switch, a PMF250XNE, adds about 0.25 Ω at the
  probe's 3.3 V gate drive, and the firmware counts it.
- **Which loads calibrate which range.** Each range uses the loads that land inside its full
  scale (the table below).
- **The point with no load.** With no load on, only the divider's 5 µA flows. The low ranges take
  that as a point too. The high ranges cannot: their sense resistors turn 5 µA into microvolts, and
  the amplifier's output does not go that low: on the Pro measured it stops at 55 to 57 ADC codes.
  The divider keeps the low ranges clear of that floor whenever the target is powered: 2.7 µA at 1.8
  V is still 88 codes in the 350 mA setting and 355 in the 50 mA setting.
- **Below the lowest point.** A table extends its first segment below its lowest point. Extended
  down from R3, the 350 mA setting's high range reads currents under 2 mA a few percent high, so R1
  is a point there too where it clears the floor (below). With R1 out, each code R3's point moves
  between boots moves such a reading by about 0.5%.
- **The floor.** Each high range first converts with no load, which reads the amplifier's floor. A
  point less than 24 codes above that reading is left out: one pinned at the floor would read the
  currents between it and the next point low. At the nominal resistance R1 gives about 84 codes less
  the amplifier's offset, about 9 codes on the Pro measured, so it lands 18 to 27 codes over the
  floor and is kept only on a unit with little offset. R1 counts on a unit whose high range has more
  resistance, as J17's contact can add (Accuracy limits).

| setting | range | points | across the resistor |
| --- | --- | --- | --- |
| 350 mA | low | no load, R1, R3 | 0.25 mV, 25 mV, 97 mV |
| | high | R1 if clear of the floor, R3, R2, R4 | 0.13 mV, 0.5 mV, 13 mV, 49 mV |
| 50 mA | low | no load, R1 | 1 mV, 97 mV |
| | high | R1, R3, R2 | 1 mV, 4 mV, 96 mV |

- **The latch during calibration.** The low ranges' loads put more than the threshold across their
  resistor, so the comparator would reset the latch under them. The firmware takes PIO0_29 as an
  output and holds the reset high. It clocks the latch to the low range and takes those points.
  Then it drives the reset low for the high range's points. Afterwards PIO0_29 goes back to the
  comparator.
- **Saturation.** A load sized for 97 mV on a range that ends at 100 mV leaves 2.6% for tolerances
  before the firmware's limit, a point that reads above 99.8% of full scale, which it leaves out.
  The sheet gives no tolerance for the amplifier's resistors. A range left with fewer than two
  points fails calibration, and the capture does not start. The 50 mA setting's low range has only
  R1, so it is the range a unit with too much gain would lose.
- **The conversion.** Each range's points make a piecewise-linear table from ADC code to the current
  through the sense resistor. From every sample the firmware then takes the divider's current, the
  target-side voltage over 660 kΩ. Each calibration conversion is averaged 128 times, whatever rate
  the capture runs at. A host can read each range's points back
  (docs/host-interface.md, `0x90` mode 4).

### Accuracy limits

- The 3.0 V reference sets the absolute scale of every voltage and current: 0.5% initial and
  100 ppm/°C for the LM4040's C grade.
- Q3's on-resistance follows the target's voltage, and calibration runs at 3.3 V. With a 1.8 V
  target, the 350 mA setting's high range reads 2.8% high: Q3 goes from 16.5 mΩ to 23.7 mΩ
  (PMPB15XP Fig. 8) beside the 0.24 Ω resistor. In the 50 mA setting the same target reads 0.35% high.
  UM11673 gives the 350 mA setting's high range 5% accuracy and the others 1%.
- In the 350 mA setting the high range reads currents under 2 mA high: 3 to 4% with R1 as a point
  on the Pro measured (Calibration, "Below the lowest point"). A capture on auto reads them on the
  low range. In the 50 mA setting the high range read a 1.08 mA load 0.9% over the 350 mA
  setting's low range.
- A boot can calibrate the 350 mA setting's high range high at every point, by 19% the one time
  it was seen, in more than twenty boots. `verge energy calibration` shows the points: every
  high-range code up by the same factor, with no jumper moved, marks such a boot. A restart
  calibrates again.
- The high range's resistance moves from boot to boot, and J17's contact is in that path: on the
  Pro measured, the 350 mA setting's high range read 0.27 to 0.75 Ω at different boots, which
  brought its full scale down to 133 mA. Calibration measures it, but moving the
  jumpers away and back leaves the setting as it was, so only a reset calibrates again. Each boot
  reads its own resistance right; a current past that boot's full scale saturates.
- The low range lowers the target's supply (the drop across the sense resistors), so a target
  whose current follows its supply draws differently on the two ranges, and is read that way. A
  target that reaches the switch up on the low range alone, again and again, is read mostly on the
  high range once the backoff holds it there (Range switching).
- The high range reads unreliably within 64 codes of the amplifier's output floor (Calibration, "The
  point with no load"). The stream marks a high-range pair there under range, in either range
  setting. Through the calibration of the Pro measured, the mark starts at about 0.65 mA in the 350
  mA setting. Scaled to the 50 mA setting's sense resistor it is about 90 µA, which has not been
  measured. A capture that holds the high range reads every current under the mark this way. One
  that switches automatically does so only between a fall and its switch down.
- The calibration switches are counted at their typical on-resistance. Their spread moves the
  350 mA setting's high range by up to about 0.3%, through R4.
- The sheet gives no tolerance for the divider. It moves the current taken off each sample, which
  matters in the low ranges.
- Each sample converts the voltage and the current together, averaged over up to 128
  conversions, and then the range. A switch up while the current converts mixes codes from both
  ranges, and neither table reads the mix right. The comparator switches up at any moment, and
  the range converted after the current cannot tell a sample that switched during its
  conversions from one that switched before them. So a sample that switched up takes a stand-in
  for its current in the energy total: the next sample's current, less a share of the difference
  from the one before it. The share is half the part of the sample period its conversions span:
  almost half just under each averaging step's limit, 34% at 100,000 samples a second, 11% at
  1,000, and less below that. Over
  many switches at moments the target sets, that is the right total on average. A sample that
  switched down between samples keeps its own current; at a rate with too little time between
  samples it takes the next sample's. Either keeps its own if the next sample switched too, and
  keeps its own voltage. A start counts the sample before it as on the low range, because it
  clocks the latch to the low range, and leaves no sample before it for the stand-in. That
  includes each resume, and capture resumes after every target-sense read a host asks for. When
  capture pauses after a switching sample, before the next one arrives, that sample keeps its own
  current. The reply with the latest sample shows each sample as it was converted.
- The stand-in leaves out what happens inside the switching sample. That is most of the
  decoupling's recharge at a switch up, about its capacitance times 90 mV in the 350 mA setting.
  From about 40,000 samples a second the samples after the switch catch part of it, so the total
  rises with the rate there. A target with about 5 µF that wakes every 10 ms reads about 1.5% apart
  across 1,000 to 100,000 samples a second, highest at both ends. In the 50 mA setting, where its
  1.1 mA idle stays on the high range and nothing switches, the same rates read within 0.1% of
  each other. The stand-in also leaves out a burst shorter than one sample period that starts on
  the low range: the same target with 200 µs bursts reads 7 to 8% low at 1,000 samples a second
  against 20,000. Pick a rate whose sample period
  is well under the shortest burst.
- The voltage and the current are the A and B sides of channel 0, and one dual single-ended
  conversion takes both at once, so they average over the same stretch. Both results land in FIFO0,
  the voltage's first: with both FIFO selects alike, "the CH_A results will always be stored before
  the CH_B results" (UM11126 section 38.7.8), and the part does. Both acquire for 11.5 cycles of the
  18.75 MHz ADC clock, 613 ns, and the range for 3.5 cycles. A much shorter acquisition reads an
  unaveraged voltage high, by 0.27% at 292 ns. 625 ns read within 0.02% of 16 averages, with the two
  sides converted one after the other (the measurement log). A sample with N averages takes 27 + 32
  N ADC cycles. The firmware uses the most averaging that finishes a sample before the next is due:
  128 up to 4,546 samples a second, 64 up to 9,036, 32 up to 17,840, 16 up to 34,786, 8 up to
  66,254, and 4 up to 100,000. The ADC and the timer that starts each sample both run from PLL0, so
  a sample's length in timer counts is exact.
- While the probe measures the target's read latency, it stops the conversions, and the energy total
  leaves that time out. It measures at a connection at fast clocks, when the host changes the clock,
  again when a measurement contradicts itself, and when Verge's own tools ask. The capture's DMA
  would otherwise delay the measurement's samples, and it would find later latencies, or none.
